/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/spot_service_contracts.hpp"

#include <zlink/framework.hpp>

namespace e2e = zlink::framework::e2e::spot_service;

class push_bound_session_handler_t
{
  public:

    explicit push_bound_session_handler_t (zlink::framework::session_actor_manager_t &actors) :
        _actors (actors)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &http)
    {
        const auto request = nlohmann::json::parse (http.body).get<e2e::bound_session_push_req_t> ();
        auto actor = _actors.find (request.actor_id);
        if (!actor) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "push actor route was not found");
        }
        auto reply =
          actor->relay_request ("PushReq", zlink::message_t::from_json (request.push))
            .async ()
            .result ();
        if (!reply) {
            throw zlink::framework::framework_exception_t (
              reply.error_kind (),
              reply.error () ? reply.error ()->what () : "PushReq failed");
        }

        zlink::framework::http_response_t response;
        try {
            response.body = nlohmann::json (reply.value ().parse_json<e2e::actor_push_res_t> ()).dump ();
        }
        catch (const std::exception &error) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::protocol_error,
              std::string ("PushReq reply decode failed: ") + error.what ()
                + " body=" + reply.value ().to_string ());
        }
        return response;
    }

  private:
    zlink::framework::session_actor_manager_t &_actors;
};
