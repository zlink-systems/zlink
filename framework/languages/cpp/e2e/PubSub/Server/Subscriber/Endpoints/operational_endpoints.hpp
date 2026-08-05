/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Handlers/event_msg_handler.hpp"

namespace zlink::framework::e2e::pubsub::server::subscriber
{

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;

    explicit evidence_handler_t (evidence_store_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_state.snapshot ()).dump ();
        return response;
    }

  private:
    evidence_store_t &_state;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<evidence_store_t>;
    using request_type = zlink::framework::e2e::pubsub::evidence_wait_req_t;
    using reply_type = std::vector<std::string>;

    explicit evidence_wait_handler_t (evidence_store_t &state) : _state (state) {}

    std::vector<std::string> handle (
      const zlink::framework::e2e::pubsub::evidence_wait_req_t &request)
    {
        return _state.wait (request);
    }

  private:
    evidence_store_t &_state;
};

} // namespace zlink::framework::e2e::pubsub::server::subscriber
