/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §11.2 channel-echo-only. Question: what the same request costs over RouteMesh and over ClientServer. Roles: HTTP
// Client x1, Channel source x1 (this scenario), Channel target x1 (channel_echo_handler_t). Two Channel processes, manual
// RouteMesh or ClientServer, no Store and no objects. One operation = the source `route_client_t.request_to_channel(name,dto)
// .async<PerfEchoReply>()` through the typed identity and full-byte validation. 1024/4096 bytes JSON, request/ordinary.
// Physical connectors, Actor, Spot, worker and fanout metrics do not apply.

#include <perf/server/stream_loops.hpp>

namespace perf
{
// The typed Channel request handler of the target (§11.2, §10.5): the return value is the reply.
class channel_echo_handler_t
{
  public:
    using request_type = echo_request_t;
    using reply_type = echo_reply_t;
    explicit channel_echo_handler_t (role_t &role) : _role (role) {}

    echo_reply_t handle (const echo_request_t &request)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const handler_scope_t scope (measurement);
        try {
            measurement.validate_request (request);
            const auto reply = payload_pattern_t::reply (request, received);
            measurement.record_reply (request);
            if (measurement.phase () == "setup")
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"},
                                                               {"source", "channel_echo_handler_t (echo_request_t -> echo_reply_t)"},
                                                               {"observedValue", request.correlation_id}}}));
            return reply;
        }
        catch (...) {
            measurement.record_diagnostic (std::current_exception ());
            throw;
        }
    }

  private:
    role_t &_role;
};

class channel_echo_only_scenario_t
{
  public:
    explicit channel_echo_only_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
    }

    // The public status shows when the topology has a selectable target; the probe call itself is never retried.
    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        json channel_target_status;
        wait_for_public (_role, stopping, [&] {
            if (config.topology == "routemesh") {
                const auto status = _role.mesh.load ()->snapshot (*config.mesh_name);
                const auto channel = std::find_if (status.channels.begin (), status.channels.end (), [&] (const auto &item) {
                    return item.channel_name == config.channel_name;
                });
                if (!status.is_ready || channel == status.channels.end () || !channel->is_ready || channel->ready_target_count == 0)
                    return false;
                channel_target_status = {{"channelName", channel->channel_name}, {"isReady", channel->is_ready},
                                         {"readyTargetCount", channel->ready_target_count}};
                return true;
            }
            const auto status = _role.client_server.load ()->snapshot (*config.channel_name);
            if (!status.is_ready || status.ready_target_count == 0)
                return false;
            channel_target_status = {{"channelName", status.channel_name}, {"isReady", status.is_ready},
                                     {"readyTargetCount", status.ready_target_count}};
            return true;
        }, "a ready Channel target");
        auto &route = _role.service<fw::route_client_t> ();
        const auto request = measurement.request (0, _sequences.next (0), true);
        const auto reply = route.request_to_channel (*config.channel_name, request)
                             .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                             .async<echo_reply_t> ()
                             .result ()
                             .value ();
        payload_pattern_t::validate_identity (request, reply);
        measurement.pattern ().validate (reply.payload);
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "route_client_t.request_to_channel.async<PerfEchoReply>"},
                                                       {"observedValue", {{"correlationId", request.correlation_id}, {"receivedTicks", reply.received_ticks},
                                                                          {"clockDomainId", reply.clock_domain_id}}}}}));
        _role.objects->set (true, "", json::array ({{{"kind", "channelTarget"},
                                                      {"source", config.topology == "routemesh" ? "route_mesh_runtime_t.snapshot" : "client_server_runtime_t.snapshot"},
                                                      {"observedValue", channel_target_status}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_stream_loops (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        const auto &config = _role.config;
        while (measurement.can_issue ()) {
            auto request = measurement.request (stream, _sequences.next (stream));
            std::int64_t started = 0;
            if (!measurement.begin_operation (started))
                break;
            request.sent_ticks = dec (started);
            std::exception_ptr error;
            try {
                const auto reply = co_await route.request_to_channel (*config.channel_name, request)
                                     .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                                     .async<echo_reply_t> ();
                payload_pattern_t::validate_identity (request, reply);
                measurement.pattern ().validate (reply.payload);
            }
            catch (...) {
                error = std::current_exception ();
            }
            measurement.complete_operation (started, error);
        }
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
