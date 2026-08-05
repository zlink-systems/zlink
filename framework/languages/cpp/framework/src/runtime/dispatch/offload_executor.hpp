/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

namespace zlink::framework::runtime
{

class offload_executor_t
{
  public:
    explicit offload_executor_t (std::size_t worker_count = 1,
                                 std::size_t max_queue_length = 0,
                                 std::string thread_name = "zlink-offload");
    offload_executor_t (std::size_t min_worker_count,
                        std::size_t max_worker_count,
                        std::size_t max_queue_length,
                        std::chrono::milliseconds idle_timeout,
                        std::string thread_name = "zlink-offload");
    ~offload_executor_t ();

    offload_executor_t (const offload_executor_t &) = delete;
    offload_executor_t &operator= (const offload_executor_t &) = delete;

    bool try_submit (std::function<void ()> work);
    bool try_submit_cancellable (std::function<void (std::stop_token)> work);
    // Internal scheduler work is accounted for by its owning bounded queue.
    // It must not be rejected only because application worker submissions
    // filled the executor's public queue.
    bool try_submit_internal (std::function<void ()> work);
    void submit (std::function<void ()> work);
    void request_stop () noexcept;
    void drain ();
    bool drain_until (std::chrono::steady_clock::time_point deadline);
    bool drained () const;
    std::size_t live_worker_count () const;

  private:
    struct work_item_t
    {
        std::function<void (std::stop_token)> work;
        std::stop_source cancellation;
    };

    void worker_loop ();
    void start_worker_locked ();

    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::condition_variable _empty;
    std::queue<work_item_t> _queue;
    std::vector<std::thread> _workers;
    std::vector<std::stop_source *> _active_cancellations;
    std::size_t _min_worker_count = 1;
    std::size_t _max_worker_count = 1;
    std::size_t _max_queue_length = 0;
    std::chrono::milliseconds _idle_timeout{0};
    std::string _thread_name;
    bool _stopping = false;
    std::size_t _active = 0;
    std::size_t _live_workers = 0;
    std::size_t _idle_workers = 0;
};

} // namespace zlink::framework::runtime
