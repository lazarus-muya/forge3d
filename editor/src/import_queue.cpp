// SPDX-License-Identifier: Apache-2.0
#include "f3d/editor/import_queue.hpp"

#include "f3d/assets/pipeline.hpp"
#include "f3d/core/log.hpp"

#include <algorithm>
#include <utility>

namespace f3d::editor {

const char* to_string(ImportQueue::State state) noexcept {
    switch (state) {
    case ImportQueue::State::Queued:
        return "Queued";
    case ImportQueue::State::Running:
        return "Importing";
    case ImportQueue::State::Done:
        return "Done";
    case ImportQueue::State::Failed:
        return "Failed";
    case ImportQueue::State::Cancelled:
        return "Cancelled";
    }
    return "?";
}

ImportQueue::ImportQueue(core::jobs::JobSystem& jobs, core::vfs::FileSystem& fs) : jobs_(jobs), fs_(fs) {}

std::size_t ImportQueue::active() const noexcept {
    return static_cast<std::size_t>(std::count_if(items_.begin(), items_.end(), [](const Item& i) {
        return i.state == State::Queued || i.state == State::Running;
    }));
}

bool ImportQueue::enqueue(const assets::AssetRecord& asset, bool force) {
    for (const Item& item : items_) {
        if (item.guid == asset.guid && (item.state == State::Queued || item.state == State::Running)) {
            return false;
        }
    }
    if (const auto it = failed_.find(asset.guid); !force && it != failed_.end() && it->second == asset.source_hash) {
        return false; // failed for these exact contents: wait for the source to change
    }
    const std::size_t index = items_.size();
    items_.push_back({asset.guid, asset.path, asset.source_hash, State::Queued, 0.0f, {}, {}});
    auto shared = std::make_shared<Shared>();
    shared_.push_back(shared);
    core::jobs::CancellationToken token = token_;
    jobs_.submit(
        core::jobs::Priority::Background, token,
        [this, asset, shared](const core::jobs::CancellationToken& t) {
            shared->running = true;
            return assets::import_asset(fs_, asset, &t, [shared](float p) { shared->progress = p; });
        },
        [this, index](assets::ImportOutcome outcome) {
            finished(index, outcome.ok, outcome.cancelled, std::move(outcome.error), std::move(outcome.warnings));
        });
    return true;
}

std::size_t ImportQueue::enqueue_stale(const assets::AssetDatabase& database) {
    std::size_t queued = 0;
    for (const assets::AssetRecord& record : database.records()) {
        if (record.stale() && enqueue(record)) {
            ++queued;
        }
    }
    return queued;
}

void ImportQueue::finished(std::size_t index, bool ok, bool cancelled, std::string error,
                           std::vector<std::string> warnings) {
    if (index >= items_.size()) {
        return;
    }
    Item& item = items_[index];
    item.progress = 1.0f;
    item.warnings = std::move(warnings);
    for (const std::string& warning : item.warnings) {
        F3D_LOG_WARN("Import {}: {}", item.path, warning);
    }
    if (ok) {
        item.state = State::Done;
        failed_.erase(item.guid);
        F3D_LOG_INFO("Imported {}", item.path);
    } else if (cancelled) {
        item.state = State::Cancelled;
    } else {
        item.state = State::Failed;
        item.error = std::move(error);
        failed_[item.guid] = item.source_hash;
        F3D_LOG_ERROR("Import failed: {}", item.error);
    }
    if (on_finished) {
        on_finished(item);
    }
}

void ImportQueue::cancel_all() {
    token_.cancel();
    token_ = core::jobs::CancellationToken{}; // new imports use a fresh token
    for (Item& item : items_) {
        if (item.state == State::Queued || item.state == State::Running) {
            item.state = State::Cancelled; // their continuations won't run
        }
    }
}

void ImportQueue::clear_finished() {
    for (std::size_t i = items_.size(); i-- > 0;) {
        if (items_[i].state != State::Queued && items_[i].state != State::Running) {
            // Keep indices stable for running jobs: only drop finished items at the end.
            if (i + 1 == items_.size()) {
                items_.pop_back();
                shared_.pop_back();
            }
        }
    }
    if (active() == 0) {
        items_.clear();
        shared_.clear();
    }
}

void ImportQueue::update() {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        Item& item = items_[i];
        if (item.state == State::Queued && shared_[i]->running) {
            item.state = State::Running;
        }
        if (item.state == State::Running) {
            item.progress = shared_[i]->progress;
        }
    }
}

} // namespace f3d::editor
