/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../common_codecs.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace zlink::samples::gamequest
{

using namespace framework;

static constexpr const char *gamequest_player_actor_type = "gamequest-player";

class game_api_store_t
{
  public:
    void bind (const std::string &player_id, const std::string &api_name)
    {
        const std::lock_guard lock (_mutex);
        _bindings[player_id] = api_name;
    }

    void unbind (const std::string &player_id)
    {
        const std::lock_guard lock (_mutex);
        _bindings.erase (player_id);
    }

    void merge_projection (const std::string &player_id,
                           const std::vector<quest_progress_t> &projection)
    {
        const std::lock_guard lock (_mutex);
        _projections[player_id] = projection;
    }

    std::vector<quest_progress_t> projection (const std::string &player_id) const
    {
        const std::lock_guard lock (_mutex);
        const auto found = _projections.find (player_id);
        return found == _projections.end () ? std::vector<quest_progress_t>{} : found->second;
    }

    void record_event (const gameplay_msg_t &event)
    {
        const std::lock_guard lock (_mutex);
        const auto existing = std::find_if (
          _events.begin (), _events.end (), [&] (const gameplay_msg_t &stored) {
              return stored.player_id == event.player_id && stored.event_id == event.event_id;
          });
        if (existing != _events.end ()) {
            return;
        }
        _events.push_back (event);
    }

    void add_unpublished_kills (const std::string &player_id, int count)
    {
        const std::lock_guard lock (_mutex);
        _unpublished_kills[player_id] += count;
    }

    int snapshot_kill_count (const std::string &player_id) const
    {
        const std::lock_guard lock (_mutex);
        int count = 0;
        for (const auto &event : _events) {
            if (event.player_id == player_id && event.type == "MonsterKilled") {
                count += gameplay_payload (event).value ("count", 0);
            }
        }
        const auto unpublished = _unpublished_kills.find (player_id);
        return count + (unpublished == _unpublished_kills.end () ? 0 : unpublished->second);
    }

    bool record_notify (const notify_quest_progress_msg_t &notify)
    {
        const std::lock_guard lock (_mutex);
        _projections[notify.player_id] = notify.projection;
        return true;
    }

    server_assertion_res_t assert_state () const
    {
        const std::lock_guard lock (_mutex);
        std::vector<std::string> evidence;
        bool alice_first_hunt = false;
        bool bob_herb = false;
        for (const auto &[player_id, projection] : _projections) {
            for (const auto &progress : projection) {
                evidence.push_back (player_id + ":" + progress.quest_id + ":" + progress.status
                                    + ":" + std::to_string (progress.current_count) + "/"
                                    + std::to_string (progress.required_count));
                alice_first_hunt = alice_first_hunt
                                   || (progress.player_id == "player-alice"
                                       && progress.quest_id == quest_ids_t::first_hunt
                                       && progress.status == quest_status_t::reward_granted);
                bob_herb = bob_herb
                           || (progress.player_id == "player-bob"
                               && progress.quest_id == quest_ids_t::herb_gathering
                               && progress.status == quest_status_t::reward_granted);
            }
        }
        for (const auto &[player, api] : _bindings) {
            evidence.push_back ("binding:" + player + ":" + api);
        }
        for (const auto &event : _events) {
            evidence.push_back ("event:" + event.player_id + ":" + event.type + ":"
                                + event.event_id);
        }
        for (const auto &[player, count] : _unpublished_kills) {
            evidence.push_back ("unpublished-kills:" + player + ":" + std::to_string (count));
        }
        return {alice_first_hunt || bob_herb, evidence};
    }

  private:
    mutable std::mutex _mutex;
    std::map<std::string, std::string> _bindings;
    std::map<std::string, std::vector<quest_progress_t>> _projections;
    std::vector<gameplay_msg_t> _events;
    std::map<std::string, int> _unpublished_kills;
};

class player_actor_t : public zlink::framework::actor_t
{
  public:
    explicit player_actor_t (actor_context_t value) :
        actor_id (value.actor_ref ().actor_id ().value ()),
        actor_ref (value.actor_ref ()),
        actor_context (std::move (value))
    {
    }

    actor_context_t &context () noexcept override { return actor_context; }
    const actor_context_t &context () const noexcept override { return actor_context; }

    std::string actor_id;
    zlink::framework::actor_ref_t actor_ref;
    actor_context_t actor_context;
};

struct player_actor_factory_t final
    : public zlink::framework::actor_factory_t<player_actor_t>
{
    zlink::framework::task_t<std::shared_ptr<player_actor_t>>
    create (actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<player_actor_t> (
          std::move (context));
    }
};

/* owner spot이 보낸 진행 notify가 이 노드의 entry spot으로 route돼 들어온다. 어느 노드로 갈지는
 * location store의 session binding이 정하므로, API는 자기 노드의 actor만 보면 된다. */
class player_entry_spot_t : public entry_spot_t<player_actor_t>
{
  public:
    player_entry_spot_t (entry_spot_context_t context,
                         game_api_store_t &store) :
        _store (store), _context (std::move (context))
    {
    }

    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ()
          .add_actor_send<&player_entry_spot_t::quest_progress_notified> (
            notify_quest_progress_msg_t::packet_name)
          .add_actor_request<&player_entry_spot_t::join_session> (join_session_req_t::packet_name);
    }

    task_t<spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return spot_actor_join_result_t::accept ();
    }

    task_t<void> on_actor_joined (player_actor_t &) override { co_return; }
    task_t<void> on_leave_actor (player_actor_t &) override { co_return; }

    /* session이 join을 actor로 relay한다. actor가 이 노드의 entry spot에 붙어 있어야 owner spot이
     * session binding으로 이 노드를 찾을 수 있다. */
    join_session_res_t join_session (player_actor_t &,
                                     message_context_t &,
                                     const join_session_req_t &request)
    {
        return {request.player_id, _store.projection (request.player_id)};
    }

    void quest_progress_notified (player_actor_t &actor,
                                  message_context_t &,
                                  const notify_quest_progress_msg_t &notify)
    {
        /* The actor may be hosted by another GameApi process. The local store
         * records the projection, while Framework bound_session() routes the
         * push to the current session owner. */
        if (!_store.record_notify (notify)) {
            return;
        }
        for (const auto &progress : notify.projection) {
            actor.actor_context.bound_session ()
              .send (quest_progress_notify_t{notify.player_id, progress})
              .submit ();
        }
        if (!notify.completed_quest_id.empty ()) {
            const auto completed = std::find_if (
              notify.projection.begin (), notify.projection.end (),
              [&] (const quest_progress_t &progress) {
                  return progress.quest_id == notify.completed_quest_id;
              });
            if (completed != notify.projection.end ()) {
                actor.actor_context.bound_session ()
                  .send (quest_completed_notify_t{
                    notify.player_id, *completed, true})
                  .submit ();
            }
        }
    }

  private:
    game_api_store_t &_store;
    entry_spot_context_t _context;
};

class gamequest_session_t final : public packet_stream_session_t
{
  public:
    using dependency_types = dependency_list_t<route_client_t,
                                               game_api_store_t,
                                               sample_topology_t,
                                               session_actor_manager_t>;

    gamequest_session_t (route_client_t &routes,
                         game_api_store_t &store,
                         sample_topology_t &topology,
                         session_actor_manager_t &actors) :
        _routes (routes),
        _store (store),
        _topology (topology),
        _actors (actors)
    {
    }

    task_t<void> on_connected (stream_t &) override { co_return; }

    task_t<void> on_disconnected (stream_t &) override
    {
        if (_player_id) {
            _store.unbind (*_player_id);
            _player_id.reset ();
        }
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);
        if (packet == join_session_req_t::packet_name) {
            const auto request = payload.parse_json<join_session_req_t> ();
            auto actor = _actors.get_or_create (gamequest_player_actor_type, request.player_id);
            if (!actor) {
                throw framework_exception_t (
                  actor.error_kind (),
                  actor.error () ? actor.error ()->what () : "gamequest session actor bind failed");
            }
            auto bound = co_await _actors.bind_or_get (actor.value ().ref ()).submit ();
            _player_id = request.player_id;
        _store.bind (request.player_id, _topology.api_name);
            auto synced = co_await sync_projection (request.player_id);
            _store.merge_projection (request.player_id, synced.updated_quests);
            auto current = _actors.find (std::string (bound.actor_id ()));
            if (!current) {
                throw framework_exception_t (framework_error_kind_t::not_found,
                                             "joined player actor route is not found");
            }
            auto reply = co_await current
                           ->relay_request (join_session_req_t::packet_name,
                                            zlink::message_t::from_json (request))
                           .submit ();
            stream.reply_packet (reply).submit ();
            co_return;
        }
        if (packet == get_quest_progress_req_t::packet_name) {
            const auto request = payload.parse_json<get_quest_progress_req_t> ();
            auto synced = co_await sync_projection (request.player_id);
            _store.merge_projection (request.player_id, synced.updated_quests);
            stream
              .reply_packet (
                zlink::message_t::from_json (get_quest_progress_res_t{synced.updated_quests}))
              .submit ();
            co_return;
        }
        if (packet == sync_quest_progress_req_t::packet_name) {
            const auto request = payload.parse_json<sync_quest_progress_req_t> ();
            auto synced = co_await sync_projection (request.player_id);
            _store.merge_projection (request.player_id, synced.updated_quests);
            stream.reply_packet (zlink::message_t::from_json (synced))
              .submit ();
            co_return;
        }
        if (packet == projection_admin_req_t::packet_name) {
            const auto request = payload.parse_json<projection_admin_req_t> ();
            auto result = co_await _routes
                            .request_to_spot (
                              player_spot_id (request.player_id), request)
                            .instance_spot (
                              sample_names_t::player_quest_spot)
                            .template submit<projection_admin_res_t> ();
            stream.reply_packet (zlink::message_t::from_json (result)).submit ();
            co_return;
        }
        if (packet == unpublished_kill_req_t::packet_name) {
            const auto request = payload.parse_json<unpublished_kill_req_t> ();
            _store.add_unpublished_kills (request.player_id, request.count);
            stream
              .reply_packet (zlink::message_t::from_json (unpublished_kill_res_t{true}))
              .submit ();
            co_return;
        }
        if (packet == kill_monster_req_t::packet_name) {
            const auto request = payload.parse_json<kill_monster_req_t> ();
            const auto event = event_for (request.player_id, request.idempotency_key,
                                          "MonsterKilled", request.monster_id, 1);
            co_await apply_event (event);
            stream.reply_packet (zlink::message_t::from_json (kill_monster_res_t{event.event_id}))
              .submit ();
            co_return;
        }
        if (packet == collect_item_req_t::packet_name) {
            const auto request = payload.parse_json<collect_item_req_t> ();
            const auto event = event_for (request.player_id, request.idempotency_key,
                                          "ItemCollected", request.item_id, request.count);
            co_await apply_event (event);
            co_return;
        }
        if (packet == enter_area_req_t::packet_name) {
            const auto request = payload.parse_json<enter_area_req_t> ();
            const auto event = event_for (request.player_id, request.idempotency_key, "AreaEntered",
                                          request.area_id, 1);
            co_await apply_event (event);
            co_return;
        }
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "Unsupported GameQuest packet: " + packet);
    }

  private:
    gameplay_msg_t event_for (std::string player_id,
                              std::string idempotency_key,
                              std::string event_type,
                              std::string value,
                              int count) const
    {
        return {player_id + "-" + idempotency_key,
                std::move (player_id),
                std::move (event_type),
                gameplay_payload (value, count),
                static_cast<long long> (std::time (nullptr)) * 1000LL};
    }

    task_t<sync_quest_progress_res_t> sync_projection (const std::string &player_id)
    {
        auto synced = co_await _routes
                        .request_to_spot (
                          player_spot_id (player_id),
                          sync_quest_progress_owner_req_t{
                            player_id, _store.snapshot_kill_count (player_id)})
                        .instance_spot (
                          sample_names_t::player_quest_spot)
                        .template submit<sync_quest_progress_res_t> ();
        co_return synced;
    }

    /* 공통 sample spec §11.2: gameplay event는 owner spot으로 보내는 응답 없는 one-way다.
     * client에는 event id만 즉시 돌려주고, 진행은 notify로 돌아온다. */
    task_t<void> apply_event (const gameplay_msg_t &event)
    {
        co_await _routes
          .send_to_spot (player_spot_id (event.player_id), event)
          .instance_spot (sample_names_t::player_quest_spot)
          .submit ();
        _store.record_event (event);
        std::cerr << "gamequest api event routed player=" << event.player_id
                  << " type=" << event.type << "\n";
        co_return;
    }

    route_client_t &_routes;
    game_api_store_t &_store;
    sample_topology_t &_topology;
    session_actor_manager_t &_actors;
    std::optional<std::string> _player_id;
};

class server_assertion_http_handler_t
{
  public:
    using dependency_types = dependency_list_t<game_api_store_t>;
    using request_type = server_assertion_req_t;
    using reply_type = server_assertion_res_t;
    static constexpr const char *topic_name = server_assertion_req_t::packet_name;

    explicit server_assertion_http_handler_t (game_api_store_t &store) : _store (store) {}

    server_assertion_res_t handle (const server_assertion_req_t &)
    {
        return _store.assert_state ();
    }

  private:
    game_api_store_t &_store;
};

class route_ready_http_handler_t
{
  public:
    using dependency_types = dependency_list_t<route_mesh_runtime_t>;

    explicit route_ready_http_handler_t (route_mesh_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    http_response_t handle (const http_request_t &request)
    {
        const auto found = request.query_values.find ("targetRid");
        if (found == request.query_values.end () || found->second.empty ()) {
            return {.status = 400, .body = R"({"error":"targetRid is required"})"};
        }

        const auto snapshot = _runtime.snapshot ("gamequest");
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == found->second
                && snapshot.placement.is_available
                && peer.state == peer_state_t::ready) {
                return {.body = nlohmann::json{{"ready", true},
                                               {"targetRid", found->second}}
                                  .dump ()};
            }
        }

        nlohmann::json peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers) {
            peers.push_back ({{"rid", peer.node_rid.to_string ()},
                              {"state", static_cast<int> (peer.state)}});
        }
        return {.status = 503,
                .body = nlohmann::json{{"ready", false},
                                       {"targetRid", found->second},
                                       {"peers", std::move (peers)}}
                          .dump ()};
    }

  private:
    route_mesh_runtime_t &_runtime;
};

} // namespace zlink::samples::gamequest

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::gamequest;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label (topology.api_name);
        options.services ().add_singleton<sample_topology_t> (
          std::make_unique<sample_topology_t> (topology));
        auto api_store = std::make_unique<game_api_store_t> ();
        auto *store_ptr = api_store.get ();
        options.services ().add_singleton<game_api_store_t> (std::move (api_store));
        add_gamequest_json_codecs (options.codecs ());
        add_gamequest_location_store (options, topology);
        /* GameApi는 player entry Spot을 제공하는 Object Server다. API와 QuestMission은
         * 같은 RouteMesh에서 global Spot routing을 사용한다. */
        auto gamequest = options.add_route_mesh ("gamequest");
        gamequest
          .set_routing_id (zlink::routing_id_t::from (
            "gamequest-" + topology.api_name + "-spot"))
          .set_object_role (object_role_t::server)
          .listen (topology.selected_api_spot_route_endpoint ());
        /* GameApi hosts the player Entry Spot and also sends gameplay and
         * projection requests to either QuestMission owner. Keep both
         * object-server peers in the same public RouteMesh path so owner
         * placement is resolved by the Framework runtime. */
        gamequest.peer_connections ().connect (
          zlink::routing_id_t::from ("gamequest-mission-a-spot"),
          topology.mission_a_spot_route_endpoint);
        gamequest.peer_connections ().connect (
          zlink::routing_id_t::from ("gamequest-mission-b-spot"),
          topology.mission_b_spot_route_endpoint);
        /* Location Store discovery owns the peer set for this shared
         * RouteMesh. Do not add a one-time API-to-API manual connection:
         * manual endpoints are excluded from discovery reconciliation and a
         * startup race could otherwise leave a required peer disconnected. */
        gamequest
          .add_entry_spot<player_entry_spot_t> ([store_ptr] (
                                                  entry_spot_context_t context) {
              return std::make_shared<player_entry_spot_t> (
                std::move (context), *store_ptr);
          })
          .add_actor_factory<player_actor_t, player_actor_factory_t> (
            gamequest_player_actor_type,
            std::make_shared<player_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
        options.add_stream_node (sample_names_t::stream_node)
          .bind (topology.selected_api_stream_endpoint ())
          .register_session<gamequest_session_t> ();
        options.http ()
          .listen (topology.selected_api_http_url ())
            .map_health ("/health")
          .map_get<route_ready_http_handler_t> ("/ready")
          .map_post<server_assertion_http_handler_t> ("/self-check/assert");
    });
    return app.run (argc, argv);
}
