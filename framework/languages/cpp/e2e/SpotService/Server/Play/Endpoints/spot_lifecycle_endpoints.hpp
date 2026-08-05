/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Handlers/play_control_handlers.hpp"

#include <zlink/framework.hpp>

inline void map_spot_lifecycle_endpoints (zlink::framework::http_options_builder_t &http)
{
    http.map_post<create_spot_handler_t> ("/spot/create")
      .map_post<create_alternate_spot_handler_t> ("/spot/create-alternate")
      .map_post<lifecycle_spot_handler_t> ("/spot/lifecycle")
      .map_post<close_spot_handler_t> ("/spot/close")
      .map_post<type_mismatch_spot_handler_t> ("/spot/type-mismatch");
}
