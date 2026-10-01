/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Perf spec §5.1, §5.2: the role config a server executable reads (one file, `--config`), the endpoint manifest the
// client reads, and the workload values both carry. The runner writes them (framework/perf/runner/roles.py); nothing
// here decides a value.

#include <perf/core.hpp>

#include <map>

namespace perf
{
struct workload_t
{
    int payload_size = 0;
    double duration_seconds = 0, warmup_seconds = 0;
    int inflight = 1;
    std::optional<int> connections, logical_streams;
    int client_count = 1;
    std::optional<int> connect_concurrency;
    int request_timeout_ms = 0, correlation_expiry_ms = 0, driver_timeout_ms = 0, setup_timeout_ms = 0,
        admin_timeout_ms = 0, socket_send_timeout_ms = 0;
};
inline void from_json (const json &in, workload_t &v)
{
    v.payload_size = in.at ("payloadSize").get<int> ();
    v.duration_seconds = in.at ("durationSeconds").get<double> ();
    v.warmup_seconds = in.at ("warmupSeconds").get<double> ();
    v.inflight = in.at ("inflight").get<int> ();
    v.connections = in.at ("connections").get<std::optional<int>> ();
    v.logical_streams = in.at ("logicalStreams").get<std::optional<int>> ();
    v.client_count = in.at ("clientCount").get<int> ();
    v.connect_concurrency = in.at ("connectConcurrency").get<std::optional<int>> ();
    v.request_timeout_ms = in.at ("requestTimeoutMs").get<int> ();
    v.correlation_expiry_ms = in.at ("correlationExpiryMs").get<int> ();
    v.driver_timeout_ms = in.at ("driverTimeoutMs").get<int> ();
    v.setup_timeout_ms = in.at ("setupTimeoutMs").get<int> ();
    v.admin_timeout_ms = in.at ("adminTimeoutMs").get<int> ();
    v.socket_send_timeout_ms = in.at ("socketSendTimeoutMs").get<int> ();
}

// §20: the run-owned Docker Redis and this cell's namespace.
struct store_config_t
{
    std::string provider, endpoint, container_id, image, image_digest, ns;
};
inline void from_json (const json &in, store_config_t &v)
{
    v.provider = in.at ("provider").get<std::string> ();
    v.endpoint = in.at ("endpoint").get<std::string> ();
    v.container_id = in.at ("containerId").get<std::string> ();
    v.image = in.at ("image").get<std::string> ();
    v.image_digest = in.at ("imageDigest").get<std::string> ();
    v.ns = in.at ("namespace").get<std::string> ();
}

// §5.2, §10.8: the public worker options and the CPU task every callback runs.
struct worker_config_t
{
    std::string algorithm;
    int task_millis = 0, min_threads = 0, max_threads = 0, idle_timeout_ms = 0, worker_timeout_ms = 0;
};
inline void from_json (const json &in, worker_config_t &v)
{
    v.algorithm = in.at ("algorithm").get<std::string> ();
    v.task_millis = in.at ("taskMillis").get<int> ();
    v.min_threads = in.at ("minThreads").get<int> ();
    v.max_threads = in.at ("maxThreads").get<int> ();
    v.idle_timeout_ms = in.at ("idleTimeoutMs").get<int> ();
    v.worker_timeout_ms = in.at ("workerTimeoutMs").get<int> ();
}

struct diagnostics_config_t
{
    std::string level, flow_file;
};

struct role_config_t
{
    std::string run_id, cell_id, config_hash, language, role;
    int role_instance = 0;
    std::string scenario;
    std::optional<std::string> topology, channel_name, mesh_name;
    std::map<std::string, std::string> transport_endpoints;
    std::optional<std::string> peer_endpoint;
    std::string metrics_url, application_trigger_url;
    bool source = false;
    std::string object_role;
    std::optional<store_config_t> store;
    std::vector<std::string> spot_ids, actor_ids;
    std::string execution_mode;
    workload_t workload;
    json provenance;
    std::optional<diagnostics_config_t> diagnostics;
    std::string mode = "request", terminal = "ordinary";
    std::optional<int> spot_count, subscriber_count;
    std::optional<worker_config_t> worker;
    bool await_remote_targets = true;

    // The role's first listener; roles with several transports read transport_endpoints by key.
    std::optional<std::string> listener_endpoint () const
    {
        return transport_endpoints.empty () ? std::nullopt : std::optional (transport_endpoints.begin ()->second);
    }
};
inline void from_json (const json &in, role_config_t &v)
{
    v.run_id = in.at ("runId").get<std::string> ();
    v.cell_id = in.at ("cellId").get<std::string> ();
    v.config_hash = in.at ("configHash").get<std::string> ();
    v.language = in.at ("language").get<std::string> ();
    v.role = in.at ("role").get<std::string> ();
    v.role_instance = in.at ("roleInstance").get<int> ();
    v.scenario = in.at ("scenario").get<std::string> ();
    v.topology = in.at ("topology").get<std::optional<std::string>> ();
    v.channel_name = in.at ("channelName").get<std::optional<std::string>> ();
    v.mesh_name = in.at ("meshName").get<std::optional<std::string>> ();
    v.transport_endpoints = in.at ("transportEndpoints").get<std::map<std::string, std::string>> ();
    v.peer_endpoint = in.at ("peerEndpoint").get<std::optional<std::string>> ();
    v.metrics_url = in.at ("metricsUrl").get<std::string> ();
    v.application_trigger_url = in.at ("applicationTriggerUrl").get<std::string> ();
    v.source = in.at ("source").get<bool> ();
    v.object_role = in.at ("objectRole").get<std::string> ();
    v.store = in.at ("store").get<std::optional<store_config_t>> ();
    v.spot_ids = in.at ("spotIds").get<std::vector<std::string>> ();
    v.actor_ids = in.at ("actorIds").get<std::vector<std::string>> ();
    v.execution_mode = in.at ("executionMode").get<std::string> ();
    v.workload = in.at ("workload").get<workload_t> ();
    v.provenance = in.at ("provenance");
    if (const auto found = in.find ("diagnostics"); found != in.end () && !found->is_null ())
        v.diagnostics = diagnostics_config_t{found->at ("level").get<std::string> (), found->at ("flowFile").get<std::string> ()};
    v.mode = in.at ("mode").get<std::string> ();
    v.terminal = in.at ("terminal").get<std::string> ();
    v.spot_count = in.at ("spotCount").get<std::optional<int>> ();
    v.subscriber_count = in.at ("subscriberCount").get<std::optional<int>> ();
    v.worker = in.at ("worker").get<std::optional<worker_config_t>> ();
    v.await_remote_targets = in.at ("awaitRemoteTargets").get<bool> ();
}

inline int port_of (const std::string &url)
{
    return std::stoi (url.substr (url.rfind (':') + 1));
}

inline role_config_t read_role_config (int argc, char **argv)
{
    if (argc != 3 || std::string (argv[1]) != "--config")
        throw std::invalid_argument ("Server requires --config <file> only.");
    std::ifstream file (argv[2]);
    if (!file)
        throw std::invalid_argument (std::string ("Cannot read role config ") + argv[2]);
    auto config = json::parse (file).get<role_config_t> ();
    if (port_of (config.metrics_url) == port_of (config.application_trigger_url))
        throw std::invalid_argument ("Admin and application trigger require separate listeners.");
    return config;
}

// §5.1: the endpoint manifest a client reads.
struct endpoint_role_t
{
    std::string role;
    int role_instance = 0;
    std::optional<std::string> stream_endpoint;
    std::string application_trigger_url, metrics_base_url;
};
struct endpoint_manifest_t
{
    std::string run_id, cell_id, config_hash, language;
    workload_t workload;
    std::vector<endpoint_role_t> roles;
    json provenance;
};
inline void from_json (const json &in, endpoint_manifest_t &v)
{
    v.run_id = in.at ("runId").get<std::string> ();
    v.cell_id = in.at ("cellId").get<std::string> ();
    v.config_hash = in.at ("configHash").get<std::string> ();
    v.language = in.at ("language").get<std::string> ();
    v.workload = in.at ("workload").get<workload_t> ();
    v.provenance = in.at ("provenance");
    for (const auto &role : in.at ("roles"))
        v.roles.push_back ({role.at ("role").get<std::string> (), role.at ("roleInstance").get<int> (),
                            role.at ("streamEndpoint").get<std::optional<std::string>> (),
                            role.at ("applicationTriggerUrl").get<std::string> (),
                            role.at ("metrics").at ("baseUrl").get<std::string> ()});
}
} // namespace perf
