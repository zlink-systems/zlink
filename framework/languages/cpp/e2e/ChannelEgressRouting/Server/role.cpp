/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Shared/messages.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace e2e = zlink::framework::e2e::channel_egress;
namespace fw = zlink::framework;

namespace
{

std::vector<std::string> split_list (std::string value)
{
    std::vector<std::string> result;
    std::stringstream input (std::move (value));
    std::string item;
    while (std::getline (input, item, ',')) {
        const auto first = item.find_first_not_of (' ');
        const auto last = item.find_last_not_of (' ');
        if (first != std::string::npos) {
            result.push_back (item.substr (first, last - first + 1));
        }
    }
    return result;
}

std::uint16_t endpoint_port (const std::string &endpoint)
{
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator + 1 >= endpoint.size ()) {
        throw std::runtime_error ("TCP endpoint must end with a port: " + endpoint);
    }
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value > 65535) {
        throw std::runtime_error ("TCP endpoint port is out of range: " + endpoint);
    }
    return static_cast<std::uint16_t> (value);
}

struct role_options_t
{
    std::string role;
    std::string rid;
    std::string http_endpoint;
    std::string game_endpoint;
    std::string audit_endpoint;
    std::string workflow_endpoint;
    std::string game_peers;
    std::string audit_peers;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::string evidence_file;
    std::string game_servers;
    std::string game_clients;
    std::string audit_servers;
    std::string audit_clients;
    std::string workflow_servers;
    std::string workflow_clients;
    std::string invalid_mode;
    int workflow_weight = 100;
    int hold_timeout_ms = 0;
    std::string instance_marker;

    static role_options_t bind (const fw::configuration_section_t &section)
    {
        int workflow_weight = 100;
        if (const auto value = section.get ("workflowWeight")) {
            workflow_weight = std::stoi (*value);
        }
        int hold_timeout_ms = 0;
        if (const auto value = section.get ("holdTimeoutMs")) {
            hold_timeout_ms = std::stoi (*value);
        }
        return {.role = section.require ("role"),
                .rid = section.require ("rid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .game_endpoint = section.get ("gameEndpoint").value_or (""),
                .audit_endpoint = section.get ("auditEndpoint").value_or (""),
                .workflow_endpoint = section.get ("workflowEndpoint").value_or (""),
                .game_peers = section.get ("gamePeers").value_or (""),
                .audit_peers = section.get ("auditPeers").value_or (""),
                .redis_endpoint = section.get ("redis.endpoint").value_or (""),
                .redis_key_prefix = section.get ("redis.keyPrefix").value_or (""),
                .log_dir = section.require ("logDir"),
                .evidence_file = section.get ("evidenceFile").value_or (""),
                .game_servers = section.get ("gameServers").value_or (""),
                .game_clients = section.get ("gameClients").value_or (""),
                .audit_servers = section.get ("auditServers").value_or (""),
                .audit_clients = section.get ("auditClients").value_or (""),
                .workflow_servers = section.get ("workflowServers").value_or (""),
                .workflow_clients = section.get ("workflowClients").value_or (""),
                .invalid_mode = section.get ("invalidMode").value_or (""),
                .workflow_weight = workflow_weight,
                .hold_timeout_ms = hold_timeout_ms,
                .instance_marker = section.get ("instanceMarker").value_or ("initial")};
    }
};

class role_state_t
{
  public:
    void hold ()
    {
        std::lock_guard lock (_mutex);
        _held = true;
        _release_at.reset ();
    }

    void hold_for (std::chrono::milliseconds duration)
    {
        std::lock_guard lock (_mutex);
        _held = true;
        _release_at = std::chrono::steady_clock::now () + duration;
    }

    void release ()
    {
        {
            std::lock_guard lock (_mutex);
            _held = false;
            _release_at.reset ();
        }
        _condition.notify_all ();
    }

    void wait_until_released ()
    {
        std::unique_lock lock (_mutex);
        if (_release_at) {
            if (!_condition.wait_until (lock, *_release_at,
                                        [this] { return !_held; })) {
                _held = false;
                _release_at.reset ();
            }
            return;
        }
        _condition.wait (lock, [this] { return !_held; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _condition;
    bool _held = false;
    std::optional<std::chrono::steady_clock::time_point> _release_at;
};

class shutdown_state_t
{
  public:
    void request () noexcept { _requested.store (true, std::memory_order_release); }

    bool requested () const noexcept
    {
        return _requested.load (std::memory_order_acquire);
    }

  private:
    std::atomic<bool> _requested {false};
};

class evidence_store_t
{
  public:
    evidence_store_t (std::string role,
                      std::string rid,
                      std::string instance_marker,
                      std::string evidence_file) :
        _role (std::move (role)), _rid (std::move (rid)),
        _instance_marker (std::move (instance_marker)),
        _evidence_file (std::move (evidence_file))
    {
    }

    void add (std::string marker)
    {
        {
            std::lock_guard lock (_mutex);
            _entries.push_back (std::move (marker));
            persist_locked (_entries.back ());
        }
        _condition.notify_all ();
    }

    nlohmann::json snapshot () const
    {
        std::lock_guard lock (_mutex);
        return { {"role", _role},
                 {"rid", _rid},
                 {"instanceMarker", _instance_marker},
                 {"entries", _entries} };
    }

    bool wait_for (std::string_view marker, std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock (_mutex);
        const auto found = [this, marker] {
            return std::any_of (_entries.begin (), _entries.end (), [marker] (const auto &entry) {
                return entry.find (marker) != std::string::npos;
            });
        };
        return _condition.wait_for (lock, timeout, found);
    }

    const std::string &role () const noexcept { return _role; }
    const std::string &rid () const noexcept { return _rid; }
    const std::string &instance_marker () const noexcept { return _instance_marker; }

  private:
    void persist_locked (const std::string &entry) const
    {
        if (_evidence_file.empty ()) {
            return;
        }
        std::ofstream output (_evidence_file, std::ios::app);
        if (output) {
            output << nlohmann::json{{"role", _role}, {"rid", _rid}, {"entry", entry}}
                            .dump ()
                   << '\n';
        }
    }

    std::string _role;
    std::string _rid;
    std::string _instance_marker;
    std::string _evidence_file;
    mutable std::mutex _mutex;
    mutable std::condition_variable _condition;
    std::vector<std::string> _entries;
};

fw::http_response_t json_response (nlohmann::json value, int status = 200)
{
    return {.status = status, .body = std::move (value).dump ()};
}

class channel_probe_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t,
                                                    role_state_t,
                                                    role_options_t,
                                                    fw::channel_client_t,
                                                    fw::route_client_t>;
    using request_type = e2e::channel_probe_request_t;
    using reply_type = e2e::channel_probe_reply_t;

    channel_probe_handler_t (evidence_store_t &evidence,
                             role_state_t &state,
                             role_options_t &options,
                             fw::channel_client_t &channels,
                             fw::route_client_t &routes) :
        _evidence (evidence), _state (state),
        _options (options),
        _channels (channels), _routes (routes)
    {
    }

    fw::task_t<reply_type> handle (const request_type &request)
    {
        co_return co_await handle_request (request, e2e::workflow_channel);
    }

    fw::task_t<reply_type> handle (const request_type &request,
                                   const fw::route_message_context_t &context)
    {
        const auto channel = context.channel_name.value_or (std::string{});
        co_return co_await handle_request (request, channel);
    }

  private:
    fw::task_t<reply_type> handle_request (const request_type &request,
                                           std::string_view channel)
    {
        const auto channel_name = channel.empty () ? std::string ("<none>")
                                                    : std::string (channel);
        const auto effective_channel = channel.empty () && _evidence.role () == "play"
                                         ? std::string (e2e::play_channel)
                                         : channel_name;
        _evidence.add ("request-start|role=" + _evidence.role ()
                       + "|channel=" + channel_name + "|id=" + request.id);
        if (_evidence.role () == "workflow-a" && request.mode == "hold") {
            if (_options.hold_timeout_ms > 0) {
                _state.hold_for (std::chrono::milliseconds (_options.hold_timeout_ms));
            } else {
                _state.hold ();
            }
            _evidence.add ("request-held|role=" + _evidence.role () + "|id=" + request.id);
            _state.wait_until_released ();
        }

        std::vector<std::string> downstream;
        if (_evidence.role () == "play"
            && (channel.empty () || channel == e2e::play_channel)
            && request.mode == "cascade") {
            const auto audit = co_await _routes
                                 .request_to_channel (
                                   std::string (e2e::audit_channel),
                                   request_type{request.id + "-audit", "echo"})
                                 .timeout (std::chrono::seconds (5))
                                 .submit<reply_type> ();
            downstream.push_back (audit.role + ":" + audit.channel);
            const auto workflow = co_await _channels
                                    .request_to_channel (
                                      std::string (e2e::workflow_channel),
                                      request_type{request.id + "-workflow", "echo"})
                                    .timeout (std::chrono::seconds (5))
                                    .submit<reply_type> ();
            downstream.push_back (workflow.role + ":" + workflow.channel);
        }

        _evidence.add ("request-end|role=" + _evidence.role ()
                       + "|channel=" + channel_name + "|id=" + request.id);
        co_return reply_type{.id = request.id,
                             .role = _evidence.role ().rfind ("workflow", 0) == 0
                                       ? _evidence.rid ()
                                       : _evidence.role (),
                             .lifecycle = _evidence.instance_marker (),
                             .channel = effective_channel,
                             .downstream = std::move (downstream)};
    }

    evidence_store_t &_evidence;
    role_state_t &_state;
    role_options_t &_options;
    fw::channel_client_t &_channels;
    fw::route_client_t &_routes;
};

class workflow_spot_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;
    using request_type = e2e::spot_workflow_request_t;
    using reply_type = e2e::spot_workflow_reply_t;

    explicit workflow_spot_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    fw::task_t<reply_type> handle (const request_type &request)
    {
        _evidence.add ("workflow-spot-start|id=" + request.id
                       + "|timer=" + request.timer_name);
        _evidence.add ("workflow-spot-end|id=" + request.id
                       + "|timer=" + request.timer_name);
        co_return reply_type{.id = request.id, .sequence = {"workflow-reply"}};
    }

  private:
    evidence_store_t &_evidence;
};

class channel_probe_send_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;
    using message_type = e2e::channel_probe_message_t;

    explicit channel_probe_send_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    void handle (const message_type &message)
    {
        _evidence.add ("send|role=" + _evidence.role () + "|channel=<none>|id=" + message.id);
    }

    void handle (const message_type &message, const fw::route_message_context_t &context)
    {
        _evidence.add ("send|role=" + _evidence.role ()
                       + "|channel=" + context.channel_name.value_or ("<none>")
                       + "|id=" + message.id);
    }

  private:
    evidence_store_t &_evidence;
};

class route_channel_probe_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t,
                                                    role_state_t,
                                                    role_options_t,
                                                    fw::channel_client_t,
                                                    fw::route_client_t>;

    route_channel_probe_handler_t (evidence_store_t &evidence,
                                   role_state_t &state,
                                   role_options_t &options,
                                   fw::channel_client_t &channels,
                                   fw::route_client_t &routes) :
        _handler (evidence, state, options, channels, routes)
    {
    }

    fw::task_t<e2e::channel_probe_reply_t>
    handle (const e2e::channel_probe_request_t &request,
            const fw::route_message_context_t &context)
    {
        return _handler.handle (request, context);
    }

  private:
    channel_probe_handler_t _handler;
};

class route_channel_probe_send_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;

    explicit route_channel_probe_send_handler_t (evidence_store_t &evidence) :
        _handler (evidence)
    {
    }

    void handle (const e2e::channel_probe_message_t &message,
                 const fw::route_message_context_t &context)
    {
        _handler.handle (message, context);
    }

  private:
    channel_probe_send_handler_t _handler;
};

class config12_timer_handler_t;

class config12_instance_spot_t final : public fw::instance_spot_t
{
  public:
    config12_instance_spot_t (fw::instance_spot_context_t context,
                              evidence_store_t &evidence) :
        _context (std::move (context)), _evidence (evidence)
    {
    }

    fw::instance_spot_context_t &context () noexcept override { return _context; }
    const fw::instance_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&config12_instance_spot_t::handle_request> (
          e2e::spot_workflow_request_t::packet_name);
    }

    fw::task_t<void> on_initialize () override
    {
        _evidence.add ("spot-initialize|rid=" + _evidence.rid ()
                       + "|spot=" + _context.spot_id ());
        co_return;
    }

    fw::task_t<void> on_closing (const fw::spot_closing_context_t &, std::stop_token) override
    {
        _evidence.add ("spot-closing|rid=" + _evidence.rid ()
                       + "|spot=" + _context.spot_id ());
        co_return;
    }

    fw::task_t<e2e::spot_workflow_reply_t>
    handle_request (const e2e::spot_workflow_request_t &request)
    {
        const auto spot = _context.spot_id ();
        _evidence.add ("spot-handler-start|spot=" + spot + "|id=" + request.id);
        const auto workflow = co_await _context.outbound ()
                                .request (std::string (e2e::workflow_channel),
                                         e2e::spot_workflow_request_t{
                                           request.id + "-workflow", request.timer_name})
                                .timeout (std::chrono::seconds (5))
                                .submit<e2e::spot_workflow_reply_t> ();
        (void) workflow;
        _evidence.add ("spot-workflow-reply|spot=" + spot + "|id=" + request.id);
        auto timer = _context.add_timer<config12_timer_handler_t> (
          request.timer_name, std::chrono::milliseconds (1));
        _timer = std::move (timer);
        _evidence.add ("spot-timer-start|spot=" + spot + "|id=" + request.id
                       + "|sequence=handler-start,workflow-reply,handler-end,timer-start");
        _evidence.add ("spot-handler-end|spot=" + spot + "|id=" + request.id);
        co_return e2e::spot_workflow_reply_t{
          .id = request.id,
          .sequence = {"handler-start", "workflow-reply", "handler-end"}};
    }

    fw::task_t<void> handle_timer (const fw::timer_tick_t &tick)
    {
        const auto spot = _context.spot_id ();
        const auto workflow = co_await _context.outbound ()
                                .request (std::string (e2e::workflow_channel),
                                         e2e::spot_workflow_request_t{
                                           spot + "-timer-workflow", tick.name})
                                .timeout (std::chrono::seconds (5))
                                .submit<e2e::spot_workflow_reply_t> ();
        (void) workflow;
        _evidence.add ("spot-timer-workflow-reply|spot=" + spot + "|timer=" + tick.name);
        _evidence.add ("spot-timer-end|spot=" + spot + "|timer=" + tick.name
                       + "|sequence=handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end");
        _timer.cancel ();
        co_return;
    }

  private:
    fw::instance_spot_context_t _context;
    evidence_store_t &_evidence;
    fw::timer_t _timer;
};

class config12_timer_handler_t
{
  public:
    fw::task_t<void> handle (config12_instance_spot_t &spot,
                             const fw::timer_tick_t &tick) const
    {
        co_await spot.handle_timer (tick);
    }
};

class request_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::channel_client_t, fw::route_client_t>;

    request_handler_t (fw::channel_client_t &channels, fw::route_client_t &routes) :
        _channels (channels), _routes (routes)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        try {
            const auto body = nlohmann::json::parse (request.body);
            const auto channel = body.at ("channel").get<std::string> ();
            const auto message = e2e::channel_probe_request_t{
              body.at ("id").get<std::string> (), body.value ("mode", "echo")};
            const auto timeout = message.mode == "hold"
                                   ? std::chrono::seconds (20)
                                   : std::chrono::seconds (5);
            e2e::channel_probe_reply_t reply;
            if (channel == e2e::workflow_channel) {
                reply = co_await _channels.request_to_channel (channel, message)
                          .timeout (timeout)
                          .submit<e2e::channel_probe_reply_t> ();
            } else {
                reply = co_await _routes.request_to_channel (channel, message)
                          .timeout (timeout)
                          .submit<e2e::channel_probe_reply_t> ();
            }
            co_return json_response ({ {"succeeded", true}, {"reply", reply} });
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response ({ {"succeeded", false},
                                       {"error", error.what ()},
                                       {"errorKind", static_cast<int> (error.kind ())} },
                                      200);
        }
        catch (const std::exception &error) {
            co_return json_response ({ {"succeeded", false}, {"error", error.what ()} }, 400);
        }
    }

  private:
    fw::channel_client_t &_channels;
    fw::route_client_t &_routes;
};

class send_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::channel_client_t, fw::route_client_t>;

    send_handler_t (fw::channel_client_t &channels, fw::route_client_t &routes) :
        _channels (channels), _routes (routes)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        try {
            const auto body = nlohmann::json::parse (request.body);
            const auto channel = body.at ("channel").get<std::string> ();
            const auto message = e2e::channel_probe_message_t{body.at ("id").get<std::string> ()};
            if (channel == e2e::workflow_channel) {
                co_await _channels.send_to_channel (channel, message).submit ();
            } else {
                co_await _routes.send_to_channel (channel, message).submit ();
            }
            co_return json_response ({ {"succeeded", true} });
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response ({ {"succeeded", false},
                                       {"error", error.what ()},
                                       {"errorKind", static_cast<int> (error.kind ())} },
                                      200);
        }
        catch (const std::exception &error) {
            co_return json_response ({ {"succeeded", false}, {"error", error.what ()} }, 400);
        }
    }

  private:
    fw::channel_client_t &_channels;
    fw::route_client_t &_routes;
};

class evidence_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;
    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}
    fw::http_response_t handle (const fw::http_request_t &) { return json_response (_evidence.snapshot ()); }

  private:
    evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;
    explicit evidence_wait_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto found = request.query_values.find ("contains");
        if (found == request.query_values.end () || found->second.empty ()) {
            return json_response ({ {"error", "contains is required"} }, 400);
        }
        const auto ok = _evidence.wait_for (found->second, std::chrono::seconds (30));
        return json_response (_evidence.snapshot (), ok ? 200 : 408);
    }

  private:
    evidence_store_t &_evidence;
};

class ready_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::route_mesh_runtime_t>;

    explicit ready_handler_t (fw::route_mesh_runtime_t &runtime) : _runtime (runtime) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto target = request.query_values.find ("targetRid");
        if (target == request.query_values.end () || target->second.empty ()) {
            return json_response ({ {"error", "targetRid is required"} }, 400);
        }
        const auto mesh = request.query_values.find ("mesh");
        const auto mesh_name = mesh != request.query_values.end () && mesh->second == "audit"
                                 ? std::string (e2e::audit_mesh)
                                 : std::string (e2e::game_mesh);
        const auto snapshot = _runtime.snapshot (mesh_name);
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == target->second
                && peer.state == fw::peer_state_t::ready) {
                return json_response ({ {"ready", true}, {"targetRid", target->second} });
            }
        }
        return json_response ({ {"ready", false}, {"targetRid", target->second} }, 503);
    }

  private:
    fw::route_mesh_runtime_t &_runtime;
};

fw::listener_kind_t listener_kind (std::string_view value)
{
    if (value == "route_mesh") {
        return fw::listener_kind_t::route_mesh;
    }
    if (value == "client_server") {
        return fw::listener_kind_t::client_server;
    }
    if (value == "fanout") {
        return fw::listener_kind_t::fanout;
    }
    if (value == "stream") {
        return fw::listener_kind_t::stream;
    }
    throw std::invalid_argument ("unknown listener kind: " + std::string (value));
}

class listener_status_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::framework_runtime_t>;

    explicit listener_status_handler_t (fw::framework_runtime_t &runtime) : _runtime (runtime) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto kind = request.query_values.find ("kind");
        const auto name = request.query_values.find ("name");
        if (kind == request.query_values.end () || kind->second.empty ()
            || name == request.query_values.end () || name->second.empty ()) {
            return json_response ({ {"error", "kind and name are required"} }, 400);
        }
        try {
            const auto status = _runtime.listener_status (listener_kind (kind->second), name->second);
            return json_response ({ {"kind", kind->second},
                                    {"name", status.name},
                                    {"endpoint", status.endpoint},
                                    {"ready", true} });
        }
        catch (const fw::framework_exception_t &error) {
            return json_response ({ {"ready", false},
                                    {"error", error.what ()},
                                    {"errorKind", static_cast<int> (error.kind ())} },
                                   404);
        }
        catch (const std::exception &error) {
            return json_response ({ {"ready", false}, {"error", error.what ()} }, 400);
        }
    }

  private:
    fw::framework_runtime_t &_runtime;
};

class client_server_status_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::client_server_runtime_t>;

    explicit client_server_status_handler_t (fw::client_server_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto channel = request.query_values.find ("channel");
        if (channel == request.query_values.end () || channel->second.empty ()) {
            return json_response ({ {"error", "channel is required"} }, 400);
        }
        try {
            const auto snapshot = _runtime.snapshot (channel->second);
            nlohmann::json servers = nlohmann::json::array ();
            for (const auto &server : snapshot.servers) {
                servers.push_back ({
                  {"rid", server.server_rid.to_string ()},
                  {"state", static_cast<int> (server.state)},
                  {"weight", server.weight},
                  {"ready", server.ready}});
            }
            return json_response ({ {"channel", snapshot.channel_name},
                                    {"selectable", snapshot.selectable},
                                    {"readyServerCount", snapshot.ready_server_count},
                                    {"servers", std::move (servers)} });
        }
        catch (const std::exception &error) {
            return json_response ({ {"error", error.what ()} }, 404);
        }
    }

  private:
    fw::client_server_runtime_t &_runtime;
};

class hold_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<role_state_t>;
    explicit hold_handler_t (role_state_t &state) : _state (state) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        _state.hold ();
        return json_response ({ {"status", "held"} });
    }

  private:
    role_state_t &_state;
};

class release_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<role_state_t>;
    explicit release_handler_t (role_state_t &state) : _state (state) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        _state.release ();
        return json_response ({ {"status", "released"} });
    }

  private:
    role_state_t &_state;
};

class shutdown_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<shutdown_state_t>;
    explicit shutdown_handler_t (shutdown_state_t &state) : _state (state) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        _state.request ();
        return json_response ({ {"status", "stopping"} });
    }

  private:
    shutdown_state_t &_state;
};

class spot_workflow_http_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::route_client_t>;
    explicit spot_workflow_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body);
        const auto id = body.at ("id").get<std::string> ();
        const auto spot_id = body.at ("spotId").get<std::string> ();
        try {
            const auto reply = co_await _routes
                                 .request_to_spot (spot_id,
                                                   e2e::spot_workflow_request_t{id, id + "-timer"})
                                 .instance_spot (std::string (e2e::spot_type))
                                 .in_mesh (std::string (e2e::game_mesh))
                                 .timeout (std::chrono::seconds (5))
                                 .submit<e2e::spot_workflow_reply_t> ();
            co_return json_response ({ {"succeeded", true}, {"reply", reply} });
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response ({ {"succeeded", false},
                                       {"error", error.what ()},
                                       {"errorKind", static_cast<int> (error.kind ())} });
        }
    }

  private:
    fw::route_client_t &_routes;
};

void configure_route_channels (fw::mesh_node_builder_t &mesh,
                               fw::service_collection_t &services,
                               bool &route_services_registered,
                               const std::vector<std::string> &servers,
                               const std::vector<std::string> &clients)
{
    if (!servers.empty ()) {
        if (!route_services_registered) {
            services.add_transient<route_channel_probe_handler_t,
                                   evidence_store_t,
                                   role_state_t,
                                   role_options_t,
                                   fw::channel_client_t,
                                   fw::route_client_t> ();
            services.add_transient<route_channel_probe_send_handler_t, evidence_store_t> ();
            route_services_registered = true;
        }
    }
    for (const auto &channel : servers) {
        auto server = mesh.channel_name (channel).server ();
        server.add_request_handler<route_channel_probe_handler_t,
                                   e2e::channel_probe_request_t,
                                   e2e::channel_probe_reply_t> ();
        server.add_send_handler<route_channel_probe_send_handler_t,
                                e2e::channel_probe_message_t> ();
    }
    for (const auto &channel : clients) {
        mesh.channel_name (channel).client ();
    }
}

} // namespace

int main (int argc, char **argv)
{
    try {
        auto app = fw::app_t::create ();
        app.config ().load_cli (argc, argv);
        const auto path = app.config ().model ().get ("config");
        if (!path) {
            throw std::runtime_error ("ChannelEgressRouting role requires --config=<path>");
        }
        app.config ().load_json (*path);
        const auto options = app.config ().bind_required<role_options_t> ("e2e");
        std::filesystem::create_directories (options.log_dir);
        app.logging ().use_file (options.log_dir + "/" + options.rid + ".log")
          .set_min_level (fw::log_level_t::debug);

        std::atomic<shutdown_state_t *> shutdown_pointer {nullptr};
        app.add_zlink_framework ([options, &shutdown_pointer] (fw::zlink_framework_options_t &framework) {
            auto evidence = std::make_unique<evidence_store_t> (
              options.role, options.rid, options.instance_marker, options.evidence_file);
            auto *evidence_ptr = evidence.get ();
            framework.services ().add_singleton<evidence_store_t> (std::move (evidence));
            auto role_state = std::make_unique<role_state_t> ();
            framework.services ().add_singleton<role_state_t> (std::move (role_state));
            framework.services ().add_singleton<role_options_t> (
              std::make_unique<role_options_t> (options));
            auto shutdown = std::make_unique<shutdown_state_t> ();
            auto *shutdown_ptr = shutdown.get ();
            shutdown_pointer.store (shutdown_ptr, std::memory_order_release);
            framework.services ().add_singleton<shutdown_state_t> (std::move (shutdown));
            bool route_services_registered = false;

            if (!options.redis_endpoint.empty ()) {
                framework.add_location_store (
                  std::make_shared<fw::redis::redis_location_store_t> (
                    fw::redis::redis_location_options_t{
                      .connection_string = options.redis_endpoint,
                      .key_prefix = options.redis_key_prefix}));
                framework.add_relocation_store (
                  std::make_shared<fw::redis::redis_relocation_store_t> (
                    fw::redis::redis_relocation_options_t{
                      .connection_string = options.redis_endpoint,
                      .key_prefix = options.redis_key_prefix + ":relocation"}));
                auto &locations = framework.configure_locations ();
                locations.polling_interval = std::chrono::milliseconds (100);
                locations.owner_lease_renew_interval = std::chrono::milliseconds (100);
                locations.owner_lease_ttl = std::chrono::seconds (5);
                locations.owner_lease_fencing_margin = std::chrono::milliseconds (500);
                locations.owner_lease_renew_timeout = std::chrono::milliseconds (500);
            }

            if (!options.game_endpoint.empty ()) {
                auto game = framework.add_route_mesh (std::string (e2e::game_mesh))
                              .listen (options.game_endpoint)
                              .set_routing_id (zlink::routing_id_t::from (options.rid));
                for (const auto &peer : split_list (options.game_peers)) {
                    game.peer_connections ().connect (peer);
                }
                configure_route_channels (game, framework.services (), route_services_registered,
                                          split_list (options.game_servers),
                                          split_list (options.game_clients));
                if (options.role == "play") {
                    game.channel_name (std::string (e2e::game_mesh)).server ();
                    game.set_object_role (fw::object_role_t::server);
                    game.add_instance_spot_factory<config12_instance_spot_t> (
                      std::string (e2e::spot_type),
                      [evidence_ptr] (fw::instance_spot_context_t context) {
                          return std::make_shared<config12_instance_spot_t> (
                            std::move (context), *evidence_ptr);
                      },
                      [] (auto &factory) { factory.disable_relocation (); });
                } else if (options.role == "spot-caller") {
                    game.channel_name (std::string (e2e::game_mesh)).client ();
                    game.set_object_role (fw::object_role_t::client);
                }
            }

            if (!options.audit_endpoint.empty ()) {
                auto audit = framework.add_route_mesh (std::string (e2e::audit_mesh))
                               .listen (options.audit_endpoint)
                               .set_routing_id (zlink::routing_id_t::from (options.rid + "-audit"));
                for (const auto &peer : split_list (options.audit_peers)) {
                    audit.peer_connections ().connect (peer);
                }
                configure_route_channels (audit, framework.services (), route_services_registered,
                                          split_list (options.audit_servers),
                                          split_list (options.audit_clients));
            }

            const auto workflow_servers = split_list (options.workflow_servers);
            const auto workflow_clients = split_list (options.workflow_clients);
            if (!workflow_servers.empty () || !workflow_clients.empty ()
                || !options.workflow_endpoint.empty ()) {
                auto workflow = framework.add_client_server_channel (
                  std::string (e2e::workflow_channel));
                if (!workflow_clients.empty ()) {
                    workflow.client ();
                }
                if (!workflow_servers.empty () || !options.workflow_endpoint.empty ()) {
                    auto server = workflow.server ();
                    server.set_bind_host ("127.0.0.1")
                      .set_advertise_host ("127.0.0.1")
                      .listen (endpoint_port (options.workflow_endpoint))
                      .set_weight (options.workflow_weight)
                      .add_request_handler<channel_probe_handler_t,
                                           e2e::channel_probe_request_t,
                                           e2e::channel_probe_reply_t> ()
                      .add_request_handler<workflow_spot_handler_t,
                                           e2e::spot_workflow_request_t,
                                           e2e::spot_workflow_reply_t> ()
                      .add_send_handler<channel_probe_send_handler_t,
                                        e2e::channel_probe_message_t> ();
                }
                if (options.invalid_mode == "duplicate-workflow-client") {
                    workflow.client ();
                }
            }

            if (options.invalid_mode == "route-clientserver-conflict") {
                framework.add_client_server_channel (std::string (e2e::play_channel)).client ();
            }

            framework.http ()
              .listen (options.http_endpoint)
              .map_health ("/health")
              .map_get<ready_handler_t> ("/ready")
              .map_get<listener_status_handler_t> ("/listener-status")
              .map_get<client_server_status_handler_t> ("/client-status")
              .map_get<evidence_handler_t> ("/evidence")
              .map_get<evidence_wait_handler_t> ("/evidence/wait")
              .map_post<request_handler_t> ("/request")
              .map_post<send_handler_t> ("/send")
              .map_post<hold_handler_t> ("/control/hold")
              .map_post<release_handler_t> ("/control/release")
              .map_post<shutdown_handler_t> ("/shutdown");
            if (options.role == "play" || options.role == "spot-caller") {
                framework.http ().map_post<spot_workflow_http_handler_t> ("/spot/workflow");
            }
        });

        std::thread watcher ([&app, &shutdown_pointer] {
            shutdown_state_t *state = nullptr;
            while ((state = shutdown_pointer.load (std::memory_order_acquire)) == nullptr) {
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            }
            while (!state->requested ()) {
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
            }
            app.request_stop ();
        });
        const int code = app.run (argc, argv);
        if (auto *state = shutdown_pointer.load (std::memory_order_acquire)) {
            state->request ();
        }
        watcher.join ();
        return code;
    }
    catch (const std::exception &error) {
        std::cerr << "channel-egress role failed: " << error.what () << "\n";
        return 2;
    }
}
