/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_topology.hpp"
#include "../host_support.hpp"
#include "../sample_log_dir.hpp"
#include "Application/bingo_match_reservation_store.hpp"
#include "Infrastructure/Redis/redis_bingo_match_reservation_store.hpp"

#include <chrono>
#include <memory>
#include <zlink/locations/redis.hpp>
#include <zlink/codecs/protobuf.hpp>

namespace zlink::samples::bingo
{

using namespace framework;

class bingo_matchmaker_t;

struct bingo_matchmaker_idle_timer_t
{
    void handle (bingo_matchmaker_t &spot, const timer_tick_t &) const;
};

class bingo_matchmaker_t : public instance_spot_t
{
  public:
    bingo_matchmaker_t (instance_spot_context_t context,
                        bingo_match_reservation_store_t &reservations) :
        _reservations (reservations), _context (std::move (context))
    {
    }

    instance_spot_context_t &context () noexcept override { return _context; }

    const instance_spot_context_t &context () const noexcept override { return _context; }

    void configure () override
    {
        _context.handlers ().add_handler<&bingo_matchmaker_t::reserve> ();
    }

    task_t<void> on_initialize () override
    {
        _last_activity = std::chrono::steady_clock::now ();
        _idle_timer = _context.add_timer<bingo_matchmaker_idle_timer_t> ("bingo-matchmaker-idle",
                                                                         std::chrono::seconds (10));
        co_return;
    }

    reserve_bingo_room_res_t reserve (const reserve_bingo_room_req_t &request)
    {
        _last_activity = std::chrono::steady_clock::now ();
        return _reservations.reserve (request);
    }

    void close_if_idle ()
    {
        if (std::chrono::steady_clock::now () - _last_activity >= std::chrono::seconds (30)) {
            _context.close ();
        }
    }

  private:
    bingo_match_reservation_store_t &_reservations;
    instance_spot_context_t _context;
    zlink::framework::timer_t _idle_timer;
    std::chrono::steady_clock::time_point _last_activity{};
};

inline void bingo_matchmaker_idle_timer_t::handle (bingo_matchmaker_t &spot,
                                                   const timer_tick_t &) const
{
    spot.close_if_idle ();
}

class matchmaking_server_host_factory_t
{
  public:
    static app_t &configure (app_t &app, const sample_topology_t &topology)
    {
        app.logging ().use_console ().set_min_level (log_level_t::info);
        app.logging ().use_file (flow_log_path (topology.log_dir, "matchmaking"));
        observe_runtime_metrics (app, topology.log_dir, "matchmaking");
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
        options.codecs ().use (zlink::framework_codecs::protobuf ());
        options.add_location_store<redis::redis_location_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "location:");
        options.add_relocation_store<redis::redis_relocation_store_t> ()
          .set_connection_string (topology.redis_endpoint)
          .set_key_prefix (topology.redis_key_prefix + "relocation:");

        std::unique_ptr<bingo_match_reservation_store_t> reservations =
          std::make_unique<redis_bingo_match_reservation_store_t> (topology);
        options.services ().add_singleton<bingo_match_reservation_store_t> (
          std::move (reservations));

        auto mesh = options.add_route_mesh (sample_names_t::matchmaking_mesh);
        mesh.set_routing_id (zlink::routing_id_t::from ("bingo-matchmaking"))
          .listen (topology.matchmaking_route_endpoint);
        mesh.channel (sample_names_t::matchmaking_mesh).server ();
        mesh.objects ()
          .server ()
          .add_instance_spot_factory<bingo_matchmaker_t, bingo_match_reservation_store_t> (
            sample_names_t::matchmaker_spot)
          .recreate_on_relocation ();
        return app;
    }
};

} // namespace zlink::samples::bingo
