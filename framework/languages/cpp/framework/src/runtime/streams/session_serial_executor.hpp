/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace zlink::framework::runtime
{

/* Owns the one serial execution queue associated with one STREAM session.
 * The four semantic entrypoints preserve the pre-extraction C++ lane mapping:
 * every existing session submission used the queue's application lane. */
class session_serial_executor_t
{
  public:
    using queue_t = serial_execution_queue_t;
    using async_work_t = queue_t::async_work_t;

    explicit session_serial_executor_t (std::shared_ptr<offload_executor_t> worker_executor) :
        _worker_executor (std::move (worker_executor)),
        _queue (std::make_shared<queue_t> (*_worker_executor,
                                           serial_execution_queue_options_t{},
                                           queue_t::error_handler_t{},
                                           serial_lane_policy_t::session ()))
    {
    }

    session_serial_executor_t (const session_serial_executor_t &) = delete;
    session_serial_executor_t &operator= (const session_serial_executor_t &) = delete;

    bool execute_application (std::string name,
                              async_work_t work,
                              std::function<bool ()> cancelled = {}) const
    {
        return execute_preserved_lane (std::move (name), std::move (work), std::move (cancelled));
    }

    bool execute_control (std::string name,
                          async_work_t work,
                          std::function<bool ()> cancelled = {}) const
    {
        return execute_preserved_lane (std::move (name), std::move (work), std::move (cancelled));
    }

    bool execute_infrastructure (std::string name,
                                 async_work_t work,
                                 std::function<bool ()> cancelled = {}) const
    {
        return execute_preserved_lane (std::move (name), std::move (work), std::move (cancelled));
    }

    bool
    execute_final (std::string name, async_work_t work, std::function<bool ()> cancelled = {}) const
    {
        return execute_preserved_lane (std::move (name), std::move (work), std::move (cancelled));
    }

    void drain () const
    {
        if (_queue)
            _queue->drain ();
    }

  private:
    bool execute_preserved_lane (std::string name,
                                 async_work_t work,
                                 std::function<bool ()> cancelled) const
    {
        return _queue
               && _queue->post_async_wait (std::move (name), std::move (work),
                                           serial_work_options_t{serial_work_lane_t::application},
                                           std::move (cancelled));
    }

    std::shared_ptr<offload_executor_t> _worker_executor;
    std::shared_ptr<queue_t> _queue;
};

} // namespace zlink::framework::runtime
