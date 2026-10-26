// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/jobs.hpp"

#include "f3d/core/log.hpp"
#include "f3d/core/profile.hpp"

#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <exception>
#include <thread>

namespace f3d::core::jobs {

JobSystem::JobSystem(unsigned interactive_workers, unsigned background_workers) {
    const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    interactive_workers_ = interactive_workers > 0 ? interactive_workers : std::max(1u, cores - 1);
    background_workers_ = background_workers > 0 ? background_workers : std::max(1u, cores / 4);
    interactive_ = std::make_unique<tf::Executor>(interactive_workers_);
    background_ = std::make_unique<tf::Executor>(background_workers_);
}

JobSystem::~JobSystem() {
    wait_idle();
}

void JobSystem::enqueue(Priority priority, std::function<void()> task) {
    ++pending_;
    tf::Executor& executor = priority == Priority::Interactive ? *interactive_ : *background_;
    executor.silent_async([this, task = std::move(task)]() mutable {
        F3D_ZONE_N("Job");
        try {
            task();
        } catch (const std::exception& e) {
            F3D_LOG_ERROR("Job failed: {}", e.what());
        } catch (...) {
            F3D_LOG_ERROR("Job failed with an unknown exception");
        }
        --pending_;
    });
}

void JobSystem::submit(Priority priority, CancellationToken token, std::function<void(const CancellationToken&)> work) {
    enqueue(priority, [token, work = std::move(work)] {
        if (!token.cancelled()) {
            work(token);
        }
    });
}

void JobSystem::post(CancellationToken token, std::function<void()> fn) {
    std::scoped_lock lock(mutex_);
    continuations_.push_back({std::move(token), std::move(fn)});
}

std::size_t JobSystem::run_continuations() {
    std::vector<Continuation> ready;
    {
        std::scoped_lock lock(mutex_);
        ready.swap(continuations_);
    }
    if (ready.empty()) {
        return 0;
    }
    F3D_ZONE;
    std::size_t ran = 0;
    for (Continuation& c : ready) {
        if (!c.token.cancelled()) {
            c.fn();
            ++ran;
        }
    }
    return ran;
}

void JobSystem::wait_idle() {
    interactive_->wait_for_all();
    background_->wait_for_all();
}

void parallel_for(std::size_t count, const std::function<void(std::size_t, std::size_t)>& fn, std::size_t min_chunk) {
    if (count == 0) {
        return;
    }
    static tf::Executor executor(std::max(1u, std::thread::hardware_concurrency()));
    const std::size_t workers = executor.num_workers();
    const std::size_t chunks = std::max<std::size_t>(1, std::min(workers * 4, count / std::max<std::size_t>(min_chunk, 1)));
    if (chunks == 1) {
        fn(0, count);
        return;
    }
    tf::Taskflow flow;
    const std::size_t size = (count + chunks - 1) / chunks;
    for (std::size_t begin = 0; begin < count; begin += size) {
        const std::size_t end = std::min(count, begin + size);
        flow.emplace([&fn, begin, end] { fn(begin, end); });
    }
    executor.run(flow).wait();
}

} // namespace f3d::core::jobs
