/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../../Shared/messages.hpp"
#include "../Shared/node_host.hpp"

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
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace e2e = zlink::e2e::spot_actor_transfer;
namespace fw = zlink::framework;
using transfer_host_role_t =
  zlink::framework::e2e::spot_actor_transfer::server::host_role_t;

namespace
{

struct node_options_t
{
    std::string initial_actor_node;
    std::string rid;
    std::string http_url;
    std::string router_endpoint;
    std::string stream_endpoint;
    std::string pub_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::string log_dir;
    std::string evidence_file;

    static node_options_t bind (const fw::configuration_section_t &section)
    {
        return {.initial_actor_node = section.require ("initialActorNode"),
                .rid = section.require ("rid"),
                .http_url = section.require ("httpUrl"),
                .router_endpoint = section.require ("routerEndpoint"),
                .stream_endpoint = section.require ("streamEndpoint"),
                .pub_endpoint = section.require ("pubEndpoint"),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .log_dir = section.require ("logDir"),
                .evidence_file = section.require ("evidenceFile")};
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
        e2e::actor_evidence_t entry{std::move (scenario), std::move (actor_id), std::move (kind),
                                    std::move (value), _node_rid};
        append (std::move (entry));
    }

    void add_flow (const fw::message_flow_event_t &event)
    {
        if (!event.packet_name || !event.actor_id || !event.correlation_id
            || !event.flow_id) {
            return;
        }
        static const std::set<std::string> transfer_markers{
          "commit_request", "location_committed", "commit_ack", "source_cleanup",
          "pending_admission_expired", "message_follow_registered",
          "message_follow_route_removed", "message_follow_relay", "message_follow_expired",
          "handoff_backlog",
          "backlog_enqueued", "handoff_request_frame", "backlog_request_frame"};
        if (!transfer_markers.contains (*event.packet_name)) {
            return;
        }
        auto value = *event.correlation_id;
        if (*event.packet_name == "message_follow_registered" && event.channel_name) {
            value = *event.channel_name;
            if (event.spot_id) {
                value += ":" + *event.spot_id;
            }
        }
        const bool request_frame = *event.packet_name == "handoff_request_frame"
                                   || *event.packet_name == "backlog_request_frame";
        if (request_frame && event.channel_name) {
            value = *event.channel_name;
        }
        e2e::actor_evidence_t entry{
          "message_flow", *event.actor_id, *event.packet_name, std::move (value), _node_rid,
          request_frame ? *event.flow_id : *event.correlation_id, *event.correlation_id,
          *event.flow_id};
        append (std::move (entry));
    }

  private:
    void append (e2e::actor_evidence_t entry)
    {
        const auto line = e2e::evidence_text (entry);
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _entries.push_back (std::move (entry));
            if (!_evidence_file.empty ()) {
                std::ofstream file (_evidence_file, std::ios::app);
                file << line << '\n';
            }
        }
        _changed.notify_all ();
    }

  public:

    std::vector<e2e::actor_evidence_t> snapshot () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _entries;
    }

    std::vector<e2e::actor_evidence_t> wait_until_contains_all (
      const std::vector<std::string> &expected, std::chrono::milliseconds timeout)
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
            if (!found) {
                return false;
            }
        }
        return true;
    }

    std::string _node_rid;
    std::string _evidence_file;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::vector<e2e::actor_evidence_t> _entries;
};

// Persists per-actor domain state so the empty-state transfer scenario can
// reload it on the target node (the state deliberately does not travel with
// the transfer message).
class domain_state_store_t
{
  public:
    explicit domain_state_store_t (std::string directory) : _directory (std::move (directory)) {}

    void save (const std::string &actor_id, int state_version)
    {
        std::ofstream file (path_for (actor_id), std::ios::trunc);
        file << state_version;
    }

    int load (const std::string &actor_id) const
    {
        std::ifstream file (path_for (actor_id));
        int state_version = 0;
        file >> state_version;
        return state_version;
    }

  private:
    std::string path_for (const std::string &actor_id) const
    {
        return _directory + "/domain-state-" + actor_id + ".txt";
    }

    std::string _directory;
};

// Named gates the client releases over HTTP. wait() intentionally blocks the
// calling (spot serial / adapter) thread: the scenarios use it to hold
// on_actor_joined or transfer_out open until the client observed the pending
// state.
class gate_store_t
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
            if (!_released.insert (key).second) {
                return false;
            }
        }
        _changed.notify_all ();
        return true;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::set<std::string> _released;
};

class joined_gate_store_t : public gate_store_t
{
};

class transfer_gate_store_t : public gate_store_t
{
};

class relocation_store_activity_t
{
  public:
    void record_read () noexcept
    {
        _reads.fetch_add (1, std::memory_order_relaxed);
    }

    void record_write () noexcept
    {
        _writes.fetch_add (1, std::memory_order_relaxed);
    }

    std::uint64_t reads () const noexcept
    {
        return _reads.load (std::memory_order_relaxed);
    }

    std::uint64_t writes () const noexcept
    {
        return _writes.load (std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> _reads{0};
    std::atomic<std::uint64_t> _writes{0};
};

class counting_relocation_store_t final : public fw::relocation_store_t
{
  public:
    counting_relocation_store_t (
      std::shared_ptr<fw::relocation_store_t> inner,
      relocation_store_activity_t &activity) :
        _inner (std::move (inner)), _activity (activity)
    {
    }

    fw::task_t<fw::blob_put_result_t> put (
      fw::blob_reference_t reference,
      std::span<const std::byte> payload,
      std::chrono::milliseconds retention) override
    {
        _activity.record_write ();
        return _inner->put (
          std::move (reference), payload, retention);
    }

    fw::task_t<fw::blob_read_result_t> read (
      fw::blob_reference_t reference) override
    {
        _activity.record_read ();
        return _inner->read (std::move (reference));
    }

    fw::task_t<fw::blob_renew_result_t> renew (
      fw::blob_reference_t reference,
      std::chrono::milliseconds retention) override
    {
        _activity.record_write ();
        return _inner->renew (std::move (reference), retention);
    }

    fw::task_t<void> erase (
      fw::blob_reference_t reference) override
    {
        _activity.record_write ();
        return _inner->erase (std::move (reference));
    }

  private:
    std::shared_ptr<fw::relocation_store_t> _inner;
    relocation_store_activity_t &_activity;
};

// Actor factories/adapters must be default constructible, so the e2e wires
// its singletons through file-scope pointers set once in main.
evidence_store_t *g_evidence = nullptr;
domain_state_store_t *g_domain_state = nullptr;
joined_gate_store_t *g_joined_gates = nullptr;
transfer_gate_store_t *g_transfer_gates = nullptr;
std::string g_initial_actor_node_rid;

class transfer_actor_t final : public fw::actor_t
{
  public:
    explicit transfer_actor_t (fw::actor_context_t context) :
        _context (std::move (context))
    {
    }

    void set_actor_ref (const fw::actor_ref_t &actor_ref)
    {
        actor_id = std::string (actor_ref.actor_id ().value ());
        actor_type = e2e::actor_type_stateful;
    }

    fw::actor_context_t &context () noexcept override
    {
        return _context;
    }

    const fw::actor_context_t &context () const noexcept override
    {
        return _context;
    }

    fw::task_t<void>
    on_join_completed (
      const fw::actor_join_completion_t &completion) override
    {
        if (const auto *accepted =
              std::get_if<fw::actor_join_accepted_t> (
                &completion)) {
            std::string scenario = "deferred-join";
            if (accepted->reply) {
                const auto reply =
                  accepted->reply
                    ->decode<e2e::join_target_res_t> ();
                scenario = reply.scenario;
            }
            g_evidence->add (
              scenario, actor_id, "join_completion_accepted",
              std::to_string (
                accepted->operation_id_high)
                + ":"
                + std::to_string (
                  accepted->operation_id_low));
        } else if (const auto *rejected =
                     std::get_if<
                       fw::actor_join_rejected_t> (
                       &completion)) {
            g_evidence->add (
              "deferred-join", actor_id,
              "join_completion_rejected",
              std::to_string (
                rejected->operation_id_high)
                + ":"
                + std::to_string (
                  rejected->operation_id_low));
        } else {
            const auto &failed =
              std::get<fw::actor_join_failed_t> (
                completion);
            g_evidence->add (
              "deferred-join", actor_id,
              "join_completion_failed",
              std::to_string (
                static_cast<int> (
                  failed.error_kind)));
        }
        co_return;
    }

    std::string actor_id;
    std::string actor_type = e2e::actor_type_stateful;
    int state_version = 0;
  private:
    fw::actor_context_t _context;
};

class transfer_actor_factory_t final :
    public fw::actor_factory_t<transfer_actor_t>
{
  public:
    fw::task_t<std::shared_ptr<transfer_actor_t>>
    create (
      fw::actor_context_t context,
      std::stop_token) override
    {
        const auto actor_id =
          std::string (context.actor_ref ().actor_id ().value ());
        if (g_evidence != nullptr && g_evidence->node_rid () == "actor-b"
            && actor_id.rfind ("actor-no-adapter-", 0) == 0) {
            g_evidence->add ("transfer", actor_id, "transfer_in_empty_default", "actor-factory");
        }
        auto actor =
          std::make_shared<transfer_actor_t> (std::move (context));
        actor->set_actor_ref (
          actor->context ().actor_ref ());
        co_return actor;
    }
};

class transfer_actor_adapter_t
    : public fw::actor_relocation_adapter_t<transfer_actor_t>
{
  public:
    fw::task_t<std::vector<std::byte>>
    capture (transfer_actor_t &actor, std::stop_token) override
    {
        if (actor.actor_type == e2e::actor_type_fail_transfer_out) {
            g_evidence->add ("ST-C3", actor.actor_id, "transfer_out_failed",
                             std::to_string (actor.state_version));
            throw std::runtime_error ("injected transfer out failure");
        }
        if (actor.actor_type == e2e::actor_type_empty_state) {
            g_evidence->add ("transfer", actor.actor_id, "transfer_out_empty", "custom-adapter");
            co_return std::vector<std::byte>{};
        }
        g_evidence->add ("transfer", actor.actor_id, "transfer_out",
                         std::to_string (actor.state_version));
        if (actor.actor_id.rfind ("actor-source-down-before-commit-", 0) == 0) {
            g_evidence->add ("ST-C1", actor.actor_id, "before_commit_gate",
                             std::to_string (actor.state_version));
            g_transfer_gates->wait (actor.actor_id);
        }
        const auto json = nlohmann::json (
          e2e::transfer_state_dto_t{
            actor.actor_id, actor.state_version}).dump ();
        std::vector<std::byte> payload;
        payload.reserve (json.size ());
        for (const auto value : json)
            payload.push_back (static_cast<std::byte> (value));
        co_return payload;
    }

    fw::task_t<void>
    restore (transfer_actor_t &actor,
             std::vector<std::byte> payload,
             std::stop_token) override
    {
        if (payload.empty ()) {
            g_evidence->add ("transfer", actor.actor_id, "transfer_in_empty", "custom-adapter");
            actor.actor_type = e2e::actor_type_empty_state;
            co_return;
        }
        std::string json;
        json.reserve (payload.size ());
        for (const auto value : payload)
            json.push_back (static_cast<char> (value));
        const auto dto =
          nlohmann::json::parse (json)
            .get<e2e::transfer_state_dto_t> ();
        if (actor.actor_id.rfind ("actor-fail-transfer-in-", 0) == 0) {
            g_evidence->add ("ST-C3", actor.actor_id, "transfer_in_failed",
                             std::to_string (dto.state_version));
            throw std::runtime_error ("injected transfer in failure");
        }
        actor.actor_type = e2e::actor_type_stateful;
        actor.state_version = dto.state_version;
        g_evidence->add ("transfer", actor.actor_id, "transfer_in", std::to_string (actor.state_version));
        co_return;
    }
};

e2e::join_target_res_t make_join_reply (const std::string &scenario,
                                        const std::string &actor_id,
                                        bool accepted,
                                        const std::string &target_spot_id)
{
    return e2e::join_target_res_t{scenario, actor_id, accepted, std::string{}, target_spot_id, 0,
                                  std::string{}};
}

class transfer_entry_spot_t : public fw::entry_spot_t<transfer_actor_t>
{
  public:
    explicit transfer_entry_spot_t (fw::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }

    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_request<&transfer_entry_spot_t::join_target> (
          e2e::join_target_req_t::packet_name);
        _context.handlers ().add_actor_request<&transfer_entry_spot_t::bound_push> (
          e2e::bound_push_req_t::packet_name);
        _context.handlers ().add_actor_request<&transfer_entry_spot_t::probe> (
          e2e::probe_req_t::packet_name);
    }

    fw::task_t<fw::actor_create_response_t>
    on_create_actor (transfer_actor_t &actor,
                     const fw::message_t &create_request) override
    {
        if (!create_request.empty ()) {
            const auto request = create_request.decode<e2e::actor_create_req_t> ();
            actor.actor_type = request.actor_type;
            actor.state_version = request.state_version;
            if (actor.actor_type == e2e::actor_type_empty_state) {
                g_domain_state->save (actor.actor_id, actor.state_version);
            }
        }
        g_evidence->add ("create", actor.actor_id, "create",
                         actor.actor_type + ":" + std::to_string (actor.state_version));
        co_return fw::actor_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const fw::message_t &request) override
    {
        g_evidence->add ("local", std::string (actor_id), "admission", "actor-id-only");
        co_return fw::spot_actor_join_result_t::accept (request);
    }

    fw::task_t<void> on_actor_joined (transfer_actor_t &actor) override
    {
        g_evidence->add ("local", actor.actor_id, "entry_joined",
                         std::to_string (actor.state_version));
        co_return;
    }

    fw::task_t<void> on_leave_actor (transfer_actor_t &actor) override
    {
        if (actor.actor_type == e2e::actor_type_no_adapter) {
            g_evidence->add ("transfer", actor.actor_id, "transfer_out_empty_default",
                             "no-adapter");
        }
        if (actor.actor_type == e2e::actor_type_fail_leave) {
            g_evidence->add ("ST-C3", actor.actor_id, "leave_failed",
                             std::to_string (actor.state_version));
            throw std::runtime_error ("injected source leave failure");
        }
        g_evidence->add ("transfer", actor.actor_id, "leave",
                         std::to_string (actor.state_version));
        co_return;
    }

    fw::task_t<e2e::join_target_res_t> join_target (transfer_actor_t &actor,
                                                    fw::message_context_t &,
                                                    const e2e::join_target_req_t &request)
    {
        auto &context = actor.context ();
        context
          .join_spot (request.target_spot_id, request)
          .timeout (std::chrono::seconds (10))
          .defer ();
        co_return e2e::join_target_res_t{request.scenario,
                                         actor.actor_id,
                                         true,
                                         g_evidence->node_rid (),
                                         request.target_spot_id,
                                         actor.state_version,
                                         "deferred"};
    }

    e2e::bound_push_res_t bound_push (const transfer_actor_t &actor,
                                      fw::message_context_t &,
                                      const e2e::bound_push_req_t &request)
    {
        actor.context ().bound_session ()
          .send (e2e::bound_push_notify_t{request.scenario, actor.actor_id,
                                          _context.spot_id (),
                                          g_evidence->node_rid (), request.marker,
                                          actor.state_version})
          .submit ();
        g_evidence->add (request.scenario, actor.actor_id, "bound_push", request.marker);
        return e2e::bound_push_res_t{request.scenario,      actor.actor_id,
                                     _context.spot_id (),
                                     g_evidence->node_rid (), request.marker,
                                     actor.state_version};
    }

    e2e::probe_res_t probe (const transfer_actor_t &actor,
                            fw::message_context_t &,
                            const e2e::probe_req_t &request)
    {
        g_evidence->add (request.scenario, actor.actor_id, "packet_handler", request.marker);
        return e2e::probe_res_t{request.scenario,
                                actor.actor_id,
                                _context.spot_id (),
                                g_evidence->node_rid (),
                                actor.state_version,
                                request.marker};
    }

  private:
    fw::entry_spot_context_t _context;
};

class transfer_user_spot_t : public fw::spot_t<transfer_actor_t>
{
  public:
    explicit transfer_user_spot_t (fw::spot_context_t context) :
        _context (std::move (context))
    {
    }

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_actor_request<&transfer_user_spot_t::join_target> (
          e2e::join_target_req_t::packet_name);
        _context.handlers ().add_actor_request<&transfer_user_spot_t::probe> (
          e2e::probe_req_t::packet_name);
        _context.handlers ().add_actor_send<&transfer_user_spot_t::handoff_packet> (
          e2e::handoff_packet_msg_t::packet_name);
        _context.handlers ().add_actor_request<&transfer_user_spot_t::bound_push> (
          e2e::bound_push_req_t::packet_name);
    }

    fw::task_t<fw::spot_create_response_t>
    on_create (const fw::message_t &request) override
    {
        if (!request.empty ()) {
            _mode = request.decode<e2e::create_spot_req_t> ().mode;
        }
        g_evidence->add ("create_spot", _context.spot_id (), "spot_created", _mode);
        co_return fw::spot_create_response_t::accept ();
    }

    fw::task_t<fw::spot_actor_join_result_t>
    on_actor_join (std::string_view actor_id,
                   const fw::message_t &request) override
    {
        const auto join = request.decode<e2e::join_target_req_t> ();
        const auto id = std::string (actor_id);
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _join_scenarios[id] = join.scenario;
        }
        g_evidence->add (join.scenario, id, "admission",
                         "spot=" + _context.spot_id () + "|mode=" + _mode
                           + "|input=actor-id-only");
        if (_mode == "reject" || join.expected_mode == "reject") {
            co_return fw::spot_actor_join_result_t::reject (
              make_join_reply (join.scenario, id, false, _context.spot_id ()));
        }
        co_return fw::spot_actor_join_result_t::accept (
          make_join_reply (join.scenario, id, true, _context.spot_id ()));
    }

    fw::task_t<void> on_actor_joined (transfer_actor_t &actor) override
    {
        if (_mode == "delay-joined") {
            const auto scenario = scenario_for (actor.actor_id);
            g_evidence->add (scenario, actor.actor_id, "joined_wait",
                             _context.spot_id ());
            g_joined_gates->wait (_context.spot_id ());
            g_evidence->add (scenario, actor.actor_id, "joined_released",
                             _context.spot_id ());
            g_evidence->add ("transfer", actor.actor_id, "joined",
                             _context.spot_id () + ":"
                               + std::to_string (actor.state_version));
            co_return;
        }
        if (_mode == "fail-joined") {
            const auto scenario = scenario_for (actor.actor_id);
            g_evidence->add (scenario, actor.actor_id, "joined_failed",
                             _context.spot_id ());
            throw std::runtime_error ("injected joined failure");
        }
        g_evidence->add ("transfer", actor.actor_id, "joined",
                         _context.spot_id () + ":"
                           + std::to_string (actor.state_version));
        if (actor.actor_type == e2e::actor_type_empty_state) {
            actor.state_version = g_domain_state->load (actor.actor_id);
            g_evidence->add ("transfer", actor.actor_id, "domain_state_loaded", actor.actor_id);
        }
        co_return;
    }

    fw::task_t<void> on_leave_actor (transfer_actor_t &actor) override
    {
        g_evidence->add ("transfer", actor.actor_id, "target_leave",
                         _context.spot_id ());
        co_return;
    }

    fw::task_t<e2e::join_target_res_t> join_target (transfer_actor_t &actor,
                                                    fw::message_context_t &,
                                                    const e2e::join_target_req_t &request)
    {
        auto &context = actor.context ();
        context
          .join_spot (request.target_spot_id, request)
          .timeout (std::chrono::seconds (10))
          .defer ();
        co_return e2e::join_target_res_t{request.scenario,
                                         actor.actor_id,
                                         true,
                                         g_evidence->node_rid (),
                                         request.target_spot_id,
                                         actor.state_version,
                                         "deferred"};
    }

    e2e::probe_res_t probe (const transfer_actor_t &actor,
                            fw::message_context_t &,
                            const e2e::probe_req_t &request)
    {
        g_evidence->add (request.scenario, actor.actor_id, "packet_handler", request.marker);
        if (request.scenario == "ST-F6" && request.marker == "late-reply") {
            std::this_thread::sleep_for (std::chrono::seconds (1));
            g_evidence->add (request.scenario, actor.actor_id, "late_reply_created",
                             request.marker);
        }
        return e2e::probe_res_t{request.scenario,
                                actor.actor_id,
                                _context.spot_id (),
                                g_evidence->node_rid (),
                                actor.state_version,
                                request.marker};
    }

    void handoff_packet (const transfer_actor_t &actor,
                         fw::message_context_t &,
                         const e2e::handoff_packet_msg_t &message)
    {
        g_evidence->add (message.scenario, actor.actor_id, "handoff_packet", message.marker);
    }

    e2e::bound_push_res_t bound_push (const transfer_actor_t &actor,
                                      fw::message_context_t &,
                                      const e2e::bound_push_req_t &request)
    {
        actor.context ().bound_session ()
          .send (e2e::bound_push_notify_t{request.scenario, actor.actor_id,
                                          _context.spot_id (),
                                          g_evidence->node_rid (), request.marker,
                                          actor.state_version})
          .submit ();
        g_evidence->add (request.scenario, actor.actor_id, "bound_push", request.marker);
        return e2e::bound_push_res_t{request.scenario,      actor.actor_id,
                                     _context.spot_id (),
                                     g_evidence->node_rid (), request.marker,
                                     actor.state_version};
    }

  private:
    std::string scenario_for (const std::string &actor_id)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        const auto found = _join_scenarios.find (actor_id);
        return found != _join_scenarios.end () ? found->second : std::string ("unknown");
    }

    fw::spot_context_t _context;
    std::string _mode = "accept";
    std::mutex _mutex;
    std::map<std::string, std::string> _join_scenarios;
};

class transfer_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::session_actor_manager_t, fw::actor_directory_t>;

    transfer_session_t (fw::session_actor_manager_t &actors,
                        fw::actor_directory_t &directory) :
        _actors (actors), _directory (directory)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }

    fw::task_t<void> on_disconnected (fw::stream_t &) override
    {
        _bound_actor_id.clear ();
        co_return;
    }

    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        if (dispatch.packet_name == e2e::bind_actor_session_req_t::packet_name) {
            const auto request = payload.parse_json<e2e::bind_actor_session_req_t> ();
            auto actor_ref = co_await _directory.find (request.actor_id);
            std::optional<fw::actor_ref_t> resolved;
            if (!request.node_rid.empty () && request.generation) {
                resolved.emplace (
                  fw::actor_id_t (request.actor_id),
                  static_cast<std::uint64_t> (*request.generation), e2e::mesh_name,
                  fw::node_rid_t::from_string (request.node_rid));
            } else if (actor_ref) {
                resolved = *actor_ref;
            } else {
                throw fw::framework_exception_t (
                  fw::framework_error_kind_t::not_found,
                  "actor '" + request.actor_id + "' was not found");
            }
            auto bound = co_await _actors.bind_or_get (*resolved).submit ();
            _bound_actor_id = std::string (bound.actor_id ());
            g_evidence->add (request.scenario, request.actor_id, "session_bound", "stream");
            stream
              .reply_packet (zlink::message_t::from_json (e2e::bind_actor_session_res_t{
                request.scenario, std::string (resolved->actor_id ().value ()),
                std::string (resolved->node_rid ().value ()),
                static_cast<std::int64_t> (resolved->object_generation ())}))
              .submit ();
            co_return;
        }

        if (_bound_actor_id.empty ()) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                             "no actor is bound to this session");
        }
        auto actor = _actors.find (_bound_actor_id);
        if (!actor) {
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "bound actor was not found");
        }
        if (dispatch.can_reply) {
            auto reply = co_await actor
                           ->relay_request (std::string (dispatch.packet_name), payload)
                           .submit ();
            stream.reply_packet (reply).submit ();
            co_return;
        }
        co_await actor->relay (std::string (dispatch.packet_name), payload);
    }

  private:
    fw::session_actor_manager_t &_actors;
    fw::actor_directory_t &_directory;
    std::string _bound_actor_id;
};

nlohmann::json parse_body (const fw::http_request_t &request)
{
    return request.body.empty () ? nlohmann::json::object ()
                                 : nlohmann::json::parse (request.body);
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

class evidence_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<evidence_store_t>;

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
    using dependency_types = fw::dependency_list_t<evidence_store_t>;

    explicit evidence_wait_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto wait = parse_body (request).get<e2e::evidence_wait_req_t> ();
        const auto timeout = std::chrono::milliseconds (
          std::clamp (wait.timeout_milliseconds, 1, 30000));
        return json_response (
          nlohmann::json (_evidence.wait_until_contains_all (wait.contains_all, timeout)));
    }

  private:
    evidence_store_t &_evidence;
};

class joined_gate_release_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<joined_gate_store_t>;

    explicit joined_gate_release_handler_t (joined_gate_store_t &gates) : _gates (gates) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto spot_id = route_value (request, "spotId");
        return json_response (
          nlohmann::json (e2e::gate_release_res_t{spot_id, _gates.release (spot_id)}));
    }

  private:
    joined_gate_store_t &_gates;
};

class transfer_gate_release_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<transfer_gate_store_t>;

    explicit transfer_gate_release_handler_t (transfer_gate_store_t &gates) : _gates (gates) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto actor_id = route_value (request, "actorId");
        return json_response (
          nlohmann::json (e2e::gate_release_res_t{actor_id, _gates.release (actor_id)}));
    }

  private:
    transfer_gate_store_t &_gates;
};

class create_spot_handler_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::spot_manager_t, evidence_store_t>;

    create_spot_handler_t (
      fw::spot_manager_t &spots,
      evidence_store_t &evidence) :
        _spots (spots), _evidence (evidence)
    {
    }

    fw::task_t<fw::http_response_t>
    handle (const fw::http_request_t &http_request)
    {
        const auto request = parse_body (http_request).get<e2e::create_spot_req_t> ();
        const auto created = co_await _spots
          .get_or_create (
            request.spot_id,
            "transfer-user")
          .creation_request (request)
          .submit ();
        co_return json_response (nlohmann::json (
          e2e::create_spot_res_t{
            request.spot_id,
            std::string (created.spot.node_rid ().value ()),
            created.state
                == fw::spot_create_state_t::existing
              ? "existing"
              : "created"}));
    }

  private:
    fw::spot_manager_t &_spots;
    evidence_store_t &_evidence;
};

class create_actor_handler_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::actor_manager_t, fw::session_actor_manager_t,
                            evidence_store_t>;

    create_actor_handler_t (fw::actor_manager_t &actor_manager,
                            fw::session_actor_manager_t &session_actors,
                            evidence_store_t &evidence) :
        _actor_manager (actor_manager),
        _session_actors (session_actors),
        _evidence (evidence)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto request = parse_body (http_request).get<e2e::actor_create_req_t> ();
        _evidence.add (
          "create", request.actor_id,
          "create_actor_requested",
          request.actor_type);
        fw::actor_create_result_t created;
        try {
            created = co_await _actor_manager
              .get_or_create (fw::actor_id_t (request.actor_id), request.actor_type)
              .creation_request (request)
              .submit ();
        }
        catch (const std::exception &error) {
            _evidence.add ("create", request.actor_id,
                           "create_actor_failed", error.what ());
            throw;
        }
        const auto ref = std::visit (
          [] (const auto &result) -> fw::actor_ref_t {
              using result_t = std::decay_t<decltype (result)>;
              if constexpr (std::is_same_v<
                              result_t,
                              fw::actor_create_rejected_t>) {
                  throw fw::framework_exception_t (
                    fw::framework_error_kind_t::rejected,
                    "Actor creation was rejected");
              } else {
                  return result.actor;
              }
          },
          created);
        try {
            auto bound = co_await _session_actors
              .bind_or_get (ref)
              .submit ();
            const auto &bound_ref = bound.context ().actor_ref ();
            co_return json_response (nlohmann::json (e2e::actor_create_res_t{
              request.actor_id, request.actor_type,
              std::string (bound_ref.node_rid ().value ()),
              static_cast<std::int64_t> (bound_ref.object_generation ())}));
        }
        catch (const std::exception &error) {
            _evidence.add (
              "create", request.actor_id,
              "create_actor_bind_failed",
              error.what ());
            throw;
        }
    }

  private:
    fw::actor_manager_t &_actor_manager;
    fw::session_actor_manager_t &_session_actors;
    evidence_store_t &_evidence;
};

fw::actor_ref_t require_actor_ref (fw::actor_directory_t &directory, const std::string &actor_id)
{
    auto found = directory.find (actor_id).result ();
    if (!found || !found.value ()) {
        throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                         "actor '" + actor_id + "' was not found");
    }
    return *found.value ();
}

class actor_ref_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_directory_t>;

    explicit actor_ref_handler_t (fw::actor_directory_t &directory) : _directory (directory) {}

    fw::http_response_t handle (const fw::http_request_t &request)
    {
        const auto actor_id = route_value (request, "actorId");
        const auto ref = require_actor_ref (_directory, actor_id);
        return json_response (nlohmann::json (e2e::actor_ref_snapshot_res_t{
          actor_id, std::string (ref.node_rid ().value ()),
          static_cast<std::int64_t> (ref.object_generation ())}));
    }

  private:
    fw::actor_directory_t &_directory;
};

std::string error_kind_name (const fw::framework_exception_t &error)
{
    if (fw::detail::boundary_state (error) == fw::detail::boundary_error_t::timed_out) {
        return "TimeoutException";
    }
    switch (error.kind ()) {
        case fw::framework_error_kind_t::unavailable:
            return "ActorLocationStale";
        case fw::framework_error_kind_t::not_found:
            return "ActorRouteNotFound";
        default:
            return "FrameworkError:" + std::to_string (static_cast<int> (error.kind ()));
    }
}

class join_actor_handler_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::actor_directory_t, fw::actor_client_t, evidence_store_t>;

    join_actor_handler_t (fw::actor_directory_t &directory,
                          fw::actor_client_t &actors,
                          evidence_store_t &evidence) :
        _directory (directory), _actors (actors), _evidence (evidence)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::join_target_req_t> ();
        try {
            const auto request_timeout = request.scenario == "ST-C3"
                                           ? std::chrono::seconds (5)
                                           : std::chrono::seconds (12);
            auto result = co_await _actors.request (fw::actor_id_t (actor_id), request)
                            .timeout (request_timeout)
                            .submit<e2e::join_target_res_t> ();
            _evidence.add (request.scenario, actor_id,
                           result.accepted ? "success_reply" : "reject_reply",
                           request.target_spot_id);
            co_return json_response (nlohmann::json (result));
        }
        catch (const fw::framework_exception_t &error) {
            _evidence.add (
              request.scenario, actor_id, "join_failed",
              error_kind_name (error) + ":" + error.what ());
            co_return json_response (nlohmann::json (e2e::join_target_res_t{
              request.scenario, actor_id, false, std::string{}, request.target_spot_id, 0,
              error_kind_name (error)}));
        }
        catch (const std::exception &error) {
            /* Callback failures are an application-visible rejected join.
             * Keep them inside the HTTP result contract instead of letting
             * an escaped exception close the transport without a response. */
            _evidence.add (request.scenario, actor_id, "join_failed", error.what ());
            co_return json_response (nlohmann::json (e2e::join_target_res_t{
              request.scenario, actor_id, false, std::string{}, request.target_spot_id, 0,
              error.what ()}));
        }
    }

  private:
    fw::actor_directory_t &_directory;
    fw::actor_client_t &_actors;
    evidence_store_t &_evidence;
};

class probe_actor_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_directory_t, fw::actor_client_t>;

    probe_actor_handler_t (fw::actor_directory_t &directory, fw::actor_client_t &actors) :
        _directory (directory), _actors (actors)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::probe_req_t> ();
        auto response = co_await _actors.request (fw::actor_id_t (actor_id), request)
                          .timeout (std::chrono::seconds (10))
                          .submit<e2e::probe_res_t> ();
        co_return json_response (nlohmann::json (response));
    }

  private:
    fw::actor_directory_t &_directory;
    fw::actor_client_t &_actors;
};

class probe_ref_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_client_t>;

    explicit probe_ref_handler_t (fw::actor_client_t &actors) : _actors (actors) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::actor_ref_probe_req_t> ();
        try {
            auto reply = co_await _actors
                           .request (fw::actor_id_t (actor_id),
                                              e2e::probe_req_t{request.scenario, request.marker})
                           .timeout (std::chrono::milliseconds (request.timeout_ms))
                           .submit<e2e::probe_res_t> ();
            co_return json_response (
              nlohmann::json (e2e::actor_ref_probe_res_t{true, reply, std::string{}}));
        }
        catch (const fw::framework_exception_t &error) {
            co_return json_response (nlohmann::json (e2e::actor_ref_probe_res_t{
              false, std::nullopt, error_kind_name (error)}));
        }
    }

  private:
    fw::actor_client_t &_actors;
};

class send_ref_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_client_t>;

    explicit send_ref_handler_t (fw::actor_client_t &actors) : _actors (actors) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::actor_ref_probe_req_t> ();
        _actors.send (fw::actor_id_t (actor_id),
                      e2e::handoff_packet_msg_t{request.scenario, request.marker})
          .submit ();
        co_return json_response (nlohmann::json::object ());
    }

  private:
    fw::actor_client_t &_actors;
};

class bound_push_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_directory_t, fw::actor_client_t>;

    bound_push_handler_t (fw::actor_directory_t &directory, fw::actor_client_t &actors) :
        _directory (directory), _actors (actors)
    {
    }

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &http_request)
    {
        const auto actor_id = route_value (http_request, "actorId");
        const auto request = parse_body (http_request).get<e2e::bound_push_req_t> ();
        auto response = co_await _actors.request (fw::actor_id_t (actor_id), request)
                          .timeout (std::chrono::seconds (10))
                          .submit<e2e::bound_push_res_t> ();
        co_return json_response (nlohmann::json (response));
    }

  private:
    fw::actor_directory_t &_directory;
    fw::actor_client_t &_actors;
};

class shutdown_flag_t
{
  public:
    std::atomic_bool requested{false};
};

class shutdown_handler_t
{
  public:
    using dependency_types = fw::dependency_list_t<shutdown_flag_t>;

    explicit shutdown_handler_t (shutdown_flag_t &flag) : _flag (flag) {}

    fw::http_response_t handle (const fw::http_request_t &)
    {
        _flag.requested.store (true);
        return json_response (nlohmann::json{{"status", "stopping"}});
    }

  private:
    shutdown_flag_t &_flag;
};

class relocation_store_activity_handler_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<relocation_store_activity_t>;

    explicit relocation_store_activity_handler_t (
      relocation_store_activity_t &activity) :
        _activity (activity)
    {
    }

    fw::http_response_t handle (const fw::http_request_t &)
    {
        return json_response (
          nlohmann::json{{"reads", _activity.reads ()},
                         {"writes", _activity.writes ()}});
    }

  private:
    relocation_store_activity_t &_activity;
};

} // namespace

int run_host_impl (transfer_host_role_t host_role, int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.config ().load_cli (argc, argv);
    const auto config_path = app.config ().model ().get ("config");
    if (!config_path) {
        throw std::runtime_error ("SpotActorTransfer node requires --config=<path>");
    }
    app.config ().load_json (*config_path);
    const auto configured = app.config ().bind_required<node_options_t> ("e2e");
    const auto &rid = configured.rid;
    const auto &http_url = configured.http_url;
    const auto &router_endpoint = configured.router_endpoint;
    const auto &stream_endpoint = configured.stream_endpoint;
    const auto &pub_endpoint = configured.pub_endpoint;
    const auto &redis_endpoint = configured.redis_endpoint;
    const auto &key_prefix = configured.redis_key_prefix;
    const auto &log_dir = configured.log_dir;
    const auto &evidence_file = configured.evidence_file;
    std::filesystem::create_directories (log_dir);

    auto evidence = std::make_unique<evidence_store_t> (rid, evidence_file);
    auto domain_state = std::make_unique<domain_state_store_t> (log_dir);
    auto joined_gates = std::make_unique<joined_gate_store_t> ();
    auto transfer_gates = std::make_unique<transfer_gate_store_t> ();
    auto relocation_activity =
      std::make_unique<relocation_store_activity_t> ();
    auto shutdown_flag = std::make_unique<shutdown_flag_t> ();
    g_evidence = evidence.get ();
    g_domain_state = domain_state.get ();
    g_joined_gates = joined_gates.get ();
    g_transfer_gates = transfer_gates.get ();
    g_initial_actor_node_rid = configured.initial_actor_node;
    auto *shutdown_flag_ptr = shutdown_flag.get ();

    app.logging ().use_file (log_dir + "/" + rid + ".app.log");
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &framework) {
        framework.services ().add_singleton<evidence_store_t> (std::move (evidence));
        framework.services ().add_singleton<domain_state_store_t> (std::move (domain_state));
        framework.services ().add_singleton<joined_gate_store_t> (std::move (joined_gates));
        framework.services ().add_singleton<transfer_gate_store_t> (std::move (transfer_gates));
        auto *relocation_activity_ptr = relocation_activity.get ();
        framework.services ().add_singleton<relocation_store_activity_t> (
          std::move (relocation_activity));
        framework.services ().add_singleton<shutdown_flag_t> (std::move (shutdown_flag));

        framework.set_default_request_timeout (std::chrono::seconds (3));

        framework.configure_dispatch ()
          .message_flow (fw::message_flow_log_mode_t::key_transitions)
          .set_message_flow_observer (
            [] (const fw::message_flow_event_t &event) { g_evidence->add_flow (event); });

        framework.set_message_follow_duration (std::chrono::seconds (5));
        framework.add_location_store (
          std::make_shared<fw::redis::redis_location_store_t> (
            fw::redis::redis_location_options_t{.connection_string = redis_endpoint,
                                                           .key_prefix = key_prefix}));
        framework.add_relocation_store (
          std::make_shared<counting_relocation_store_t> (
            std::make_shared<
              fw::redis::redis_relocation_store_t> (
              fw::redis::redis_relocation_options_t{
                .connection_string = redis_endpoint,
                .key_prefix = key_prefix + ":relocation"}),
            *relocation_activity_ptr));
        auto &locations = framework.configure_locations ();
        locations.owner_lease_renew_interval = std::chrono::seconds (1);
        locations.owner_lease_ttl = std::chrono::seconds (3);
        locations.owner_lease_fencing_margin =
          std::chrono::milliseconds (500);
        locations.owner_lease_renew_timeout =
          std::chrono::milliseconds (500);
        locations.polling_interval = std::chrono::milliseconds (500);
        locations.route_cache_max_age = std::chrono::milliseconds::zero ();

        auto mesh = framework.add_route_mesh (e2e::mesh_name);
        mesh.listen (router_endpoint).set_routing_id (zlink::routing_id_t::from (rid));
        auto channel = mesh.channel_name (e2e::mesh_name);
        if (host_role == transfer_host_role_t::actor_node) {
            channel.server ();
        } else {
            mesh.set_object_role (fw::object_role_t::client);
            channel.client ();
        }
        if (host_role == transfer_host_role_t::actor_node) {
            mesh.add_entry_spot<transfer_entry_spot_t> (
              [] (fw::entry_spot_context_t context) {
                  return std::make_shared<transfer_entry_spot_t> (
                    std::move (context));
              })
              .add_spot_factory<transfer_user_spot_t> (
                "transfer-user",
                [] (fw::spot_context_t context) {
                    return std::make_shared<transfer_user_spot_t> (
                      std::move (context));
                },
                [] (auto &factory) {
                    factory.disable_relocation ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_stateful,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        transfer_actor_adapter_t> ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_empty_state,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        transfer_actor_adapter_t> ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_no_adapter,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory.recreate_on_relocation ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_fail_leave,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        transfer_actor_adapter_t> ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_fail_transfer_out,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        transfer_actor_adapter_t> ();
                })
              .add_actor_factory<
                transfer_actor_t,
                transfer_actor_factory_t> (
                e2e::actor_type_fail_transfer_in,
                std::make_shared<
                  transfer_actor_factory_t> (),
                [] (auto &factory) {
                    factory
                      .template preserve_state_with<
                        transfer_actor_adapter_t> ();
                });
        }

        if (host_role == transfer_host_role_t::actor_node) {
            framework.add_stream_node (std::string (e2e::mesh_name) + "-internal-" + rid)
              .bind (stream_endpoint)
              .register_session<transfer_session_t> ();
        } else {
            framework.add_stream_node (std::string (e2e::mesh_name) + "-stream-" + rid)
              .bind (stream_endpoint)
              .register_session<transfer_session_t> ();
        }

        framework.http ()
          .configure_server ([] (fw::http_server_options_builder_t &server) {
              server.set_write_timeout (std::chrono::seconds (15));
          })
          .listen (http_url)
          .map_health ("/health")
          .map_get<evidence_handler_t> ("/evidence")
          .map_post<evidence_wait_handler_t> ("/evidence/wait");
        if (host_role == transfer_host_role_t::actor_node) {
            framework.http ()
              .map_post<joined_gate_release_handler_t> ("/joined-gates/{spotId}/release")
              .map_post<transfer_gate_release_handler_t> ("/transfer-gates/{actorId}/release")
              .map_post<create_spot_handler_t> ("/spots")
              .map_post<create_actor_handler_t> ("/actors")
              .map_post<join_actor_handler_t> ("/actors/{actorId}/join")
              .map_post<probe_actor_handler_t> ("/actors/{actorId}/probe")
              .map_get<actor_ref_handler_t> ("/actors/{actorId}/ref")
              .map_post<probe_ref_handler_t> ("/actors/{actorId}/probe-ref")
              .map_post<send_ref_handler_t> ("/actors/{actorId}/send-ref")
              .map_post<bound_push_handler_t> ("/actors/{actorId}/bound-push")
              .map_get<relocation_store_activity_handler_t> (
                "/relocation-store/activity")
              .map_post<shutdown_handler_t> ("/shutdown");
        }
    });

    std::thread shutdown_watcher ([&app, shutdown_flag_ptr] {
        while (!shutdown_flag_ptr->requested.load ()) {
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        app.request_stop ();
    });
    const int code = app.run (argc, argv);
    shutdown_flag_ptr->requested.store (true);
    shutdown_watcher.join ();
    return code;
}

int zlink::framework::e2e::spot_actor_transfer::server::run_host (host_role_t role,
                                                                  int argc,
                                                                  char **argv)
{
    return run_host_impl (role, argc, argv);
}
