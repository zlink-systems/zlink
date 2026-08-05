/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/messages.hpp"

#include <zlink/http_client.hpp>
#include <zlink/stream_connector.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace e2e = zlink::e2e::spot_actor_transfer;
namespace http = zlink::http_client;
namespace sc = zlink::stream_connector;

namespace
{

struct client_options_t
{
    std::string node_a_url;
    std::string node_b_url;
    std::string node_a_stream;
    std::string node_b_stream;
    std::string scenario;
};

client_options_t read_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown SpotActorTransfer client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("SpotActorTransfer client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open SpotActorTransfer client config: " + path);
    }
    const auto section = nlohmann::json::parse (input).at ("e2e");
    return {.node_a_url = section.at ("nodeAUrl").get<std::string> (),
            .node_b_url = section.at ("nodeBUrl").get<std::string> (),
            .node_a_stream = section.at ("nodeAStream").get<std::string> (),
            .node_b_stream = section.at ("nodeBStream").get<std::string> (),
            .scenario = section.at ("scenario").get<std::string> ()};
}

void require (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

std::string unique_suffix ()
{
    static std::mt19937_64 rng (std::random_device{}());
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << std::hex << rng () << '-' << counter.fetch_add (1);
    return out.str ();
}

http::client_t make_http (const std::string &base_url)
{
    return http::client_t::create ()
      .base_url (base_url)
      .timeout (std::chrono::seconds (30))
      .build ();
}

struct nodes_t
{
    http::client_t a;
    http::client_t b;
    std::string a_stream_endpoint;
    std::string b_stream_endpoint;
};

e2e::create_spot_res_t
create_spot (http::client_t &node, const std::string &spot_id, const std::string &mode = "accept")
{
    return node.post ("/spots")
      .body (e2e::create_spot_req_t{spot_id, mode})
      .submit<e2e::create_spot_res_t> ().result ().value ().body;
}

e2e::create_spot_res_t create_spot_until_placed_on (
  http::client_t &node,
  const std::string &base_spot_id,
  const std::string &target_node_rid,
  const std::string &mode = "accept")
{
    for (unsigned attempt = 0; attempt != 64; ++attempt) {
        const auto spot_id = attempt == 0
          ? base_spot_id
          : base_spot_id + "-placement-" + std::to_string (attempt);
        auto created = create_spot (node, spot_id, mode);
        if (created.node_rid == target_node_rid)
            return created;
    }
    throw std::runtime_error (
      "could not place Spot on " + target_node_rid + " after 64 attempts");
}

e2e::actor_create_res_t create_actor (http::client_t &node,
                                      const std::string &actor_id,
                                      const std::string &actor_type,
                                      int state_version)
{
    return node.post ("/actors")
      .body (e2e::actor_create_req_t{actor_id, actor_type, state_version})
      .submit<e2e::actor_create_res_t> ().result ().value ().body;
}

e2e::actor_create_res_t create_actor_until_placed_on (
  http::client_t &node,
  const std::string &base_actor_id,
  const std::string &actor_type,
  int state_version,
  const std::string &target_node_rid)
{
    for (unsigned attempt = 0; attempt != 64; ++attempt) {
        const auto actor_id = attempt == 0
          ? base_actor_id
          : base_actor_id + "-placement-" + std::to_string (attempt);
        auto created = create_actor (node, actor_id, actor_type, state_version);
        if (created.node_rid == target_node_rid)
            return created;
    }
    throw std::runtime_error (
      "could not place Actor on " + target_node_rid + " after 64 attempts");
}

e2e::gate_release_res_t release_joined_gate (http::client_t &node, const std::string &spot_id)
{
    return node.post ("/joined-gates/" + spot_id + "/release").submit<e2e::gate_release_res_t> ().result ().value ().body;
}

e2e::gate_release_res_t release_transfer_gate (http::client_t &node, const std::string &actor_id)
{
    return node.post ("/transfer-gates/" + actor_id + "/release").submit<e2e::gate_release_res_t> ().result ().value ().body;
}

e2e::actor_ref_snapshot_res_t get_actor_ref (http::client_t &node, const std::string &actor_id)
{
    return node.get ("/actors/" + actor_id + "/ref").submit<e2e::actor_ref_snapshot_res_t> ().result ().value ().body;
}

std::vector<e2e::actor_evidence_t> get_evidence (http::client_t &node)
{
    return node.get ("/evidence").submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
}

std::pair<std::uint64_t, std::uint64_t>
get_relocation_store_activity (http::client_t &node)
{
    const auto activity =
      node.get ("/relocation-store/activity")
        .submit<nlohmann::json> ().result ().value ().body;
    return {activity.at ("reads").get<std::uint64_t> (),
            activity.at ("writes").get<std::uint64_t> ()};
}

void shutdown_node (http::client_t &node)
{
    (void) node.post ("/shutdown").submit<nlohmann::json> ().result ().value ().body;
}

e2e::join_target_res_t join_actor (http::client_t &node,
                                   const std::string &actor_id,
                                   const e2e::join_target_req_t &request)
{
    return node.post ("/actors/" + actor_id + "/join")
      .body (request)
      .submit<e2e::join_target_res_t> ().result ().value ().body;
}

e2e::probe_res_t
probe_actor (http::client_t &node, const std::string &actor_id, const e2e::probe_req_t &request)
{
    return node.post ("/actors/" + actor_id + "/probe").body (request).submit<e2e::probe_res_t> ().result ().value ().body;
}

e2e::actor_ref_probe_res_t probe_ref (http::client_t &node,
                                      const std::string &actor_id,
                                      const e2e::actor_ref_snapshot_res_t &actor,
                                      const e2e::probe_req_t &request,
                                      std::chrono::milliseconds timeout = std::chrono::seconds (5))
{
    return node.post ("/actors/" + actor_id + "/probe-ref")
      .body (e2e::actor_ref_probe_req_t{request.scenario, request.marker, actor.node_rid,
                                        actor.generation, static_cast<int> (timeout.count ())})
      .submit<e2e::actor_ref_probe_res_t> ().result ().value ().body;
}

void send_ref (http::client_t &node,
               const std::string &actor_id,
               const e2e::actor_ref_snapshot_res_t &actor,
               const e2e::handoff_packet_msg_t &packet)
{
    (void) node.post ("/actors/" + actor_id + "/send-ref")
      .body (e2e::actor_ref_probe_req_t{packet.scenario, packet.marker, actor.node_rid,
                                        actor.generation, 5000})
      .submit<nlohmann::json> ().result ().value ().body;
}

e2e::bound_push_res_t
bound_push (http::client_t &node, const std::string &actor_id, const e2e::bound_push_req_t &request)
{
    return node.post ("/actors/" + actor_id + "/bound-push")
      .body (request)
      .submit<e2e::bound_push_res_t> ().result ().value ().body;
}

bool evidence_contains (const std::vector<e2e::actor_evidence_t> &evidence,
                        const std::string &expected)
{
    return std::any_of (evidence.begin (), evidence.end (), [&] (const auto &entry) {
        return e2e::evidence_text (entry).find (expected) != std::string::npos;
    });
}

std::vector<e2e::actor_evidence_t> wait_evidence (http::client_t &node,
                                                  const std::vector<std::string> &contains_all)
{
    auto evidence = node.post ("/evidence/wait")
                      .body (e2e::evidence_wait_req_t{contains_all, 10000})
                      .submit<std::vector<e2e::actor_evidence_t>> ().result ().value ().body;
    for (const auto &expected : contains_all) {
        require (evidence_contains (evidence, expected),
                 "Expected evidence marker was not observed: " + expected);
    }
    return evidence;
}

void require_no_contains (const std::vector<e2e::actor_evidence_t> &evidence,
                          const std::string &expected,
                          const std::string &message)
{
    require (!evidence_contains (evidence, expected), message);
}

void assert_evidence_order (http::client_t &node,
                            const std::string &actor_id,
                            const std::string &kind,
                            const std::vector<std::string> &values)
{
    std::vector<std::string> expected;
    expected.reserve (values.size ());
    for (const auto &value : values) {
        expected.push_back (actor_id + "|" + kind + "|" + value);
    }
    wait_evidence (node, expected);
    std::vector<std::string> observed;
    for (const auto &entry : get_evidence (node)) {
        if (entry.actor_id == actor_id && entry.kind == kind) {
            observed.push_back (entry.value);
        }
    }
    std::ostringstream joined;
    for (const auto &value : observed) {
        joined << value << ',';
    }
    require (observed == values,
             "Actor '" + actor_id + "' " + kind + " order mismatch: " + joined.str ());
}

void assert_evidence_sequence (http::client_t &node, const std::vector<std::string> &expected)
{
    const auto evidence = wait_evidence (node, expected);
    std::size_t next = 0;
    for (const auto &entry : evidence) {
        if (next < expected.size ()
            && e2e::evidence_text (entry).find (expected[next]) != std::string::npos) {
            ++next;
        }
    }
    require (next == expected.size (), "Lifecycle evidence order mismatch.");
}

void assert_correlated_transfer_markers (const std::vector<http::client_t *> &nodes,
                                         const std::string &actor_id,
                                         const std::vector<std::string> &kinds)
{
    std::optional<std::string> transfer_id;
    std::set<std::string> observed;
    for (auto *node : nodes) {
        for (const auto &entry : get_evidence (*node)) {
            if (entry.scenario != "message_flow" || entry.actor_id != actor_id
                || std::find (kinds.begin (), kinds.end (), entry.kind) == kinds.end ()) {
                continue;
            }
            require (!entry.transfer_id.empty () && !entry.correlation_id.empty ()
                       && !entry.flow_id.empty (),
                     "Transfer marker has no correlation fields: " + entry.kind);
            if (!transfer_id) {
                transfer_id = entry.transfer_id;
            }
            require (entry.transfer_id == *transfer_id && entry.correlation_id == *transfer_id
                       && entry.flow_id == *transfer_id,
                     "Transfer marker correlation mismatch: " + entry.kind);
            observed.insert (entry.kind);
        }
    }
    require (transfer_id.has_value () && observed.size () == kinds.size (),
             "Correlated transfer marker set is incomplete.");
}

void assert_request_handoff_frame (http::client_t &source,
                                   http::client_t &target,
                                   const std::string &actor_id,
                                   const std::string &handler_marker)
{
    wait_evidence (source, {"message_flow|" + actor_id + "|handoff_request_frame|request"});
    wait_evidence (target, {"message_flow|" + actor_id + "|backlog_request_frame|request"});
    std::optional<e2e::actor_evidence_t> source_frame;
    std::optional<e2e::actor_evidence_t> target_frame;
    std::size_t handler_count = 0;
    for (const auto &entry : get_evidence (source)) {
        if (entry.actor_id == actor_id && entry.kind == "handoff_request_frame") {
            require (!source_frame.has_value (),
                     "Source recorded duplicate request handoff frames.");
            source_frame = entry;
        }
    }
    for (const auto &entry : get_evidence (target)) {
        if (entry.actor_id == actor_id && entry.kind == "backlog_request_frame") {
            require (!target_frame.has_value (),
                     "Target recorded duplicate request backlog frames.");
            target_frame = entry;
        }
        if (entry.actor_id == actor_id && entry.kind == "packet_handler"
            && entry.value == handler_marker) {
            ++handler_count;
        }
    }
    require (source_frame && target_frame, "Request handoff frame evidence is incomplete.");
    require (source_frame->value == "request" && target_frame->value == "request"
               && !source_frame->correlation_id.empty ()
               && source_frame->correlation_id == target_frame->correlation_id
               && source_frame->transfer_id == target_frame->transfer_id
               && source_frame->flow_id == target_frame->flow_id,
             "Request id or request flag changed across actor handoff.");
    require (handler_count == 1, "In-flight request was not dispatched exactly once.");
}

std::vector<e2e::actor_evidence_t> message_follow_entries (
  http::client_t &node, const std::string &actor_id)
{
    std::vector<e2e::actor_evidence_t> entries;
    for (const auto &entry : get_evidence (node)) {
        if (entry.scenario == "message_flow" && entry.actor_id == actor_id
            && entry.kind == "message_follow_registered") {
            entries.push_back (entry);
        }
    }
    return entries;
}

class bound_session_t
{
  public:
    bound_session_t (const std::string &endpoint,
                     const std::string &scenario,
                     const e2e::actor_ref_snapshot_res_t &actor)
    {
        sc::connector_options_t options;
        options.endpoint = endpoint;
        options.connect_timeout = std::chrono::seconds (5);
        options.request_timeout = std::chrono::seconds (10);
        options.heartbeat.enabled = false;
        options.dispatch_mode = sc::dispatch_mode_t::immediate;
        options.max_received_messages = 1024;
        _connector.emplace (sc::connector_factory_t::create (options));
        auto connected = _connector->connect ();
        require (static_cast<bool> (connected), scenario + " bound session connect failed");
        auto bound = _connector
                       ->request (e2e::bind_actor_session_req_t{scenario, actor.actor_id,
                                                                actor.node_rid, actor.generation})
                       .packet_name (e2e::bind_actor_session_req_t::packet_name)
                       .submit<e2e::bind_actor_session_res_t> ();
        require (static_cast<bool> (bound), scenario + " session bind failed");
        require (bound.value ().actor_id == actor.actor_id,
                 scenario + " session bind actor mismatch");
    }

    ~bound_session_t ()
    {
        if (_connector) {
            _connector->close ();
        }
    }

    // Registers interest in a marker BEFORE the triggering call, mirroring the
    // dotnet WaitBoundPushAsync-then-trigger pattern.
    std::future<e2e::bound_push_notify_t> expect_push (std::string marker)
    {
        auto promise = std::make_shared<std::promise<e2e::bound_push_notify_t>> ();
        auto future = promise->get_future ();
        _connector->wait_for<e2e::bound_push_notify_t> (e2e::bound_push_notify_t::packet_name)
          .where (
            [marker] (const e2e::bound_push_notify_t &notify) { return notify.marker == marker; })
          .timeout (std::chrono::seconds (10))
          .submit ([promise] (sc::result_t<e2e::bound_push_notify_t> result) {
              if (result) {
                  promise->set_value (std::move (result.value ()));
              } else {
                  promise->set_exception (std::make_exception_ptr (std::runtime_error (
                    "bound push wait failed: code="
                    + std::to_string (static_cast<int> (result.error_code ())))));
              }
          });
        return future;
    }

    void send_packet (const e2e::handoff_packet_msg_t &packet)
    {
        _connector->send (packet).packet_name (e2e::handoff_packet_msg_t::packet_name).submit ();
    }

    // Issues a request over the bound session stream (mirrors dotnet
    // bound.Request). The server relays it to the actor through the bound
    // session route, so the actor learns where to push replies/notifies even
    // when it lives on a different node than the session (spot-actor §9).
    e2e::bound_push_res_t bound_push (const e2e::bound_push_req_t &request)
    {
        auto reply = _connector->request (request)
                       .packet_name (e2e::bound_push_req_t::packet_name)
                       .template submit<e2e::bound_push_res_t> ();
        require (static_cast<bool> (reply),
                 "bound session push request failed: code="
                   + std::to_string (static_cast<int> (reply.error_code ())));
        return reply.value ();
    }

  private:
    std::optional<sc::connector_t> _connector;
};

struct message_follow_setup_t
{
    std::string actor_id;
    e2e::actor_ref_snapshot_res_t old_ref;
};


class scenario_runner_t
{
  public:
    explicit scenario_runner_t (nodes_t &nodes);
    void run_st_a1_scenario ();
    void run_st_a2_scenario ();
    void run_st_a3_scenario ();
    void run_st_b1_scenario ();
    void run_st_b2_scenario ();
    void run_st_b3_scenario ();
    void run_st_b4_scenario ();
    void run_st_c1_scenario ();
    void run_st_c2_scenario ();
    void run_st_c3_scenario ();
    void run_st_d1_scenario ();
    void run_st_d2_scenario ();
    void run_st_e1_scenario ();
    void run_st_e2_scenario ();
    void run_st_f1_scenario ();
    void run_st_f2_scenario ();
    void run_st_f3_scenario ();
    void run_st_f4_scenario ();
    void run_st_f5_scenario ();
    void run_st_f6_scenario ();

  private:
    void in_flight_request_correlation ();
    void in_flight_request_timeout ();
    message_follow_setup_t relocate_for_message_follow (
      const std::string &scenario,
      int state_version);
    void transfer_out_failure ();
    void source_leave_failure ();
    void transfer_in_failure ();
    void joined_failure ();
    void local_location_commit_timing ();
    void remote_location_commit_timing ();

    nodes_t &_nodes;
};

} // namespace
