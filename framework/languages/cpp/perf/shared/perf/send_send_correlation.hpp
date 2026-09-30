/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// The harness correlation of a send/send operation (§13): the first public send and the return send are two one-way
// calls, tied together only by the correlationId in the DTO. The first result of a correlation stands; a reply that
// arrives after that is only counted (duplicate, late or unknown). The table clears with the window at reset.
//
// Order of one operation: register (right before the first public send, fixing the expiry deadline), then
// first_send_ended with that send's terminal, then complete for the final result. The return handler calls reply.

#include <perf/scenario_metrics.hpp>

#include <deque>

namespace perf
{
class send_send_correlation_t
{
  public:
    static constexpr int pending = 0, succeeded = 1, failed = 2, expired = 3;

    struct entry_t
    {
        entry_t (echo_request_t request_, std::int64_t started, std::int64_t expires) :
            request (std::move (request_)), started_ticks (started), expires_at_ticks (expires),
            // The waiter resumes through the host executor, like a native binding awaitable, never on the completing thread.
            result (zlink::framework::detail::capture_native_continuation_scheduler ())
        {
        }
        echo_request_t request;
        std::int64_t started_ticks, expires_at_ticks;
        zlink::framework::detail::task_completion_source_t<int> result;
        std::atomic<int> state{pending};
        std::int64_t closed_ticks = 0;
        std::exception_ptr error;

        bool close (int to, std::exception_ptr why)
        {
            int expected = pending;
            if (!state.compare_exchange_strong (expected, to))
                return false;
            closed_ticks = now_ticks ();
            error = std::move (why);
            result.complete (zlink::framework::result_t<int>::success (to));
            return true;
        }
    };
    using entry_ptr_t = std::shared_ptr<entry_t>;

    send_send_correlation_t (measurement_t &measurement, scenario_metrics_t &metrics) :
        _measurement (measurement), _metrics (metrics)
    {
        _metrics.counters ({"messages.admitted", "messages.expired", "messages.duplicateReply", "messages.lateReply",
                            "messages.unknownCorrelation"});
        _metrics.on_reset ([this] {
            std::lock_guard lock (_gate);
            _entries.clear ();
            _expiry.clear ();
        });
        _expiry_thread = std::thread ([this] { expire_loop (); });
    }
    ~send_send_correlation_t ()
    {
        {
            std::lock_guard lock (_gate);
            _stop = true;
        }
        _wake.notify_all ();
        _expiry_thread.join ();
    }
    send_send_correlation_t (const send_send_correlation_t &) = delete;
    send_send_correlation_t &operator= (const send_send_correlation_t &) = delete;

    entry_ptr_t register_request (const echo_request_t &request, std::int64_t started_ticks)
    {
        auto entry = std::make_shared<entry_t> (
          request, started_ticks, now_ticks () + static_cast<std::int64_t> (_measurement.config ().workload.correlation_expiry_ms) * 1'000'000);
        {
            std::lock_guard lock (_gate);
            if (!_entries.emplace (request.correlation_id, entry).second)
                throw validation_error_t ("IdentityMismatch", "A correlationId was issued twice.");
            _expiry.push_back (entry);
        }
        _wake.notify_all ();
        return entry;
    }

    entry_ptr_t find (const std::string &correlation_id)
    {
        std::lock_guard lock (_gate);
        const auto found = _entries.find (correlation_id);
        return found == _entries.end () ? nullptr : found->second;
    }

    // The terminal of the first public send: a normal admission is counted; a failure is the final result unless
    // the echo was already fixed first.
    void first_send_ended (const entry_ptr_t &entry, std::exception_ptr error)
    {
        if (!error) {
            if (_measurement.phase () != "setup")
                _metrics.count ("messages.admitted");
        }
        else
            entry->close (failed, std::move (error));
    }

    // The return handler's one call: the reply's identity and payload decide the first result.
    void reply (const echo_reply_t &reply)
    {
        entry_ptr_t entry;
        {
            std::lock_guard lock (_gate);
            const auto found = _entries.find (reply.correlation_id);
            if (found != _entries.end ())
                entry = found->second;
        }
        if (!entry) {
            _metrics.count ("messages.unknownCorrelation");
            return;
        }
        std::exception_ptr invalid;
        try {
            payload_pattern_t::validate_identity (entry->request, reply);
            _measurement.pattern ().validate (reply.payload);
        }
        catch (const validation_error_t &) {
            invalid = std::current_exception ();
        }
        if (!entry->close (invalid ? failed : succeeded, invalid))
            _metrics.count (entry->state.load () == succeeded ? "messages.duplicateReply" : "messages.lateReply");
    }

    // The final result once the first send has ended: the first result of the correlation, or its expiry (closed by
    // the expiry thread). The time is when that result was fixed, so an echo seen before the first send's terminal
    // keeps its own time.
    zlink::framework::task_t<std::pair<std::exception_ptr, std::int64_t>> complete (entry_ptr_t entry)
    {
        auto waiting = entry->result.task ();
        (void) co_await waiting;
        co_return std::make_pair (entry->error, entry->closed_ticks);
    }

  private:
    void expire_loop ()
    {
        std::unique_lock lock (_gate);
        while (!_stop) {
            if (_expiry.empty ()) {
                _wake.wait (lock);
                continue;
            }
            const auto front = _expiry.front ();
            const auto remaining = front->expires_at_ticks - now_ticks ();
            if (remaining > 0) {
                _wake.wait_for (lock, std::chrono::nanoseconds (remaining));
                continue;
            }
            _expiry.pop_front ();
            lock.unlock ();
            if (front->close (expired, std::make_exception_ptr (validation_error_t ("CorrelationExpired", "No return send arrived before the correlation deadline."))))
                _metrics.count ("messages.expired");
            lock.lock ();
        }
    }

    measurement_t &_measurement;
    scenario_metrics_t &_metrics;
    std::mutex _gate;
    std::condition_variable _wake;
    std::unordered_map<std::string, entry_ptr_t> _entries;
    std::deque<entry_ptr_t> _expiry;
    bool _stop = false;
    std::thread _expiry_thread;
};
} // namespace perf
