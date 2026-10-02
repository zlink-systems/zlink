/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/backend/raw_route_port.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/host/bound_session_send_stage_trace.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"

#include <zlink.hpp>

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
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
    const auto initial = backend::map_binding_request_submit_result (
      zlink::submit_result_t::backpressured,
      backend::raw_request_failure_phase_t::initial_admission,
      [] () -> backend::raw_request_result_t {
          throw zlink::submit_error_t (zlink::submit_result_t::backpressured, EAGAIN);
      });
    assert (initial == backend::raw_request_result_t::failed);
    const auto refused = client_server::client_server_operation_exception (
      foundation::operation_terminal_t::transport_failed, "tokenless capacity");
    assert (refused.kind () == framework_error_kind_t::unavailable);
    assert (zlink::framework::detail::boundary_state (refused)
            != zlink::framework::detail::boundary_error_t::timed_out);
    const auto completion = backend::map_binding_request_submit_result (
      zlink::submit_result_t::backpressured,
      backend::raw_request_failure_phase_t::completion_terminal,
      [] { return backend::raw_request_result_t::failed; });
    assert (completion == backend::raw_request_result_t::timed_out);
    const auto expired = client_server::client_server_operation_exception (
      foundation::operation_terminal_t::timed_out, "expired WRITABLE token");
    assert (expired.kind () == framework_error_kind_t::deadline_exceeded);
    std::cout << "f20 tokenless=Unavailable writable_timeout=DeadlineExceeded" << std::endl;
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
    source.options ().send_timeout (raw_request_capacity_timeout);
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
    auto pending =
      port.request (target_rid.to_bytes (), request_parts (), raw_route_fixture_timeout);
    assert (!pending.await_ready ());
    const auto deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline)
        (void) port.poll (raw_route_fixture_poll_interval);
    assert (pending.await_ready ());
    assert (pending.result ());
    const auto &completion = pending.result ().value ();
    assert (completion.failure);
    std::cout << "f20 phase=" << static_cast<int> (completion.failure->phase) << " submit="
              << (completion.failure->submit_result
                    ? static_cast<int> (*completion.failure->submit_result)
                    : -1)
              << " request="
              << (completion.failure->request_result
                    ? static_cast<int> (*completion.failure->request_result)
                    : -1)
              << " raw_result=" << static_cast<int> (completion.result) << std::endl;
    assert (completion.failure->phase == backend::raw_request_failure_phase_t::completion_terminal);
    assert (completion.failure->submit_result == zlink::submit_result_t::backpressured);
    assert (completion.result == backend::raw_request_result_t::timed_out);
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
    std::atomic_bool observed{false};
    zlink::framework::detail::observe_task_completion (pending, [&] (const auto &settled) {
        assert (settled && settled.value ().result == backend::raw_request_result_t::ok);
        observed.store (true, std::memory_order_release);
    });
    std::optional<backend::raw_received_t> received;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!received && std::chrono::steady_clock::now () < deadline)
        received = target_port.receive_if_ready (target_port.poll (10ms));
    assert (received && received->reply_token);
    assert (target_port.reply (*received, request_parts ()));
    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (!observed.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) source_port.poll (10ms);
    }
    // The only handler-executor worker is still blocked. The raw binding
    // terminal must therefore reach its observer on the binding completion
    // resource rather than waiting for that executor.
    assert (observed.load (std::memory_order_acquire));

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

    target.set_receive_flow_state (zlink::receive_flow_state_t::running);
    const auto deadline = std::chrono::steady_clock::now () + raw_route_fixture_timeout;
    while (!waiting->await_ready () && std::chrono::steady_clock::now () < deadline) {
        zlink::poll_event_t event;
        (void) source_poller.wait (&event, 1, raw_route_fixture_poll_interval);
    }
    assert (waiting->await_ready ());
    assert (waiting->result ().value () == zlink::submit_result_t::ok);

    source_port.close ();
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
    assert (settled.value ().result == backend::raw_request_result_t::route_unavailable);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::initial_admission);
    assert (settled.value ().failure->submit_result == zlink::submit_result_t::not_connected);
    assert (!settled.value ().failure->request_result);
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
    assert (completion.result == backend::raw_request_result_t::route_unavailable);
    assert (completion.failure);
    assert (completion.failure->phase == backend::raw_request_failure_phase_t::completion_terminal);
    assert (!completion.failure->submit_result);
    assert (completion.failure->request_result == zlink::request_result_t::not_connected);
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
    assert (settled.value ().result == backend::raw_request_result_t::failed);
    assert (settled.value ().failure);
    assert (settled.value ().failure->phase
            == backend::raw_request_failure_phase_t::completion_terminal);
    assert (settled.value ().failure->submit_result == zlink::submit_result_t::not_found);
    assert (!settled.value ().failure->request_result);
    assert (settled.value ().failure->internal_errno == ENOENT);
    port.close ();
    client_monitor.close ();
    server_monitor.close ();
}
}

int main ()
{
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
