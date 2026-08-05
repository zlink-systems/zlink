/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/play_support.hpp"

#include <zlink/framework.hpp>

#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

class ensure_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<evidence_store_t,
                                          zlink::framework::spot_node_manager_t>;
    using request_type = ensure_spot_req_t;
    using reply_type = ensure_spot_res_t;

    ensure_spot_handler_t (evidence_store_t &evidence,
                           zlink::framework::spot_node_manager_t &spots) :
        _evidence (evidence), _spots (spots)
    {
    }

    ensure_spot_res_t handle (const ensure_spot_req_t &request,
                                const zlink::framework::route_handler_context_t &)
    {
        try {
            const auto rid = (request.spot_id);
            const auto created = _spots.get_or_create_spot (probe_spot_name, rid);
            _evidence.add ("spot-ensured|rid=" + _evidence.node_rid + "|spot="
                           + created.spot_id + "|request="
                           + request.spot_id);
            return {.spot_id = created.spot_id,
                    .node_rid = _evidence.node_rid};
        }
        catch (const zlink::framework::framework_exception_t &) {
            throw;
        }
        catch (const std::exception &error) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::internal_failure,
              std::string ("ensure spot failed: ") + error.what ());
        }
    }

  private:
    evidence_store_t &_evidence;
    zlink::framework::spot_node_manager_t &_spots;
};

class bind_await_actors_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<evidence_store_t,
                                          zlink::framework::spot_node_manager_t,
                                          zlink::framework::session_actor_manager_t>;
    using request_type = bind_await_actors_req_t;
    using reply_type = bind_await_actors_res_t;

    bind_await_actors_handler_t (evidence_store_t &evidence,
                                 zlink::framework::spot_node_manager_t &spots,
                                 zlink::framework::session_actor_manager_t &actors) :
        _evidence (evidence), _spots (spots), _actors (actors)
    {
    }

    bind_await_actors_res_t handle (
      const bind_await_actors_req_t &request,
      const zlink::framework::route_handler_context_t &)
    {
        const auto spot_id = (request.spot_id);
        (void) _spots.get_or_create_spot (probe_spot_name, spot_id);
        _evidence.add ("bind-start|rid=" + _evidence.node_rid + "|spot=" + request.spot_id
                       + "|actors=" + std::to_string (request.actor_ids.size ()));
        bind_await_actors_res_t reply{.spot_id = request.spot_id};
        for (const auto &actor_id : request.actor_ids) {
            auto actor = _actors.get_or_create (actor_type, actor_id);
            if (!actor) {
                throw zlink::framework::framework_exception_t (
                  actor.error_kind (),
                  actor.error () ? actor.error ()->what () : "actor get or create failed");
            }
            auto bound = _actors.bind_or_get (actor.value ().ref ()).submit ().result ();
            if (!bound) {
                throw zlink::framework::framework_exception_t (
                  bound.error_kind (),
                  bound.error () ? bound.error ()->what () : "actor bind failed");
            }
            auto joined =
              bound.value ()
                .context ()
                .join_spot (spot_id,
                            delay_req_t{.request_id = "bind-" + actor_id,
                                        .delay_ms = 0,
                                        .marker = "bind"})
                .timeout (std::chrono::milliseconds (3000))
                .template async<delay_res_t> ()
                .result ();
            if (!joined) {
                throw zlink::framework::framework_exception_t (
                  joined.error_kind (),
                  joined.error () ? joined.error ()->what () : "actor join failed");
            }
            const auto *joined_accepted =
              std::get_if<zlink::framework::actor_join_accepted_t<yd::delay_res_t>> (&joined.value ());
            if (joined_accepted == nullptr) {
                throw zlink::framework::framework_exception_t (
                  zlink::framework::framework_error_kind_t::internal_failure,
                  "await actor join was rejected: " + actor_id);
            }
            auto actor_ref = joined_accepted->actor;
            _evidence.add ("bind-actor|rid=" + _evidence.node_rid + "|spot="
                           + request.spot_id + "|actor=" + actor_id + "|generation="
                           + std::to_string (actor_ref.object_generation ()));
            reply.actors.push_back (
              {.actor_id = actor_id,
               .node_rid = std::string (actor_ref.node_rid ().value ()),
               .generation = actor_ref.object_generation ()});
        }
        return reply;
    }

  private:
    evidence_store_t &_evidence;
    zlink::framework::spot_node_manager_t &_spots;
    zlink::framework::session_actor_manager_t &_actors;
};

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using request_type = await_evidence_req_t;
    using reply_type = await_evidence_res_t;

    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    await_evidence_res_t handle (const await_evidence_req_t &request,
                                   const zlink::framework::route_handler_context_t &)
    {
        return _evidence.snapshot (request.request_id);
    }

  private:
    evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using request_type = await_evidence_wait_req_t;
    using reply_type = await_evidence_res_t;

    explicit evidence_wait_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    await_evidence_res_t handle (const await_evidence_wait_req_t &request,
                                   const zlink::framework::route_handler_context_t &)
    {
        return _evidence.wait (request);
    }

  private:
    evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
