/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Shared/Contracts/messages.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::samples::shoppingmall
{

inline void ensure (bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error ("Ensure failed: " + message);
}

inline order_state_t get_order (zlink::http_client::client_t &api, const std::string &order_id)
{
    return api.post ("/orders/get")
      .body (get_order_state_req_t{order_id})
      .fetch<get_order_state_res_t> ()
      .state;
}

inline order_state_t wait_for_status (zlink::http_client::client_t &api,
                                      const std::string &order_id,
                                      const std::string &status)
{
    for (int i = 0; i < 80; ++i) {
        auto state = get_order (api, order_id);
        if (state.status == status)
            return state;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("Timed out waiting for order status " + status);
}

inline bool is_started_or_confirmed (const order_state_t &state)
{
    return state.status == order_status_t::created
           || state.status == order_status_t::inventory_reserved
           || state.status == order_status_t::payment_authorized
           || state.status == order_status_t::confirmed;
}

class shoppingmall_client_scenario_t
{
  public:
    void run (const std::string &api_a_http_url, const std::string &api_b_http_url)
    {
        auto api_a = zlink::http_client::client_t::create (api_a_http_url)
                       .timeout (std::chrono::milliseconds (5000))
                       .build ();
        auto api_b = zlink::http_client::client_t::create (api_b_http_url)
                       .timeout (std::chrono::milliseconds (5000))
                       .build ();

        const auto success_req =
          start_order_req_t{"cart-success", "addr-home", "pm-ok", "order-success-001"};
        auto success = api_a.post ("/orders/start").body (success_req).fetch<start_order_res_t> ();
        /* 공통 sample spec §15: 새 주문의 StartOrderRes는 `Created`만 담고 즉시 돌아온다. 종료는
     * GetOrderStateReq 폴링으로 확인한다. */
        ensure (success.state.status == order_status_t::created, "new order responds Created");
        auto created = get_order (api_a, success.order_id);
        ensure (is_started_or_confirmed (created), "successful order was created");
        ensure (created.shipping_address_id.value_or ("") == success_req.shipping_address_id,
                "shipping address");
        auto confirmed = wait_for_status (api_a, success.order_id, order_status_t::confirmed);
        ensure (confirmed.reservation_id.has_value (), "reservation id");
        ensure (confirmed.payment_id.has_value (), "payment id");
        ensure (confirmed.amount == decimal_t ("120.00"), "amount");
        ensure (confirmed.currency.value_or ("") == "USD", "currency");

        auto duplicate =
          api_b.post ("/orders/start").body (success_req).fetch<start_order_res_t> ();
        ensure (duplicate.order_id == success.order_id, "duplicate idempotency");

        const auto concurrent_req =
          start_order_req_t{"cart-success", "addr-office", "pm-ok", "order-concurrent-001"};
        auto concurrent_a =
          api_a.post ("/orders/start").body (concurrent_req).fetch<start_order_res_t> ();
        auto concurrent_b =
          api_b.post ("/orders/start").body (concurrent_req).fetch<start_order_res_t> ();
        ensure (concurrent_a.order_id == concurrent_b.order_id, "concurrent idempotency");
        auto concurrent_confirmed =
          wait_for_status (api_a, concurrent_a.order_id, order_status_t::confirmed);
        ensure (concurrent_confirmed.status == order_status_t::confirmed, "concurrent confirmed");

        const auto pending_req =
          start_order_req_t{"cart-success", "addr-office", "pm-ok", "order-pending-001"};
        auto pending_ok = api_a.post ("/self-check/idempotency/pending")
                            .body (pending_mapping_req_t{pending_req.idempotency_key,
                                                         "order-pending-0001"})
                            .fetch<ok_res_t> ();
        ensure (pending_ok.ok, "pending hook");
        auto pending = api_b.post ("/orders/start").body (pending_req).fetch<start_order_res_t> ();
        ensure (pending.order_id == "order-pending-0001", "pending order id");
        ensure (pending.state.status == order_status_t::created, "pending recovered as Created");
        auto pending_confirmed =
          wait_for_status (api_a, pending.order_id, order_status_t::confirmed);
        ensure (pending_confirmed.status == order_status_t::confirmed, "pending confirmed");

        const auto resume_req =
          start_order_req_t{"cart-success", "addr-home", "pm-ok", "order-resume-001"};
        auto inventory_reserved = api_a.post ("/self-check/workflow/inventory-reserved")
                                    .body (resume_req)
                                    .fetch<start_order_res_t> ();
        ensure (inventory_reserved.state.status == order_status_t::inventory_reserved,
                "inventory reserved checkpoint");
        auto resumed = api_b.post ("/self-check/workflow/continue")
                         .body (continue_order_workflow_req_t{inventory_reserved.order_id,
                                                              "continue:" + inventory_reserved.order_id})
                         .fetch<continue_order_workflow_res_t> ();
        ensure (resumed.state.status == order_status_t::confirmed, "resumed confirmed");
        ensure (resumed.state.reservation_id.value_or ("")
                  == "reservation-" + inventory_reserved.order_id,
                "resumed reservation");
        ensure (resumed.state.payment_id.value_or ("")
                  == "payment-" + inventory_reserved.order_id,
                "resumed payment");

        const auto inventory_req =
          start_order_req_t{"cart-inventory-fail", "addr-home", "pm-ok", "order-inventory-001"};
        auto inventory_started =
          api_a.post ("/orders/start").body (inventory_req).fetch<start_order_res_t> ();
        auto inventory_failed =
          wait_for_status (api_a, inventory_started.order_id, order_status_t::failed);
        ensure (inventory_failed.reason.value_or ("").find ("inventory") != std::string::npos,
                "inventory failure");

        const auto payment_req =
          start_order_req_t{"cart-success", "addr-home", "pm-decline", "order-payment-001"};
        auto payment_started =
          api_b.post ("/orders/start").body (payment_req).fetch<start_order_res_t> ();
        auto payment_failed =
          wait_for_status (api_b, payment_started.order_id, order_status_t::failed);
        ensure (payment_failed.reservation_id.has_value (), "payment failure reservation");
        ensure (payment_failed.reason.value_or ("").find ("payment") != std::string::npos,
                "payment failure");

        ensure (api_a.post ("/self-check/projection/delete")
                  .body (delete_projection_req_t{success.order_id})
                  .fetch<ok_res_t> ()
                  .ok,
                "delete projection");
        auto healed = api_b.post ("/self-check/workflow/continue")
                        .body (continue_order_workflow_req_t{success.order_id,
                                                             "continue:" + success.order_id})
                        .fetch<continue_order_workflow_res_t> ();
        ensure (healed.state.status == order_status_t::confirmed, "healed projection");
        ensure (api_a.post ("/self-check/projection/delete")
                  .body (delete_projection_req_t{success.order_id})
                  .fetch<ok_res_t> ()
                  .ok,
                "delete projection again");
        auto rebuilt = api_a.post ("/self-check/projection/rebuild")
                         .body (rebuild_order_projection_req_t{success.order_id,
                                                               "rebuild:" + success.order_id})
                         .fetch<rebuild_order_projection_res_t> ();
        ensure (rebuilt.state.status == order_status_t::confirmed, "rebuilt projection");
        auto rebuilt_read = get_order (api_b, success.order_id);
        ensure (rebuilt_read.status == order_status_t::confirmed, "rebuilt read");

        auto delayed_first = get_order (api_b, payment_started.order_id);
        auto delayed_second = get_order (api_a, payment_started.order_id);
        ensure (delayed_first.status == delayed_second.status, "delayed read consistency");
        ensure (delayed_second.status == order_status_t::failed, "delayed read failed");

        const auto scale_req =
          start_order_req_t{"cart-success", "addr-office", "pm-ok", "order-scale-001"};
        auto scale = api_b.post ("/orders/start").body (scale_req).fetch<start_order_res_t> ();
        auto scale_confirmed = wait_for_status (api_a, scale.order_id, order_status_t::confirmed);
        ensure (scale_confirmed.status == order_status_t::confirmed, "scale confirmed");

        auto assertion =
          api_a.post ("/self-check/assert")
            .body (server_assertion_req_t{success.order_id, pending.order_id, concurrent_a.order_id,
                                          inventory_reserved.order_id, inventory_started.order_id,
                                          payment_started.order_id, scale.order_id})
            .fetch<server_assertion_res_t> ();
        ensure (assertion.passed, "server evidence");
    }
};

} // namespace zlink::samples::shoppingmall
