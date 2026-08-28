/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/state_lane.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace zlink::framework::runtime
{

/* Owns the one serial execution queue and state lane associated with one Actor
 * activation. The application queue remains the common bounded serial queue
 * primitive; the state lane does not allocate a dedicated worker. */
class actor_serial_executor_t
{
  public:
    using queue_t = serial_execution_queue_t;
    using queue_ptr_t = std::shared_ptr<queue_t>;

    explicit actor_serial_executor_t (std::shared_ptr<offload_executor_t> worker_executor,
                                      queue_ptr_t queue = {}) :
        _worker_executor (std::move (worker_executor)),
        _state_lane (*_worker_executor),
        _queue (std::move (queue))
    {
        if (!_queue) {
            _queue = std::make_shared<queue_t> (
              *_worker_executor, serial_execution_queue_options_t{}, queue_t::error_handler_t{},
              serial_lane_policy_t::actor_delivery ());
        }
    }

    actor_serial_executor_t (const actor_serial_executor_t &) = delete;
    actor_serial_executor_t &operator= (const actor_serial_executor_t &) = delete;

    bool execute_actor (std::string name,
                        queue_t::async_work_t work,
                        serial_work_options_t options = {}) const
    {
        return _queue
               && _queue->try_post_async (std::move (name), std::move (work), std::move (options));
    }

    bool execute_actor (std::string name,
                        std::function<void ()> work,
                        serial_work_options_t options = {}) const
    {
        return _queue && _queue->try_post (std::move (name), std::move (work), std::move (options));
    }

    result_t<serial_submission_id_t> execute_actor (std::string name,
                                                    queue_t::async_work_t work,
                                                    std::function<void ()> cancel,
                                                    serial_work_options_t options = {}) const
    {
        return _queue->try_post_cancellable_async (std::move (name), std::move (work),
                                                   std::move (cancel), std::move (options));
    }

    bool execute_lifecycle (std::string name, queue_t::async_work_t work) const
    {
        return _queue
               && _queue->try_post_async (std::move (name), std::move (work),
                                          serial_work_options_t{serial_work_lane_t::lifecycle});
    }

    bool execute_lifecycle (std::string name, std::function<void ()> work) const
    {
        return _queue
               && _queue->try_post (std::move (name), std::move (work),
                                    serial_work_options_t{serial_work_lane_t::lifecycle});
    }

    result_t<std::shared_ptr<detail::deferred_barrier_t>> execute_lifecycle (std::string name) const
    {
        return _queue->reserve_handoff_barrier (std::move (name));
    }

    const queue_ptr_t &queue () const noexcept { return _queue; }

    const void *queue_identity () const noexcept { return _queue.get (); }

    bool closed () const noexcept { return !_queue || _queue->closed (); }

    void close ()
    {
        if (_queue)
            _queue->close ();
        _state_lane.close ();
    }

  private:
    std::shared_ptr<offload_executor_t> _worker_executor;
    state_lane_t _state_lane;
    queue_ptr_t _queue;
};

} // namespace zlink::framework::runtime
