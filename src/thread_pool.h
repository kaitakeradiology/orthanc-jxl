/*
 * Copyright (C) 2025 Ryan Walklin <ryan@kaitakeradiology.co.nz>
 *
 * This file is part of orthanc-jxl.
 *
 * orthanc-jxl is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * orthanc-jxl is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * orthanc-jxl. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace orthanc_jxl {

/**
 * Fixed-size worker pool reused for the lifetime of the plugin.
 *
 * Frame-level encode/decode work is dispatched here so that importing a study
 * does not pay the cost of spinning up and tearing down OS threads for every
 * instance. Tasks are independent and never block on each other, so a simple
 * queue is sufficient.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop_(false) {
        if (numThreads == 0) {
            numThreads = 1;
        }
        workers_.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t Size() const { return workers_.size(); }

    // Enqueue a task. Exceptions thrown by the task are swallowed by the worker;
    // callers that need results/errors should capture them into shared state.
    void Enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

private:
    void WorkerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_;
};

/**
 * Run fn(0..n-1) across the pool and block until all complete. Each index is
 * processed exactly once; fn must be safe to invoke concurrently for distinct
 * indices (e.g. writing to its own result slot). The first exception thrown by
 * any task is rethrown to the caller after all tasks finish.
 *
 * Falls back to sequential execution for trivial sizes. Must not be called from
 * a pool worker thread (would risk deadlock).
 */
template <typename Fn>
void ParallelFor(ThreadPool& pool, size_t n, Fn&& fn) {
    if (n == 0) {
        return;
    }
    if (n == 1 || pool.Size() <= 1) {
        for (size_t i = 0; i < n; ++i) {
            fn(i);
        }
        return;
    }

    std::mutex m;
    std::condition_variable done;
    size_t remaining = n;
    std::exception_ptr firstError;

    for (size_t i = 0; i < n; ++i) {
        pool.Enqueue([&, i] {
            try {
                fn(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(m);
                if (!firstError) {
                    firstError = std::current_exception();
                }
            }
            std::lock_guard<std::mutex> lock(m);
            if (--remaining == 0) {
                done.notify_one();
            }
        });
    }

    std::unique_lock<std::mutex> lock(m);
    done.wait(lock, [&] { return remaining == 0; });
    if (firstError) {
        std::rethrow_exception(firstError);
    }
}

}  // namespace orthanc_jxl
