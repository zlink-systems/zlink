/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace zlink::framework::runtime
{

// Single-owner execution lane for one component's mutable state.  This is
// deliberately separate from serial_execution_queue_t: a state owner needs
// FIFO single-turn execution, not handler admission or lifecycle policy.
class state_lane_t
{
  public:
    explicit state_lane_t (offload_executor_t &executor) noexcept;
    ~state_lane_t ();

    state_lane_t (const state_lane_t &) = delete;
    state_lane_t &operator= (const state_lane_t &) = delete;

    template<typename Work>
    auto run (Work &&work)
      -> std::future<std::invoke_result_t<std::decay_t<Work> &>>
    {
        using result_t = std::invoke_result_t<std::decay_t<Work> &>;

        throw_if_reentrant ();
        if (_closed.load (std::memory_order_acquire)) {
            throw std::runtime_error ("state lane is closed");
        }

        auto completion = std::make_shared<std::promise<result_t>> ();
        auto result = completion->get_future ();
        if (!enqueue (
          [completion, work = std::forward<Work> (work)] () mutable {
              try {
                  if constexpr (std::is_void_v<result_t>) {
                      std::invoke (work);
                      completion->set_value ();
                  }
                  else {
                      completion->set_value (std::invoke (work));
                  }
              }
              catch (...) {
                  completion->set_exception (std::current_exception ());
              }
          },
          [completion] (std::exception_ptr error) {
              completion->set_exception (std::move (error));
          })) {
            throw std::runtime_error ("state lane is closed");
        }
        return result;
    }

    // Queues synchronous state work without waiting for it.  Its exception is
    // intentionally contained so one fire-and-forget callback cannot strand
    // the turns behind it.
    bool try_post (std::function<void ()> work);

    void throw_if_reentrant () const;
    bool is_on_lane () const noexcept;
    static state_lane_t *current () noexcept;

    // Stops admission and waits for the already accepted FIFO mailbox to
    // finish.  Repeated calls are safe.
    void close ();
    bool closed () const noexcept
    {
        return _closed.load (std::memory_order_acquire);
    }

  private:
    struct mailbox_item_t
    {
        std::function<void ()> work;
        std::function<void (std::exception_ptr)> abandon;
    };

    bool enqueue (std::function<void ()> work,
                  std::function<void (std::exception_ptr)> abandon);
    void schedule_drain ();
    void drain_loop ();
    void abandon_pending (std::exception_ptr error) noexcept;

    static constexpr std::size_t drain_batch_limit = 100;
    static thread_local state_lane_t *_current_lane;

    offload_executor_t &_executor;
    mutable std::mutex _mailbox_mutex;
    std::condition_variable _drained;
    std::deque<mailbox_item_t> _mailbox;
    std::atomic_bool _scheduled{false};
    std::atomic_bool _closed{false};
};

} // namespace zlink::framework::runtime
