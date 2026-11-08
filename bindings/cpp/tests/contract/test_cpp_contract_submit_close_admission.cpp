/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

namespace
{
static_assert (std::atomic<uint64_t>::is_always_lock_free);

struct hook_t
{
    void *socket = nullptr;
    bool close = false;
    bool completion = false;
    bool fail_close = false;
    std::function<void ()> on_close;
    std::mutex mutex;
    std::condition_variable changed;
    int entered = 0;
    bool released = false;

    void enter ()
    {
        std::unique_lock lock (mutex);
        ++entered;
        changed.notify_all ();
        changed.wait (lock, [&] { return released; });
    }
    void wait (int count_)
    {
        std::unique_lock lock (mutex);
        assert (
          changed.wait_for (lock, std::chrono::seconds (5), [&] { return entered == count_; }));
    }
    void release ()
    {
        std::lock_guard lock (mutex);
        released = true;
        changed.notify_all ();
    }
};
hook_t *active = nullptr;
std::atomic<int> native_submits{0};

void before_submit (void *socket_)
{
    ++native_submits;
    if (active && socket_ == active->socket && !active->close && !active->completion)
        active->enter ();
}

struct fixture_t
{
    zlink::context_t context;
    zlink::router_socket_t server{context};
    zlink::dealer_socket_t client{context};
    zlink::pub_socket_t publisher{context};
    zlink::poller_t poller;
    std::array<zlink::received_t, 2> received;
    std::vector<zlink::request_submission_t> requests;

    fixture_t ()
    {
        client.options ().send_timeout (std::chrono::seconds (5));
        server.options ().recv_timeout (std::chrono::seconds (5));
        const auto endpoint = zlink_cpp_contract::unique_inproc ("submit-admission");
        server.bind (endpoint);
        client.connect (endpoint);
        poller.add (client, zlink::poll_event_flag_t::pollcompletion, 1);
        for (auto &result : received) {
            auto message = zlink_cpp_contract::make_message ("request");
            requests.push_back (client.request ().message (message).async ());
            assert (server.recv (result) == 0);
        }
    }

    zlink::socket_t &socket (int kind_)
    {
        return kind_ == 2   ? static_cast<zlink::socket_t &> (server)
               : kind_ == 3 ? static_cast<zlink::socket_t &> (publisher)
                            : static_cast<zlink::socket_t &> (client);
    }

    void submit (int kind_, zlink::message_t &message_, int index_ = 0)
    {
        switch (kind_) {
            case 0:
                client.send ().message (message_).submit ();
                break;
            case 1: {
                auto submission = client.request ().message (message_).async ();
                assert (submission.result == ZLINK_SUBMIT_OK);
                break;
            }
            case 2:
                received[index_].reply ().message (message_).submit ();
                break;
            case 3:
                assert (publisher.publish ("topic").message (message_).submit ());
                break;
        }
    }
};

void terminated (fixture_t &fixture_, int kind_)
{
    auto message = zlink_cpp_contract::make_message ("rejected");
    const int before = native_submits.load ();
    try {
        fixture_.submit (kind_, message);
        assert (false);
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::terminated);
        assert (error.internal_errno () == ESHUTDOWN);
    }
    assert (native_submits.load () == before);
    assert (message.valid () && message.to_string () == "rejected");
}

void reserved_calls_are_concurrent (int kind_)
{
    fixture_t fixture;
    auto &socket = fixture.socket (kind_);
    hook_t hook;
    hook.socket = zlink::detail::native_handle (socket);
    active = &hook;
    std::array<std::thread, 2> threads;
    for (int i = 0; i != 2; ++i) {
        threads[i] = std::thread ([&, i] {
            auto message = zlink_cpp_contract::make_message ("accepted");
            fixture.submit (kind_, message, i);
            assert (!message.valid ());
        });
    }
    // Both native entries must reach the hook before either is released.
    hook.wait (2);
    try {
        socket.close ();
        assert (false);
    }
    catch (const zlink::close_error_t &error) {
        assert (error.result () == zlink::close_result_t::busy);
        assert (error.internal_errno () == EBUSY);
    }
    assert (socket.valid ());
    hook.release ();
    for (auto &thread : threads)
        thread.join ();
    active = nullptr;
    socket.close ();
    terminated (fixture, kind_);
}

void closing_waits_for_outcome (int kind_, bool busy_)
{
    fixture_t fixture;
    auto &socket = fixture.socket (kind_);
    hook_t hook;
    hook.socket = zlink::detail::native_handle (socket);
    hook.close = true;
    hook.fail_close = busy_;
    active = &hook;
    std::thread closer ([&] {
        try {
            socket.close ();
            assert (!busy_);
        }
        catch (const zlink::close_error_t &error) {
            assert (busy_ && error.result () == zlink::close_result_t::busy);
            assert (error.internal_errno () == EBUSY);
        }
    });
    hook.wait (1);
    try {
        socket.close ();
        assert (false);
    }
    catch (const zlink::close_error_t &error) {
        assert (error.result () == zlink::close_result_t::busy);
        assert (error.internal_errno () == EBUSY);
    }
    const int before = native_submits.load ();
    std::promise<void> started;
    auto ready = started.get_future ();
    std::promise<void> finished;
    auto done = finished.get_future ();
    std::thread submitter ([&] {
        started.set_value ();
        if (busy_) {
            auto message = zlink_cpp_contract::make_message ("after-busy");
            fixture.submit (kind_, message);
        } else {
            terminated (fixture, kind_);
        }
        finished.set_value ();
    });
    ready.wait ();
    assert (done.wait_for (std::chrono::milliseconds (100)) == std::future_status::timeout);
    assert (native_submits.load () == before);
    hook.release ();
    closer.join ();
    submitter.join ();
    active = nullptr;
    assert (native_submits.load () == before + (busy_ ? 1 : 0));
    if (busy_) {
        assert (socket.valid ());
        socket.close ();
    }
    terminated (fixture, kind_);
}

void native_close_reentrancy_is_busy ()
{
    fixture_t fixture;
    hook_t hook;
    hook.socket = zlink::detail::native_handle (fixture.client);
    hook.close = true;
    hook.released = true;
    bool called = false;
    hook.on_close = [&] {
        called = true;
        try {
            fixture.client.close ();
            assert (false);
        }
        catch (const zlink::close_error_t &error) {
            assert (error.result () == zlink::close_result_t::busy);
            assert (error.internal_errno () == EBUSY);
        }
    };
    active = &hook;
    fixture.client.close ();
    active = nullptr;
    assert (called);
}

void completion_receive_holds_reservation ()
{
    fixture_t fixture;
    assert (fixture.poller.remove (fixture.client));
    hook_t hook;
    hook.socket = zlink::detail::native_handle (fixture.client);
    hook.completion = true;
    active = &hook;
    std::thread requester ([&] {
        auto message = zlink_cpp_contract::make_message ("blocking");
        auto parts = fixture.client.request ().message (message).submit ();
        assert (parts.size () == 1 && parts[0].to_string () == "reply");
    });
    hook.wait (1);
    try {
        fixture.client.close ();
        assert (false);
    }
    catch (const zlink::close_error_t &error) {
        assert (error.result () == zlink::close_result_t::busy);
        assert (error.internal_errno () == EBUSY);
    }
    zlink::received_t received;
    assert (fixture.server.recv (received) == 0);
    auto reply = zlink_cpp_contract::make_message ("reply");
    received.reply ().message (reply).submit ();
    hook.release ();
    requester.join ();
    active = nullptr;
    fixture.client.close ();
}

void preparation_failure_releases_reservation ()
{
    for (int kind = 0; kind != 4; ++kind) {
        fixture_t fixture;
        auto invalid = zlink_cpp_contract::make_message ("invalid");
        invalid.close ();
        const int before = native_submits.load ();
        try {
            fixture.submit (kind, invalid);
            assert (false);
        }
        catch (const zlink::binding_error_t &error) {
            assert (error.internal_errno () == EINVAL);
        }
        assert (native_submits.load () == before);
        fixture.socket (kind).close ();
    }
    fixture_t fixture;
    auto message = zlink_cpp_contract::make_message ("timeout-overflow");
    const int before = native_submits.load ();
    try {
        auto submission = fixture.client.request ()
                            .message (message)
                            .timeout (std::chrono::milliseconds (uint64_t{1} << 40))
                            .async ();
        assert (false);
    }
    catch (const zlink::config_error_t &error) {
        assert (error.result () == zlink::config_result_t::invalid_argument);
    }
    assert (native_submits.load () == before && message.valid ());
    fixture.client.close ();
}

void received_after_owner_destruction (bool reply_)
{
    zlink::received_t received;
    {
        fixture_t fixture;
        received = std::move (fixture.received[0]);
    }
    auto message = zlink_cpp_contract::make_message ("expired-context");
    const int before = native_submits.load ();
    try {
        if (reply_)
            received.reply ().message (message).submit ();
        else
            received.send ().message (message).submit ();
        assert (false);
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::terminated);
        assert (error.internal_errno () == ESHUTDOWN);
    }
    assert (native_submits.load () == before && message.valid ());
    assert (message.to_string () == "expired-context");
}

void builder_handle_outlives_runtime_across_threads ()
{
    zlink::context_t context;
    auto message = zlink_cpp_contract::make_message ("cross-thread-owner");
    std::optional<zlink::send_submit_operation_t> operation;
    std::weak_ptr<zlink::detail::socket_runtime_state_t> runtime;
    std::weak_ptr<zlink::detail::socket_handle_t> handle;
    {
        zlink::pair_socket_t socket (context);
        const auto owner = zlink::detail::runtime_state (socket);
        runtime = owner;
        handle = owner->completion->socket_handle ();
        operation.emplace (socket.send ().message (message));
    }
    assert (runtime.expired () && !handle.expired ());
    const int before = native_submits.load ();
    std::thread submitter ([operation = std::move (operation)] () mutable {
        try {
            std::move (*operation).submit ();
            assert (false);
        }
        catch (const zlink::submit_error_t &error) {
            assert (error.result () == zlink::submit_result_t::terminated);
            assert (error.internal_errno () == ESHUTDOWN);
        }
        operation.reset ();
    });
    submitter.join ();
    assert (handle.expired ());
    assert (native_submits.load () == before && message.valid ());
    assert (message.to_string () == "cross-thread-owner");
}

template <typename Factory> void expired_builder_rejects_without_native_access (Factory factory_)
{
    auto message = zlink_cpp_contract::make_message ("expired");
    using operation_t = decltype (factory_ (std::declval<fixture_t &> (), message));
    std::optional<operation_t> operation;
    {
        fixture_t fixture;
        operation.emplace (factory_ (fixture, message));
    }
    const int before = native_submits.load ();
    try {
        if constexpr (std::is_same_v<operation_t, zlink::request_submit_operation_t>) {
            auto submission = std::move (*operation).async ();
        } else {
            std::move (*operation).submit ();
        }
        assert (false);
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::terminated);
        assert (error.internal_errno () == ESHUTDOWN);
    }
    assert (native_submits.load () == before && message.valid ());
    assert (message.to_string () == "expired");
}
} // namespace

extern "C" zlink_submit_result_t __real_zlink_send (
  void *, zlink_msg_t *, size_t, zlink_send_flags_t, void *, zlink_completion_id_t *);
extern "C" zlink_submit_result_t __wrap_zlink_send (void *socket_,
                                                    zlink_msg_t *parts_,
                                                    size_t count_,
                                                    zlink_send_flags_t flags_,
                                                    void *context_,
                                                    zlink_completion_id_t *id_)
{
    before_submit (socket_);
    return __real_zlink_send (socket_, parts_, count_, flags_, context_, id_);
}
extern "C" zlink_submit_result_t __real_zlink_request (void *,
                                                       const zlink_routing_id_t *,
                                                       zlink_msg_t *,
                                                       size_t,
                                                       zlink_send_flags_t,
                                                       uint32_t,
                                                       void *,
                                                       zlink_completion_id_t *);
extern "C" zlink_submit_result_t __wrap_zlink_request (void *socket_,
                                                       const zlink_routing_id_t *rid_,
                                                       zlink_msg_t *parts_,
                                                       size_t count_,
                                                       zlink_send_flags_t flags_,
                                                       uint32_t timeout_,
                                                       void *context_,
                                                       zlink_completion_id_t *id_)
{
    before_submit (socket_);
    return __real_zlink_request (socket_, rid_, parts_, count_, flags_, timeout_, context_, id_);
}
extern "C" zlink_submit_result_t
__real_zlink_reply (void *, const zlink_routing_id_t *, zlink_reply_token_t, zlink_msg_t *, size_t);
extern "C" zlink_submit_result_t __wrap_zlink_reply (void *socket_,
                                                     const zlink_routing_id_t *rid_,
                                                     zlink_reply_token_t token_,
                                                     zlink_msg_t *parts_,
                                                     size_t count_)
{
    before_submit (socket_);
    return __real_zlink_reply (socket_, rid_, token_, parts_, count_);
}
extern "C" zlink_submit_result_t
__real_zlink_publish (void *, const char *, zlink_msg_t *, size_t, zlink_send_flags_t);
extern "C" zlink_submit_result_t __wrap_zlink_publish (
  void *socket_, const char *topic_, zlink_msg_t *parts_, size_t count_, zlink_send_flags_t flags_)
{
    before_submit (socket_);
    return __real_zlink_publish (socket_, topic_, parts_, count_, flags_);
}
extern "C" zlink_close_result_t __real_zlink_close (void *);
extern "C" zlink_close_result_t __wrap_zlink_close (void *socket_)
{
    if (active && socket_ == active->socket && active->close) {
        active->enter ();
        if (active->fail_close) {
            errno = EBUSY;
            return ZLINK_CLOSE_BUSY;
        }
        if (active->on_close)
            active->on_close ();
    }
    return __real_zlink_close (socket_);
}

extern "C" zlink_recv_result_t
__real_zlink_completion_recv (void *, zlink_completion_t *, zlink_recv_flags_t);
extern "C" zlink_recv_result_t __wrap_zlink_completion_recv (void *socket_,
                                                             zlink_completion_t *completion_,
                                                             zlink_recv_flags_t flags_)
{
    if (active && socket_ == active->socket && active->completion)
        active->enter ();
    return __real_zlink_completion_recv (socket_, completion_, flags_);
}

int main ()
{
    for (int kind = 0; kind != 4; ++kind) {
        reserved_calls_are_concurrent (kind);
        closing_waits_for_outcome (kind, false);
        closing_waits_for_outcome (kind, true);
    }
    native_close_reentrancy_is_busy ();
    completion_receive_holds_reservation ();
    preparation_failure_releases_reservation ();
    builder_handle_outlives_runtime_across_threads ();
    received_after_owner_destruction (false);
    received_after_owner_destruction (true);
    expired_builder_rejects_without_native_access (
      [] (auto &f, auto &m) { return f.client.send ().message (m); });
    expired_builder_rejects_without_native_access (
      [] (auto &f, auto &m) { return f.client.request ().message (m); });
    expired_builder_rejects_without_native_access (
      [] (auto &f, auto &m) { return f.received[0].reply ().message (m); });
    expired_builder_rejects_without_native_access (
      [] (auto &f, auto &m) { return f.publisher.publish ("topic").message (m); });
}
