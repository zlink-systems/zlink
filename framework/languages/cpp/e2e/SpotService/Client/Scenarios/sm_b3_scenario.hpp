/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <zlink/http_client.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework::e2e::spot_service::client::scenarios
{

inline void run_sm_b3_scenario (const std::string &play_http_endpoint)
{
    if (play_http_endpoint.empty ()) {
        throw std::runtime_error ("playHttpEndpoint is required for SM-B3");
    }

    constexpr auto actor_id = "sm-b3-complex";
    auto play_a = zlink::http_client::client_t::create ()
                    .base_url (play_http_endpoint)
                    .build ();
    auto complex =
      play_a.post ("/spot/complex")
        .body (spot_complex_actor_req_t{
          .join = join_req_t{.key = "sm-b3-complex",
                             .actor_id = actor_id,
                             .display_name = "SM-B3 Join",
                             .level = 3,
                             .tags = {"join", "complex"}},
          .complex = complex_actor_req_t{
            .display_name = "Ada Lovelace",
            .level = 42,
            .tags = {"alpha", "beta", "gamma"},
            .attributes = {{"role", "analyst"}, {"region", "west"}}}})
        .submit_raw ()
        .result ();
    if (!complex) {
        throw std::runtime_error (complex.error () ? complex.error ()->what ()
                                                   : "SM-B3 complex HTTP failed");
    }
    if (complex.value ().status >= 400) {
        throw std::runtime_error ("SM-B3 complex HTTP status "
                                  + std::to_string (complex.value ().status) + ": "
                                  + complex.value ().body);
    }

    const auto reply =
      nlohmann::json::parse (complex.value ().body).get<spot_complex_actor_res_t> ();
    if (reply.join.actor_id != actor_id || reply.join.owner_node_rid != "play-a"
        || reply.join.spot_id != "user:play-a:sm-b3-complex"
        || reply.join.tags != std::vector<std::string> ({"join", "complex"})) {
        throw std::runtime_error ("SM-B3 join payload fidelity mismatch");
    }
    if (reply.complex.actor_id != actor_id || reply.complex.display_name != "Ada Lovelace"
        || reply.complex.level != 42
        || reply.complex.tags != std::vector<std::string> ({"alpha", "beta", "gamma"})
        || reply.complex.attributes.at ("role") != "analyst"
        || reply.complex.attributes.at ("region") != "west") {
        throw std::runtime_error ("SM-B3 complex payload fidelity mismatch");
    }
}

} // namespace zlink::framework::e2e::spot_service::client::scenarios
