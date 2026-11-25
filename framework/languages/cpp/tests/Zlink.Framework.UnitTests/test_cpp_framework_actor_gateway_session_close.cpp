/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int actor_unbound_follows_accepted_push_submission ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("actor-unbound-fifo").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("actor-unbound-fifo");
    auto push_terminal = std::make_shared<task_completion_source_t<void>> ();
    std::mutex events_mutex;
    std::vector<std::string> events;
    runtime.attach_transport_writer (
      stream, [push_terminal, &events_mutex, &events] (const stream_header_t &header,
                                                       const zlink::message_t &) {
          {
              std::lock_guard lock (events_mutex);
              events.emplace_back (header.packet_name ());
          }
          return header.packet_name () != "$zlink.actor.unbound"
                   ? push_terminal->task ()
                   : task_t<void> (result_t<void>::success ());
      });
    const auto actor = test_actor_ref ("actor-node", "player", "fifo-actor", 7);
    gateway.bind_session_stream ("fifo-actor", stream, stream_codec_t::message_pack, "fifo-session",
                                 23, actor, 1);
    const auto accepted = gateway.actor_context (actor)
                            .bound_session ()
                            .send (std::string ("payload"))
                            .async ()
                            .result ();
    if (!accepted)
        return 1;
    auto direct = std::async (std::launch::async, [&] {
        return gateway.dispatch_bound_session_send (actor, "direct.push", stream_codec_t::raw,
                                                    zlink::message_t::from ("direct"));
    });
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock (events_mutex);
            if (events.size () == 2)
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    gateway.unbind_session_stream ("fifo-actor", "fifo-session", 23);
    bool unbound_overtook_push = false;
    {
        std::lock_guard lock (events_mutex);
        unbound_overtook_push = events.size () != 3 || events.front () == "$zlink.actor.unbound"
                                || events[1] != "direct.push"
                                || events.back () != "$zlink.actor.unbound";
    }
    if (unbound_overtook_push) {
        push_terminal->complete (result_t<void>::success ());
        return 2;
    }
    const bool waiting_for_terminal =
      direct.wait_for (std::chrono::seconds (0)) != std::future_status::ready;
    push_terminal->complete (result_t<void>::success ());
    if (!waiting_for_terminal || !direct.get ())
        return 4;
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock (events_mutex);
            if (events.size () == 3)
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    std::lock_guard lock (events_mutex);
    return events.size () == 3 && events.back () == "$zlink.actor.unbound" ? 0 : 3;
}

int actor_unbound_follows_session_termination (bool physical, bool disconnect_then_shutdown)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    constexpr auto stream_name = "unbound-termination";
    constexpr auto submission_wait_timeout = std::chrono::seconds (5);
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    auto builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream (stream_name).bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session (stream_name);
    auto unbound = std::make_shared<std::atomic_int> (0);
    auto unbound_submitted = std::make_shared<std::promise<void>> ();
    auto submitted = unbound_submitted->get_future ();
    runtime.attach_transport_writer (
      stream,
      [unbound, unbound_submitted] (const stream_header_t &header, const zlink::message_t &) {
          if (header.packet_name () == "$zlink.actor.unbound" && ++(*unbound) == 1)
              unbound_submitted->set_value ();
          return task_t<void> (result_t<void>::success ());
      });
    session_actor_manager_access_t::attach (manager, stream);
    session_actor_manager_access_t::bind_native (
      manager, [] (actor_ref_t, std::uint64_t, std::uint16_t) {
          return request_task_t<void> (result_t<void>::success ());
      });
    auto disconnect_completion = std::make_shared<task_completion_source_t<void>> ();
    if (!disconnect_then_shutdown)
        disconnect_completion->complete (result_t<void>::success ());
    state->disconnect_dispatcher =
      [disconnect_completion] (const actor_ref_t &,
                               const std::optional<runtime::stateful::stream_binding_t> &) {
          return disconnect_completion->task ();
      };
    auto binding =
      manager.bind (test_actor_ref ("node", "player", "actor-a", 1)).async ().result ();
    if (!binding)
        return 1;
    if (physical) {
        if (!stream.close ().result ())
            return 2;
        session_actor_manager_access_t::disconnect (manager);
        if (disconnect_then_shutdown) {
            // The physical disconnect callback is still pending when shutdown closes the stream.
            if (!stream.close ().result ())
                return 5;
            disconnect_completion->complete (result_t<void>::success ());
            session_actor_manager_access_t::disconnect (manager);
        }
    } else {
        if (!binding.value ().notify_disconnected ().result ())
            return 3;
        if (submitted.wait_for (submission_wait_timeout) != std::future_status::ready)
            return 4;
    }
    return unbound->load () == (physical ? 0 : 1) ? 0 : 4;
}

int session_disconnect_uses_actor_slot_issue_order ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("disconnect-slot-order").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("disconnect-slot-order");
    runtime.attach_transport_writer (stream,
                                     [] (const stream_header_t &, const zlink::message_t &) {
                                         return task_t<void> (result_t<void>::success ());
                                     });
    session_actor_manager_access_t::attach (manager, stream);
    session_actor_manager_access_t::bind_native (
      manager, [] (actor_ref_t, std::uint64_t, std::uint16_t) {
          return request_task_t<void> (result_t<void>::success ());
      });
    std::vector<std::string> disconnected;
    state->disconnect_dispatcher =
      [&disconnected] (const actor_ref_t &actor,
                       const std::optional<runtime::stateful::stream_binding_t> &) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return task_t<void> (result_t<void>::success ());
      };
    if (!manager.bind (test_actor_ref ("node", "player", "z-issued-first", 1)).async ().result ()
        || !manager.bind (test_actor_ref ("node", "player", "a-issued-second", 1))
              .async ()
              .result ())
        return 1;
    session_actor_manager_access_t::disconnect (manager);
    return disconnected == std::vector<std::string>{"z-issued-first", "a-issued-second"} ? 0 : 2;
}

int close_unbound_session_reports_invalid_operation ()
{
    using namespace zlink::framework;
    detail::actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "unbound-close", 1);
    const auto result = gateway.actor_context (actor).bound_session ().disconnect ().result ();
    return !result && result.error_kind () == framework_error_kind_t::invalid_operation ? 0 : 1;
}

int close_exact_session_binding_uses_physical_terminal ()
{
    using namespace zlink::framework;
    namespace sf = runtime::stateful;
    namespace wire = runtime::protocol;
    const sf::object_ref_t actor{
      sf::object_kind_t::actor, "close-actor", 7, 11, "game", "actor-owner"};
    sf::stream_session_registry_t sessions (
      [&] (const std::string &) { return std::optional (actor); });
    const auto rid = zlink::routing_id_t::from ("session-rid");
    const auto owner = zlink::routing_id_t::from ("session-owner");
    unsigned closes = 0;
    const auto connection = sessions.open (rid.to_hex (), {}, owner, 13, [&] () -> task_t<void> {
        ++closes;
        co_return;
    });
    const auto [error, binding] = sessions.bind (connection, actor, 17, 19);
    if (error != sf::stateful_error_t::none)
        return 1;
    const wire::retired_bound_session_route_fence_t fence{
      owner.to_bytes (), 13, owner.to_string (), 13, rid.to_bytes (), binding.binding_generation};
    const wire::bound_session_close_t request{23, actor.key, actor.object_generation, fence};
    const auto decoded =
      wire::decode_bound_session_close (wire::encode_bound_session_close (request));
    if (decoded.actor_id != request.actor_id || decoded.session != fence)
        return 2;
    auto stale = request;
    ++stale.session.retired_binding_generation;
    auto denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 3;
    stale = request;
    ++stale.object_generation;
    denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 4;
    stale = request;
    ++stale.session.session_owner_node_generation;
    denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 5;
    auto closed = sessions.close_current (request, fence).result ();
    if (!closed || closes != 1 || !sessions.current_binding (actor.key))
        return 6;
    const auto replacement_rid = zlink::routing_id_t::from ("replacement-session");
    unsigned replacement_closes = 0;
    const auto replacement =
      sessions.open (replacement_rid.to_hex (), {}, owner, 13, [&] () -> task_t<void> {
          ++replacement_closes;
          co_return;
      });
    const auto [replacement_error, current] = sessions.bind (replacement, actor, 17, 19);
    if (replacement_error != sf::stateful_error_t::none)
        return 7;
    denied = sessions.close_current (request, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable
        || replacement_closes != 0 || closes != 1)
        return 8;
    auto current_request = request;
    current_request.session.session_routing_id = replacement_rid.to_bytes ();
    current_request.session.retired_binding_generation = current.binding_generation;
    closed = sessions.close_current (current_request, fence).result ();
    if (!closed || replacement_closes != 1)
        return 9;
    sessions.close (connection);
    if (!sessions.current_binding (actor.key))
        return 10;
    sessions.close (replacement);
    return sessions.current_binding (actor.key) ? 11 : 0;
}
} // namespace zlink::framework::test::actor_gateway
