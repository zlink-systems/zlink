/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Handlers/play_spot_route_handlers.hpp"

#include <zlink/framework.hpp>

inline void map_spot_failure_endpoints (zlink::framework::http_options_builder_t &http)
{
    http.map_post<spot_slow_route_handler_t> ("/spot/slow/request")
      .map_post<spot_missing_handler_request_handler_t> ("/spot/missing-handler/request")
      .map_post<spot_missing_handler_command_handler_t> ("/spot/missing-handler/command")
      .map_post<spot_missing_target_request_handler_t> ("/spot/missing-target/request")
      .map_post<spot_missing_route_handler_t> ("/spot/missing-route")
      .map_post<spot_to_spot_timeout_route_handler_t> ("/spot/to-spot/timeout")
      .map_post<spot_to_spot_negative_route_handler_t> ("/spot/to-spot/negative");
}
