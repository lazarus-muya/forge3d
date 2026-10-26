// SPDX-License-Identifier: Apache-2.0
#pragma once

// The .forge container (phases.md P2.6): a ZIP archive whose entries are either
// stored or zstd-compressed (ZIP method 93, APPNOTE 6.3.7), never deflated.
// Written as a stream (local headers + data, then the central directory) and
// read through the central directory. Timestamps are fixed, so the same content
// always produces the same bytes. Limits: 4 GiB per entry and per archive,
// 65,535 entries (no ZIP64).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3d::io {

using Bytes = std::vector<std::uint8_t>;

enum class Compression : std::uint8_t {
    Store, ///< ZIP method 0
    Zstd,  ///< ZIP method 93
};

[[nodiscard]] std::string_view to_string(Compression compression) noexcept;

/// CRC-32 (ISO-HDLC, as used by ZIP), continuing from `crc`.
[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) noexcept;

/// One entry of an archive's central directory.
struct ArchiveEntry {
    std::string name;
    Compression compression = Compression::Store;
    std::uint32_t crc = 0;
    std::uint64_t size = 0;        ///< uncompressed
    std::uint64_t stored_size = 0; ///< as stored in the file
    std::uint64_t header_offset = 0;
};

/// Streams an archive into an open file. Entries are written as they are added.
class ArchiveWriter {
public:
    /// `file` must be open for binary writing at offset 0; the writer doesn't own it.
    explicit ArchiveWriter(std::FILE* file);

    /// Adds an entry. Names use '/' separators and must be unique. zstd entries
    /// that don't get smaller are stored instead. Returns false (see error()) for
    /// a rejected entry, which leaves the archive usable, or an I/O error, which
    /// doesn't.
    bool add(std::string_view name, std::span<const std::uint8_t> data, Compression compression, int zstd_level = 3);
    bool add(std::string_view name, std::string_view text, Compression compression, int zstd_level = 3);

    /// Writes the central directory. Returns false on an I/O error or a limit overflow.
    bool finish();

    [[nodiscard]] const std::vector<ArchiveEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    bool write(const void* data, std::size_t size);
    bool fail(std::string message);

    std::FILE* file_;
    std::uint64_t offset_ = 0;
    std::vector<ArchiveEntry> entries_;
    std::string error_;
    bool broken_ = false; ///< an I/O error: nothing more can be written
    bool finished_ = false;
};

/// Reads an archive file (central directory up front, entries on demand).
class ArchiveReader {
public:
    /// Opens `path` and reads its central directory. Check ok() / error().
    explicit ArchiveReader(const std::filesystem::path& path);
    ~ArchiveReader();
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    [[nodiscard]] bool ok() const noexcept { return file_ != nullptr && error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    [[nodiscard]] const std::vector<ArchiveEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const ArchiveEntry* find(std::string_view name) const;

    /// Reads and decompresses an entry, verifying its size and CRC. Empty on
    /// error (see error()).
    [[nodiscard]] std::optional<Bytes> read(std::string_view name);
    [[nodiscard]] std::optional<std::string> read_text(std::string_view name);

private:
    bool fail(std::string message);

    std::FILE* file_ = nullptr;
    std::vector<ArchiveEntry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    std::string error_;
};

/// Opens a file with a UTF-8-safe path (wide on Windows). `mode` as for fopen.
[[nodiscard]] std::FILE* open_file(const std::filesystem::path& path, const char* mode);

} // namespace f3d::io
