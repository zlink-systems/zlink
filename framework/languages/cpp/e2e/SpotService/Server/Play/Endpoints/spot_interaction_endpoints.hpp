/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Handlers/play_actor_handlers.hpp"
#include "../Handlers/play_session_handlers.hpp"
#include "../Handlers/play_spot_route_handlers.hpp"

#include <zlink/framework.hpp>

inline void map_spot_interaction_endpoints (zlink::framework::http_options_builder_t &http)
{
    http.map_post<join_spot_handler_t> ("/spot/join")
      .map_post<complex_actor_handler_t> ("/spot/complex")
      .map_post<missing_actor_handler_t> ("/spot/missing-actor")
      .map_post<mutate_spot_state_handler_t> ("/spot/state")
      .map_post<push_bound_session_handler_t> ("/spot/push-bound-session")
      .map_post<remote_actor_flow_handler_t> ("/spot/remote-actor")
      .map_post<remote_actor_request_handler_t> ("/spot/remote-actor-request")
      .map_post<spot_worker_start_route_handler_t> ("/spot/worker/start")
      .map_post<spot_worker_complete_handler_t> ("/spot/worker/complete")
      .map_post<route_spot_state_handler_t> ("/spot/state/request")
      .map_post<spot_stage_probe_route_handler_t> ("/spot/stage/request")
      .map_post<spot_stage_timer_route_handler_t> ("/spot/stage/timer")
      .map_post<spot_state_command_route_handler_t> ("/spot/state/command")
      .map_post<spot_publish_route_handler_t> ("/spot/publish")
      .map_post<spot_publish_wait_handler_t> ("/spot/publish/wait")
      .map_post<spot_idle_close_route_handler_t> ("/spot/idle-close/start")
      .map_post<spot_overrun_start_route_handler_t> ("/spot/overrun/start")
      .map_post<spot_outbound_route_handler_t> ("/spot/outbound")
      .map_post<spot_outbound_negative_route_handler_t> ("/spot/outbound-negative")
      .map_post<spot_to_spot_route_handler_t> ("/spot/to-spot/request")
      .map_post<spot_to_spot_route_handler_t> ("/spot/to-spot/request-cross")
      .map_post<direct_spot_route_handler_t> ("/spot/direct")
      .map_post<direct_spot_command_route_handler_t> ("/spot/direct-command");
}
