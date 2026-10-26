// SPDX-License-Identifier: Apache-2.0
#pragma once

// Background jobs (phases.md P3.1), on Taskflow.
//
//   jobs.submit<Mesh>(Priority::Background, token,
//       [](const CancellationToken& t) { return import_mesh(path, t); },  // worker thread
//       [](Mesh mesh) { upload(std::move(mesh)); });                      // main thread
//
// Two worker pools give the priorities: interactive work (a thumbnail the user
// is looking at) never queues behind background work (a full re-import).
// Continuations run on the main thread inside run_continuations(), which the
// editor calls once per frame. A cancelled job never runs its continuation; a
// job that hasn't started when cancelled doesn't run at all, and a running job
// can poll its token to stop early.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace tf {
class Executor;
} // namespace tf

namespace f3d::core::jobs {

enum class Priority : std::uint8_t {
    Interactive, ///< the user is waiting for it
    Background,  ///< everything else
};

/// A shared cancellation flag. Copies refer to the same flag.
class CancellationToken {
public:
    CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const noexcept { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept { return flag_->load(std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class JobSystem {
public:
    /// `interactive_workers` / `background_workers` = 0 picks defaults from the
    /// hardware concurrency (all cores but one; a quarter of them, at least one).
    explicit JobSystem(unsigned interactive_workers = 0, unsigned background_workers = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Runs `work(token)` on a worker; then, unless `token` was cancelled by then,
    /// queues `continuation(result)` for the main thread.
    template <class Work, class Continuation>
    void submit(Priority priority, CancellationToken token, Work work, Continuation continuation) {
        using Result = std::invoke_result_t<Work&, const CancellationToken&>;
        auto task = [this, token, work = std::move(work), continuation = std::move(continuation)]() mutable {
            if (token.cancelled()) {
                return; // cancelled before it started: don't run at all
            }
            if constexpr (std::is_void_v<Result>) {
                work(token);
                post(token, [continuation = std::move(continuation)]() mutable { continuation(); });
            } else {
                post(token, [continuation = std::move(continuation), result = work(token)]() mutable {
                    continuation(std::move(result));
                });
            }
        };
        enqueue(priority, std::move(task));
    }

    /// Fire-and-forget work without a continuation.
    void submit(Priority priority, CancellationToken token, std::function<void(const CancellationToken&)> work);

    /// Queues `fn` for the main thread (from any thread); skipped if `token` is
    /// cancelled when its turn comes.
    void post(CancellationToken token, std::function<void()> fn);

    /// Runs the queued continuations (main thread). Returns how many ran.
    std::size_t run_continuations();

    /// Blocks until every submitted job has finished (continuations may be queued).
    void wait_idle();

    [[nodiscard]] unsigned interactive_workers() const noexcept { return interactive_workers_; }
    [[nodiscard]] unsigned background_workers() const noexcept { return background_workers_; }
    /// Jobs submitted but not yet finished.
    [[nodiscard]] std::size_t pending() const noexcept { return pending_.load(); }

private:
    void enqueue(Priority priority, std::function<void()> task);

    struct Continuation {
        CancellationToken token;
        std::function<void()> fn;
    };

    unsigned interactive_workers_ = 1;
    unsigned background_workers_ = 1;
    std::unique_ptr<tf::Executor> interactive_;
    std::unique_ptr<tf::Executor> background_;
    std::mutex mutex_;
    std::vector<Continuation> continuations_;
    std::atomic<std::size_t> pending_{0};
};

/// Calls `fn(begin, end)` on disjoint chunks covering [0, count) on a shared worker
/// pool (one per hardware thread) and waits for all of them. Chunks are at least
/// `min_chunk` long. Don't call it from inside a chunk.
void parallel_for(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn,
                  std::size_t min_chunk = 1);

} // namespace f3d::core::jobs
