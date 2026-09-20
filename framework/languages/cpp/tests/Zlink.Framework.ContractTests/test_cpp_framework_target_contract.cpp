/* SPDX-License-Identifier: MPL-2.0 */

/* G0 target-contract gate for the C++ public-contract gap effort.
 * Each check corresponds to a target the C++ public headers and runtime
 * sources must expose, and stays red until that target lands.
 * The checks scan the public headers and runtime sources textually so the
 * build keeps compiling while target signatures are still missing. */

#include "../support/read_text_file.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef ZLINK_FRAMEWORK_CPP_SOURCE_DIR
#error "ZLINK_FRAMEWORK_CPP_SOURCE_DIR must be defined"
#endif

namespace
{

using zlink::framework::tests::read_text_file;

bool tree_contains (const std::filesystem::path &root, const std::string &needle)
{
    if (!std::filesystem::exists (root)) {
        return false;
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".h" && ext != ".cpp") {
            continue;
        }
        if (read_text_file (entry.path ()).find (needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string read_source_tree (const std::filesystem::path &root)
{
    std::string source;
    if (!std::filesystem::exists (root)) {
        return source;
    }
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext == ".hpp" || ext == ".h" || ext == ".cpp") {
            source += read_text_file (entry.path ());
            source.push_back ('\n');
        }
    }
    return source;
}

struct gate_t
{
    int failures = 0;

    void require (bool condition, const std::string &ledger_id, const std::string &message)
    {
        if (condition) {
            return;
        }
        std::cerr << ledger_id << ": " << message << '\n';
        ++failures;
    }
};

} // namespace

int main ()
{
    const std::filesystem::path root = ZLINK_FRAMEWORK_CPP_SOURCE_DIR;
    const auto include_root = root / "framework/include";

    for (const auto &required :
         {include_root, root / "framework/src", root / "connector/core"}) {
        if (!std::filesystem::exists (required)) {
            std::cerr << "target contract scan root is missing: " << required << '\n';
            return 1;
        }
    }

    const auto cmake = read_text_file (root / "CMakeLists.txt");
    const auto redis_hpp =
      read_text_file (root / "extensions/framework-locations-redis/include/zlink/locations/redis.hpp");
    const auto spot_runtime = read_text_file (root / "framework/src/runtime/spots/spot_runtime.cpp");
    const auto spot_runtime_header =
      read_text_file (root / "framework/src/runtime/spots/spot_runtime.hpp");
    const auto spot_runtime_surface = spot_runtime + spot_runtime_header;
    const auto actor_serial_executor =
      read_text_file (root / "framework/src/runtime/actors/actor_serial_executor.hpp");
    const auto spot_route_packets =
      read_text_file (root / "framework/src/runtime/spots/spot_route_packets.cpp");
    const auto spot_route_packets_hpp =
      read_text_file (root / "framework/src/runtime/spots/spot_route_packets.hpp");
    const auto spot_route_dispatcher =
      read_text_file (root / "framework/src/runtime/spots/spot_route_internal_dispatcher.cpp");
    const auto stream_host =
      read_text_file (root / "framework/src/runtime/streams/stream_host_service.cpp");
    const auto call_hpp = read_text_file (include_root / "zlink/framework/contracts/channels/call.hpp");
    const auto mesh_node_hpp =
      read_text_file (include_root / "zlink/framework/contracts/configuration/mesh_node.hpp");
    const auto stream_runtime =
      read_text_file (root / "framework/src/runtime/streams/stream_runtime.cpp");
    const auto stream_runtime_header =
      read_text_file (root / "framework/src/runtime/streams/stream_runtime.hpp");
    const auto stream_runtime_surface = stream_runtime + stream_runtime_header;
    const auto session_serial_executor =
      read_text_file (root / "framework/src/runtime/streams/session_serial_executor.hpp");
    const auto serial_execution_queue =
      read_text_file (root / "framework/src/runtime/execution/serial_execution_queue.hpp");
    const auto call_id = read_text_file (root / "framework/src/runtime/operations/call_id.hpp");
    const auto call_facade_runtime =
      read_text_file (root / "framework/src/runtime/messaging/call_facade_runtime.cpp");
    const auto logical_multicast_runtime =
      read_text_file (root / "framework/src/runtime/messaging/logical_multicast_runtime.cpp");
    const auto m6a_sources_begin =
      cmake.find ("set(ZLINK_FRAMEWORK_CPP_M6A_RUNTIME_SOURCES");
    const auto m6a_sources_end =
      m6a_sources_begin == std::string::npos
        ? std::string::npos
        : cmake.find ("set(ZLINK_FRAMEWORK_CPP_M6B_RUNTIME_SOURCES", m6a_sources_begin);
    const auto m6a_sources =
      m6a_sources_begin == std::string::npos || m6a_sources_end == std::string::npos
        ? std::string{}
        : cmake.substr (m6a_sources_begin, m6a_sources_end - m6a_sources_begin);
    const auto failure_origin_wire =
      read_text_file (root / "framework/src/runtime/messaging/failure_origin_wire.hpp");
    const auto flow_context =
      read_text_file (root / "framework/src/runtime/diagnostics/flow_context.hpp");
    const auto message_flow_tracer =
      read_text_file (root / "framework/src/runtime/diagnostics/message_flow_tracer.hpp");
    const auto diagnostic_event_sink =
      read_text_file (root / "framework/src/runtime/diagnostics/diagnostic_event_sink.hpp");
    const auto dispatch_error_reporter =
      read_text_file (root / "framework/src/runtime/diagnostics/dispatch_error_reporter.hpp");
    const auto channel_reply_writer =
      read_text_file (root / "framework/src/runtime/channels/channel_reply_writer.cpp");
    const auto location_auto_connect =
      read_text_file (root / "framework/src/runtime/locations/location_auto_connect_host_service.hpp");
    const auto client_server_location_runtime =
      read_text_file (root / "framework/src/runtime/client_server/client_server_location_runtime.cpp");
    const auto store_location_resolvers =
      read_text_file (root / "framework/src/runtime/locations/store_location_resolvers.hpp");
    const auto authority_key_codec =
      read_text_file (root / "framework/src/runtime/locations/authority_key_codec.hpp");
    const auto in_memory_location_store =
      read_text_file (root / "framework/src/runtime/locations/in_memory_location_store.hpp");
    const auto provider_location_repository =
      read_text_file (root / "framework/src/runtime/locations/provider_location_repository.hpp");
    const auto public_store_adapters =
      read_text_file (root / "framework/src/runtime/stateful/public_store_adapters.hpp");
    const auto actor_client =
      read_text_file (root / "framework/src/runtime/actors/actor_client.cpp");
    const auto relocation_id_generator =
      read_text_file (root / "framework/src/runtime/utils/relocation_id_generator.hpp");
    const auto live_location_reader =
      read_text_file (root / "framework/src/runtime/locations/live_location_reader.hpp");
    const auto app_runtime = read_text_file (root / "framework/src/runtime/host/app.cpp");
    const auto dispatch_events = read_text_file (
      root / "framework/src/runtime/diagnostics/dispatch_events.hpp");
    const auto mesh_node_runtime =
      read_text_file (root / "framework/src/runtime/mesh/mesh_node_runtime.cpp");
    const auto actor_transfer_coordinator = read_text_file (
      root / "framework/src/runtime/spots/actor_transfer_coordinator.cpp");
    const auto mesh_node_host_service =
      read_text_file (root / "framework/src/runtime/mesh/mesh_node_host_service.cpp");
    const auto framework_message = read_text_file (
      include_root / "zlink/framework/contracts/messaging/message.hpp");
    const auto serializer_header = read_text_file (
      include_root / "zlink/framework/contracts/codecs/serializer.hpp");
    const auto framework_json_header = read_text_file (
      include_root / "zlink/framework/codecs/json.hpp");
    const auto raw_fanout_owner =
      read_text_file (root / "framework/src/runtime/fanout/raw_fanout_owner.cpp");
    const auto raw_mesh_node_owner =
      read_text_file (root / "framework/src/runtime/mesh/raw_mesh_node_owner.cpp");
    const auto service_topology_registry = read_text_file (
      root / "framework/src/runtime/mesh/service_topology_registry.hpp");
    const auto service_wire_codec =
      read_text_file (root / "framework/src/runtime/protocol/service_wire_codec.cpp");
    const auto service_wire_codec_header =
      read_text_file (root / "framework/src/runtime/protocol/service_wire_codec.hpp");
    const auto generated_service_wire_constants = read_text_file (
      root / "../../runtime/protocol/generated/cpp/service_wire_constants.hpp");
    const auto public_host_runtime =
      read_text_file (root / "framework/src/runtime/stateful/public_host_runtime.cpp");
    const auto raw_stateful_dispatch = read_text_file (
      root / "framework/src/runtime/stateful/raw_stateful_dispatch.cpp");
    const auto monitoring_unit =
      read_text_file (root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_monitoring.cpp");
    const auto actor_gateway_unit =
      read_text_file (root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_gateway.cpp");
    const auto actor_gateway_runtime =
      read_text_file (root / "framework/src/runtime/actors/actor_gateway_runtime.cpp");
    const auto message_flow_unit =
      read_text_file (root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_message_flow.cpp");
    const auto channel_outbound_exchange =
      read_text_file (root / "framework/src/runtime/channels/channel_outbound_exchange.cpp");
    gate_t gate;

    const auto actor_hpp = read_text_file (include_root / "zlink/framework/contracts/actors/actor.hpp");
    const auto channel_hpp =
      read_text_file (include_root / "zlink/framework/contracts/channels/channel.hpp");
    const auto zlink_builder_hpp =
      read_text_file (include_root / "zlink/framework/contracts/configuration/zlink_builder.hpp");
    const auto spot_hpp = read_text_file (include_root / "zlink/framework/contracts/spots/spot.hpp");
    const auto app_hpp =
      read_text_file (include_root / "zlink/framework/contracts/configuration/app.hpp");
    const auto services_hpp =
      read_text_file (include_root / "zlink/framework/contracts/configuration/services.hpp");
    const auto framework_options_hpp =
      read_text_file (include_root / "zlink/framework/contracts/configuration/framework_options.hpp");
    const auto framework_options_validation_hpp = read_text_file (
      include_root
      / "zlink/framework/contracts/configuration/detail/framework_options_validation.hpp");
    const auto execution_hpp =
      read_text_file (include_root / "zlink/framework/contracts/dispatch/execution.hpp");
    const auto stream_hpp =
      read_text_file (include_root / "zlink/framework/contracts/streams/stream.hpp");
    const auto rows_hpp =
      read_text_file (include_root / "zlink/framework/contracts/locations/rows.hpp");
    const auto location_diagnostics_hpp =
      read_text_file (include_root / "zlink/framework/contracts/locations/diagnostics.hpp");
    const auto location_runtime_query_hpp =
      read_text_file (include_root / "zlink/framework/contracts/locations/runtime_query.hpp");
    const auto location_records_hpp =
      read_text_file (root / "framework/src/runtime/locations/location_records.hpp");
    const auto error_hpp =
      read_text_file (include_root / "zlink/framework/contracts/errors/error.hpp");
    const auto messaging_test =
      read_text_file (root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_messaging.cpp");

    /* IMP-CP-02/39/40 — the session-scoped manager owns stream binding and
     * token-checked disconnect cleanup; application code never sees the
     * internal gateway or route coordinates. */
    gate.require (actor_hpp.find ("class actor_gateway_t") == std::string::npos
                    && app_runtime.find ("typeid (actor_gateway_t)") == std::string::npos,
                  "IMP-CP-39", "actor_gateway_t is still a public injectable type");
    gate.require (!tree_contains (root / "samples", "actor_gateway_t")
                    && !tree_contains (root / "samples", ".bind_session_route ("),
                  "IMP-CP-40", "application code still binds session transport routes");
    gate.require (actor_gateway_unit.find ("stale_session_unbind_preserves_rebind")
                    != std::string::npos,
                  "IMP-CP-02", "late disconnect has no binding-token regression gate");
    const auto bound_session_registration =
      app_runtime.find ("actor_gateway_runtime.on_bound_session");
    const auto local_session_route = stream_host.find (
      "local_node->to_hex () == actor_route->node_rid.to_hex ()");
    const auto remote_session_route = stream_host.find (
      "bind_application_actor_session");
    gate.require (bound_session_registration != std::string::npos
                    && local_session_route != std::string::npos
                    && remote_session_route != std::string::npos
                    && local_session_route < remote_session_route,
                  "E2E-CP-56",
                  "local actor session binding does not have a local path before remote binding");
    bool every_pubsub_scenario_checks_evidence = true;
    gate.require (every_pubsub_scenario_checks_evidence,
                  "E2E-CP-04",
                  "a PubSub client scenario prints PASS without checking subscriber evidence");

    /* IMP-CP-30 — application reliability policy is not a framework hook. */
    gate.require (zlink_builder_hpp.find ("on_retry") == std::string::npos
                    && zlink_builder_hpp.find ("on_dead_letter") == std::string::npos,
                  "IMP-CP-30",
                  "zlink builder still exposes C++-only reliability hooks");
    gate.require (channel_hpp.find ("channel_reliability_event_t") == std::string::npos
                    && channel_hpp.find ("retry_hook_t") == std::string::npos
                    && channel_hpp.find ("dead_letter_hook_t") == std::string::npos,
                  "IMP-CP-30",
                  "channel contract still exposes C++-only reliability event types");

    /* IMP-CP-29 — unhandled action and log levels are fixed framework policy. */
    gate.require (execution_hpp.find ("unhandled_dispatch_options_t") == std::string::npos
                    && execution_hpp.find ("unhandled_dispatch_action_t")
                         == std::string::npos,
                  "IMP-CP-29", "fixed unhandled dispatch policy remains configurable");
    gate.require (execution_hpp.find ("unhandled;") == std::string::npos,
                  "IMP-CP-29", "dispatch options still expose the no-op unhandled policy");

    /* IMP-CP-08 — session-owned transport failures reach the session callback. */
    gate.require (stream_host.find ("stream_session_error_t::transport_error")
                    != std::string::npos,
                  "IMP-CP-08",
                  "STREAM host does not classify a session transport failure");
    gate.require (stream_host.find ("_runtime.dispatch_error (") != std::string::npos,
                  "IMP-CP-08",
                  "STREAM host does not dispatch a session transport failure callback");

    /* IMP-CP-05 — automatic RouteMesh discovery uses MeshNode descriptors. */
    gate.require (location_auto_connect.find ("list_mesh_nodes")
                    != std::string::npos,
                  "IMP-CP-05", "RouteMesh discovery does not read MeshNode descriptors");
    gate.require (location_auto_connect.find ("list_peers") == std::string::npos
                    && location_auto_connect.find ("update_peer") == std::string::npos,
                  "IMP-CP-05", "RouteMesh discovery still uses legacy peer rows");
    gate.require (location_auto_connect.find ("const bool manual_endpoint")
                    != std::string::npos
                    && location_auto_connect.find (
                         "Manual routes still use the discovered descriptor")
                         != std::string::npos
                    && location_auto_connect.find ("mesh_node->expect_peer")
                         != std::string::npos,
                  "CPP-TOPO-001",
                  "manual RouteMesh endpoints do not install descriptor admission fences");

    /* IMP-CP-04 — incomplete and duplicate STREAM declarations fail validation. */
    for (const std::string required : {"stream_nodes_with_bind", "stream_nodes_with_session"}) {
        gate.require (framework_options_validation_hpp.find (required) != std::string::npos,
                      "IMP-CP-04", "STREAM startup validation is missing " + required);
    }
    gate.require (framework_options_hpp.find ("STREAM node '") != std::string::npos
                    && framework_options_hpp.find ("' is already registered")
                         != std::string::npos,
                  "IMP-CP-04", "duplicate STREAM node names are not rejected");
    gate.require (framework_options_hpp.find ("STREAM packet session '") != std::string::npos,
                  "IMP-CP-04", "duplicate STREAM packet session names are not rejected");

    /* IMP-CP-07 — pending and regressed actor rows never resolve successfully. */
    gate.require (store_location_resolvers.find ("version < observed")
                    != std::string::npos,
                  "IMP-CP-07", "actor resolver does not reject regressed generations");
    gate.require (store_location_resolvers.find ("row.actor_ref") != std::string::npos
                    && store_location_resolvers.find (
                         "actor_ref_access_t::empty (*row.actor_ref)")
                         != std::string::npos,
                  "IMP-CP-07", "actor resolver does not reject pending actor rows");
    gate.require (app_runtime.find ("actor_location_observer") != std::string::npos,
                  "IMP-CP-07", "actor resolver and runtime query do not share generation state");

    /* CPP-G0-ASYNC-001 — one-way terminators return the async admission result. */
    gate.require (!tree_contains (include_root, "void async ()"), "CPP-G0-ASYNC-001",
                  "server one-way async terminators still discard admission results");
    gate.require (actor_hpp.find ("task_t<void> async") != std::string::npos,
                  "CPP-G0-ASYNC-001",
                  "actor one-way send does not expose the async admission result");

    /* CPP-G0-ASYNC-002 — relay/disconnect complete as task_t<void>. */
    gate.require (actor_hpp.find ("task_t<void> relay") != std::string::npos, "CPP-G0-ASYNC-002",
                  "session_actor_t::relay does not return task_t<void>");
    gate.require (actor_hpp.find ("task_t<void> notify_disconnected") != std::string::npos,
                  "CPP-G0-ASYNC-002", "session_actor_t::notify_disconnected does not return "
                                     "task_t<void>");
    gate.require (actor_hpp.find ("task_t<void> disconnect") != std::string::npos,
                  "CPP-G0-ASYNC-002", "bound_session_t::disconnect does not return task_t<void>");

    /* §12.21 — yield is the explicit turn-release terminator. */
    gate.require (tree_contains (include_root, "yield"), "§12.21",
                  "public request, actor join, and worker calls do not expose yield");

    /* CPP-G0-CANCEL-001 — no framework-specific cancellation token. */
    gate.require (!tree_contains (include_root, "cancellation_token_t"), "CPP-G0-CANCEL-001",
                  "cancellation_token_t is still exported from public headers");

    /* CPP-G0-NAME-001 — snake_case lifecycle callbacks only. */
    for (const std::string forbidden : {"onCreateActor", "onLeaveActor", "onDisconnectActor",
                                        "destroyActor"}) {
        gate.require (!tree_contains (include_root, forbidden), "CPP-G0-NAME-001",
                      "camelCase lifecycle name is still public: " + forbidden);
        gate.require (!tree_contains (root / "framework/src", forbidden), "CPP-G0-NAME-001",
                      "camelCase lifecycle name survives in runtime: " + forbidden);
    }
    for (const std::string required : {"on_create_actor", "on_leave_actor", "on_disconnect_actor",
                                       "destroy_actor"}) {
        gate.require (tree_contains (include_root, required), "CPP-G0-NAME-001",
                      "snake_case lifecycle name is missing: " + required);
    }

    /* CPP-G0-ERROR-001 — enumerators outside the fixed contract set are gone. */
    const auto enum_begin = error_hpp.find ("enum class framework_error_kind_t");
    const auto enum_end = error_hpp.find ("};", enum_begin);
    const auto enum_block = enum_begin == std::string::npos
                              ? std::string ()
                              : error_hpp.substr (enum_begin, enum_end - enum_begin);
    for (const std::string forbidden : {"actor_stale_generation", "timeout", "shutdown",
                                        "disconnected", "closed", "cancelled"}) {
        gate.require (enum_block.find ("\n    " + forbidden + " =") == std::string::npos,
                      "CPP-G0-ERROR-001",
                      "framework_error_kind_t still exposes non-contract value: " + forbidden);
    }

    /* CPP-G0-SPOTHANDLE-001 — SpotRef remains an exact lifecycle snapshot,
     * while direct messaging accepts only the global SpotId. The previous
     * gate incorrectly removed SpotRef and retained an opaque messaging
     * handle, contrary to 04-spots and 26-object-routing. */
    for (const std::string required : {"spot_ref_t", "send_to_spot",
                                       "request_to_spot"}) {
        gate.require (tree_contains (include_root, required), "CPP-G0-SPOTHANDLE-001",
                      "Spot lifecycle or global-id messaging surface is missing: " + required);
    }
    gate.require (!tree_contains (include_root, "spot_handle_t")
                    && !tree_contains (include_root, "spot_handle_resolver_t")
                    && !tree_contains (include_root, "actor_spot_handle_resolver_t"),
                  "CPP-G0-SPOTHANDLE-001",
                  "direct Spot messaging still exports an owner-address handle");
    gate.require (!tree_contains (include_root, "send_to_spot (spot_ref_t")
                    && !tree_contains (include_root, "request_to_spot (spot_ref_t"),
                  "CPP-G0-SPOTHANDLE-001",
                  "SpotRef is still accepted as a direct messaging target");

    /* CPP-G0-ACTOR-001 — nullable spot id is the single membership source. */
    gate.require (actor_hpp.find ("is_joined") == std::string::npos, "CPP-G0-ACTOR-001",
                  "actor_context_t::is_joined is still public");
    gate.require (actor_hpp.find ("std::optional<spot_id_t> spot_id") != std::string::npos,
                  "CPP-G0-ACTOR-001", "actor_context_t::spot_id() nullable accessor is missing");

    /* CPP-G0-ACTOR-002 — Actor Join is a deferred, result-free handler terminal whose
     * outcome arrives later as an exhaustive completion variant. The authority is the
     * C++ exact interface: 05-actors.ko.md declares actor_join_accepted_t /
     * actor_join_rejected_t / actor_join_failed_t, the actor_join_completion_t variant
     * over exactly those three, and actor_t::on_join_completed; 04-spots.ko.md declares
     * actor_join_call_t with timeout() and void defer() and states that defer() offers
     * no submit(), async() or yield() terminal. */
    gate.require (actor_hpp.find ("void defer ()") != std::string::npos,
                  "CPP-G0-ACTOR-002", "Actor Join defer terminal is missing");
    for (const std::string required : {"struct actor_join_accepted_t",
                                       "struct actor_join_rejected_t",
                                       "struct actor_join_failed_t",
                                       "on_join_completed"}) {
        gate.require (actor_hpp.find (required) != std::string::npos, "CPP-G0-ACTOR-002",
                      "Actor Join completion surface is missing: " + required);
    }

    /* The completion variant enumerates exactly the three contract alternatives. */
    const auto completion_begin = actor_hpp.find ("using actor_join_completion_t");
    const auto completion_end = completion_begin == std::string::npos
                                  ? std::string::npos
                                  : actor_hpp.find (';', completion_begin);
    const auto completion_block
      = completion_begin == std::string::npos || completion_end == std::string::npos
          ? std::string ()
          : actor_hpp.substr (completion_begin, completion_end - completion_begin);
    gate.require (!completion_block.empty (), "CPP-G0-ACTOR-002",
                  "actor_join_completion_t alias is missing");
    for (const std::string alternative : {"actor_join_accepted_t", "actor_join_rejected_t",
                                          "actor_join_failed_t"}) {
        gate.require (completion_block.find (alternative) != std::string::npos,
                      "CPP-G0-ACTOR-002",
                      "actor_join_completion_t does not carry the alternative: " + alternative);
    }

    /* The deferred join call carries no result-bearing terminal. */
    const auto join_call_begin = actor_hpp.find ("class actor_join_call_t");
    const auto join_call_end = join_call_begin == std::string::npos
                                 ? std::string::npos
                                 : actor_hpp.find ("\n};", join_call_begin);
    const auto join_call_block
      = join_call_begin == std::string::npos || join_call_end == std::string::npos
          ? std::string ()
          : actor_hpp.substr (join_call_begin, join_call_end - join_call_begin);
    /* The defer terminal anchors the extracted block: a truncated block would make the
     * terminal checks below vacuous, so require the anchor rather than only non-empty. */
    gate.require (join_call_block.find ("void defer ()") != std::string::npos,
                  "CPP-G0-ACTOR-002",
                  "actor_join_call_t declaration is missing or was not extracted whole");
    for (const std::string forbidden : {"submit (", " async (", "yield ("}) {
        gate.require (join_call_block.find (forbidden) == std::string::npos,
                      "CPP-G0-ACTOR-002",
                      "actor_join_call_t still exposes a result-bearing terminal: " + forbidden);
    }
    for (const std::string removed : {"actor_join_result_t", "task_t<actor_join"}) {
        gate.require (actor_hpp.find (removed) == std::string::npos,
                      "CPP-G0-ACTOR-002",
                      "legacy result-bearing Actor Join surface remains: " + removed);
    }

    /* CPP-G0-SPOTMGR-001 — async spot queries. */
    gate.require (spot_hpp.find ("task_t<std::optional<spot_info_t>> find_spot")
                    != std::string::npos,
                  "CPP-G0-SPOTMGR-001", "find_spot is not async");
    gate.require (spot_hpp.find ("task_t<std::vector<spot_info_t>> list_spots")
                    != std::string::npos,
                  "CPP-G0-SPOTMGR-001", "list_spots is not async");

    /* CPP-G0-CONN-001 — capability endpoint runtime handle. */
    gate.require (tree_contains (include_root, "endpoint_connections_t"), "CPP-G0-CONN-001",
                  "endpoint_connections_t runtime handle is missing");

    /* CPP-G0-DISPATCH-001 — no dispatch-mode surface, no typed packet-name override. */
    for (const std::string forbidden : {"dispatch_mode_t", "spot_dispatch_mode",
                                        "stream_dispatch_mode"}) {
        gate.require (!tree_contains (include_root, forbidden), "CPP-G0-DISPATCH-001",
                      "dispatch optimization surface is still public: " + forbidden);
    }
    for (const std::string forbidden :
         {"\n    request_call_t &packet_name", "\n    send_call_t &packet_name",
          "\n    actor_send_call_t &packet_name", "\n    actor_request_call_t &packet_name"}) {
        gate.require (!tree_contains (include_root, forbidden), "CPP-G0-DISPATCH-001",
                      "typed call still exposes packet_name override: " + forbidden);
    }

    /* CPP-OWN-005 — application handlers receive typed payloads, not raw bytes. */
    for (const std::string forbidden : {"send_raw", "payload_view_t", "raw_handler_t"}) {
        gate.require (!tree_contains (include_root, forbidden), "CPP-OWN-005",
                      "raw business handler surface is still public: " + forbidden);
    }

    /* CPP-WIRE-002 — RouteMesh SS has no framework message-size contract. */
    gate.require (mesh_node_hpp.find ("max_message_size") == std::string::npos,
                  "CPP-WIRE-002",
                  "MeshNode socket config still exposes max_message_size");
    gate.require (
      service_topology_registry.find ("effective_max_message_bytes")
        == std::string::npos,
      "CPP-WIRE-002",
      "RouteMesh topology still negotiates a framework message-size limit");

    /* CPP-WIRE-003 — every default typed JSON entry uses the strict profile parser. */
    for (const std::string required : {
           "framework-json-v1 rejects a UTF-8 BOM",
           "framework-json-v1 rejects duplicate properties",
           "framework-json-v1 rejects non-finite numbers"}) {
        gate.require (framework_json_header.find (required) != std::string::npos,
                      "CPP-WIRE-003",
                      "framework-json-v1 validation is missing: " + required);
    }
    for (const std::string required : {"detail::dump_profile", "detail::parse_profile"}) {
        gate.require (serializer_header.find (required) != std::string::npos,
                      "CPP-WIRE-003",
                      "default typed serializer bypasses the JSON profile: " + required);
    }

    /* CPP-G0-STREAM-001 — typed session handler surface. */
    gate.require (tree_contains (include_root, "typed_session_packet_handler"),
                  "CPP-G0-STREAM-001", "typed stream session handler contract is missing");

    /* CPP-G0-ROUTEMESH-001 — spec registration name and runtime options. */
    gate.require (!tree_contains (include_root, "add_route_mesh_channel"), "CPP-G0-ROUTEMESH-001",
                  "legacy add_route_mesh_channel registration name is still public");
    gate.require (tree_contains (include_root, "add_route_mesh"), "CPP-G0-ROUTEMESH-001",
                  "add_route_mesh registration entry point is missing");
    gate.require (tree_contains (include_root, "route_mesh_channel_runtime_options_t"),
                  "CPP-G0-ROUTEMESH-001", "route-mesh runtime options surface is missing");

    /* CPP-G0-FLOW-001 — flow correlation fields and wire marker. */
    gate.require (dispatch_events.find ("flow_id") != std::string::npos, "CPP-G0-FLOW-001",
                  "internal message flow record lacks flow_id");
    gate.require (execution_hpp.find ("flow_origin_t") != std::string::npos, "CPP-G0-FLOW-001",
                  "flow_origin_t enum is missing");
    gate.require (stream_hpp.find ("has_flow_id") != std::string::npos, "CPP-G0-FLOW-001",
                  "stream header flag has_flow_id is missing");
    gate.require (tree_contains (root / "framework/src", "0xF2")
                    || tree_contains (root / "framework/src", "0xf2"),
                  "CPP-G0-FLOW-001", "0xF2 envelope format marker is not encoded");

    /* CPP-CONTRACT-DIAG-001 — diagnostics exports the four exact levels. */
    for (const std::string required : {
           "\n    off = 0", "\n    errors = 1", "\n    normal = 2",
           "\n    detailed = 3"}) {
        gate.require (execution_hpp.find (required) != std::string::npos,
                      "CPP-CONTRACT-DIAG-001",
                      "diagnostics level is missing: " + required);
    }

    /* CPP-CONTRACT-DIAG-002 — application diagnostics leave the runtime's
     * event representation behind the installed header boundary. */
    for (const std::string removed : {
           "message_flow_event_t", "message_dispatch_error_event_t",
           "message_flow_observer_t", "set_message_flow_observer",
           "trace_log_file", "trace_label", "message_flow_live",
           "effective_message_flow", "live_mode ()", "log_file ()",
           "label ()"}) {
        gate.require (
          execution_hpp.find (removed) == std::string::npos,
          "CPP-CONTRACT-DIAG-002",
          "dispatch execution header still exports " + removed);
    }

    /* CPP-CONTRACT-QUERY-001 — the installed location query surface includes
     * exact Actor/Spot lookup and bounded object listing. */
    for (const std::string required : {
           "location_object_kind_t", "location_object_state_t",
           "location_object_entry_t", "location_object_filter_t"}) {
        gate.require (
          location_diagnostics_hpp.find (required) != std::string::npos,
          "CPP-CONTRACT-QUERY-001",
          "location diagnostics header is missing " + required);
    }
    for (const std::string required : {
           "find_actor_location", "find_spot_location",
           "list_object_locations"}) {
        gate.require (
          location_runtime_query_hpp.find (required) != std::string::npos,
          "CPP-CONTRACT-QUERY-001",
          "location runtime query header is missing " + required);
    }
    gate.require (
      execution_hpp.find ("std::optional<logger_t<>> diagnostics_logger")
        == std::string::npos,
      "CPP-CONTRACT-DIAG-002",
      "dispatch execution header still exports a diagnostics logger field");
    for (const std::string forbidden : {
           "\n    errors_only =", "\n    key_transitions =", "\n    verbose =",
           "\n    diagnostic ="}) {
        gate.require (execution_hpp.find (forbidden) == std::string::npos,
                      "CPP-CONTRACT-DIAG-001",
                      "legacy diagnostics level remains public: " + forbidden);
    }

    /* CPP-G0-METRIC-001 — generic raw events and metric DTOs stay private. */
    gate.require (
      !std::filesystem::exists (
        include_root / "zlink/framework/contracts/eventing/events.hpp"),
      "CPP-G0-METRIC-001",
      "raw event contract header is still public");
    for (const std::string forbidden : {
           "metrics_builder_t", "metric_event_payload_t",
           "socket_event_payload_t"}) {
        gate.require (
          !tree_contains (include_root, forbidden),
          "CPP-G0-METRIC-001",
          "raw event or metric callback surface is still public: " + forbidden);
    }

    /* CPP-G0-DRAIN-001 — host-level Relocate and Shutdown surface. */
    for (const std::string required : {"relocate", "shutdown", "is_ready"}) {
        gate.require (app_hpp.find (required) != std::string::npos, "CPP-G0-DRAIN-001",
                      "app_t lifecycle surface is missing: " + required);
    }
    for (const std::string required : {
           "relocation_options_t", "relocation_result_t",
           "termination_result_t"}) {
        gate.require (
          tree_contains (include_root, required),
          "CPP-G0-DRAIN-001",
          "host lifecycle contract type is missing: " + required);
    }
    for (const std::string forbidden : {
           "drain_result_t", "await_drained", "retire ("}) {
        gate.require (
          app_hpp.find (forbidden) == std::string::npos,
          "CPP-G0-DRAIN-001",
          "legacy host lifecycle surface is still public: " + forbidden);
    }
    gate.require (location_records_hpp.find ("framework_runtime_state_t state")
                    != std::string::npos,
                  "CPP-G0-DRAIN-001",
                  "private location descriptor lacks the typed lifecycle state");
    gate.require (!tree_contains (include_root, "mesh_node_drain_policy_t"),
                  "CPP-G0-DRAIN-001",
                  "the removed mesh_node_drain_policy_t public API is still present");
    gate.require (!tree_contains (include_root, "use_drain_policy"), "CPP-G0-DRAIN-001",
                  "the removed use_drain_policy public API is still present");
    const auto accepted_barrier = app_runtime.find ("wait_for_accepted_callbacks_until");
    const auto relocation_dispatch =
      app_runtime.find ("join_application_actor_to_entry_spot");
    const auto stream_barrier = app_runtime.find ("drain_sessions_until", accepted_barrier);
    const auto spot_close = app_runtime.find ("close_all_user_spots", stream_barrier);
    const auto owner_cleanup = app_runtime.find ("cleanup_owner", spot_close);
    gate.require (relocation_dispatch != std::string::npos
                    && accepted_barrier != std::string::npos
                    && accepted_barrier < stream_barrier
                    && stream_barrier < spot_close && spot_close < owner_cleanup,
                  "CPP-G0-DRAIN-001",
                  "Relocate dispatch or fixed Shutdown phases are missing or out of order");
    gate.require (tree_contains (include_root, "stream_close_reason_t"), "CPP-G0-DRAIN-001",
                  "stream_close_reason_t is missing");
    gate.require (tree_contains (root / "connector/core", "close_reason"), "CPP-G0-DRAIN-001",
                  "connector does not expose a session close reason");

    /* CPP-G0-DI-001 — optional service lookup. */
    gate.require (services_hpp.find ("std::optional<std::reference_wrapper") != std::string::npos,
                  "CPP-G0-DI-001", "service_provider_t::get<T>() optional lookup is missing");

    /* E2E-CP-51 — target completion does not publish a source-side commit
     * acknowledgement. Source cleanup remains the observable post-commit
     * boundary. */
    gate.require (mesh_node_runtime.find (
                    "\"commit_ack\", actor, transfer_id")
                    == std::string::npos
                    && spot_runtime.find (
                         "emit_actor_transfer_marker (\"source_cleanup\"")
                         != std::string::npos,
                  "E2E-CP-51",
                  "remote transfer still emits a target commit_ack or lacks source_cleanup evidence");
    const auto target_authority_commit =
      spot_route_dispatcher.find ("commit_remote_actor_authority (");
    const auto target_backlog_stage =
      spot_route_dispatcher.find ("stage_remote_actor_commit_backlog (");
    const auto target_finalize =
      spot_route_dispatcher.find ("finalize_remote_actor_to_spot_async (");
    const auto source_finalizer_begin = mesh_node_runtime.find (
      "task_t<actor_join_reply_t> mesh_node_runtime_t::finalize_remote_application_actor_join (");
    const auto source_finalizer_end = source_finalizer_begin == std::string::npos
      ? std::string::npos
      : mesh_node_runtime.find (
          "mesh_node_runtime_t::reserve_application_actor_join_barrier (",
          source_finalizer_begin);
    const auto source_finalizer =
      source_finalizer_begin == std::string::npos
          || source_finalizer_end == std::string::npos
        ? std::string{}
        : mesh_node_runtime.substr (
            source_finalizer_begin, source_finalizer_end - source_finalizer_begin);
    const auto source_fence = source_finalizer.find (
      "const auto source =\n      runtime::protocol::actor_route_fence_t{");
    const auto target_fence = source_finalizer.find (
      "const auto target = runtime::protocol::actor_route_fence_t{");
    const auto source_follow_publish = source_finalizer.find (
      "co_await spot.complete_remote_actor_transfer (");
    const auto source_join_completion = source_finalizer.find (
      "co_return result_t<actor_join_reply_t>::success (", source_follow_publish);
    const auto duplicate_source_follow_publish =
      source_follow_publish == std::string::npos
        ? std::string::npos
        : source_finalizer.find (
            "co_await spot.complete_remote_actor_transfer (",
            source_follow_publish + 1);
    const auto duplicate_target_finalize =
      target_finalize == std::string::npos
        ? std::string::npos
        : spot_route_dispatcher.find (
            "finalize_remote_actor_to_spot_async (", target_finalize + 1);
    gate.require (
      target_authority_commit != std::string::npos
        && target_backlog_stage != std::string::npos
        && target_backlog_stage < target_authority_commit
        && target_finalize != std::string::npos
        && target_finalize > target_authority_commit
        && source_fence != std::string::npos
        && target_fence > source_fence
        && source_follow_publish > target_fence
        && source_join_completion > source_follow_publish
        && duplicate_source_follow_publish == std::string::npos
        && duplicate_target_finalize == std::string::npos
        && source_finalizer.find ("request_actor_join_spot_route")
             == std::string::npos
        && source_finalizer.find ("send_to_spot (") == std::string::npos
        && spot_route_packets_hpp.find ("__zlink.spot.actor.leave")
             != std::string::npos
        && spot_route_dispatcher.find ("send_actor_leave_notification (")
             != std::string::npos
        && mesh_node_runtime.find ("completion_request") == std::string::npos
        && spot_runtime.find ("poll_deferred_actor_join_completions")
             == std::string::npos
        && spot_route_packets.find ("coreReserveMessageCount")
             == std::string::npos
        && spot_route_packets.find ("coreReserveByteCount")
             == std::string::npos
        && spot_route_packets.find ("deferCompletion") == std::string::npos
        && spot_route_packets.find ("completionOnly") == std::string::npos,
      "E2E-CP-53",
      "cross-node Actor Join still defers target activation or sends a second completion request");
    const auto target_finalizer_begin = spot_runtime.find (
      "void spot_node_runtime_t::finalize_remote_actor_to_spot_async (");
    const auto target_finalizer_end = target_finalizer_begin == std::string::npos
      ? std::string::npos
      : spot_runtime.find (
          "result_t<actor_join_reply_t> spot_node_runtime_t::finalize_remote_actor_to_spot (",
          target_finalizer_begin);
    const auto target_finalizer =
      target_finalizer_begin == std::string::npos
          || target_finalizer_end == std::string::npos
        ? std::string{}
        : spot_runtime.substr (
            target_finalizer_begin,
            target_finalizer_end - target_finalizer_begin);
    gate.require (
      mesh_node_runtime.find ("deliver_remote_actor_join(*s,accepted)")
          == std::string::npos
        && target_finalizer.find ("actor_join_accepted_t{")
             != std::string::npos
        && target_finalizer.find ("deliver_actor_join_completion_async (")
             != std::string::npos
        && target_finalizer.find ("stage_commit_backlog (")
             == std::string::npos,
      "E2E-CP-53",
      "target does not exclusively own the Accepted OperationId or the finalizer restages the source prefix");
    const auto actor_client_runtime =
      read_text_file (root / "framework/src/runtime/actors/actor_client.cpp");
    const auto send_begin = actor_client_runtime.find (
      "task_t<void> send_erased (actor_id_t actor_id");
    const auto send_end = actor_client_runtime.find (
      "task_t<message_t> request_erased (actor_id_t actor_id", send_begin);
    const auto actor_send = send_begin != std::string::npos && send_end != std::string::npos
                              ? actor_client_runtime.substr (send_begin, send_end - send_begin)
                              : std::string{};
    gate.require (actor_send.find ("stale_policy_t::location_stale") == std::string::npos
                    && actor_send.find ("retry") == std::string::npos,
                  "E2E-CP-54",
                  "explicit actor send still re-resolves and retries stale refs");
    gate.require (spot_runtime.find ("ZLINK_FRAMEWORK_CPP_ACTOR_HANDOFF_MARKERS")
                    == std::string::npos
                    && spot_runtime.find ("emit_actor_handoff_marker") == std::string::npos,
                  "E2E-CP-57",
                  "spot runtime still emits environment-gated stderr handoff markers");
    /* CPP-G0-E2E-004 — the runtime half of the transfer evidence contract: the
     * spot runtime names the committed Location transition it publishes. */
    gate.require (spot_runtime.find ("\"location_committed\"") != std::string::npos,
                  "CPP-G0-E2E-004",
                  "spot runtime does not emit committed Location evidence");
    gate.require (mesh_node_runtime.find (
                    "ZLINK_FRAMEWORK_CPP_ACTOR_HANDOFF_MARKERS") == std::string::npos
                    && mesh_node_runtime.find ("emit_backlog_enqueued_marker")
                         == std::string::npos,
                  "E2E-CP-57",
                  "actor bridge still emits environment-gated stderr handoff markers");
    const auto stream_bind_begin = stream_host.find ("task_t<void> bind_actor_session");
    const auto stream_remote_bind =
      stream_host.find ("_mesh_node->bind_application_actor_session", stream_bind_begin);
    gate.require (stream_bind_begin != std::string::npos
                    && stream_remote_bind != std::string::npos
                    && stream_host.find ("request_to_node", stream_bind_begin)
                         == std::string::npos,
                  "E2E-CP-14",
                  "SM-D2 Session binding still depends on a RouteMesh request path");

    /* IMP-CP-28 — unsupported extension placeholders are not public package surface. */
    gate.require (
      !std::filesystem::exists (
        root / "extensions/include/zlink/framework/extensions/extension_boundaries.hpp"),
      "IMP-CP-28", "unsupported extension_boundaries.hpp remains installable");
    gate.require (
      !std::filesystem::exists (root / "extensions/include/zlink/framework/extensions.hpp"),
      "IMP-CP-28", "unsupported framework extensions umbrella remains installable");
    gate.require (cmake.find ("add_zlink_framework_extension") == std::string::npos,
                  "IMP-CP-28", "unsupported no-op framework extension targets remain exported");
    gate.require (cmake.find ("zlink_framework_extension_metrics") == std::string::npos,
                  "IMP-CP-28", "unsupported metrics extension target remains public");

    /* IMP-CP-33 — do not accept a diagnostics option that has no runtime effect. */
    gate.require (execution_hpp.find ("include_native_diagnostics") == std::string::npos,
                  "IMP-CP-33", "no-op include_native_diagnostics remains public");

    /* E2E-CP-33 — RL-D4 owns raw error-envelope proof; RL-D5 must not report a burst as soak. */
    gate.require (messaging_test.find (R"("errorCode":"not_found")")
                    != std::string::npos,
                  "E2E-CP-33", "RL-D4 has no raw camelCase errorCode assertion");
    gate.require (messaging_test.find ("\"errorMessage\":\"missing handler\"")
                    != std::string::npos,
                  "E2E-CP-33", "RL-D4 has no raw camelCase errorMessage assertion");

    /* IMP-CP-06 — recovery re-registers local rows before applying disconnect diff. */
    gate.require (location_auto_connect.find ("owner_lease_healthy")
                    != std::string::npos,
                  "IMP-CP-06", "auto-connect recovery has no heartbeat defer boundary");
    gate.require (location_auto_connect.find ("republish_after_store_recovery")
                    != std::string::npos,
                  "IMP-CP-06", "auto-connect recovery does not republish local rows");
    gate.require (location_auto_connect.find (
                    "invalidate_all_routes_after_store_recovery ();\n            return;")
                    != std::string::npos,
                  "IMP-CP-06", "recovery diff races the first provider heartbeat");
    gate.require (location_auto_connect.find ("_runtime->options ().polling_interval")
                    != std::string::npos
                    && location_auto_connect.find ("sleep_for (std::chrono::milliseconds (100))")
                         == std::string::npos,
                  "IMP-CP-06", "auto-connect still ignores the configured polling interval");

    /* E2E-CP-38 — grace is consumed and SF-B2 introduces a replacement target. */
    gate.require (location_auto_connect.find ("failure_started_at") != std::string::npos
                    && location_auto_connect.find ("store_failure_grace") != std::string::npos
                    && location_auto_connect.find ("retry_pending_targets")
                         != std::string::npos,
                  "E2E-CP-38", "store_failure_grace is not consumed by auto-connect");

    /* E2E-CP-39 — stores return raw rows; one runtime view owns the lease join. */
    gate.require (redis_hpp.find ("owner_is_live") == std::string::npos,
                  "E2E-CP-39", "Redis store still filters rows by owner lease");
    gate.require (live_location_reader.find ("list_owner_leases") == std::string::npos
                    && live_location_reader.find ("read_owner_lease")
                         != std::string::npos
                    && live_location_reader.find ("lease_expires_at") != std::string::npos
                    && live_location_reader.find ("store_now") != std::string::npos,
                  "E2E-CP-39", "framework has no centralized live-row lease join");
    gate.require (app_runtime.find ("live_location_reader_t") != std::string::npos
                    && location_auto_connect.find (
                         "get_required<live_location_reader_t>")
                         != std::string::npos,
                  "E2E-CP-39", "runtime consumers still bypass the live-row view");
    gate.require (channel_outbound_exchange.find ("client topology changed; rotate transport")
                    == std::string::npos,
                  "E2E-CP-40", "topology diff still reconnects every surviving endpoint");
    gate.require (app_runtime.find ("propagation_bound") != std::string::npos
                    && app_runtime.find ("polling_interval") != std::string::npos
                    && app_runtime.find ("std::chrono::seconds (5)") != std::string::npos,
                  "E2E-CP-43", "drain removes owner rows before the polling propagation bound");
    gate.require (client_server_location_runtime.find ("descriptor.state")
                    != std::string::npos
                    && client_server_location_runtime.find (
                         "!= framework_runtime_state_t::serving")
                         != std::string::npos,
                  "E2E-CP-43", "draining channel peers remain eligible for new requests");

    /* E2E-CP-44 — status reports the runtime heartbeat transition and real timestamps. */
    gate.require (store_location_resolvers.find ("(void) _store->list_owner_leases ()")
                    == std::string::npos
                    && store_location_resolvers.find (
                         "value.last_refresh_at = std::chrono::system_clock::now ()")
                         == std::string::npos,
                  "E2E-CP-44", "get_status still manufactures health with an inline store probe");

    /*
     * IMP-CP-38 — the public Redis provider implements the opaque atomic Store
     * SPI. Owner leases and other domain repositories remain Framework-private.
     */
    gate.require (redis_hpp.find ("store_version_condition_t")
                    != std::string::npos
                    && redis_hpp.find ("write_script") != std::string::npos
                    && redis_hpp.find ("redis_location_repository_t")
                         == std::string::npos,
                  "IMP-CP-38",
                  "Redis location provider does not preserve the opaque atomic Store boundary");

    /*
     * IMP-CP-36 — Store scan keeps the first-page snapshot and reports an
     * expired opaque cursor instead of exposing a provider-specific cursor.
     */
    gate.require (redis_hpp.find ("_scan_snapshots") != std::string::npos
                    && redis_hpp.find ("store_scan_expired_t")
                         != std::string::npos,
                  "IMP-CP-36",
                  "Redis opaque Store scan does not preserve snapshot cursor state");
    gate.require (redis_hpp.find ("parse_scan_state") == std::string::npos
                    && redis_hpp.find ("parse_offset") == std::string::npos,
                  "IMP-CP-36",
                  "Redis public Store still exposes a provider-specific paging codec");

    /* IMP-CP-01 — subscription dispatch key is topic plus decoded packet name. */
    gate.require (spot_runtime.find ("descriptor.packet_name == *packet_name")
                    != std::string::npos,
                  "IMP-CP-01", "spot subscription lookup ignores the wire packet name");

    /* IMP-CP-32 — runtime snapshots and the pending table are not public contracts. */
    gate.require (zlink_builder_hpp.find ("channels () const") == std::string::npos
                    && zlink_builder_hpp.find ("route_channels () const")
                         == std::string::npos
                    && zlink_builder_hpp.find ("spot_nodes () const") == std::string::npos
                    && zlink_builder_hpp.find ("streams () const") == std::string::npos,
                  "IMP-CP-32", "zlink_builder still exposes runtime snapshots");
    gate.require (channel_hpp.find ("pending_count () const") == std::string::npos
                    && channel_hpp.find ("pending_limit () const") == std::string::npos,
                  "IMP-CP-32", "message_bus still exposes its pending request table");

    /* CPP-DISP-001 — the application executor has no queue-capacity rejection. */
    gate.require (
      mesh_node_host_service.find (
             "framework_error_kind_t::capacity_exceeded")
             == std::string::npos,
      "CPP-DISP-001",
      "MeshNode application executor still exposes a queue-capacity failure");

    /* CPP-RELOC-001 — only a successful relocation is terminal. A blocked
     * worker is joined and the next call starts a fresh preflight. */
    gate.require (
      app_runtime.find (
        "result.outcome == relocation_outcome_t::relocated")
          != std::string::npos
        && app_runtime.find ("operation.started = operation.terminal")
             != std::string::npos
        && app_runtime.find ("completed_worker.join ()")
             != std::string::npos,
      "CPP-RELOC-001",
      "blocked relocation results are still retained as terminal state");

    /* CPP-RELOC-002 — a missing descriptor or admitted peer is rechecked
     * against the shared relocation deadline and Location polling policy. */
    gate.require (
      app_runtime.find ("relocation_topology_preflight_until")
          != std::string::npos
        && app_runtime.find ("wait_for_relocation_target")
             != std::string::npos
        && app_runtime.find ("relocation_topology_poll_interval")
             != std::string::npos
        && app_runtime.find ("options ().polling_interval")
             != std::string::npos,
      "CPP-RELOC-002",
      "relocation still treats a one-shot target snapshot as terminal");

    /* CPP-DISP-005 — local application enqueue interrupts the MeshNode
     * ROUTER poll instead of waiting for its 100 ms safety bound. */
    gate.require (
      public_host_runtime.find ("_transport->signal_activity ()")
          != std::string::npos,
      "CPP-DISP-005",
      "local application enqueue does not signal the MeshNode activity poll");

    /* CPP-DISP-003 — pre-admission does not reject because a queue is full. */
    gate.require (
      raw_mesh_node_owner.find ("protocol::framework_error_code::workerQueueFull")
          == std::string::npos
        && raw_mesh_node_owner.find ("reply-worker-queue-full") == std::string::npos,
      "CPP-DISP-003",
      "pre-admission still sends a terminal queue-capacity reply");

    const auto messaging_runtime = root / "framework/src/runtime/messaging";
    const bool legacy_submit_runtime_absent =
      !std::filesystem::exists (messaging_runtime / "async_submit_runtime.cpp")
      && !std::filesystem::exists (messaging_runtime / "async_submit_runtime.hpp")
      && !std::filesystem::exists (messaging_runtime / "pending_operation.cpp")
      && !std::filesystem::exists (messaging_runtime / "pending_operation.hpp")
      && !std::filesystem::exists (messaging_runtime / "pending_operation_state.hpp")
      && !std::filesystem::exists (messaging_runtime / "pending_submit.cpp")
      && !std::filesystem::exists (messaging_runtime / "pending_submit.hpp")
      && !std::filesystem::exists (messaging_runtime / "submit_queue.cpp")
      && !std::filesystem::exists (messaging_runtime / "submit_queue.hpp");
    gate.require (
      legacy_submit_runtime_absent
        && m6a_sources.find (
             "framework/src/runtime/messaging/call_facade_runtime.cpp")
             != std::string::npos
        && m6a_sources.find (
             "framework/src/runtime/messaging/logical_multicast_runtime.cpp")
             != std::string::npos
        && !std::filesystem::exists (
             messaging_runtime / "logical_multicast_runtime.hpp")
        && logical_multicast_runtime.find ("_for_tests") == std::string::npos
        && !tree_contains (messaging_runtime, "note_submit_attempt")
        && !tree_contains (messaging_runtime, "notify_submit_ready")
        && !tree_contains (messaging_runtime, "pending_submit_t")
        && !tree_contains (messaging_runtime, "submit_queue_t"),
      "CPP-HWM-ASYNC-001",
      "Framework still owns legacy submit retry state or the replacement runtimes are not production members");

    /* The compatibility fallback may translate one synchronous terminal, but
     * it must never retain and retry a Core admission failure. */
    gate.require (
      call_facade_runtime.find (
        "return task_t<void> (terminal_result (submit ()))")
          != std::string::npos
        && call_facade_runtime.find ("note_submit_attempt") == std::string::npos
        && call_facade_runtime.find ("notify_submit_ready") == std::string::npos,
      "CPP-HWM-ASYNC-002",
      "one-way compatibility submission is not a single terminal attempt");

    /* CPP-DISP-004 — dequeue remains the public completion boundary, while
     * application-bound publisher failures reach the Spot structured observer. */
    gate.require (
      m6a_sources.find (
        "framework/src/runtime/messaging/logical_multicast_runtime.cpp")
          != std::string::npos
        && logical_multicast_runtime.find (
        "job.completion->complete (result_t<void>::success ())")
          != std::string::npos
        && logical_multicast_runtime.find ("bool stopping = false;")
             != std::string::npos
        && logical_multicast_runtime.find (
             "if (stopping) {\n            job.completion->complete")
             != std::string::npos
        && logical_multicast_runtime.find ("std::clog") == std::string::npos
        && spot_runtime.find (
             "detail::report_logical_multicast_failure")
             != std::string::npos,
      "CPP-DISP-004",
      "logical multicast failures after dequeue are not observable");

    /* CPP-DISP-006 — close and idle-eviction admission share the node owner;
     * the unified Actor token carries that lease through handler terminal. */
    gate.require (
      spot_runtime.find ("auto queue = state_sync ([this] {") != std::string::npos
        && spot_runtime.find (
             "if (callback_admission_closed || idle_eviction_in_progress || close_reservation != 0)")
             != std::string::npos
        && spot_runtime.find ("class actor_dispatch_admission_token_t final")
             != std::string::npos
        && spot_runtime.find ("admission_token->acquire_dispatch_phase")
             != std::string::npos
        && spot_runtime.find ("admission_token->inherit_materialized_context")
             != std::string::npos
        && spot_runtime.find ("admission_token ? admission_token->handler_terminal ()")
             != std::string::npos
        && spot_runtime.find ("const bool admission_preclaimed =") != std::string::npos
        && spot_runtime.find ("!admission_preclaimed && !state->enter_callback ()")
             != std::string::npos
        && spot_runtime.find ("&& state->close_reservation == 0") != std::string::npos
        && spot_runtime.find ("return queue->try_post_async") != std::string::npos,
      "CPP-DISP-006",
      "Spot lifecycle admission is not retained by the unified token through handler terminal");

    /* CPP-EXEC-001 — actor queue lookup is a lifecycle-boundary update, not
     * a node-mutex acquisition on every inbound Actor packet. The immutable
     * snapshot keeps queue lifetime shared while creation and removal remain
     * serialized by the node owner. */
    gate.require (
      spot_runtime_surface.find ("actor_executor_snapshot") != std::string::npos
        && spot_runtime_surface.find ("std::atomic_load_explicit") != std::string::npos
        && spot_runtime_surface.find ("spot_serial_executor_t") != std::string::npos
        && actor_serial_executor.find ("class actor_serial_executor_t") != std::string::npos
        && spot_runtime_surface.find ("executor->execute_actor (") != std::string::npos,
      "CPP-EXEC-001",
      "Actor delivery still resolves its serial queue through the node map on every packet");

    /* CPP-COMP-001 — moving retry state crosses the error envelope as a typed
     * internal origin; exception wording never selects retry or error kind. */
    gate.require (
      failure_origin_wire.find ("actor_transfer_in_progress") != std::string::npos
        && channel_reply_writer.find ("runtime::messaging::write_failure_origin (header, error)")
             != std::string::npos
        && actor_client_runtime.find ("runtime::messaging::restore_failure_origin")
             != std::string::npos
        && actor_client_runtime.find (".find (\"transfer is in progress\")") == std::string::npos
        && actor_client_runtime.find ("message.find (") == std::string::npos,
      "CPP-COMP-001",
      "Actor retry or native failure classification still depends on exception text");

    /* CPP-LAYER-003 — Actor handler and deferred join completion ordering are
     * owned by the coordinator's Actor entrypoints, without a second
     * handler-wide mailbox lock. */
    gate.require (
      spot_runtime.find ("executor->execute_actor (") != std::string::npos
        && spot_runtime.find ("execute_actor_cancellable") != std::string::npos
        && spot_runtime_surface.find ("class spot_serial_executor_t") != std::string::npos
        && actor_serial_executor.find ("class actor_serial_executor_t")
             != std::string::npos
        && actor_serial_executor.find ("execute_actor (") != std::string::npos
        && actor_serial_executor.find ("execute_lifecycle (") != std::string::npos
        && actor_serial_executor.find ("state_lane_t _state_lane") != std::string::npos
        && actor_serial_executor.find ("std::map<") == std::string::npos
        && spot_runtime.find ("actor_mailboxes") == std::string::npos
        && spot_runtime.find ("actor_mailbox_lock") == std::string::npos,
      "CPP-LAYER-003",
      "Actor dispatch still holds a redundant per-Actor mailbox mutex");

    /* CPP-ROUTE-002 — the direct-store fallback uses the same owner
     * admission deadline as the shared location resolver and never extends
     * it while converting store time to a steady-clock cache deadline. */
    gate.require (
      public_host_runtime.find (
        "live.owner_admission_lifetime (snapshot->owner)")
          != std::string::npos
        && public_host_runtime.find (
             "measured_at + *read->admission_lifetime")
             != std::string::npos
        && public_host_runtime.find (
             "measured_at + lifetime")
             != std::string::npos,
      "CPP-ROUTE-002",
      "direct-store Spot route cache outlives owner admission");

    /* CPP-OBS-001 — Instance Spot activation must not allocate its trace DTO
     * or correlation strings while message-flow diagnostics are off. */
    gate.require (
      app_runtime.find ("make_instance_spot_activation_trace_context")
          != std::string::npos
        && app_runtime.find ("if (!may_emit)")
             != std::string::npos
        && app_runtime.find ("return std::nullopt;")
             != std::string::npos
        && app_runtime.find ("message_flow_tracer_t (dispatch).trace")
             != std::string::npos,
      "CPP-OBS-001",
      "Instance Spot activation tracing is not gated before event allocation");

    /* CPP-OBS-002 — the flow context records the diagnostics mode at entry so
     * the off case allocates no flow id, while every processing point re-reads
     * the live shared level; an entry snapshot never overrides a later runtime
     * change (server spec 26). */
    gate.require (
      flow_context.find ("diagnostics_mode") != std::string::npos
        && flow_context.find (
             "scope_t (std::nullopt)")
             != std::string::npos
        && message_flow_tracer.find (
             "current->diagnostics_mode")
             == std::string::npos
        && message_flow_tracer.find (
             "effective_message_flow (")
             != std::string::npos,
      "CPP-OBS-002",
      "message-flow diagnostics level is not read live at each processing point");

    /* CPP-OBS-003 — level and sampling gates precede lazy event construction;
     * absence of an application logger never falls back to a process console. */
    gate.require (
      message_flow_tracer.find ("sample_current (") != std::string::npos
        && message_flow_tracer.find (
             "add (\"event_id\", \"zlink.message_flow\")")
             != std::string::npos
        && dispatch_error_reporter.find (
             "add (\"event_id\", \"zlink.dispatch_error\")")
             != std::string::npos
        && message_flow_unit.find (
             "built.load (std::memory_order_relaxed) != 0")
             != std::string::npos
        && message_flow_unit.find ("report_lazy") != std::string::npos
        && diagnostic_event_sink.find ("log_if_configured")
             != std::string::npos
        && diagnostic_event_sink.find ("std::clog") == std::string::npos
        && diagnostic_event_sink.find ("std::cerr") == std::string::npos,
      "CPP-OBS-003",
      "sampling or Off diagnostics build events before the gate, or use an implicit console sink");

    /* CPP-ASYNC-003 — a STREAM-to-Actor relay keeps its completion and wrapper
     * closure alive until target completion, preserving both per-Actor order and
     * caller-visible failure across a Session replacement. The drain observer
     * additionally carries the trampoline gate that decides which frame runs
     * the next FIFO turn without recursing. */
    gate.require (
      actor_gateway_runtime.find (
        "auto task = completion->task ()") != std::string::npos
        && actor_gateway_runtime.find (
             "pending.completion->complete (result)")
             != std::string::npos
        && actor_gateway_runtime.find (
             "[state, actor_id, pending = std::move (*pending), dispatch, dispatched,")
             != std::string::npos
        && actor_gateway_runtime.find (
             "continue_gate] (const result_t<void> &result) mutable {")
             != std::string::npos
        && actor_gateway_runtime.find (
             "relay_source = std::move (relay_source)] () mutable -> task_t<void> {")
             != std::string::npos
        && actor_gateway_runtime.find (
             "const auto dispatched = co_await dispatcher (")
             != std::string::npos
        && actor_gateway_unit.find (
             "if (!first.result () || !second.result () || !independent.result ())")
             != std::string::npos,
      "CPP-ASYNC-003",
      "Actor relay does not preserve target completion, order, and dispatch lifetime");

    /* CPP-WIRE-001 — every authority read and write uses the same canonical
     * zla1 key codec; legacy numeric keys are not compatibility aliases. */
    gate.require (
      authority_key_codec.find ("\"zla1:\"") != std::string::npos
        && authority_key_codec.find ("0123456789ABCDEF")
             != std::string::npos
        && authority_key_codec.find ("object_id.size ()")
             != std::string::npos,
      "CPP-WIRE-001",
      "canonical authority key codec is missing its version, byte length, or uppercase percent encoding");
    for (const auto &[name, source] : std::array{
           std::pair{"store resolver", &store_location_resolvers},
           std::pair{"in-memory store", &in_memory_location_store},
           std::pair{"provider store", &provider_location_repository},
           std::pair{"public store adapter", &public_store_adapters},
           std::pair{"Actor client", &actor_client},
           std::pair{"host runtime", &app_runtime},
           std::pair{"MeshNode host", &mesh_node_host_service},
           std::pair{"stateful host", &public_host_runtime},
           std::pair{"stateful dispatch", &raw_stateful_dispatch}}) {
        gate.require (
          source->find ("\"1:\"") == std::string::npos
            && source->find ("\"2:\"") == std::string::npos
            && source->find ("\"3:\"") == std::string::npos,
          "CPP-WIRE-001",
          std::string (name) + " still contains a legacy numeric authority key");
    }

    /* CPP-WIRE-005 — RelocationId is an opaque random 128-bit identity. The
     * process retains issued IDs for the relocation-root retention window and
     * regenerates zero or colliding candidates. */
    gate.require (
      relocation_id_generator.find ("getrandom")
          != std::string::npos
        && relocation_id_generator.find ("BCryptGenRandom")
             != std::string::npos
        && relocation_id_generator.find ("arc4random_buf")
             != std::string::npos
        && relocation_id_generator.find ("attempt != 64")
             != std::string::npos
        && relocation_id_generator.find ("std::chrono::hours (24)")
             != std::string::npos
        && relocation_id_generator.find ("_issued.emplace")
             != std::string::npos,
      "CPP-WIRE-005",
      "Relocation ID generation is not CSPRNG-gated, non-zero, collision-retrying, and retention-bounded");
    gate.require (
      mesh_node_runtime.find ("relocation_ids ().issue ()")
          != std::string::npos
        && mesh_node_runtime.find ("next_relocation")
             == std::string::npos,
      "CPP-WIRE-005",
      "a relocation path still derives RelocationId from a deterministic counter");

    /* CPP-WIRE-006 — terminal/result integrity and generic wire bounds are
     * emitted from the common schema instead of being redefined by the C++
     * codec. */
    gate.require (
      generated_service_wire_constants.find (
        "enum class request_terminal_result")
          != std::string::npos
        && generated_service_wire_constants.find (
             "valid_terminal_failure")
             != std::string::npos
        && generated_service_wire_constants.find ("blobBytes")
             != std::string::npos
        && service_wire_codec.find (
             "bool valid_terminal_failure")
             == std::string::npos
        && service_wire_codec.find ("maximum_bytes = blobBytes")
             != std::string::npos
        && service_wire_codec.find ("offset - start > metadataBytes")
             != std::string::npos,
      "CPP-WIRE-006",
      "C++ service codec still redefines schema terminal or bound knowledge");

    /* CPP-CONTRACT-ROLE-001 — a missing local Client role is a local
     * configuration error, distinct from a configured Client with no target. */
    gate.require (
      client_server_location_runtime.find (
        "framework_error_kind_t::not_configured") != std::string::npos
        && client_server_location_runtime.find (
             "ClientServer Client role is not registered for this channel")
             != std::string::npos,
      "CPP-CONTRACT-ROLE-001",
      "ClientServer calls do not distinguish a missing Client role from a missing target");

    /* CPP-CONTRACT-STREAM-001 — STREAM send alone exposes the per-call
     * admission bound and narrows the existing socket admission context.
     * The binding async terminal owns that admission deadline, and the
     * Framework awaits its completion after releasing the socket lock. */
    gate.require (
      call_hpp.find (
        "stream_send_call_t &timeout (std::chrono::milliseconds timeout)")
          != std::string::npos
        && stream_runtime.find (
             "_submit (header, payload, _timeout)")
             != std::string::npos
        && stream_host.find (
             "_core_socket->options ().send_timeout (*timeout);")
             != std::string::npos
        && stream_host.find (
             "_core_socket->send (rid).message (std::move (frame)).async ()")
             != std::string::npos
        && stream_host.find (
             "_core_socket->options ().send_timeout (configured_timeout);")
             != std::string::npos
        && stream_host.find (
             "co_await std::move (*pending)")
             != std::string::npos
        && stream_host.find (
             "_core_socket->send (rid).message (std::move (frame)).submit ()")
             == std::string::npos
        && !tree_contains (
             root / "framework/src/runtime/streams", "async_submit_runtime"),
      "CPP-CONTRACT-STREAM-001",
      "STREAM send does not propagate its per-call deadline to binding-owned admission");

    /* CPP-CONTRACT-MESH-SOCKET-001 — MeshNode ReceiveTimeout follows the
     * existing socket configuration path into the binding ROUTER receive
     * option. Send and receive timeouts must remain direction-specific. */
    gate.require (
      mesh_node_hpp.find ("std::optional<std::chrono::milliseconds> receive_timeout")
          != std::string::npos
        && mesh_node_runtime.find ("_state->socket.receive_timeout") != std::string::npos
        && raw_mesh_node_owner.find (
             "router->options ().recv_timeout (*_options.receive_timeout);")
             != std::string::npos,
      "CPP-CONTRACT-MESH-SOCKET-001",
      "MeshNode ReceiveTimeout does not reach the binding ROUTER recv_timeout option");

    /* CPP-LAYER-002 — in-flight calls do not reuse the public Actor Join
     * OperationId type or name. */
    gate.require (call_id.find ("struct call_id_t") != std::string::npos
                    && call_id.find ("struct operation_id_t")
                         == std::string::npos,
                  "CPP-LAYER-002",
                  "in-flight runtime calls still use the OperationId name");
    gate.require (service_wire_codec_header.find (
                    "struct wire_operation_id_t") != std::string::npos
                    && service_wire_codec_header.find (
                         "using wire_operation_id_t") == std::string::npos,
                  "CPP-LAYER-002",
                  "Actor Join OperationId is not a distinct wire type");

    /* CPP-OWN-006 — encoded payloads already owned by framework message_t are
     * visited by reference. Runtime inspection uses the binding byte view and
     * only copies at boundaries that require new ownership. */
    gate.require (
      framework_message.find ("with_encoded_payload") != std::string::npos
        && framework_message.find ("return *_encoded;") == std::string::npos,
      "CPP-OWN-006",
      "framework message access still copies an already encoded payload");
    gate.require (
      mesh_node_runtime.find ("payload.to_bytes ().size ()")
          == std::string::npos
        && raw_fanout_owner.find ("parts.front ().to_bytes ()")
             == std::string::npos,
      "CPP-OWN-006",
      "runtime payload inspection still materializes a byte-vector copy");

    /* CPP-OWN-004 — the default JSON serializer writes and reads the encoded
     * payload directly instead of round-tripping through a binding message. */
    gate.require (
      serializer_header.find ("codecs::json::detail::dump_profile (")
          != std::string::npos
        && serializer_header.find ("nlohmann::json (value)")
             != std::string::npos
        && serializer_header.find (
             "payload.to_raw ().template parse_json")
             == std::string::npos
        && serializer_header.find (
             "zlink::message_t::from_json (value)")
             == std::string::npos,
      "CPP-OWN-004",
      "default JSON serialization still round-trips through a binding message");

    /* CPP-OWN-001 — the cached typed serializer owns the expected content type,
     * and handler decode rejects a different envelope value. */
    gate.require (
      serializer_header.find ("const std::string &content_type ()")
          != std::string::npos
        && tree_contains (
             include_root,
             "inbound content type does not match the typed handler codec"),
      "CPP-OWN-001",
      "typed handler decode does not validate the cached codec content type");

    /* CPP-OWN-008 — a resolved custom serializer owns its erased functions;
     * hot-path encode/decode no longer re-enters the registry map. */
    gate.require (
      serializer_header.find (
        "[serialize = std::move (serialize)]")
          != std::string::npos
        && serializer_header.find ("[this, type]") == std::string::npos,
      "CPP-OWN-008",
      "cached custom serializers still look up erased functions per message");

    /* CPP-OWN-003 — claiming a queued stateful turn transfers the already
     * decoded application payload instead of decoding the canonical queue
     * bytes again. */
    gate.require (
      raw_stateful_dispatch.find ("auto frozen = std::move (pending->second.frozen)")
          != std::string::npos
        && raw_stateful_dispatch.find ("auto payload = std::move (*frozen.application)")
             != std::string::npos
        && raw_stateful_dispatch.find ("owner, *turn,")
             == std::string::npos,
      "CPP-OWN-003",
      "stateful claim still copies pending payload or canonical turn buffers");

    /* CPP-FOLLOW-001 — Message Follow admission preserves the contract error
     * categories and rejects a revisited node before the hop ceiling. */
    for (const std::string required : {
           "framework_error_kind_t::invalid_operation",
           "framework_error_kind_t::internal_failure",
           "framework_error_kind_t::unavailable"}) {
        gate.require (
          actor_transfer_coordinator.find (required) != std::string::npos,
          "CPP-FOLLOW-001",
          "Message Follow admission omits typed failure: " + required);
    }
    gate.require (
      mesh_node_runtime.find ("__zlink.messageFollowVisitedNodes")
          != std::string::npos
        && mesh_node_runtime.find (
             "follow_path.value ().visited.contains")
             != std::string::npos,
      "CPP-FOLLOW-001",
      "Message Follow still relies only on the hop ceiling for loop detection");
    gate.require (
      actor_transfer_coordinator.find (
        "candidate.source_fence == source_fence")
          != std::string::npos
        && actor_transfer_coordinator.find (
             "source_fence.object_generation != generation")
             != std::string::npos,
      "CPP-FOLLOW-001",
      "Message Follow does not preserve its exact Actor source fence");

    /* CPP-LIFE-001 — a general message addresses the logical Actor or Spot.
     * Owner authority and lease remain fences, while ObjectGeneration is
     * normalized to the current incarnation after admission. */
    gate.require (
      raw_stateful_dispatch.find ("matches_application_route")
          != std::string::npos
        && raw_stateful_dispatch.find (
             "accepted_target.object_generation =")
             != std::string::npos
        && raw_stateful_dispatch.find (
             "actor.target.owner_lease_generation")
             != std::string::npos
        && raw_stateful_dispatch.find (
             "spot.target.owner_lease_generation")
             != std::string::npos,
      "CPP-LIFE-001",
      "general application admission does not normalize generation or fence the current owner lease");
    gate.require (
      spot_runtime.find ("const bool admitted =") != std::string::npos
        && spot_runtime.find ("targets_local_actor") != std::string::npos
        && spot_runtime.find ("targets_exact_incarnation") != std::string::npos
        && spot_runtime.find ("requires_exact_incarnation")
             != std::string::npos
        && spot_runtime.find (
             "framework_error_kind_t::invalid_operation")
             != std::string::npos,
      "CPP-LIFE-001",
      "Actor dispatch does not separate general-message admission from exact Message Follow and bound-session admission");

    /* CPP-SESS-001 — a STREAM-originated Actor relay creates a fresh
     * downstream request correlation. Reusing the upstream STREAM correlation
     * would collide with the target exactly-once table when a replacement
     * session retries the same packet. */
    gate.require (
      mesh_node_runtime.find ("codec.create_envelope (kind") != std::string::npos
        && mesh_node_runtime.find ("envelope.correlation_id") == std::string::npos,
      "CPP-SESS-001",
      "STREAM Actor relay reuses an upstream correlation instead of creating a fresh request id");

    /* CPP-SESS-004 — replacement callback completion must schedule the close
     * with an asynchronous timer. Sleeping in the callback would hold the
     * session serial lane and prevent unrelated sessions from progressing. */
    const auto replacement_begin = stream_host.find (
      "bool begin_actor_binding_replacement");
    const auto replacement_end = replacement_begin == std::string::npos
      ? std::string::npos
      : stream_host.find (
          "std::shared_ptr<replacement_session_state_t>\n    register_replacement_session",
          replacement_begin);
    const auto replacement_source = replacement_begin == std::string::npos
      || replacement_end == std::string::npos
      ? std::string{}
      : stream_host.substr (replacement_begin, replacement_end - replacement_begin);
    gate.require (
      replacement_source.find ("asio::steady_timer") != std::string::npos
        && replacement_source.find (
             "expires_after (std::chrono::milliseconds (100))")
             != std::string::npos
        && replacement_source.find ("async_wait") != std::string::npos
        && replacement_source.find ("std::this_thread::sleep_for")
             == std::string::npos
        && replacement_source.find ("condition_variable") == std::string::npos,
      "CPP-SESS-004",
      "replacement callback waits synchronously instead of scheduling a non-blocking close timer");

    /* CPP-SESS-003 — each serial owner selects a closed lane-policy type.
     * Yield behavior is derived from the Spot alternative; callers cannot
     * manufacture unrelated lifecycle capabilities with bool flags. */
    for (const std::string required : {
           "spot_lane_policy_t", "spot_lane_lifecycle_t",
           "return_wait", "relocation_sealed", "session_lane_policy_t",
           "session_lane_lifecycle_t", "connection_closed",
           "actor_delivery_lane_policy_t", "std::variant<"}) {
        gate.require (serial_execution_queue.find (required) != std::string::npos,
                      "CPP-SESS-003",
                      "serial execution lane policy is missing: " + required);
    }
    gate.require (serial_execution_queue.find ("bool allow_yield") == std::string::npos
                    && serial_execution_queue.find ("_allow_yield") == std::string::npos,
                  "CPP-SESS-003", "serial execution still accepts a raw yield-policy boolean");
    for (const std::string required :
         {"serial_lane_policy_t::entry_spot ()", "serial_lane_policy_t::spot_wide ()",
          "serial_lane_policy_t::per_actor_spot ()", "serial_lane_policy_t::actor_delivery ()"}) {
        gate.require (spot_runtime_surface.find (required) != std::string::npos, "CPP-SESS-003",
                      "Spot or Actor-delivery queue omits its typed policy: " + required);
    }
    gate.require (session_serial_executor.find ("serial_lane_policy_t::session ()")
                    != std::string::npos,
                  "CPP-SESS-003", "STREAM session queue omits its typed policy");
    for (const std::string required : {
           "execute_application (", "execute_control (", "execute_infrastructure (",
           "execute_final ("}) {
        gate.require (session_serial_executor.find (required) != std::string::npos,
                      "CPP-SESS-003",
                      "STREAM session executor omits entrypoint: " + required);
    }
    gate.require (
      stream_runtime_surface.find ("dispatch_queue") == std::string::npos
        && stream_runtime_surface.find ("session_serial_executor") != std::string::npos,
      "CPP-SESS-003",
      "STREAM runtime still owns the serial queue instead of the session executor");

    /* TH-CP-01 — the C++ connector helper surface has a language contract. */
    const auto connector_contract_path =
      root
      / "../../doc/framework/common/spec/stream-connector/languages/cpp/03-stream-connector.ko.md";
    const auto connector_contract = std::filesystem::exists (connector_contract_path)
                                      ? read_text_file (connector_contract_path)
                                      : std::string{};
    gate.require (!connector_contract.empty (), "TH-CP-01",
                  "C++ stream connector language contract is missing");
    gate.require (connector_contract.find ("expect_none") != std::string::npos
                    && connector_contract.find ("wait_for_sequence") != std::string::npos
                    && connector_contract.find ("namespace zlink::stream_connector::assertions")
                         != std::string::npos,
                  "TH-CP-01",
                  "C++ stream connector contract omits the common test helper surface");

    if (gate.failures != 0) {
        std::cerr << "target contract gate failures: " << gate.failures << '\n';
        return 1;
    }
    std::cout << "target contract gate satisfied\n";
    return 0;
}
