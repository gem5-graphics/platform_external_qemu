// Copyright (C) 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "android/base/Compiler.h"
#include "android/base/Optional.h"
#include "android/base/threads/WorkerThread.h"

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

//
// ThreadPool<Item> - a simple collection of worker threads to process enqueued
// items on multiple cores.
//
// To create a thread pool supply a processing function and an optional number
// of threads to use (default is number of CPU cores).
// Thread pool distributes the work in simple round robin manner over all its
// workers - this means individual items should be simple and take similar time
// to process.
//
// Usage is very similar to one of WorkerThread, with difference being in the
// number of worker threads used and in existence of explicit done() method:
//
//      struct WorkItem { int number; };
//
//      ThreadPool<WorkItem> tp([](WorkItem&& item) { std::cout << item.num; });
//      tp.enqueue({1});
//      tp.enqueue({2});
//      tp.enqueue({3});
//      tp.enqueue({4});
//      tp.enqueue({5});
//      tp.done();
//      tp.join();
//
// Make sure that the processing function won't block worker threads - thread
// pool has no way of detecting it and may potentially get all workers to block,
// resulting in a hanging application.
//

namespace android {
namespace base {

template <class ItemT>
class ThreadPool {
    DISALLOW_COPY_AND_ASSIGN(ThreadPool);

public:
    using Item = ItemT;
    using Worker = WorkerThread<Optional<Item>>;
    using Processor = std::function<void(Item&&)>;

    ThreadPool(int threads, Processor&& processor)
        : mProcessor(std::move(processor)) {
        if (threads < 1) {
            threads = System::get()->getCpuCoreCount();
        }
        mWorkers.resize(threads);
        for (auto& workerPtr : mWorkers) {
            workerPtr.reset(new Worker([this](Optional<Item>&& item) {
                if (!item) {
                    return Worker::Result::Stop;
                }
                mProcessor(std::move(item.value()));
                return Worker::Result::Continue;
            }));
        }
    }
    explicit ThreadPool(Processor&& processor)
        : ThreadPool(0, std::move(processor)) {}
    ~ThreadPool() {
        done();
        join();
    }

    void start() {
        for (auto& workerPtr : mWorkers) {
            workerPtr->start();
        }
    }

    void done() {
        for (auto& workerPtr : mWorkers) {
            workerPtr->enqueue(kNullopt);
        }
    }

    void join() {
        for (auto& workerPtr : mWorkers) {
            workerPtr->join();
        }
        mWorkers.clear();
    }

    void enqueue(Item&& item) {
        int currentIndex =
                mNextWorkerIndex.fetch_add(1, std::memory_order_relaxed);
        mWorkers[currentIndex % mWorkers.size()]->enqueue(std::move(item));
    }

private:
    Processor mProcessor;
    std::vector<std::unique_ptr<Worker>> mWorkers;
    std::atomic<int> mNextWorkerIndex{0};
};

}  // namespace base
}  // namespace android
