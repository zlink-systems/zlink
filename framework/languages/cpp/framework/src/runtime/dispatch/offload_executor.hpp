/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
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
    class role_t
    {
      public:
        virtual ~role_t () = default;
        // Returns true after transferring the receive role.
        // Exceptions preserve the dedicated receive thread's terminate policy.
        virtual bool run_until_handoff () = 0;
    };

    template <typename Work> static void run_application_turn (Work &&work) noexcept
    {
        try {
            work ();
        }
        catch (...) {
            // The application owner converts failures into task completions.
        }
    }

    explicit offload_executor_t (std::size_t worker_count = 1,
                                 std::string thread_name = "zlink-offload");
    offload_executor_t (std::size_t min_worker_count,
                        std::size_t max_worker_count,
                        std::chrono::milliseconds idle_timeout,
                        std::string thread_name = "zlink-offload");
    virtual ~offload_executor_t ();

    offload_executor_t (const offload_executor_t &) = delete;
    offload_executor_t &operator= (const offload_executor_t &) = delete;

    bool try_submit (std::function<void ()> work);
    bool try_submit_cancellable (std::function<void (std::stop_token)> work);
    // Internal scheduling retains its separate entry point because shutdown
    // handling differs from public worker submission.
    virtual bool try_submit_internal (std::function<void ()> work);
    // Initial placement uses the workers already created by the constructor.
    bool start_role (std::shared_ptr<role_t> role);
    // Only the current holder calls this, at a receive rotation boundary.
    bool try_assign_role (std::shared_ptr<role_t> role);
    // Seals submission and drains application work while receive roles keep progressing.
    void drain_application ();
    void submit (std::function<void ()> work);
    void request_stop () noexcept;
    void drain ();
    bool drain_until (std::chrono::steady_clock::time_point deadline);
    bool drained () const;
    std::size_t live_worker_count () const;
    // Threads this executor still owns: running workers and returned workers
    // that the next worker start or drain joins.
    std::size_t owned_thread_count () const;
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    static const offload_executor_t *current_worker_executor () noexcept;
#endif

  private:
    friend struct offload_executor_test_access_t;

#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
    static thread_local const offload_executor_t *_current_worker_executor;
#endif
    struct work_item_t
    {
        std::function<void (std::stop_token)> work;
        std::optional<std::stop_source> cancellation;
    };

    void worker_loop ();
    void start_worker_locked ();
    void retire_current_worker_locked ();
    void join_retired_workers_locked ();

    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::condition_variable _empty;
    std::queue<work_item_t> _queue;
    std::queue<std::shared_ptr<role_t>> _roles;
    // [0, _live_workers) run worker_loop; the rest have returned.
    std::vector<std::thread> _workers;
    std::vector<std::stop_source *> _active_cancellations;
    std::size_t _min_worker_count = 1;
    std::size_t _max_worker_count = 1;
    std::chrono::milliseconds _idle_timeout{0};
    std::string _thread_name;
    bool _stopping = false;
    std::size_t _active = 0;
    std::size_t _role_active = 0;
    std::size_t _live_workers = 0;
    std::size_t _idle_workers = 0;
};

} // namespace zlink::framework::runtime
