/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <functional>
#include <memory>
#include <stop_token>
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
    using discard_t = std::function<void (result_t<void>)>;

    explicit session_serial_executor_t (std::shared_ptr<offload_executor_t> worker_executor,
                                        std::stop_token shutdown_cancellation = {}) :
        _worker_executor (std::move (worker_executor)),
        _queue (std::make_shared<queue_t> (*_worker_executor,
                                           serial_execution_queue_options_t{},
                                           queue_t::error_handler_t{},
                                           serial_lane_policy_t::session ())),
        _cancel_on_shutdown (std::move (shutdown_cancellation), cancel_pending_t{_queue})
    {
    }

    session_serial_executor_t (const session_serial_executor_t &) = delete;
    session_serial_executor_t &operator= (const session_serial_executor_t &) = delete;

    bool execute_application (async_work_t work,
                              std::function<bool ()> cancelled = {},
                              discard_t discarded = {}) const
    {
        return execute_lane (std::move (work), std::move (cancelled), std::move (discarded));
    }

    bool execute_control (async_work_t work,
                          std::function<bool ()> cancelled = {},
                          discard_t discarded = {}) const
    {
        return execute_lane (std::move (work), std::move (cancelled), std::move (discarded),
                             serial_work_lane_t::lifecycle);
    }

    bool execute_infrastructure (async_work_t work,
                                 std::function<bool ()> cancelled = {},
                                 discard_t discarded = {}) const
    {
        return execute_lane (std::move (work), std::move (cancelled), std::move (discarded));
    }

    bool execute_final (async_work_t work,
                        std::function<bool ()> cancelled = {},
                        discard_t discarded = {}) const
    {
        return execute_lane (std::move (work), std::move (cancelled), std::move (discarded));
    }

    void drain () const
    {
        if (_queue)
            _queue->drain ();
    }

  private:
    bool execute_lane (async_work_t work,
                       std::function<bool ()> cancelled,
                       discard_t discarded,
                       serial_work_lane_t lane = serial_work_lane_t::application) const
    {
        serial_work_options_t options{lane};
        options.completion = std::move (discarded);
        return _queue
               && _queue->post_async_wait ({}, std::move (work), std::move (options),
                                           std::move (cancelled));
    }

    struct cancel_pending_t
    {
        std::shared_ptr<queue_t> queue;
        void operator() () const noexcept { queue->cancel_pending (); }
    };

    std::shared_ptr<offload_executor_t> _worker_executor;
    std::shared_ptr<queue_t> _queue;
    std::stop_callback<cancel_pending_t> _cancel_on_shutdown;
};

} // namespace zlink::framework::runtime
