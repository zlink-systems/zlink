/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/channels/channel_runtime.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace zlink::framework::test
{

// Hold a real configuration-owner turn through its existing notification input.
// Blocking an unrelated executor work item alone does not hold a state lane:
// an uncontended state-lane call is allowed to run on the caller's thread.
class runtime_options_owner_pause_t
{
  public:
    runtime_options_owner_pause_t (detail::channel_runtime_t runtime,
                                   std::string channel_name,
                                   int weight) :
        _runtime (std::move (runtime)),
        _state (std::make_shared<state_t> ()),
        _entered (_state->entered.get_future ()),
        _finished (_state->finished.get_future ())
    {
        _runtime.bind_server_options_changed ([state = _state] {
            if (state->notifications.fetch_add (1) == 0)
                return; // bind delivers the current notification once.
            state->entered.set_value ();
            state->released.wait ();
        });
        _runtime.state_executor ().submit ([runtime = _runtime, state = _state,
                                            channel_name = std::move (channel_name),
                                            weight] () mutable {
            try {
                runtime.set_server_weight (channel_name, weight);
                state->finished.set_value ();
            }
            catch (...) {
                state->finished.set_exception (std::current_exception ());
            }
        });
    }

    ~runtime_options_owner_pause_t ()
    {
        _state->release.set_value ();
        _finished.get ();
        _runtime.bind_server_options_changed ({});
    }

    bool wait_until_entered (std::chrono::milliseconds timeout)
    {
        return _entered.wait_for (timeout) == std::future_status::ready;
    }

    bool still_paused () const
    {
        return _state->released.wait_for (std::chrono::milliseconds::zero ())
               == std::future_status::timeout;
    }

  private:
    struct state_t
    {
        std::atomic_uint notifications{0};
        std::promise<void> entered;
        std::promise<void> release;
        std::promise<void> finished;
        std::shared_future<void> released = release.get_future ().share ();
    };
    detail::channel_runtime_t _runtime;
    std::shared_ptr<state_t> _state;
    std::future<void> _entered;
    std::future<void> _finished;
};

} // namespace zlink::framework::test
