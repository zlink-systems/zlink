/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6a_bound_session_fixture.hpp"
#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "../support/m6a_native_calls.hpp"

#include "mailbox_claim_fixture.hpp"

#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

void verify_bound_session_close_reply_grammar ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("close-reply-source"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("close-reply-target"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto owner = target.topology ().local_descriptor ();
    admit_pair (source, target, owner);
    enum class reply_case_t
    {
        canonical,
        payload,
        correlation_mismatch
    };
    for (const auto reply_case :
         {reply_case_t::canonical, reply_case_t::payload, reply_case_t::correlation_mismatch}) {
        auto close = source.request_bound_session_close (
          {0,
           "close-actor",
           1,
           {owner.node_routing_id, owner.lifecycle_generation, "owner", 1, bytes ("session"), 1}},
          2s);
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        while (!claim && std::chrono::steady_clock::now () < deadline) {
            (void) await_task (
              target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        }
        assert (claim && claim->records.size () == 1);
        auto &request = *claim->records.front ();
        const auto decoded = protocol::decode_bound_session_close (
          protocol::message_bytes (request.parts ().front ()));
        assert (decoded.actor_id == "close-actor");
        if (reply_case == reply_case_t::correlation_mismatch)
            request.correlation = *request.correlation + 1;
        if (reply_case == reply_case_t::payload)
            assert (target.reply (request, {"forbidden", "application/json", bytes ("{}")}));
        else
            assert (target.reply_bound_session_bind (request, 0, 0));
        assert (target.mailbox ().release (*claim));
        while (!close.await_ready () && std::chrono::steady_clock::now () < deadline)
            (void) await_task (
              source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (close.await_ready ());
        const auto terminal = close.result ();
        if (reply_case == reply_case_t::canonical)
            assert (terminal);
        else {
            assert (!terminal);
            assert (terminal.error ()->kind ()
                    == zlink::framework::framework_error_kind_t::protocol_error);
        }
    }
    source.close ();
    target.close ();
}

void verify_bound_session_bind_retries_until_route_is_admitted ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-delay-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-delay-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    source.expect_peer (target_descriptor);
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "delayed-bind-actor"), 2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    assert (completion.wait_for (0ms) == std::future_status::timeout);

    admit_pair (source, target, target_descriptor);
    complete_bound_session_bind (source, target, completion, "delayed-bind-actor");
    source.close ();
    target.close ();
}

void verify_bound_session_bind_exposes_only_accepted_attempt ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-stage-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-stage-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto remote = target.topology ().local_descriptor ();
    source.expect_peer (remote);
    auto accepted = std::make_shared<zlink::framework::task_completion_source_t<void>> ();
    auto admission = accepted->task ();
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      remote.node_routing_id, bound_session_bind_request (remote, "stage-bind-actor"), 2s,
      [&completion_promise] (foundation::operation_terminal_t terminal,
                             ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      },
      accepted)));
    assert (!admission.await_ready ());
    assert (completion.wait_for (0ms) == std::future_status::timeout);
    admit_pair (source, target, remote);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!admission.await_ready () && std::chrono::steady_clock::now () < deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        (void) await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (admission.await_ready () && admission.result ());
    assert (completion.wait_for (0ms) == std::future_status::timeout);
    complete_bound_session_bind (source, target, completion, "stage-bind-actor");
    source.close ();
    target.close ();
}

using bound_session_completion_t =
  std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;

zlink::framework::request_task_t<bound_session_completion_t>
bound_session_request (mesh::raw_mesh_node_owner_t &source,
                       const mesh::service_node_descriptor_t &target,
                       std::chrono::milliseconds timeout)
{
    using namespace zlink::framework;
    source.expect_peer (target);
    auto reply = std::make_shared<task_completion_source_t<bound_session_completion_t>> ();
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    request_task_t<bound_session_completion_t> result (reply->task (), admitted);
    assert (await_task (source.request_bound_session_bind (
      target.node_routing_id, bound_session_bind_request (target, "durable-bind-actor"), timeout,
      [reply] (foundation::operation_terminal_t terminal,
               ::zlink::framework::result_t<zlink::message_t> completion_payload) {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          if (terminal == foundation::operation_terminal_t::completed) {
              reply->complete (
                result_t<bound_session_completion_t>::success ({terminal, std::move (payload)}));
          } else {
              const auto kind = terminal == foundation::operation_terminal_t::timed_out
                                  ? framework_error_kind_t::deadline_exceeded
                                  : framework_error_kind_t::unavailable;
              reply->complete (result_t<bound_session_completion_t>::failure (
                kind, "Bound Session request did not complete"));
          }
      },
      admitted)));
    return result;
}

// Discovery publishes expectations and admission; the operation owner handles expiry and fence removal.
void verify_bound_session_bind_missing_descriptor (std::string_view outcome)
{
    using namespace zlink::framework;
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("bind-missing-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("bind-missing-target")}));
    source.start ();
    target.start ();
    auto remote = target.topology ().local_descriptor ();
    const bool invalidated_other_generation = outcome == "invalidated-other-generation";
    if (outcome == "other-generation" || invalidated_other_generation) {
        admit_pair (source, target, remote);
        remote.lifecycle_generation =
          remote.lifecycle_generation == std::numeric_limits<std::uint64_t>::max ()
            ? 1
            : std::numeric_limits<std::uint64_t>::max ();
    }
    if (outcome == "invalidated" || invalidated_other_generation)
        source.expect_peer (remote);
#if !defined(_WIN32)
    request_submissions.store (0);
    count_request_submissions.store (true);
#endif
    std::promise<bound_session_completion_t> promise;
    auto completion = promise.get_future ();
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    const auto invalidation_deadline = invalidated_other_generation
                                         ? std::chrono::steady_clock::now () + 2s
                                         : std::chrono::steady_clock::time_point::max ();
    const auto request_timeout = outcome == "late-endpoint-failure" ? 5s : 2s;
    assert (await_task (source.request_bound_session_bind (
      remote.node_routing_id, bound_session_bind_request (remote, "missing-bind-actor"),
      request_timeout,
      [&promise] (foundation::operation_terminal_t terminal, result_t<zlink::message_t> payload) {
          promise.set_value ({terminal, payload.value ().to_bytes ()});
      },
      admitted)));
    assert (!admitted->task ().await_ready ());
    assert (completion.wait_for (0ms) == std::future_status::timeout);
    assert (!target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
#if !defined(_WIN32)
    assert (request_submissions.load () == 0);
#endif
    if (outcome == "late") {
        source.expect_peer (remote);
        admit_pair (source, target, remote);
        complete_bound_session_bind (source, target, completion, "missing-bind-actor");
        assert (admitted->task ().await_ready ());
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    } else if (outcome == "late-endpoint-failure") {
        target.close ();
        source.expect_peer (remote);
        assert (source.connect_peer (remote.advertised_endpoint, remote));
        // Windows confirms a refused connect at about 2s, so this request must outlast that event.
        const auto failure_deadline = std::chrono::steady_clock::now () + request_timeout;
        bool observed_failure = false;
        while (!observed_failure && std::chrono::steady_clock::now () < failure_deadline) {
            // Core snapshot changes signal POLLCONNECT; use the existing ingress poller.
            const auto readiness = source.wait_for_activity (
              std::max (0ms, std::chrono::ceil<std::chrono::milliseconds> (
                               failure_deadline - std::chrono::steady_clock::now ())));
            assert (readiness);
            assert (source.begin_receive_turn (*readiness));
            const auto observation = source.topology ().observe_peer (
              remote.node_routing_id, remote.advertised_endpoint) ();
            const auto attempt = observation.connection_attempt ();
            observed_failure = attempt && attempt->last_failure_id != 0;
        }
        assert (observed_failure);
#if !defined(_WIN32)
        assert (request_submissions.load () == 0);
#endif
        assert (completion.wait_for (250ms) == std::future_status::ready);
        assert (completion.get ().first == foundation::operation_terminal_t::route_unavailable);
        assert (!admitted->task ().await_ready ());
    } else if (invalidated_other_generation) {
        source.forget_peer (remote.node_routing_id, remote.advertised_endpoint);
        const auto remaining_peer = source.topology ().peer (remote.node_routing_id);
        assert (remaining_peer
                && remaining_peer->descriptor.lifecycle_generation != remote.lifecycle_generation);
        assert (peer_ready (source, remote.node_routing_id));
#if !defined(_WIN32)
        assert (request_submissions.load () == 0);
#endif
        // The older admitted peer must not retain the authority-selected request.
        assert (completion.wait_for (250ms) == std::future_status::ready);
        assert (std::chrono::steady_clock::now () < invalidation_deadline);
        assert (completion.get ().first == foundation::operation_terminal_t::route_unavailable);
        assert (!admitted->task ().await_ready ());
    } else {
        const auto expected = outcome != "invalidated"
                                ? foundation::operation_terminal_t::timed_out
                                : foundation::operation_terminal_t::route_unavailable;
        if (outcome != "invalidated")
            assert (await_task (source.expire_requests (std::chrono::steady_clock::now () + 3s))
                    == 1);
        else {
            assert (outcome == "invalidated");
            source.forget_peer (remote.node_routing_id, remote.advertised_endpoint);
        }
        // Terminal callbacks are delivered on an independent completion turn.
        completion.wait ();
        assert (completion.get ().first == expected);
        assert (!admitted->task ().await_ready ());
        remote = target.topology ().local_descriptor ();
        source.expect_peer (remote);
        admit_pair (source, target, remote);
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    }
#if !defined(_WIN32)
    assert (request_submissions.load () == (outcome == "late" ? 1 : 0));
    count_request_submissions.store (false);
#endif
    source.close ();
    target.close ();
}

void verify_bound_session_bind_does_not_replay_after_route_change ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("Z-bind-replay-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("A-bind-replay-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    admit_pair (source, target, remote);
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    auto request = bound_session_request (source, remote, 200ms);
    auto admitted = request.admitted ();
    std::atomic<int> admission_completions = 0;
    std::promise<void> admission_observed;
    auto observed = admission_observed.get_future ();
    zlink::framework::detail::observe_task_completion (admitted, [&] (const auto &) {
        ++admission_completions;
        admission_observed.set_value ();
    });
    const auto pump = [&] {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.expire_requests (now).result ().value ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
    };
    const auto receive = [&] {
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        while (!claim && std::chrono::steady_clock::now () < deadline) {
            pump ();
            claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        }
        assert (claim && claim->records.size () == 1);
        return claim;
    };
    auto first = receive ();
    const auto bind = protocol::decode_bound_session_bind (
      protocol::message_bytes (first->records.front ()->parts ().front ()));
    // Bind replies select their wire terminal from the original request header.
    auto first_record = target.mailbox ().take ({first, 0});
    assert (first_record && !first_record->parts ().empty ());
    std::size_t submissions = first->records.size ();
    assert (admitted.await_ready () && admitted.result ());
    const auto *first_admission = &admitted.result ();
    observed.get ();
    assert (admission_completions == 1 && !request.await_ready ());
    assert (target.mailbox ().release (*first));
    assert (target.connect_peer (source.endpoint (), local));
    while (!request.await_ready () && std::chrono::steady_clock::now () < deadline) {
        pump ();
        const auto extra =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        assert (!extra);
    }
    assert (request.await_ready ());
    assert (!request.result ());
    assert (request.result ().error () != nullptr);
    assert (admitted.result () && admission_completions == 1);
    assert (&admitted.result () == first_admission && submissions == 1);
    const auto terminal = request.result ().exception ();
    (void) target.reply_bound_session_bind (
      *first_record, 0, 0,
      protocol::stream_bind_reply_tail_t{bind.binding.generation,
                                         bind.actor.authority_owner_generation});
    for (int turn = 0; turn < 32; ++turn) {
        pump ();
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    }
    assert (request.result ().exception () == terminal);
    source.close ();
    target.close ();
}

void verify_bound_session_bind_unadmitted_deadline_fails_both_stages ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("bind-no-admission-source")}));
    source.start ();
    auto remote = descriptor ("bind-no-admission-target", "tcp://127.0.0.1:1");
    remote.state = mesh::service_node_state_t::serving;
    auto request = bound_session_request (source, remote, 50ms);
    auto admitted = request.admitted ();
    assert (!admitted.await_ready () && !request.await_ready ());
    const auto deadline = std::chrono::steady_clock::now () + 500ms;
    while (!request.await_ready () && std::chrono::steady_clock::now () < deadline) {
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    assert (request.await_ready () && admitted.await_ready ());
    const auto &reply = request.result ();
    const auto &admission = admitted.result ();
    assert (!reply && !admission);
    assert (reply.error ()->kind () == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (admission.error ()->kind ()
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (reply.exception () == admission.exception ());
    source.close ();
}

void verify_bound_session_bind_permanent_absence_is_bounded ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-timeout-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    auto target_descriptor = descriptor ("bind-timeout-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    source.expect_peer (target_descriptor);
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    const auto started = std::chrono::steady_clock::now ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "missing-bind-actor"), 50ms,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    const auto deadline = started + 500ms;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    assert (std::chrono::steady_clock::now () - started >= 40ms);
    assert (completion.get ().first == foundation::operation_terminal_t::timed_out);
    source.close ();
}

void verify_bound_session_bind_reply_completes_registered_operation ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-reply-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("bind-reply-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    assert (await_task (source.request_bound_session_bind (
      target_descriptor.node_routing_id,
      bound_session_bind_request (target_descriptor, "reply-bind-actor"), 2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      })));
    complete_bound_session_bind (source, target, completion, "reply-bind-actor");
    source.close ();
    target.close ();
}
}
