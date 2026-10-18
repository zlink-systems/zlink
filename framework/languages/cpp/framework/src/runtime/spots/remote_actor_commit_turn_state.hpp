/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/execution/serial_execution_queue.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace zlink::framework::detail
{

class remote_actor_commit_turn_state_t
{
  public:
    enum class phase_t
    {
        queued,
        active,
        committing,
        committed,
        terminal
    };

    struct outcome_t
    {
        bool success = false;
        framework_error_kind_t error_kind = framework_error_kind_t::internal_failure;
        std::string error;
    };

    bool begin_active ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal)
            return false;
        _phase = phase_t::active;
        return true;
    }

    bool try_begin_commit ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal || _stop_requested)
            return false;
        _phase = phase_t::committing;
        return true;
    }

    void mark_committed ()
    {
        std::lock_guard lock (_mutex);
        if (_phase != phase_t::terminal)
            _phase = phase_t::committed;
    }

    void select_cancel_error (framework_error_kind_t kind, std::string error)
    {
        std::lock_guard lock (_mutex);
        _cancel_kind = kind;
        _cancel_error = std::move (error);
    }

    void request_stop (runtime::serial_cancel_submission_outcome_t disposition)
    {
        terminal_t terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            _stop_requested = true;
            if (_phase == phase_t::queued
                && disposition == runtime::serial_cancel_submission_outcome_t::queued_cancelled) {
                _phase = phase_t::terminal;
                _outcome = outcome_t{false, _cancel_kind, _cancel_error};
                outcome = _outcome;
                terminal = std::move (_terminal);
            }
        }
        if (terminal)
            terminal (std::move (outcome), {});
    }

    using terminal_t = std::function<void (outcome_t,
                                          runtime::serial_execution_queue_t::async_completion_t)>;

    void settle (outcome_t value,
                 runtime::serial_execution_queue_t::async_completion_t actor_complete = {})
    {
        terminal_t terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal)
                return;
            _phase = phase_t::terminal;
            _outcome = std::move (value);
            outcome = _outcome;
            terminal = std::move (_terminal);
        }
        if (terminal)
            terminal (std::move (outcome), std::move (actor_complete));
    }

    void on_terminal (terminal_t terminal)
    {
        outcome_t outcome;
        bool invoke = false;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal) {
                outcome = _outcome;
                invoke = true;
            } else {
                _terminal = std::move (terminal);
            }
        }
        if (invoke && terminal)
            terminal (std::move (outcome), {});
    }

    outcome_t cancel_outcome () const
    {
        std::lock_guard lock (_mutex);
        return outcome_t{false, _cancel_kind, _cancel_error};
    }

    bool terminal () const
    {
        std::lock_guard lock (_mutex);
        return _phase == phase_t::terminal;
    }

  private:
    mutable std::mutex _mutex;
    phase_t _phase = phase_t::queued;
    bool _stop_requested = false;
    framework_error_kind_t _cancel_kind = framework_error_kind_t::shutting_down;
    std::string _cancel_error = "remote Actor handoff replay was cancelled";
    outcome_t _outcome;
    terminal_t _terminal;
};


} // namespace zlink::framework::detail
