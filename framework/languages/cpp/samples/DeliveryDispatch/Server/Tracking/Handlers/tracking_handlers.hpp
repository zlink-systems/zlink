/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Configuration/evidence_store.hpp"
#include <zlink/framework.hpp>

#include <iostream>
#include <string>

namespace zlink::samples::deliverydispatch
{

/* 공통 sample spec §7.3: Tracking은 상태 event를 기록한 뒤 고객 actor에게
 * `DeliveryStatusUpdatedMsg`를 응답 없는 one-way로 보낸다. push는 고객 actor가 자기 bound
 * session으로 한다 — Tracking은 client session을 알지 않는다. */
class delivery_status_changed_handler_t
{
  public:
    using request_type = delivery_status_changed_req_t;
    using reply_type = delivery_status_changed_res_t;
    using dependency_types =
      zlink::framework::dependency_list_t<evidence_store_t,
                                          zlink::framework::actor_directory_t,
                                          zlink::framework::actor_client_t>;
    static constexpr const char *topic_name = "DeliveryStatusChangedReq";

    delivery_status_changed_handler_t (evidence_store_t &evidence,
                                       zlink::framework::actor_directory_t &actor_directory,
                                       zlink::framework::actor_client_t &actors) :
        _evidence (evidence), _actor_directory (actor_directory), _actors (actors)
    {
    }

    zlink::framework::task_t<delivery_status_changed_res_t>
    handle (const delivery_status_changed_req_t &request)
    {
        _evidence.append (request);

        auto actor_ref = co_await _actor_directory.find (request.customer_id);
        if (!actor_ref) {
            throw zlink::framework::framework_exception_t (
              zlink::framework::framework_error_kind_t::not_found,
              "customer actor route was not found");
        }
        _actors
          .send (actor_ref->actor_id (),
                          delivery_status_updated_msg_t{request.delivery_id,
                                                        request.customer_id,
                                                        request.status, request.courier_id,
                                                        request.occurred_at_unix_ms})
          .submit ();

        std::cerr << "deliverydispatch tracking: status delivery=" << request.delivery_id
                  << " status=" << request.status << " courier="
                  << request.courier_id.value_or ("none") << "\n";
        co_return delivery_status_changed_res_t{request.delivery_id, request.status};
    }

  private:
    evidence_store_t &_evidence;
    zlink::framework::actor_directory_t &_actor_directory;
    zlink::framework::actor_client_t &_actors;
};

} // namespace zlink::samples::deliverydispatch
