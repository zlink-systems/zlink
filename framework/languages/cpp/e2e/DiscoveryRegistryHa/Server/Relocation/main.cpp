/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/* Track F node role for the DiscoveryRegistryHa (Config 6) harness.
 *
 * Tracks A-E (Server/Provider, Server/Consumer) test discovery/messaging
 * only and never place a stateful Actor/Spot, so SF-F2, SF-F3, SF-F7 and
 * SF-F11 need a separate relocation-capable participant. This role mirrors
 * the proven pattern in e2e/SpotActorTransfer/Server/ActorNode/main.cpp
 * (entry spot + user spot + a generic Actor whose relocation adapter throws
 * or gates by actor type), trimmed to only what these four scenarios need:
 * no stream/bound-session, no message-follow evidence parsing, no
 * empty-state/no-adapter/fail-leave actor type variants. Direct-transfer
 * only -- base/delta capture was removed from the product this session and
 * nothing here depends on it. */

#include "../../Shared/relocation_contracts.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace e2e = zlink::e2e::store_failure_relocation;
namespace fw = zlink::framework;

namespace
{

struct node_options_t
{
    std::string rid;
    std::string http_url;
    std::string router_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    /* The Relocation Store's own Redis, separate from the Location Store's,
     * so SF-F3 can take only it down. */
    std::string relocation_redis_endpoint;
    std::string relocation_redis_key_prefix;
    std::string log_dir;
    std::string evidence_file;
    std::uint64_t relocation_payload_chunk_limit_bytes = 262144;
    std::uint64_t relocation_in_flight_payload_budget_bytes = 16777216;

    static node_options_t bind (const fw::configuration_section_t &section)
    {
        return {.rid = section.require ("rid"),
                .http_url = section.require ("httpUrl"),
                .router_endpoint = section.require ("routerEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .relocation_redis_endpoint = section.require ("relocationRedis.endpoint"),
                .relocation_redis_key_prefix = section.require ("relocationRedis.keyPrefix"),
                .log_dir = section.require ("logDir"),
                .evidence_file = section.require ("evidenceFile"),
                .relocation_payload_chunk_limit_bytes = section.get ("relocationChunkLimitBytes")
                  ? static_cast<std::uint64_t> (
                      std::stoull (*section.get ("relocationChunkLimitBytes")))
                  : std::uint64_t{262144},
                .relocation_in_flight_payload_budget_bytes =
                  section.get ("relocationInFlightBudgetBytes")
                    ? static_cast<std::uint64_t> (
                        std::stoull (*section.get ("relocationInFlightBudgetBytes")))
                    : std::uint64_t{16777216}};
    }
};

class evidence_store_t
{
  public:
    evidence_store_t (std::string node_rid, std::string evidence_file) :
        _node_rid (std::move (node_rid)), _evidence_file (std::move (evidence_file))
    {
    }

    const std::string &node_rid () const noexcept { return _node_rid; }

    void add (std::string scenario, std::string actor_id, std::string kind, std::string value)
    {
        e2e::relocation_evidence_t entry{std::move (scenario), std::move (actor_id),
                                         std::move (kind), std::move (value), _node_rid};
        const auto line = e2e::evidence_text (entry);
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _entries.push_back (entry);
            if (!_evidence_file.empty ()) {
                std::ofstream file (_evidence_file, std::ios::app);
                file << line << '\n';
            }
        }
        _changed.notify_all ();
    }

    std::vector<e2e::relocation_evidence_t> snapshot () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _entries;
    }

    std::vector<e2e::relocation_evidence_t>
    wait_until_contains_all (const std::vector<std::string> &expected,
                             std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _changed.wait_for (lock, timeout, [&] { return contains_all_locked (expected); });
        return _entries;
    }

  private:
    bool contains_all_locked (const std::vector<std::string> &expected) const
    {
        for (const auto &needle : expected) {
            bool found = false;
            for (const auto &entry : _entries) {
                if (e2e::evidence_text (entry).find (needle) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }

    std::string _node_rid;
    std::string _evidence_file;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<e2e::relocation_evidence_t> _entries;
};

/* Named gates the client releases over HTTP. wait() blocks the calling
 * (adapter capture) thread -- SF-F2's long-running variant holds capture
 * open past several owner-lease renew intervals, then the client releases
 * it explicitly. Same pattern as SpotActorTransfer's transfer_gate_store_t. */
class capture_gate_store_t
{
  public:
    void wait (const std::string &key)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _changed.wait (lock, [&] { return _released.count (key) != 0; });
    }

    bool release (const std::string &key)
    {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (!_released.insert (key).second)
                return false;
        }
        _changed.notify_all ();
        return true;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::set<std::string> _released;
};

class shutdown_flag_t
{
  public:
    std::atomic<bool> requested{false};
};

evidence_store_t *g_evidence = nullptr;
capture_gate_store_t *g_capture_gates = nullptr;

/* A single generic Actor class; behavior is selected at creation time by
 * actor_type (df-stateful / df-fail-transfer-out / df-fail-transfer-in /
 * df-hold-capture), the same convention SpotActorTransfer uses for its
 * transfer-fail-* types. */
class df_actor_t final : public fw::actor_t
{
  public:
    explicit df_actor_t (fw::actor_context_t context) : _context (std::move (context)) {}

    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }

    std::string actor_id;
    std::string actor_type = e2e::actor_type_stateful;
    std::vector<std::uint8_t> payload;

    fw::task_t<void> on_join_completed (const fw::actor_join_completion_t &completion) override
    {
        if (const auto *accepted = std::get_if<fw::actor_join_accepted_t> (&completion)) {
            std::string scenario;
            if (accepted->reply)
                scenario = accepted->reply->decode<e2e::relocate_res_t> ().scenario;
            g_evidence->add (scenario, actor_id, "join_completion_accepted", "");
        } else if (const auto *rejected = std::get_if<fw::actor_join_rejected_t> (&completion)) {
            std::string scenario;
            if (rejected->reply)
                scenario = rejected->reply->decode<e2e::relocate_res_t> ().scenario;
            g_evidence->add (scenario, actor_id, "join_completion_rejected", "");
        } else {
            const auto &failed = std::get<fw::actor_join_failed_t> (completion);
            g_evidence->add ("", actor_id, "join_completion_failed",
                             std::to_string (static_cast<int> (failed.error_kind)));
        }
        co_return;
    }

  private:
    fw::actor_context_t _context;
};

class df_actor_factory_t final : public fw::actor_factory_t<df_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<df_actor_t>> create (fw::actor_context_t context,
                                                    std::stop_token) override
    {
        auto actor = std::make_shared<df_actor_t> (std::move (context));
        actor->actor_id = std::string (actor->context ().actor_ref ().actor_id ().value ());
        co_return actor;
    }
};

class df_actor_adapter_t : public fw::actor_relocation_adapter_t<df_actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>> capture (df_actor_t &actor, std::stop_token) override
    {
        if (actor.actor_type == e2e::actor_type_fail_transfer_out) {
            g_evidence->add ("", actor.actor_id, "transfer_out_failed", "");
            throw std::runtime_error ("injected direct-transfer failure before relay-ready");
        }
        if (actor.actor_type == e2e::actor_type_hold_capture) {
            g_evidence->add ("", actor.actor_id, "capture_held", "");
            g_capture_gates->wait (actor.actor_id);
            g_evidence->add ("", actor.actor_id, "capture_released", "");
        }
        g_evidence->add ("", actor.actor_id, "transfer_out",
                         std::to_string (actor.payload.size ()));
        std::vector<std::byte> bytes;
        bytes.reserve (actor.payload.size ());
        for (const auto value : actor.payload)
            bytes.push_back (static_cast<std::byte> (value));
        co_return bytes;
    }

    fw::task_t<void>
    restore (df_actor_t &actor, std::vector<std::byte> payload, std::stop_token) override
    {
        /* actor.actor_type is not yet known here -- the target's placeholder
         * instance always starts at the df_actor_t default (df-stateful)
         * regardless of which registered type is relocating in, the same
         * reason SpotActorTransfer's transfer_actor_adapter_t keys its
         * fail-transfer-in check off the actor_id naming convention instead
         * of actor.actor_type. */
        if (actor.actor_id.rfind ("actor-df-fail-in-", 0) == 0) {
            g_evidence->add ("", actor.actor_id, "transfer_in_failed", "");
            throw std::runtime_error ("injected explicit target failure before relay-ready");
        }
        actor.payload.clear ();
        actor.payload.reserve (payload.size ());
        for (const auto value : payload)
            actor.payload.push_back (static_cast<std::uint8_t> (value));
        actor.actor_type = e2e::actor_type_stateful;
        g_evidence->add ("", actor.actor_id, "transfer_in",
                         std::to_string (actor.payload.size ()));
        co_return;
    }
};

e2e::relocate_res_t make_relocate_reply (const std::string &scenario,
                                        const std::string &actor_id,
                                        bool accepted,
                                        const std::string &target_spot_id)
{
    return e2e::relocate_res_t{scenario, actor_id, accepted, g_evidence->node_rid (),
                              target_spot_id};
}

class df_entry_spot_t : public fw::entry_spot_t<df_actor_t>
{
  public:
    explicit df_entry_spot_t (fw::entry_spot_context_t context) : _context (std::move (context))
    {
    }

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&df_entry_spot_t::relocate> (
          e2e::relocate_req_t::packet_name);
        _context.handlers ().add_actor_request<&df_entry_spot_t::probe> (
          e2e::probe_state_req_t::packet_name);
    }

    fw::task_t<fw::actor_create_response_t>
    on_create_actor (df_actor_t &actor, const fw::message_t &create_request) override
    {
        if (!create_request.empty ()) {
            const auto request = create_request.decode<e2e::actor_create_req_t> ();
            actor.actor_type = request.actor_type;
            actor.payload = e2e::deterministic_payload (request.payload_length);
        }
        g_evidence->add ("", actor.actor_id, "create",
                         actor.actor_type + ":" + std::to_string (actor.payload.size ()));
        co_return fw::actor_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        g_evidence->add ("", std::string (actor_id), "admission", "entry");
        co_return fw::spot_actor_join_result_t::accept (request);
    }

    fw::task_t<void> on_actor_joined (df_actor_t &actor) override
    {
        g_evidence->add ("", actor.actor_id, "entry_joined", "");
        co_return;
    }

    fw::task_t<void> on_leave_actor (df_actor_t &actor) override
    {
        g_evidence->add ("", actor.actor_id, "entry_leave", "");
        co_return;
    }

    fw::task_t<e2e::relocate_res_t> relocate (df_actor_t &actor,
                                             fw::message_context_t &,
                                             const e2e::relocate_req_t &request)
    {
        actor.context ().join_spot (request.target_spot_id, request)
          .timeout (std::chrono::seconds (20))
          .defer ();
        co_return make_relocate_reply (request.scenario, actor.actor_id, true,
                                       request.target_spot_id);
    }

    e2e::probe_state_res_t
    probe (const df_actor_t &actor, fw::message_context_t &, const e2e::probe_state_req_t &request)
    {
        g_evidence->add (request.scenario, actor.actor_id, "packet_handler", request.marker);
        return e2e::probe_state_res_t{actor.actor_id, g_evidence->node_rid (), request.marker,
                                     actor.payload.size (), e2e::crc32c (actor.payload)};
    }

  private:
    fw::entry_spot_context_t _context;
};

class df_user_spot_t : public fw::spot_t<df_actor_t>
{
  public:
    explicit df_user_spot_t (fw::spot_context_t context) : _context (std::move (context)) {}

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_actor_request<&df_user_spot_t::probe> (
          e2e::probe_state_req_t::packet_name);
    }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view actor_id,
                                                            const fw::message_t &request) override
    {
        g_evidence->add ("", std::string (actor_id), "admission",
                         "spot=" + _context.spot_id ());
        co_return fw::spot_actor_join_result_t::accept (request);
    }

    fw::task_t<void> on_actor_joined (df_actor_t &actor) override
    {
        g_evidence->add ("", actor.actor_id, "joined",
                         _context.spot_id () + ":" + std::to_string (actor.payload.size ()));
        co_return;
    }

    fw::task_t<void> on_leave_actor (df_actor_t &actor) override
    {
        g_evidence->add ("", actor.actor_id, "target_leave", _context.spot_id ());
        co_return;
    }

    e2e::probe_state_res_t
    probe (const df_actor_t &actor, fw::message_context_t &, const e2e::probe_state_req_t &request)
    {
        g_evidence->add (request.scenario, actor.actor_id, "packet_handler", request.marker);
        return e2e::probe_state_res_t{actor.actor_id, g_evidence->node_rid (), request.marker,
                                     actor.payload.size (), e2e::crc32c (actor.payload)};
    }

  private:
    fw::spot_context_t _context;
};

nlohmann::json parse_body (const fw::http_request_t &request)
{
    return request.body.empty () ? nlohmann::json::object () : nlohmann::json::parse (request.body);
}

fw::http_response_t json_response (const nlohmann::json &body, int status = 200)
{
    fw::http_response_t response;
    response.status = status;
    response.body = body.dump ();
    return response;
}

std::string route_value (const fw::http_request_t &request, const char *key)
{
    const auto found = request.route_values.find (key);
    return found != request.route_values.end () ? found->second : std::string ();
}

std::string error_kind_name (const fw::framework_exception_t &error)
{
    switch (error.kind ()) {
    case fw::framework_error_kind_t::not_found:
        return "not_found";
    case fw::framework_error_kind_t::rejected:
        return "rejected";
    case fw::framework_error_kind_t::deadline_exceeded:
        return "deadline_exceeded";
    default:
        return "internal_failure";
    }
}

class evidence_handler_t
{
  public:
    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        return json_response (nlohmann::json (_evidence.snapshot ()));
    }

  private:
    evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    explicit evidence_wait_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}
    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto contains = parse_body (request).value ("contains", std::vector<std::string>{});
        const auto entries = _evidence.wait_until_contains_all (contains, std::chrono::seconds (20));
        return json_response (nlohmann::json (entries));
    }

  private:
    evidence_store_t &_evidence;
};

class capture_gate_release_handler_t
{
  public:
    explicit capture_gate_release_handler_t (capture_gate_store_t &gates) : _gates (gates) {}
    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto actor_id = route_value (request, "actorId");
        return json_response (
          nlohmann::json (e2e::gate_release_res_t{actor_id, _gates.release (actor_id)}));
    }

  private:
    capture_gate_store_t &_gates;
};

class create_spot_handler_t
{
  public:
    explicit create_spot_handler_t (fw::spot_manager_t &spots) : _spots (spots) {}
    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto request = parse_body (http_request).get<e2e::create_spot_req_t> ();
        const auto created = co_await _spots.get_or_create (request.spot_id, "df-user")
                               .creation_request (request)
                               .async ();
        co_return json_response (nlohmann::json (e2e::create_spot_res_t{
          request.spot_id, std::string (created.spot.node_rid ().value ())}));
    }

  private:
    fw::spot_manager_t &_spots;
};

class close_spot_handler_t
{
  public:
    explicit close_spot_handler_t (fw::spot_manager_t &spots) : _spots (spots) {}
    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto spot_id = route_value (request, "spotId");
        const auto found = co_await _spots.find (spot_id);
        const auto closed = found && co_await _spots.close (*found);
        co_return json_response (nlohmann::json{{"closed", closed}});
    }

  private:
    fw::spot_manager_t &_spots;
};

class create_actor_handler_t
{
  public:
    create_actor_handler_t (fw::actor_manager_t &actor_manager, evidence_store_t &evidence) :
        _actor_manager (actor_manager), _evidence (evidence)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto request = parse_body (http_request).get<e2e::actor_create_req_t> ();
        _evidence.add ("", request.actor_id, "create_requested", request.actor_type);
        auto created = co_await _actor_manager
                        .get_or_create (fw::actor_id_t (request.actor_id), request.actor_type)
                        .creation_request (request)
                        .async ();
        const auto ref = std::visit (
          [] (const auto &result) -> fw::actor_ref_t {
              using result_t = std::decay_t<decltype (result)>;
              if constexpr (std::is_same_v<result_t, fw::actor_create_rejected_t>) {
                  throw fw::framework_exception_t (fw::framework_error_kind_t::rejected,
                                                   "Actor creation was rejected");
              } else {
                  return result.actor;
              }
          },
          created);
        co_return json_response (nlohmann::json (e2e::actor_create_res_t{
          request.actor_id, request.actor_type, std::string (ref.node_rid ().value ()),
          static_cast<std::int64_t> (ref.object_generation ())}));
    }

  private:
    fw::actor_manager_t &_actor_manager;
    evidence_store_t &_evidence;
};

class relocate_actor_handler_t
{
  public:
    relocate_actor_handler_t (fw::actor_client_t &actors, evidence_store_t &evidence) :
        _actors (actors), _evidence (evidence)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::relocate_req_t> ();
        try {
            auto result = co_await _actors.request (fw::actor_id_t (actor_id), request)
                            .timeout (std::chrono::seconds (20))
                            .async<e2e::relocate_res_t> ();
            co_return json_response (nlohmann::json (result));
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response (
              nlohmann::json{{"error", error_kind_name (error)}, {"message", error.what ()}}, 409);
        }
    }

  private:
    fw::actor_client_t &_actors;
    evidence_store_t &_evidence;
};

class probe_actor_handler_t
{
  public:
    explicit probe_actor_handler_t (fw::actor_client_t &actors) : _actors (actors) {}
    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::probe_state_req_t> ();
        try {
            auto response = co_await _actors.request (fw::actor_id_t (actor_id), request)
                              .timeout (std::chrono::seconds (10))
                              .async<e2e::probe_state_res_t> ();
            co_return json_response (nlohmann::json (response));
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response (
              nlohmann::json{{"error", error_kind_name (error)}, {"message", error.what ()}}, 409);
        }
    }

  private:
    fw::actor_client_t &_actors;
};

class actor_ref_handler_t
{
  public:
    explicit actor_ref_handler_t (fw::actor_directory_t &directory) : _directory (directory) {}
    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto ref = co_await _directory.find (actor_id);
        if (!ref)
            co_return json_response (nlohmann::json{{"error", "not_found"}}, 404);
        co_return json_response (nlohmann::json{
          {"actorId", actor_id},
          {"nodeRid", std::string (ref->node_rid ().value ())},
          {"generation", static_cast<std::int64_t> (ref->object_generation ())}});
    }

  private:
    fw::actor_directory_t &_directory;
};

class shutdown_handler_t
{
  public:
    explicit shutdown_handler_t (shutdown_flag_t &flag) : _flag (flag) {}
    fw::http_response_t handle (const fw::http_request_t &)
    {
        _flag.requested.store (true);
        return json_response (nlohmann::json{{"status", "stopping"}});
    }

  private:
    shutdown_flag_t &_flag;
};

} // namespace

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path)
        throw std::runtime_error ("DiscoveryRegistryHa relocation node requires --config=<path>");
    app.config ().load_json (*config_path);
    const auto configured = app.config ().bind_required<node_options_t> ("e2e");
    std::filesystem::create_directories (configured.log_dir);

    auto evidence = std::make_unique<evidence_store_t> (configured.rid, configured.evidence_file);
    auto capture_gates = std::make_unique<capture_gate_store_t> ();
    auto shutdown_flag = std::make_unique<shutdown_flag_t> ();
    g_evidence = evidence.get ();
    g_capture_gates = capture_gates.get ();

    app.logging ().use_file (configured.log_dir + "/" + configured.rid + ".app.log");
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &framework) {
        framework.services ().add_singleton<evidence_store_t> (std::move (evidence));
        framework.services ().add_singleton<capture_gate_store_t> (std::move (capture_gates));
        framework.services ().add_singleton<shutdown_flag_t> (std::move (shutdown_flag));
        framework.set_default_request_timeout (std::chrono::seconds (20));
        framework.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);

        framework.add_location_store (
          std::make_shared<fw::redis::redis_location_store_t> (fw::redis::redis_location_options_t{
            .connection_string = configured.redis_endpoint,
            .key_prefix = configured.redis_key_prefix}));
        framework.add_relocation_store (
          std::make_shared<fw::redis::redis_relocation_store_t> (fw::redis::redis_relocation_options_t{
            .connection_string = configured.relocation_redis_endpoint,
            .key_prefix = configured.relocation_redis_key_prefix}));

        auto &locations = framework.configure_locations ();
        locations.owner_lease_renew_interval = std::chrono::seconds (1);
        locations.owner_lease_ttl = std::chrono::seconds (3);
        locations.owner_lease_fencing_margin = std::chrono::milliseconds (500);
        locations.owner_lease_renew_timeout = std::chrono::milliseconds (500);
        locations.polling_interval = std::chrono::milliseconds (500);
        /* SF-F7: small chunk limit / in-flight budget so KB-scale fixtures
         * exercise the real chunk and budget boundaries instead of needing
         * multi-megabyte payloads. */
        locations.relocation_payload_chunk_limit_bytes =
          configured.relocation_payload_chunk_limit_bytes;
        locations.relocation_in_flight_payload_budget_bytes =
          configured.relocation_in_flight_payload_budget_bytes;

        auto mesh = framework.add_route_mesh (e2e::mesh_name);
        mesh.listen (configured.router_endpoint)
          .set_routing_id (zlink::routing_id_t::from (configured.rid));
        mesh.channel_name (e2e::mesh_name).server ();
        mesh.add_entry_spot<df_entry_spot_t> (
              [] (fw::entry_spot_context_t context) {
                  return std::make_shared<df_entry_spot_t> (std::move (context));
              })
          .add_spot_factory<df_user_spot_t> (
            "df-user",
            [] (fw::spot_context_t context) {
                return std::make_shared<df_user_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<df_actor_t, df_actor_factory_t> (
            e2e::actor_type_stateful, std::make_shared<df_actor_factory_t> (),
            [] (auto &factory) { factory.template preserve_state_with<df_actor_adapter_t> (); })
          .add_actor_factory<df_actor_t, df_actor_factory_t> (
            e2e::actor_type_fail_transfer_out, std::make_shared<df_actor_factory_t> (),
            [] (auto &factory) { factory.template preserve_state_with<df_actor_adapter_t> (); })
          .add_actor_factory<df_actor_t, df_actor_factory_t> (
            e2e::actor_type_fail_transfer_in, std::make_shared<df_actor_factory_t> (),
            [] (auto &factory) { factory.template preserve_state_with<df_actor_adapter_t> (); })
          .add_actor_factory<df_actor_t, df_actor_factory_t> (
            e2e::actor_type_hold_capture, std::make_shared<df_actor_factory_t> (),
            [] (auto &factory) { factory.template preserve_state_with<df_actor_adapter_t> (); });

        framework.http ()
          .configure_server ([] (fw::http_server_options_builder_t &server) {
              server.set_write_timeout (std::chrono::seconds (25));
              server.set_keep_alive_timeout (std::chrono::minutes (5));
              server.set_max_keep_alive_requests (100000);
          })
          .listen (configured.http_url)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait")
          .map_post<capture_gate_release_handler_t> ("/capture-gates/{actorId}/release")
          .map_post<create_spot_handler_t> ("/spots")
          .map_post<close_spot_handler_t> ("/spots/{spotId}/close")
          .map_post<create_actor_handler_t> ("/actors")
          .map_post<relocate_actor_handler_t> ("/actors/{actorId}/relocate")
          .map_post<probe_actor_handler_t> ("/actors/{actorId}/probe")
          .map_get<actor_ref_handler_t> ("/actors/{actorId}/ref")
          .map_post<shutdown_handler_t> ("/shutdown");
    });
    return app.run (argc, argv);
}
