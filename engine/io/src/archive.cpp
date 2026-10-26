// SPDX-License-Identifier: Apache-2.0
#include "f3d/io/archive.hpp"

#include "f3d/core/profile.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace f3d::io {
namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::uint32_t kCentralHeaderSignature = 0x02014b50;
constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodZstd = 93;
constexpr std::uint16_t kVersionStore = 10;                 // 1.0
constexpr std::uint16_t kVersionZstd = 63;                  // 6.3: method 93
constexpr std::uint16_t kFlagUtf8 = 1 << 11;                // names are UTF-8
constexpr std::uint16_t kDosDate = (0 << 9) | (1 << 5) | 1; // 1980-01-01: fixed, so saves are reproducible
constexpr std::uint16_t kDosTime = 0;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kCentralHeaderSize = 46;
constexpr std::size_t kEndSize = 22;

constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    return table;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

void put16(std::uint8_t*& p, std::uint16_t v) {
    *p++ = static_cast<std::uint8_t>(v);
    *p++ = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t*& p, std::uint32_t v) {
    put16(p, static_cast<std::uint16_t>(v));
    put16(p, static_cast<std::uint16_t>(v >> 16));
}
std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(get16(p)) | (static_cast<std::uint32_t>(get16(p + 2)) << 16);
}

bool seek(std::FILE* f, std::uint64_t offset, int origin = SEEK_SET) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(offset), origin) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), origin) == 0;
#endif
}

std::uint64_t tell(std::FILE* f) {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_ftelli64(f));
#else
    return static_cast<std::uint64_t>(ftello(f));
#endif
}

bool read_exact(std::FILE* f, void* data, std::size_t size) {
    return size == 0 || std::fread(data, 1, size, f) == size;
}

} // namespace

std::string_view to_string(Compression compression) noexcept {
    return compression == Compression::Zstd ? "zstd" : "store";
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) noexcept {
    crc = ~crc;
    for (const std::uint8_t byte : data) {
        crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wide_mode(mode, mode + std::strlen(mode));
    std::FILE* f = nullptr;
    return _wfopen_s(&f, path.c_str(), wide_mode.c_str()) == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// --- Writer ----------------------------------------------------------------------------------

ArchiveWriter::ArchiveWriter(std::FILE* file) : file_(file) {}

bool ArchiveWriter::fail(std::string message) {
    error_ = std::move(message);
    return false;
}

bool ArchiveWriter::write(const void* data, std::size_t size) {
    if (size > 0 && std::fwrite(data, 1, size, file_) != size) {
        broken_ = true;
        return fail("write failed");
    }
    offset_ += size;
    return true;
}

bool ArchiveWriter::add(std::string_view name, std::string_view text, Compression compression, int zstd_level) {
    return add(name, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), compression,
               zstd_level);
}

bool ArchiveWriter::add(std::string_view name, std::span<const std::uint8_t> data, Compression compression,
                        int zstd_level) {
    F3D_ZONE;
    if (!file_ || finished_ || broken_) {
        return fail("archive is not writable");
    }
    if (name.empty() || name.size() > 0xFFFF || name.find('\\') != std::string_view::npos) {
        return fail("bad entry name '" + std::string(name) + "'");
    }
    if (std::any_of(entries_.begin(), entries_.end(), [&](const ArchiveEntry& e) { return e.name == name; })) {
        return fail("duplicate entry '" + std::string(name) + "'");
    }
    if (entries_.size() >= 0xFFFF || data.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail("archive limit exceeded (no ZIP64 support)");
    }

    ArchiveEntry entry;
    entry.name = std::string(name);
    entry.size = data.size();
    entry.crc = crc32(data);
    entry.header_offset = offset_;

    Bytes compressed;
    std::span<const std::uint8_t> payload = data;
    if (compression == Compression::Zstd && !data.empty()) {
        compressed.resize(ZSTD_compressBound(data.size()));
        const std::size_t n = ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), zstd_level);
        if (ZSTD_isError(n)) {
            broken_ = true;
            return fail(std::string("zstd: ") + ZSTD_getErrorName(n));
        }
        if (n < data.size()) {
            compressed.resize(n);
            payload = compressed;
            entry.compression = Compression::Zstd;
        }
    }
    entry.stored_size = payload.size();
    if (offset_ + kLocalHeaderSize + name.size() + payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail("archive larger than 4 GiB (no ZIP64 support)");
    }

    const bool zstd = entry.compression == Compression::Zstd;
    std::array<std::uint8_t, kLocalHeaderSize> header{};
    std::uint8_t* p = header.data();
    put32(p, kLocalHeaderSignature);
    put16(p, zstd ? kVersionZstd : kVersionStore);
    put16(p, kFlagUtf8);
    put16(p, zstd ? kMethodZstd : kMethodStore);
    put16(p, kDosTime);
    put16(p, kDosDate);
    put32(p, entry.crc);
    put32(p, static_cast<std::uint32_t>(entry.stored_size));
    put32(p, static_cast<std::uint32_t>(entry.size));
    put16(p, static_cast<std::uint16_t>(name.size()));
    put16(p, 0); // extra length
    if (!write(header.data(), header.size()) || !write(name.data(), name.size()) ||
        !write(payload.data(), payload.size())) {
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
}

bool ArchiveWriter::finish() {
    if (finished_) {
        return !broken_;
    }
    if (!file_ || broken_) {
        return fail("archive is not writable");
    }
    const std::uint64_t directory_offset = offset_;
    for (const ArchiveEntry& entry : entries_) {
        const bool zstd = entry.compression == Compression::Zstd;
        std::array<std::uint8_t, kCentralHeaderSize> header{};
        std::uint8_t* p = header.data();
        put32(p, kCentralHeaderSignature);
        put16(p, static_cast<std::uint16_t>((3u << 8) | 63u)); // made by: UNIX, spec 6.3
        put16(p, zstd ? kVersionZstd : kVersionStore);
        put16(p, kFlagUtf8);
        put16(p, zstd ? kMethodZstd : kMethodStore);
        put16(p, kDosTime);
        put16(p, kDosDate);
        put32(p, entry.crc);
        put32(p, static_cast<std::uint32_t>(entry.stored_size));
        put32(p, static_cast<std::uint32_t>(entry.size));
        put16(p, static_cast<std::uint16_t>(entry.name.size()));
        put16(p, 0);              // extra
        put16(p, 0);              // comment
        put16(p, 0);              // disk
        put16(p, 0);              // internal attributes
        put32(p, 0100644u << 16); // external: regular file, rw-r--r--
        put32(p, static_cast<std::uint32_t>(entry.header_offset));
        if (!write(header.data(), header.size()) || !write(entry.name.data(), entry.name.size())) {
            return false;
        }
    }
    const std::uint64_t directory_size = offset_ - directory_offset;
    if (offset_ > std::numeric_limits<std::uint32_t>::max()) {
        return fail("archive larger than 4 GiB (no ZIP64 support)");
    }
    std::array<std::uint8_t, kEndSize> end{};
    std::uint8_t* p = end.data();
    put32(p, kEndSignature);
    put16(p, 0);
    put16(p, 0);
    put16(p, static_cast<std::uint16_t>(entries_.size()));
    put16(p, static_cast<std::uint16_t>(entries_.size()));
    put32(p, static_cast<std::uint32_t>(directory_size));
    put32(p, static_cast<std::uint32_t>(directory_offset));
    put16(p, 0); // comment
    if (!write(end.data(), end.size())) {
        return false;
    }
    finished_ = true;
    return true;
}

// --- Reader ----------------------------------------------------------------------------------

ArchiveReader::ArchiveReader(const std::filesystem::path& path) {
    F3D_ZONE;
    file_ = open_file(path, "rb");
    if (!file_) {
        fail("cannot open " + path.string());
        return;
    }
    // The end record is in the last 22 + 65535 (max comment) bytes.
    if (!seek(file_, 0, SEEK_END)) {
        fail("cannot seek");
        return;
    }
    const std::uint64_t file_size = tell(file_);
    if (file_size < kEndSize) {
        fail("not an archive (too small)");
        return;
    }
    const std::uint64_t tail = std::min<std::uint64_t>(file_size, kEndSize + 0xFFFF);
    Bytes buffer(static_cast<std::size_t>(tail));
    if (!seek(file_, file_size - tail) || !read_exact(file_, buffer.data(), buffer.size())) {
        fail("cannot read the end record");
        return;
    }
    std::size_t end = buffer.size();
    for (std::size_t i = buffer.size() - kEndSize + 1; i-- > 0;) {
        if (get32(buffer.data() + i) == kEndSignature) {
            end = i;
            break;
        }
    }
    if (end == buffer.size()) {
        fail("not an archive (no end of central directory)");
        return;
    }
    const std::uint8_t* e = buffer.data() + end;
    const std::uint16_t count = get16(e + 10);
    const std::uint32_t directory_size = get32(e + 12);
    const std::uint32_t directory_offset = get32(e + 16);
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > file_size) {
        fail("corrupt central directory");
        return;
    }
    Bytes directory(directory_size);
    if (!seek(file_, directory_offset) || !read_exact(file_, directory.data(), directory.size())) {
        fail("cannot read the central directory");
        return;
    }
    std::size_t pos = 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + kCentralHeaderSize > directory.size() || get32(directory.data() + pos) != kCentralHeaderSignature) {
            fail("corrupt central directory");
            return;
        }
        const std::uint8_t* h = directory.data() + pos;
        const std::uint16_t method = get16(h + 10);
        const std::uint16_t name_length = get16(h + 28);
        const std::uint16_t extra_length = get16(h + 30);
        const std::uint16_t comment_length = get16(h + 32);
        if (pos + kCentralHeaderSize + name_length > directory.size()) {
            fail("corrupt central directory");
            return;
        }
        ArchiveEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(h + kCentralHeaderSize), name_length);
        if (method == kMethodStore) {
            entry.compression = Compression::Store;
        } else if (method == kMethodZstd) {
            entry.compression = Compression::Zstd;
        } else {
            fail("entry '" + entry.name + "' uses unsupported compression method " + std::to_string(method));
            return;
        }
        entry.crc = get32(h + 16);
        entry.stored_size = get32(h + 20);
        entry.size = get32(h + 24);
        entry.header_offset = get32(h + 42);
        index_.emplace(entry.name, entries_.size());
        entries_.push_back(std::move(entry));
        pos += kCentralHeaderSize + name_length + extra_length + comment_length;
    }
}

ArchiveReader::~ArchiveReader() {
    if (file_) {
        std::fclose(file_);
    }
}

bool ArchiveReader::fail(std::string message) {
    if (error_.empty()) {
        error_ = std::move(message);
    }
    return false;
}

const ArchiveEntry* ArchiveReader::find(std::string_view name) const {
    const auto it = index_.find(std::string(name));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

std::optional<Bytes> ArchiveReader::read(std::string_view name) {
    F3D_ZONE;
    const ArchiveEntry* entry = find(name);
    if (!file_ || !entry) {
        fail("missing entry '" + std::string(name) + "'");
        return std::nullopt;
    }
    std::array<std::uint8_t, kLocalHeaderSize> header{};
    if (!seek(file_, entry->header_offset) || !read_exact(file_, header.data(), header.size()) ||
        get32(header.data()) != kLocalHeaderSignature) {
        fail("corrupt local header for '" + entry->name + "'");
        return std::nullopt;
    }
    const std::uint64_t data_offset =
        entry->header_offset + kLocalHeaderSize + get16(header.data() + 26) + get16(header.data() + 28);
    Bytes stored(static_cast<std::size_t>(entry->stored_size));
    if (!seek(file_, data_offset) || !read_exact(file_, stored.data(), stored.size())) {
        fail("truncated entry '" + entry->name + "'");
        return std::nullopt;
    }
    Bytes data;
    if (entry->compression == Compression::Zstd) {
        data.resize(static_cast<std::size_t>(entry->size));
        const std::size_t n = ZSTD_decompress(data.data(), data.size(), stored.data(), stored.size());
        if (ZSTD_isError(n) || n != data.size()) {
            fail("cannot decompress '" + entry->name + "'" +
                 (ZSTD_isError(n) ? std::string(": ") + ZSTD_getErrorName(n) : std::string()));
            return std::nullopt;
        }
    } else {
        data = std::move(stored);
    }
    if (data.size() != entry->size || crc32(data) != entry->crc) {
        fail("checksum mismatch in '" + entry->name + "'");
        return std::nullopt;
    }
    return data;
}

std::optional<std::string> ArchiveReader::read_text(std::string_view name) {
    std::optional<Bytes> data = read(name);
    if (!data) {
        return std::nullopt;
    }
    return std::string(data->begin(), data->end());
}

} // namespace f3d::io
