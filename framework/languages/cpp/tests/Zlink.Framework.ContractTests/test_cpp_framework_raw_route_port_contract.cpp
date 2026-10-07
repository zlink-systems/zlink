/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/backend/raw_route_port.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/host/bound_session_send_stage_trace.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"

#include <zlink.hpp>
#include <zlink/framework/contracts/errors/result.hpp>

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <optional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace backend = zlink::framework::detail::backend;

static_assert (std::is_same_v<decltype (std::declval<backend::raw_route_port_t &> ().send (
                                std::declval<const backend::raw_bytes_t &> (),
                                std::declval<const backend::raw_message_t &> ())),
                              zlink::framework::task_t<bool>>);
static_assert (std::is_same_v<decltype (std::declval<backend::raw_route_port_t &> ().send_result (
                                std::declval<const backend::raw_bytes_t &> (),
                                std::declval<const backend::raw_message_t &> ())),
                              zlink::framework::task_t<zlink::submit_result_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<backend::raw_route_port_t &> ().try_receive ()),
                 std::optional<backend::raw_received_t>>);
static_assert (std::is_same_v<decltype (std::declval<backend::raw_route_port_t &> ().request (
                                std::declval<const backend::raw_bytes_t &> (),
                                std::declval<const backend::raw_message_t &> (),
                                std::chrono::milliseconds (1))),
                              zlink::framework::task_t<backend::raw_request_completion_t>>);
static_assert (std::is_same_v<decltype (std::declval<backend::raw_route_port_t &> ().reply (
                                std::declval<const backend::raw_received_t &> (),
                                std::declval<const backend::raw_message_t &> ())),
                              bool>);
static_assert (std::is_same_v<decltype (std::declval<backend::raw_dealer_port_t &> ().send (
                                std::declval<const backend::raw_message_t &> ())),
                              zlink::framework::task_t<bool>>);
static_assert (std::is_same_v<decltype (std::declval<backend::raw_dealer_port_t &> ().send_result (
                                std::declval<const backend::raw_message_t &> ())),
                              zlink::framework::task_t<zlink::submit_result_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<backend::raw_dealer_port_t &> ().request (
                   std::declval<const backend::raw_message_t &> (), std::chrono::milliseconds (1))),
                 zlink::framework::task_t<backend::raw_request_completion_t>>);

namespace
{
using namespace std::chrono_literals;

constexpr auto raw_route_fixture_timeout = 2s;
constexpr auto raw_route_fixture_poll_interval = 10ms;
constexpr auto raw_request_capacity_timeout = 25ms;
constexpr std::size_t raw_route_backpressure_hwm_bytes = 16;
constexpr std::size_t raw_route_backpressure_attempt_limit = 256;
constexpr std::size_t deferred_trace_payload_size = 1024;
constexpr char deferred_trace_actor_id[] = "deferred-raw-route-actor";
constexpr char deferred_trace_session_id[] = "deferred-raw-route-session";

class admission_probe_t final : public zlink::detail::async_result_state_t<void>
{
  public:
    explicit admission_probe_t (int error) : _error (error) {}

    bool ready () const noexcept override { return true; }
    bool suspend (std::coroutine_handle<>, zlink::detail::async_continuation_scheduler_t) override
    {
        return false;
    }
    void take () override
    {
        ++consumed;
        if (_error != 0)
            throw zlink::submit_error_t (zlink::submit_result_t::backpressured, _error);
    }
    void detach () noexcept override {}
    void abandon (std::coroutine_handle<>) noexcept override {}

    int consumed = 0;

  private:
    int _error;
};

class reply_probe_t final
    : public zlink::detail::async_result_state_t<std::vector<zlink::message_t>>
{
  public:
    explicit reply_probe_t (int error) : _error (error) {}

    bool ready () const noexcept override { return true; }
    bool suspend (std::coroutine_handle<>, zlink::detail::async_continuation_scheduler_t) override
    {
        return false;
    }
    std::vector<zlink::message_t> take () override
    {
        ++consumed;
        if (_error != 0)
            throw zlink::request_error_t (zlink::request_result_t::timed_out, _error);
        return {};
    }
    void detach () noexcept override {}
    void abandon (std::coroutine_handle<>) noexcept override {}

    int consumed = 0;

  private:
    int _error;
};

void verify_request_submission_stages ()
{
    for (const auto [admission_error, reply_error, expected_error] :
         {std::tuple{0, 0, 0}, std::tuple{EAGAIN, 0, EAGAIN}, std::tuple{0, ENOMEM, ENOMEM}}) {
        auto admission_probe = std::make_shared<admission_probe_t> (admission_error);
        auto reply_probe = std::make_shared<reply_probe_t> (reply_error);
        int submissions = 0;
        auto stages = backend::submit_request_once ([&] {
            ++submissions;
            return zlink::request_submission_t{
              ZLINK_SUBMIT_BACKPRESSURED,
              zlink::detail::async_result_access_t::make<void> (admission_probe),
              zlink::detail::async_result_access_t::make<std::vector<zlink::message_t>> (
                reply_probe)};
        });
        assert (submissions == 1);
        assert (stages.admission);
        auto source = std::make_shared<
          zlink::framework::task_completion_source_t<backend::raw_request_completion_t>> ();
        backend::observe_request_completion (std::move (stages), source);
        auto completion = source->task ();
        const auto &observed = completion.result ().value ();
        if (expected_error == 0) {
            assert (observed.terminal == zlink::request_result_t::ok);
            assert (!observed.failure);
        } else {
            // A reply timeout is valid; BACKPRESSURED in an admission
            // terminal is an invalid binding completion, not a deadline.
            assert (observed.terminal
                    == (admission_error ? zlink::request_result_t::internal_error
                                        : zlink::request_result_t::timed_out));
            assert (observed.failure);
            assert (observed.failure->internal_errno == expected_error);
        }
        assert (admission_probe->consumed == 1);
        assert (reply_probe->consumed == 1);
    }
}

void verify_submission_admission_consumes_only_backpressure ()
{
    for (const bool fail : {false, true}) {
        auto probe = std::make_shared<admission_probe_t> (fail ? EAGAIN : 0);
        zlink::send_submission_t submission{
          ZLINK_SUBMIT_BACKPRESSURED, zlink::detail::async_result_access_t::make<void> (probe)};
        auto admission = backend::take_submission_admission (submission);
        assert (admission);
        auto awaiter = std::move (*admission).operator co_await();
        assert (awaiter.await_ready ());
        try {
            awaiter.await_resume ();
            assert (!fail);
        }
        catch (const zlink::submit_error_t &error) {
            assert (fail);
            assert (error.result () == zlink::submit_result_t::backpressured);
        }
        assert (probe->consumed == 1);
    }

    auto probe = std::make_shared<admission_probe_t> (0);
    zlink::send_submission_t admitted{ZLINK_SUBMIT_OK,
                                      zlink::detail::async_result_access_t::make<void> (probe)};
    assert (!backend::take_submission_admission (admitted));
    assert (probe->consumed == 0);
}

bool wait_for_monitor_event (zlink::socket_monitor_t &monitor,
                             zlink::monitor_event expected,
                             std::chrono::milliseconds timeout)
{
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          deadline - std::chrono::steady_clock::now ());
        zlink::poll_event_t ready;
        if (poller.wait (&ready, 1, remaining) != 1)
            continue;
        const auto event = monitor.recv (zlink::recv_flags_t::dontwait);
        if (event && event->event == expected)
            return true;
    }
    return false;
}

backend::raw_message_t request_parts ()
{
    return backend::raw_message_t{backend::raw_bytes_t{'r', 'e', 'q', 'u', 'e', 's', 't'}};
}

void verify_capacity_refusal_phase_controls_public_terminal ()
{
    using zlink::framework::framework_error_kind_t;
    namespace foundation = zlink::framework::runtime::foundation;
    namespace client_server = zlink::framework::runtime::client_server;
    const auto initial = zlink::framework::runtime::messaging::map_submit_request_result (
      zlink::submit_result_t::backpressured, false);
    assert (initial == zlink::request_result_t::not_connected);
    const auto refused = client_server::client_server_operation_exception (
      foundation::operation_terminal_t::transport_failed, "tokenless capacity");
    try {
        std::rethrow_exception (refused);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        assert (error.kind () == framework_error_kind_t::unavailable);
        assert (zlink::framework::detail::boundary_state (error)
                != zlink::framework::detail::boundary_error_t::timed_out);
    }
    const auto completion = zlink::framework::runtime::messaging::map_submit_request_result (
      zlink::submit_result_t::backpressured, true);
    assert (completion == zlink::request_result_t::internal_error);
    const auto expired = client_server::client_server_operation_exception (
      foundation::operation_terminal_t::timed_out, "caller request budget");
    try {
        std::rethrow_exception (expired);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        assert (error.kind () == framework_error_kind_t::deadline_exceeded);
    }
    std::cout << "f20 tokenless=Unavailable invalid_completion=InternalFailure "
                 "request_budget=DeadlineExceeded"
              << std::endl;
}

void verify_writable_request_timeout_remains_deadline_exceeded ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t source (context), target (context);
    const auto source_rid = zlink::routing_id_t::from ("f20-writable-source");
    const auto target_rid = zlink::routing_id_t::from ("f20-writable-target");
    source.set_routing_id (source_rid);
    target.set_routing_id (target_rid);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    source.options ().send_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.options ().recv_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    target.bind ("inproc://framework-f20-writable-request-timeout");
    auto ready = source.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect ("inproc://framework-f20-writable-request-timeout");
    assert (wait_for_monitor_event (ready, zlink::monitor_event::connection_ready,
                                    raw_route_fixture_timeout));
    backend::raw_route_port_t port (source), target_port (target);
    const auto pause_deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (ready.status ().flow_paused_connections == 0
           && std::chrono::steady_clock::now () < pause_deadline) {
        (void) target_port.poll (raw_route_fixture_poll_interval);
        (void) port.poll (raw_route_fixture_poll_interval);
    }
    assert (ready.status ().flow_paused_connections == 1);
    std::optional<zlink::framework::task_t<zlink::submit_result_t>> blocked_send;
    for (std::size_t attempt = 0; attempt < raw_route_backpressure_attempt_limit && !blocked_send;
         ++attempt) {
        auto sent = port.send_result (target_rid.to_bytes (), request_parts ());
        if (!sent.await_ready ())
            blocked_send.emplace (std::move (sent));
        else
            assert (sent.result () && sent.result ().value () == zlink::submit_result_t::ok);
    }
    assert (blocked_send);
    const auto caller_deadline = std::chrono::steady_clock::now () + raw_request_capacity_timeout;
    auto pending = zlink::framework::runtime::messaging::with_request_deadline (
      port.request (target_rid.to_bytes (), request_parts (), raw_route_fixture_timeout),
      caller_deadline);
    std::atomic_int terminal_count{0};
    zlink::framework::detail::observe_task_completion (pending, [&] (const auto &settled) {
        assert (!settled
                && settled.error_kind ()
                     == zlink::framework::framework_error_kind_t::deadline_exceeded);
        terminal_count.fetch_add (1, std::memory_order_release);
    });
    assert (!pending.await_ready ());
    const auto deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while ((!pending.await_ready () || terminal_count.load (std::memory_order_acquire) == 0)
           && std::chrono::steady_clock::now () < deadline)
        (void) port.poll (raw_route_fixture_poll_interval);
    assert (pending.await_ready ());
    assert (!pending.result ());
    assert (pending.result ().error_kind ()
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (terminal_count.load (std::memory_order_acquire) == 1);
    port.close ();
    target_port.close ();
    ready.close ();
}

zlink::framework::task_t<zlink::submit_result_t>
submit_with_local_bound_session_trace (backend::raw_route_port_t &port,
                                       const backend::raw_bytes_t &target_rid,
                                       const zlink::framework::dispatch_options_t &trace_options)
{
    zlink::framework::detail::actor_gateway_runtime_t gateway;
    gateway.set_dispatch (trace_options);
    const std::string actor_id = deferred_trace_actor_id;
    const auto session_rid = zlink::routing_id_t::from (deferred_trace_session_id);
    zlink::framework::detail::bound_session_send_stage_trace_context_t trace_context{
      &gateway, actor_id, &session_rid};
    auto trace = zlink::framework::detail::make_bound_session_send_stage_trace (trace_context);
    return port.send_result (target_rid,
                             backend::raw_message_t{backend::raw_bytes_t (
                               deferred_trace_payload_size, static_cast<std::uint8_t> (0x5a))},
                             std::move (trace));
}

void verify_deferred_send_trace_owns_its_context ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t source (context), target (context);
    const auto source_rid = zlink::routing_id_t::from ("deferred-trace-source");
    const auto target_rid = zlink::routing_id_t::from ("deferred-trace-target");
    source.set_routing_id (source_rid);
    target.set_routing_id (target_rid);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    source.options ().mandatory (true);
    source.options ().send_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.options ().recv_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    const std::string endpoint = "inproc://framework-deferred-raw-route-trace";
    target.bind (endpoint);
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect (endpoint);
    backend::raw_route_port_t source_port (source);
    backend::raw_route_port_t target_port (target);
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready,
                                    raw_route_fixture_timeout));
    const auto pause_deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (monitor.status ().flow_paused_connections == 0
           && std::chrono::steady_clock::now () < pause_deadline) {
        (void) target_port.poll (raw_route_fixture_poll_interval);
        (void) source_port.poll (raw_route_fixture_poll_interval);
    }
    assert (monitor.status ().flow_paused_connections == 1);

    auto captured_events = std::make_shared<std::vector<zlink::framework::message_flow_event_t>> ();
    std::mutex captured_events_mutex;
    zlink::framework::dispatch_options_t trace_options;
    trace_options.message_flow (zlink::framework::message_flow_log_mode_t::detailed);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      trace_options, [&] (const zlink::framework::message_flow_event_t &event) {
          std::lock_guard lock (captured_events_mutex);
          captured_events->push_back (event);
      });

    std::optional<zlink::framework::task_t<zlink::submit_result_t>> pending;
    for (std::size_t attempt = 0; attempt < raw_route_backpressure_attempt_limit && !pending;
         ++attempt) {
        auto sent = submit_with_local_bound_session_trace (source_port, target_rid.to_bytes (),
                                                           trace_options);
        if (!sent.await_ready ()) {
            pending.emplace (std::move (sent));
        } else {
            assert (sent.result ());
            assert (sent.result ().value () == zlink::submit_result_t::ok);
        }
    }
    assert (pending);

    target.set_receive_flow_state (zlink::receive_flow_state_t::running);
    const auto completion_deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (!pending->await_ready () && std::chrono::steady_clock::now () < completion_deadline)
        (void) source_port.poll (raw_route_fixture_poll_interval);
    assert (pending->await_ready ());
    assert (pending->result ());
    assert (pending->result ().value () == zlink::submit_result_t::ok);

    const auto expected_session_rid =
      zlink::routing_id_t::from (deferred_trace_session_id).to_hex ();
    bool saw_deferred_completion = false;
    {
        std::lock_guard lock (captured_events_mutex);
        saw_deferred_completion =
          std::any_of (captured_events->begin (), captured_events->end (), [&] (const auto &event) {
              return event.detail_stage == "router_admission_complete"
                     && event.detail_result == "ok" && event.actor_id == deferred_trace_actor_id
                     && event.stream_session_id == expected_session_rid;
          });
    }
    assert (saw_deferred_completion);

    source_port.close ();
    target_port.close ();
    monitor.close ();
}

void verify_binding_completion_bypasses_handler_executor ()
{
    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    bool blocker_started = false;
    bool release_blocker = false;
    zlink::framework::runtime::handler_coroutine_executor ().post_native_continuation ([&] {
        std::unique_lock lock (blocker_mutex);
        blocker_started = true;
        blocker_changed.notify_all ();
        blocker_changed.wait (lock, [&] { return release_blocker; });
    });
    {
        std::unique_lock lock (blocker_mutex);
        assert (blocker_changed.wait_for (lock, 2s, [&] { return blocker_started; }));
    }

    zlink::context_t context;
    zlink::router_socket_t source (context), target (context);
    const auto source_rid = zlink::routing_id_t::from ("direct-completion-source");
    const auto target_rid = zlink::routing_id_t::from ("direct-completion-target");
    source.set_routing_id (source_rid);
    target.set_routing_id (target_rid);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    target.bind ("inproc://framework-direct-binding-completion");
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect ("inproc://framework-direct-binding-completion");
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));

    backend::raw_route_port_t source_port (source), target_port (target);
    auto pending = source_port.request (target_rid.to_bytes (), request_parts (), 2s);
    std::atomic_int observed_count{0};
    zlink::framework::detail::observe_task_completion (pending, [&] (const auto &settled) {
        assert (settled && settled.value ().terminal == zlink::request_result_t::ok);
        observed_count.fetch_add (1, std::memory_order_release);
    });
    std::optional<backend::raw_received_t> received;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!received && std::chrono::steady_clock::now () < deadline)
        received = target_port.receive_if_ready (target_port.poll (10ms));
    assert (received && received->reply_token);
    assert (target_port.reply (*received, request_parts ()));
    assert (!target_port.receive_if_ready (target_port.poll (0ms)));
    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (observed_count.load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) source_port.poll (10ms);
    }
    // The only handler-executor worker is still blocked. The raw binding
    // terminal must therefore reach its observer on the binding completion
    // resource rather than waiting for that executor.
    assert (observed_count.load (std::memory_order_acquire) == 1);

    {
        std::lock_guard lock (blocker_mutex);
        release_blocker = true;
    }
    blocker_changed.notify_all ();
    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    source_port.close ();
    target_port.close ();
    monitor.close ();
}

void verify_completion_only_wait_keeps_ordinary_record_unclaimed ()
{
    zlink::context_t context;
    zlink::router_socket_t source (context), target (context);
    const auto source_rid = zlink::routing_id_t::from ("permit-mask-source");
    const auto target_rid = zlink::routing_id_t::from ("permit-mask-target");
    source.set_routing_id (source_rid);
    target.set_routing_id (target_rid);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    target.bind ("inproc://framework-permit-mask");
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect ("inproc://framework-permit-mask");
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));
    backend::raw_route_port_t source_port (source), target_port (target);
    auto sent = source_port.send_result (target_rid.to_bytes (), request_parts ());
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!sent.await_ready () && std::chrono::steady_clock::now () < deadline)
        (void) source_port.poll (10ms);
    assert (sent.await_ready ());
    assert (sent.result ().value () == zlink::submit_result_t::ok);
    assert (target_port.poll (2s) == zlink::poll_event_flag_t::pollin);
    // Queued DATA does not turn a completion-only wait into a busy loop.
    const auto started = std::chrono::steady_clock::now ();
    assert (target_port.poll (20ms, false) == zlink::poll_event_flag_t::none);
    assert (std::chrono::steady_clock::now () - started >= 15ms);
    const auto received = target_port.receive_if_ready (target_port.poll (0ms));
    assert (received && received->parts == request_parts ());
    source_port.close ();
    target_port.close ();
    monitor.close ();
}

void verify_ok_send_submissions_keep_unfinished_depth_at_zero ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t source (context), target (context);
    zlink::dealer_socket_t dealer (context);
    const auto source_rid = zlink::routing_id_t::from ("ok-depth-source");
    const auto target_rid = zlink::routing_id_t::from ("ok-depth-target");
    const auto dealer_rid = zlink::routing_id_t::from ("ok-depth-dealer");
    source.set_routing_id (source_rid);
    target.set_routing_id (target_rid);
    dealer.set_routing_id (dealer_rid);
    for (auto *socket : {&source, &target}) {
        socket->options ().linger (0ms);
        socket->options ().send_hwm (zlink::byte_count_t::bytes (16u * 1024u * 1024u));
        socket->options ().recv_hwm (zlink::byte_count_t::bytes (16u * 1024u * 1024u));
    }
    dealer.options ().linger (0ms);
    dealer.options ().send_timeout (2s);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (16u * 1024u * 1024u));
    dealer.options ().recv_hwm (zlink::byte_count_t::bytes (16u * 1024u * 1024u));
    target.bind ("inproc://framework-ok-send-depth");
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    auto dealer_monitor = dealer.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect ("inproc://framework-ok-send-depth");
    dealer.connect ("inproc://framework-ok-send-depth");
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));
    assert (wait_for_monitor_event (dealer_monitor, zlink::monitor_event::connection_ready, 2s));

    backend::raw_route_port_t source_port (source);
    backend::raw_dealer_port_t dealer_port (dealer);
    std::size_t unfinished = 0;
    std::size_t max_unfinished = 0;
    const auto record_depth = [&] (bool ready) {
        if (!ready)
            ++unfinished;
        max_unfinished = std::max (max_unfinished, unfinished);
    };
    constexpr std::size_t submission_count = 64;
    for (std::size_t index = 0; index < submission_count; ++index) {
        auto routed_result = source_port.send_result (target_rid.to_bytes (), request_parts ());
        record_depth (routed_result.await_ready ());
        assert (routed_result.await_ready ());
        assert (routed_result.result ().value () == zlink::submit_result_t::ok);

        auto routed = source_port.send (target_rid.to_bytes (), request_parts ());
        record_depth (routed.await_ready ());
        assert (routed.await_ready ());
        assert (routed.result ().value ());

        auto dealer_result = dealer_port.send_result (request_parts ());
        record_depth (dealer_result.await_ready ());
        assert (dealer_result.await_ready ());
        assert (dealer_result.result ().value () == zlink::submit_result_t::ok);

        auto dealer_sent = dealer_port.send (request_parts ());
        record_depth (dealer_sent.await_ready ());
        assert (dealer_sent.await_ready ());
        assert (dealer_sent.result ().value ());
    }
    // An OK snapshot means the local transport queue already owns the record;
    // no Framework admission observer may remain in flight for that submit.
    assert (max_unfinished == 0);
    assert (unfinished == 0);

    dealer_port.close ();
    source_port.close ();
    dealer_monitor.close ();
    monitor.close ();
}

void verify_backpressured_send_waits_for_admission ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::dealer_socket_t source (context);
    zlink::router_socket_t target (context);
    const auto source_rid = zlink::routing_id_t::from ("wait-source");
    source.set_routing_id (source_rid);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    source.options ().send_timeout (raw_route_fixture_timeout);
    source.options ().send_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.options ().recv_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
    target.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    target.bind ("inproc://framework-backpressured-send-wait");
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    source.connect ("inproc://framework-backpressured-send-wait");
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready,
                                    raw_route_fixture_timeout));
    const auto pause_deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (monitor.status ().flow_paused_connections == 0
           && std::chrono::steady_clock::now () < pause_deadline)
        std::this_thread::yield ();
    assert (monitor.status ().flow_paused_connections == 1);

    zlink::poller_t source_poller;
    backend::raw_dealer_port_t source_port (source, nullptr, &source_poller);
    std::optional<zlink::framework::task_t<zlink::submit_result_t>> waiting;
    for (std::size_t index = 0; index < raw_route_backpressure_attempt_limit && !waiting; ++index) {
        auto sent = source_port.send_result (request_parts ());
        if (!sent.await_ready ()) {
            waiting.emplace (std::move (sent));
        } else {
            assert (sent.result ().value () == zlink::submit_result_t::ok);
        }
    }
    assert (waiting);

    auto &scheduler = zlink::framework::detail::deadline_scheduler_t::instance ();
    const auto before = scheduler.scheduled_count ();
    auto request = source_port.request (request_parts (), raw_request_capacity_timeout);
    assert (scheduler.scheduled_count () == before + 1);
    std::atomic_int terminals{0};
    zlink::framework::detail::observe_task_completion (request, [&] (const auto &result) {
        assert (!result
                && result.error_kind ()
                     == zlink::framework::framework_error_kind_t::deadline_exceeded);
        terminals.fetch_add (1, std::memory_order_release);
    });
    const auto held_until = std::chrono::steady_clock::now () + 4 * raw_request_capacity_timeout;
    while (std::chrono::steady_clock::now () < held_until) {
        zlink::poll_event_t event;
        (void) source_poller.wait (&event, 1, raw_route_fixture_poll_interval);
    }
    assert (request.await_ready ());
    assert (terminals.load (std::memory_order_acquire) == 1);
    assert (!request.result ()
            && request.result ().error_kind ()
                 == zlink::framework::framework_error_kind_t::deadline_exceeded);

    target.set_receive_flow_state (zlink::receive_flow_state_t::running);
    backend::raw_route_port_t target_port (target);
    int replies = 0;
    const auto deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while ((!waiting->await_ready () || replies == 0)
           && std::chrono::steady_clock::now () < deadline) {
        zlink::poll_event_t event;
        (void) source_poller.wait (&event, 1, raw_route_fixture_poll_interval);
        auto received = target_port.receive_if_ready (target_port.poll (0ms));
        if (received && received->reply_token) {
            assert (target_port.reply (*received, request_parts ()));
            ++replies;
        }
    }
    assert (replies == 1);
    assert (waiting->await_ready ());
    assert (waiting->result ().value () == zlink::submit_result_t::ok);

    auto healthy = source_port.request (request_parts (), raw_route_fixture_timeout);
    while (!healthy.await_ready () && std::chrono::steady_clock::now () < deadline) {
        auto received =
          target_port.receive_if_ready (target_port.poll (raw_route_fixture_poll_interval));
        if (received && received->reply_token) {
            assert (target_port.reply (*received, request_parts ()));
            ++replies;
        }
        zlink::poll_event_t event;
        (void) source_poller.wait (&event, 1, raw_route_fixture_poll_interval);
    }
    assert (healthy.await_ready ()
            && healthy.result ().value ().terminal == zlink::request_result_t::ok);
    assert (replies == 2);
    assert (terminals.load (std::memory_order_acquire) == 1);
    assert (!request.result ()
            && request.result ().error_kind ()
                 == zlink::framework::framework_error_kind_t::deadline_exceeded);
    source_port.close ();
    target_port.close ();
    monitor.close ();
}

void verify_immediate_request_has_no_framework_deadline ()
{
    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    zlink::router_socket_t target (context);
    source.options ().linger (0ms);
    target.options ().linger (0ms);
    target.bind ("inproc://framework-immediate-request-deadline");
    auto monitor = source.monitor_open (zlink::monitor_event::connection_ready);
    source.connect ("inproc://framework-immediate-request-deadline");
    assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready,
                                    raw_route_fixture_timeout));
    backend::raw_dealer_port_t port (source);
    auto &scheduler = zlink::framework::detail::deadline_scheduler_t::instance ();
    const auto before = scheduler.scheduled_count ();
    auto pending = port.request (request_parts (), raw_route_fixture_timeout);
    assert (!pending.await_ready ());
    assert (scheduler.scheduled_count () == before);
    port.close ();
    monitor.close ();
}

void verify_missing_rid_is_initial_not_connected_without_wait_token ()
{
    zlink::context_t context;
    zlink::router_socket_t router (context);
    router.options ().mandatory (true);
    backend::raw_route_port_t port (router);

    auto request =
      port.request (zlink::routing_id_t::from ("missing-route").to_bytes (), request_parts (), 1s);
    assert (request.await_ready ());
    const auto &settled = request.result ();
    assert (settled);
    assert (settled.value ().terminal == zlink::request_result_t::not_connected);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::initial_admission);
    assert (settled.value ().failure->submit_result == zlink::submit_result_t::not_connected);
    assert (settled.value ().failure->internal_errno == EHOSTUNREACH);
    // A token-bearing rejection would remain pending until a WRITABLE record.
    // Synchronous completion here pins the D-B85 ID/token-zero path.
    assert (port.poll (0ms) == zlink::poll_event_flag_t::none);
    port.close ();
}

void verify_handover_request_completion_is_replayable ()
{
    zlink::context_t context;
    zlink::router_socket_t target (context), source (context);
    const auto target_rid = zlink::routing_id_t::from ("A");
    const auto source_rid = zlink::routing_id_t::from ("Z");
    target.set_routing_id (target_rid);
    source.set_routing_id (source_rid);
    for (auto *socket : {&target, &source}) {
        socket->options ().linger (0ms);
        socket->options ().mandatory (true);
        socket->options ().rid_duplicate_policy (zlink::rid_duplicate_policy_t::handover);
    }
    target.bind ("inproc://framework-handover-A");
    source.bind ("inproc://framework-handover-Z");
    auto ready = source.monitor_open (zlink::monitor_event::connection_ready);
    source.options ().connect_routing_id (target_rid);
    source.connect ("inproc://framework-handover-A");
    assert (wait_for_monitor_event (ready, zlink::monitor_event::connection_ready, 2s));
    backend::raw_route_port_t source_port (source), target_port (target);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    auto pending = source_port.request (target_rid.to_bytes (), request_parts (), 2s);
    std::optional<backend::raw_received_t> admitted;
    while (!admitted && std::chrono::steady_clock::now () < deadline)
        admitted = target_port.receive_if_ready (target_port.poll (1ms));
    assert (admitted && admitted->reply_token);
    assert (!pending.await_ready ());

    // Z -> A is admitted first; reciprocal A -> Z supersedes that pair.
    const auto handover_started = std::chrono::steady_clock::now ();
    target.options ().connect_routing_id (source_rid);
    target.connect ("inproc://framework-handover-Z");
    while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline)
        (void) source_port.poll (1ms);
    assert (pending.await_ready ());
    const auto elapsed = std::chrono::steady_clock::now () - handover_started;
    const auto &completion = pending.result ().value ();
    assert (completion.terminal == zlink::request_result_t::not_connected);
    assert (completion.failure);
    assert (completion.failure->phase == backend::raw_request_failure_phase_t::completion_terminal);
    assert (!completion.failure->submit_result);
    // The binding completion owner normalizes typed NOT_CONNECTED to ENOTCONN.
    assert (completion.failure->internal_errno == ENOTCONN);
    assert (elapsed < 20ms);
    std::cout << "handover completion_us="
              << std::chrono::duration_cast<std::chrono::microseconds> (elapsed).count ()
              << " typed=NOT_CONNECTED errno=ENOTCONN outcome=route_unavailable\n";
    source_port.close ();
    target_port.close ();
    ready.close ();
}

void verify_disconnect_rid_ends_issued_wait_token_with_enoent ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t server (context);
    zlink::router_socket_t client (context);
    server.options ().linger (0ms);
    client.options ().linger (0ms);
    const auto server_rid = zlink::routing_id_t::from ("wait-terminal-server");
    const auto client_rid = zlink::routing_id_t::from ("wait-terminal-client");
    server.set_routing_id (server_rid);
    client.set_routing_id (client_rid);
    server.set_receive_flow_state (zlink::receive_flow_state_t::paused);
    client.options ().connect_routing_id (server_rid);
    auto server_monitor = server.monitor_open (zlink::monitor_event::connection_ready);
    auto client_monitor = client.monitor_open (zlink::monitor_event::connection_ready);
    const std::string endpoint = "inproc://framework-raw-route-wait-token-terminal";
    server.bind (endpoint);
    client.connect (endpoint);
    assert (wait_for_monitor_event (server_monitor, zlink::monitor_event::connection_ready, 2s));
    assert (wait_for_monitor_event (client_monitor, zlink::monitor_event::connection_ready, 2s));

    backend::raw_route_port_t port (client);
    auto request = port.request (server_rid.to_bytes (), request_parts (), 2s);
    assert (!request.await_ready ());

    client.disconnect_rid (server_rid);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!request.await_ready () && std::chrono::steady_clock::now () < deadline) {
        (void) port.poll (10ms);
    }
    assert (request.await_ready ());
    const auto &settled = request.result ();
    assert (settled);
    assert (settled.value ().terminal == zlink::request_result_t::not_connected);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::completion_terminal);
    assert (settled.value ().failure->submit_result == zlink::submit_result_t::not_found);
    assert (settled.value ().failure->internal_errno == ENOENT);
    port.close ();
    client_monitor.close ();
    server_monitor.close ();
}
void verify_pending_send_route_removal_and_shutdown ()
{
    using zlink::framework::framework_error_kind_t;
    namespace messaging = zlink::framework::runtime::messaging;
    for (const bool shutdown : {false, true}) {
        zlink::context_t context;
        zlink::router_socket_t server (context), client (context);
        const auto rid = zlink::routing_id_t::from ("pending-send-terminal-target");
        server.set_routing_id (rid);
        client.options ().send_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
        server.options ().recv_hwm (zlink::byte_count_t::bytes (raw_route_backpressure_hwm_bytes));
        server.set_receive_flow_state (zlink::receive_flow_state_t::paused);
        server.options ().linger (0ms);
        client.options ().linger (0ms);
        client.options ().mandatory (true);
        const std::string endpoint =
          shutdown ? "inproc://send-pending-shutdown" : "inproc://send-pending-remove";
        server.bind (endpoint);
        auto monitor = client.monitor_open (zlink::monitor_event::connection_ready);
        client.connect (endpoint);
        assert (wait_for_monitor_event (monitor, zlink::monitor_event::connection_ready, 2s));
        backend::raw_route_port_t port (client);
        backend::raw_route_port_t target_port (server);
        const auto paused_by = std::chrono::steady_clock::now () + 2s;
        while (monitor.status ().flow_paused_connections == 0
               && std::chrono::steady_clock::now () < paused_by) {
            (void) target_port.poll (10ms);
            (void) port.poll (10ms);
        }
        assert (monitor.status ().flow_paused_connections == 1);
        std::optional<zlink::framework::task_t<zlink::submit_result_t>> pending;
        for (std::size_t attempt = 0; attempt < raw_route_backpressure_attempt_limit && !pending;
             ++attempt) {
            auto sent = port.send_result (rid.to_bytes (), request_parts ());
            if (!sent.await_ready ())
                pending.emplace (std::move (sent));
            else
                assert (sent.result ().value () == zlink::submit_result_t::ok);
        }
        assert (pending);
        const auto hold_until = std::chrono::steady_clock::now () + 3100ms;
        while (std::chrono::steady_clock::now () < hold_until) {
            (void) port.poll (10ms);
            assert (!pending->await_ready ());
        }
        if (shutdown) {
            port.close ();
            client.close ();
        } else
            client.disconnect_rid (rid);
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (!pending->await_ready () && std::chrono::steady_clock::now () < deadline)
            (void) port.poll (10ms);
        assert (pending->await_ready ());
        const auto &result = pending->result ();
        assert (result);
        assert (messaging::map_submit_result_exception (result.value (), "pending send").kind ()
                == (shutdown ? framework_error_kind_t::shutting_down
                             : framework_error_kind_t::unavailable));
        port.close ();
        target_port.close ();
        monitor.close ();
    }
}

}

static void verify_request_deadline_preserves_timeout_boundary ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> operation;
    auto pending = runtime::messaging::with_request_deadline (operation.task (),
                                                              std::chrono::steady_clock::now ());
    const auto expired = pending.result ();
    assert (!expired);
    assert (expired.error_kind () == framework_error_kind_t::deadline_exceeded);
    assert (expired.error () != nullptr);
    assert (expired.error ()->code () == std::errc::timed_out);
    operation.complete (result_t<int>::success (7));
    const auto settled = pending.result ();
    assert (!settled);
    assert (settled.exception () == expired.exception ());
}

int main ()
{
    verify_request_deadline_preserves_timeout_boundary ();
    verify_pending_send_route_removal_and_shutdown ();
    verify_immediate_request_has_no_framework_deadline ();
    verify_request_submission_stages ();
    verify_submission_admission_consumes_only_backpressure ();
    verify_capacity_refusal_phase_controls_public_terminal ();
    verify_writable_request_timeout_remains_deadline_exceeded ();
    verify_deferred_send_trace_owns_its_context ();
    verify_binding_completion_bypasses_handler_executor ();
    verify_completion_only_wait_keeps_ordinary_record_unclaimed ();
    verify_ok_send_submissions_keep_unfinished_depth_at_zero ();
    verify_backpressured_send_waits_for_admission ();
    verify_handover_request_completion_is_replayable ();
    verify_missing_rid_is_initial_not_connected_without_wait_token ();
    verify_disconnect_rid_ends_issued_wait_token_with_enoent ();
    return 0;
}
