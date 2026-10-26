// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's import queue (phases.md P3.2): imports run as background jobs,
// several at once; the queue tracks their progress and reports results on the
// main thread (errors go to the console). UI-free; ImportQueuePanel shows it.

#include "f3d/assets/asset_database.hpp"
#include "f3d/core/guid.hpp"
#include "f3d/core/jobs.hpp"
#include "f3d/core/vfs.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3d::editor {

class ImportQueue {
public:
    enum class State { Queued, Running, Done, Failed, Cancelled };

    struct Item {
        core::Guid guid;
        std::string path;
        std::string source_hash; ///< the hash the import was requested for
        State state = State::Queued;
        float progress = 0.0f;
        std::string error;
        std::vector<std::string> warnings;
    };

    ImportQueue(core::jobs::JobSystem& jobs, core::vfs::FileSystem& fs);

    /// Queues an import of `asset`. Returns false if it is already queued or
    /// running, or (unless `force`) already failed for the same source contents.
    bool enqueue(const assets::AssetRecord& asset, bool force = false);
    /// Queues every stale asset of `database`. Returns how many were queued.
    std::size_t enqueue_stale(const assets::AssetDatabase& database);

    /// Cancels everything queued or running (running imports stop at their next check).
    void cancel_all();
    /// Drops finished, failed and cancelled items from the list.
    void clear_finished();

    /// Copies progress from the workers (main thread, once per frame).
    void update();

    [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }
    /// Items queued or running.
    [[nodiscard]] std::size_t active() const noexcept;

    /// Called on the main thread when an item finishes (done, failed or cancelled).
    std::function<void(const Item&)> on_finished;

private:
    struct Shared {
        std::atomic<float> progress{0.0f};
        std::atomic<bool> running{false};
    };
    void finished(std::size_t index, bool ok, bool cancelled, std::string error, std::vector<std::string> warnings);

    core::jobs::JobSystem& jobs_;
    core::vfs::FileSystem& fs_;
    core::jobs::CancellationToken token_;
    std::vector<Item> items_;
    std::vector<std::shared_ptr<Shared>> shared_;
    std::unordered_map<core::Guid, std::string> failed_; ///< guid -> source hash that failed
};

[[nodiscard]] const char* to_string(ImportQueue::State state) noexcept;

} // namespace f3d::editor
