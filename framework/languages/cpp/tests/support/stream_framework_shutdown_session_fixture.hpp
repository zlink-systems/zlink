/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace zlink::framework::test::stream_framework
{
class shutdown_session_control_t final
{
  public:
    zlink::framework::task_t<void> wait_for_release () { return _resume.task (); }

    void release () { _resume.complete (zlink::framework::result_t<void>::success ()); }

    void record_connected () { record (_connected, "connected"); }
    void record_packet_entered () { record (_packet_entered, "packet-entered"); }
    void record_packet_terminal () { record (_packet_terminal, "packet-terminal"); }
    void record_disconnected () { record (_disconnected, "disconnected"); }
    void record_destroyed () { record (_destroyed, "destroyed"); }

    bool wait_connected () { return wait_for (_connected, 1); }
    bool wait_connected (std::size_t count) { return wait_for (_connected, count); }
    bool wait_destroyed () { return wait_for (_destroyed, 1); }
    bool wait_destroyed (std::size_t count) { return wait_for (_destroyed, count); }
    bool wait_disconnected () { return wait_for (_disconnected, 1); }
    bool wait_disconnected (std::size_t count) { return wait_for (_disconnected, count); }
    bool wait_packet_entered () { return wait_for (_packet_entered, 1); }
    bool wait_packet_entered (std::size_t count) { return wait_for (_packet_entered, count); }

    std::size_t connected () const noexcept { return _connected.load (std::memory_order_acquire); }

    std::size_t packet_entered () const noexcept
    {
        return _packet_entered.load (std::memory_order_acquire);
    }

    std::size_t packet_terminal () const noexcept
    {
        return _packet_terminal.load (std::memory_order_acquire);
    }

    std::size_t disconnected () const noexcept
    {
        return _disconnected.load (std::memory_order_acquire);
    }

    std::size_t destroyed () const noexcept { return _destroyed.load (std::memory_order_acquire); }

    std::vector<std::string> lifecycle () const
    {
        const std::lock_guard lock (_mutex);
        return _lifecycle;
    }

  private:
    void record (std::atomic_size_t &counter, std::string event)
    {
        {
            const std::lock_guard lock (_mutex);
            _lifecycle.push_back (std::move (event));
            counter.fetch_add (1, std::memory_order_release);
        }
        _changed.notify_all ();
    }

    bool wait_for (const std::atomic_size_t &counter, std::size_t count)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (2), [&counter, count] {
            return counter.load (std::memory_order_acquire) >= count;
        });
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<std::string> _lifecycle;
    zlink::framework::task_completion_source_t<void> _resume;
    std::atomic_size_t _connected{0};
    std::atomic_size_t _packet_entered{0};
    std::atomic_size_t _packet_terminal{0};
    std::atomic_size_t _disconnected{0};
    std::atomic_size_t _destroyed{0};
};

class shutdown_failure_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    explicit shutdown_failure_session_t (std::shared_ptr<shutdown_session_control_t> control) :
        _control (std::move (control))
    {
    }

    ~shutdown_failure_session_t () override { _control->record_destroyed (); }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        _control->record_connected ();
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        _control->record_disconnected ();
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        _control->record_packet_entered ();
        co_await _control->wait_for_release ();
        _control->record_packet_terminal ();
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::internal_failure, "shutdown packet failure");
    }

  private:
    std::shared_ptr<shutdown_session_control_t> _control;
};
} // namespace zlink::framework::test::stream_framework
