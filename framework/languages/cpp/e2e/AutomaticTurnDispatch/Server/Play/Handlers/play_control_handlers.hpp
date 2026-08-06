/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/play_support.hpp"

#include <zlink/framework.hpp>

#include <string>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
{

inline zlink::framework::spot_create_result_t
ensure_probe_spot (zlink::framework::spot_manager_t &spots,
                   zlink::framework::spot_id_t spot_id)
{
    auto created = spots.get_or_create (std::move (spot_id), probe_spot_name).submit ().result ();
    if (!created) {
        throw zlink::framework::framework_exception_t (
          created.error_kind (),
          created.error () ? created.error ()->what () : "probe Spot creation failed");
    }
    return created.value ();
}

class ensure_spot_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<evidence_store_t,
                                          zlink::framework::spot_manager_t>;
    using request_type = ensure_spot_req_t;
    using reply_type = ensure_spot_res_t;

    ensure_spot_handler_t (evidence_store_t &evidence,
                           zlink::framework::spot_manager_t &spots) :
        _evidence (evidence), _spots (spots)
    {
    }

    ensure_spot_res_t handle (const ensure_spot_req_t &request,
                              const zlink::framework::route_message_context_t &)
    {
        try {
            const auto created = ensure_probe_spot (_spots, request.spot_id);
            _evidence.add ("spot-ensured|rid=" + _evidence.node_rid + "|spot="
                           + created.spot.spot_id () + "|request="
                           + request.spot_id);
            return {.spot_id = created.spot.spot_id (),
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
    zlink::framework::spot_manager_t &_spots;
};

class bind_await_actors_handler_t
{
  public:
    using dependency_types =
      zlink::framework::dependency_list_t<evidence_store_t,
                                          zlink::framework::spot_manager_t,
                                          zlink::framework::session_actor_manager_t>;
    using request_type = bind_await_actors_req_t;
    using reply_type = bind_await_actors_res_t;

    bind_await_actors_handler_t (evidence_store_t &evidence,
                                 zlink::framework::spot_manager_t &spots,
                                 zlink::framework::session_actor_manager_t &actors) :
        _evidence (evidence), _spots (spots), _actors (actors)
    {
    }

    bind_await_actors_res_t handle (
      const bind_await_actors_req_t &request,
      const zlink::framework::route_message_context_t &)
    {
        const auto spot_id = (request.spot_id);
        (void) ensure_probe_spot (_spots, spot_id);
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
            const auto request_id = "bind-" + actor_id;
            bound.value ().context ()
              .join_spot (spot_id,
                          delay_req_t{.request_id = request_id,
                                      .delay_ms = 0,
                                      .marker = "bind"})
              .timeout (std::chrono::milliseconds (3000))
              .defer ();
            auto actor_ref = bound.value ().ref ();
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
    zlink::framework::spot_manager_t &_spots;
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
                                 const zlink::framework::route_message_context_t &)
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
                                 const zlink::framework::route_message_context_t &)
    {
        return _evidence.wait (request);
    }

  private:
    evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::play
