/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Common/store.hpp"
#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_configuration.hpp"

#include <zlink/framework.hpp>
#include <zlink/http_client.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

namespace zlink::samples::shoppingmall
{
using namespace zlink::framework;

class route_ready_http_handler_t
{
  public:
    using dependency_types = dependency_list_t<route_mesh_runtime_t>;

    explicit route_ready_http_handler_t (route_mesh_runtime_t &runtime) :
        _runtime (runtime)
    {
    }

    http_response_t handle (const http_request_t &request)
    {
        const auto found = request.query_values.find ("targetRid");
        if (found == request.query_values.end () || found->second.empty ())
            return {.status = 400,
                    .body = R"({"error":"targetRid is required"})"};

        const auto snapshot = _runtime.snapshot (sample_names_t::order_workflow_channel);
        for (const auto &peer : snapshot.peers) {
            if (peer.node_rid.to_string () == found->second
                && peer.state == peer_state_t::ready)
                return {.body = nlohmann::json{{"ready", true},
                                               {"targetRid", found->second}}
                                  .dump ()};
        }

        nlohmann::json peers = nlohmann::json::array ();
        for (const auto &peer : snapshot.peers)
            peers.push_back ({{"rid", peer.node_rid.to_string ()},
                              {"state", static_cast<int> (peer.state)}});
        return {.status = 503,
                .body = nlohmann::json{{"ready", false},
                                       {"targetRid", found->second},
                                       {"peers", std::move (peers)}}
                          .dump ()};
    }

  private:
    route_mesh_runtime_t &_runtime;
};

class commerce_api_handlers_t
{
  public:
    commerce_api_handlers_t (route_client_t &routes,
                             redis_state_store_t &store) :
        _routes (routes), _store (store)
    {
    }

    /* 공통 sample spec §16: CommerceApi는 HTTP API·검증·멱등 키 조회·조회 모델 조회만 맡고
     * 도메인 이벤트를 기록하지 않는다. 새 주문의 응답은 `Created`이며, 나머지 단계는 owner가
     * 배경에서 진행한다(§9.3). 클라이언트는 GetOrderState 폴링으로 종료를 확인한다. */
    task_t<start_order_res_t> start_order (const start_order_req_t &request)
    {
        auto command = _store.update ([&] (nlohmann::json &state) {
            auto &mappings = state["idempotency"];
            if (!mappings.contains (request.idempotency_key)) {
                const auto next = state.value ("nextOrderSequence", 0) + 1;
                state["nextOrderSequence"] = next;
                mappings[request.idempotency_key] = nlohmann::json{
                  {"orderId", "order-" + std::string (4 - std::to_string (next).size (), '0')
                                + std::to_string (next)},
                  {"started", false}};
            }
            const auto order_id =
              mappings[request.idempotency_key].value ("orderId", std::string{});

            /* 장바구니는 CommerceStateStore의 시드에서 읽어 검증한다 — 금액 범위나 cart id
             * 문자열 비교로 성공·실패를 흉내내지 않는다. */
            if (!state["carts"].contains (request.cart_id)) {
                throw std::runtime_error ("Unknown cart: " + request.cart_id);
            }
            const auto cart = state["carts"][request.cart_id].get<cart_seed_t> ();
            mappings[request.idempotency_key]["started"] = true;
            return start_order_workflow_req_t{order_id,
                                              request.cart_id,
                                              request.shipping_address_id,
                                              request.payment_method_id,
                                              request.idempotency_key,
                                              "start:" + request.idempotency_key,
                                              cart.lines,
                                              cart.amount,
                                              cart.currency};
        });

        auto state =
          (co_await request_workflow<start_order_workflow_res_t> (command)).state;
        std::cerr << "shoppingmall api: start order=" << state.order_id
                  << " status=" << state.status << "\n";
        co_return start_order_res_t{state.order_id, state};
    }

    get_order_state_res_t get_order (const get_order_state_req_t &request)
    {
        return _store.read ([&] (const nlohmann::json &state) {
            if (!state["readModels"].contains (request.order_id)) {
                throw framework_exception_t (
                  framework_error_kind_t::not_found,
                  "Order read model is not available: " + request.order_id);
            }
            return get_order_state_res_t{
              state["readModels"][request.order_id].get<order_state_t> ()};
        });
    }

    ok_res_t create_pending (const pending_mapping_req_t &request)
    {
        _store.update ([&] (nlohmann::json &state) {
            state["idempotency"][request.idempotency_key] =
              nlohmann::json{{"orderId", request.order_id},
                             {"started", false}};
            return true;
        });
        return {};
    }

    /* self-check hook(§15 "죽은 뒤 재개"): 배경 재개를 InventoryReserved에서 멈춰 중간 상태를
     * 만든다. API가 이벤트 스트림을 잘라내는 게 아니라, owner의 루프가 이 hook을 보고 멈춘다. */
    task_t<start_order_res_t> prepare_inventory_reserved (const start_order_req_t &request)
    {
        const auto order_id = _store.update ([&] (nlohmann::json &state) {
            auto &mappings = state["idempotency"];
            if (!mappings.contains (request.idempotency_key)) {
                const auto next = state.value ("nextOrderSequence", 0) + 1;
                state["nextOrderSequence"] = next;
                mappings[request.idempotency_key] = nlohmann::json{
                  {"orderId", "order-" + std::string (4 - std::to_string (next).size (), '0')
                                + std::to_string (next)},
                  {"started", false}};
            }
            const auto id = mappings[request.idempotency_key].value ("orderId", std::string{});
            state["testHooks"]["stopAt"][id] = order_status_t::inventory_reserved;
            return id;
        });
        (void) order_id;

        auto response = co_await start_order (request);
        auto state = co_await wait_for_status (response.order_id,
                                               order_status_t::inventory_reserved);
        _store.update ([&] (nlohmann::json &saved) {
            saved["testHooks"]["stopAt"].erase (response.order_id);
            return true;
        });
        co_return start_order_res_t{response.order_id, state};
    }

    task_t<continue_order_workflow_res_t> continue_order (const continue_order_workflow_req_t &request)
    {
        co_return co_await request_workflow<continue_order_workflow_res_t> (request);
    }

    ok_res_t delete_projection (const delete_projection_req_t &request)
    {
        _store.update ([&] (nlohmann::json &state) {
            state["readModels"].erase (request.order_id);
            return true;
        });
        return {};
    }

    task_t<rebuild_order_projection_res_t>
    rebuild_projection_req (const rebuild_order_projection_req_t &request)
    {
        co_return co_await request_workflow<rebuild_order_projection_res_t> (request);
    }

    server_assertion_res_t assert_server (const server_assertion_req_t &request)
    {
        return _store.read ([&] (const nlohmann::json &state) {
            std::vector<std::string> evidence;
            for (const auto &order_id : {request.successful_order_id,
                                         request.pending_recovered_order_id,
                                         request.concurrent_order_id,
                                         request.resumed_order_id,
                                         request.inventory_failure_order_id,
                                         request.payment_failure_order_id,
                                         request.scale_out_order_id}) {
                const auto events = event_types_for (state, order_id);
                std::string line = order_id + ":";
                for (std::size_t i = 0; i < events.size (); ++i) {
                    if (i > 0) line += ">";
                    line += events[i];
                }
                evidence.push_back (line);
            }
            evidence.push_back ("paymentFailures=" + std::to_string (state["paymentAttempts"].size ()));
            evidence.push_back (
              "releasedReservations=" + std::to_string (state["releasedReservations"].size ()));
            evidence.push_back ("startedIdempotency=" + std::to_string (state["idempotency"].size ()));
            evidence.push_back ("routing=global-order-id");
            const auto success = std::vector<std::string>{"OrderStartedEvent",
                                                          "InventoryReservedEvent",
                                                          "PaymentAuthorizedEvent",
                                                          "OrderConfirmedEvent"};
            const auto passed =
              has_sequence (state, request.successful_order_id, success)
              && has_prefix (state, request.pending_recovered_order_id, success)
              && has_sequence (state, request.concurrent_order_id, success)
              && has_sequence (state, request.resumed_order_id, success)
              && has_sequence (state,
                               request.inventory_failure_order_id,
                               {"OrderStartedEvent",
                                "InventoryReservationFailedEvent",
                                "OrderFailedEvent"})
              && has_sequence (state,
                               request.payment_failure_order_id,
                               {"OrderStartedEvent",
                                "InventoryReservedEvent",
                                "PaymentFailedEvent",
                                "InventoryReleasedEvent",
                                "OrderFailedEvent"})
              && has_sequence (state, request.scale_out_order_id, success)
              && state["paymentAttempts"].size () >= 1 && state["releasedReservations"].size () >= 1
              && state["idempotency"].size () == 7;
            std::cerr << "shoppingmall evidence: ";
            for (const auto &line : evidence) std::cerr << line << "; ";
            std::cerr << "\n";
            return server_assertion_res_t{passed, evidence};
        });
    }

  private:
    /* 조회 모델을 폴링해 원하는 상태에 도달할 때까지 기다린다(self-check hook 전용). */
    task_t<order_state_t> wait_for_status (const std::string &order_id, const std::string &status)
    {
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto current = _store.read ([&] (const nlohmann::json &state) {
                return state["readModels"].contains (order_id)
                         ? state["readModels"][order_id].get<order_state_t> ()
                         : order_state_t{};
            });
            if (current.status == status) {
                co_return current;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "Order '" + order_id + "' did not reach status " + status);
    }

    template <typename TReply, typename TRequest>
    task_t<TReply> request_workflow (const TRequest &request)
    {
        co_return co_await _routes
          .request_to_spot (spot_id_t (request.order_id), request)
          .instance_spot (sample_names_t::order_workflow_spot)
          .timeout (std::chrono::milliseconds (5000))
          .template submit<TReply> ();
    }

    route_client_t &_routes;
    redis_state_store_t &_store;
};

/* 공통 sample spec: 샘플 handler는 framework가 처리하는 dispatch 오류를 다시 잡아
 * 로그만 남기고 되던지지 않는다. 실패는 dispatch 경계(error reply/observer/기본 로그)가
 * 소유한다. */
#define SHOPPINGMALL_HANDLER(name, req, res, method)                                               \
    class name                                                                                     \
    {                                                                                              \
      public:                                                                                      \
        using request_type = req;                                                                  \
        using reply_type = res;                                                                    \
        using dependency_types = dependency_list_t<commerce_api_handlers_t>;                       \
        static constexpr const char *topic_name = req::packet_name;                                \
        explicit name (commerce_api_handlers_t &handlers) : _handlers (handlers) {}                \
        auto handle (const request_type &request) { return _handlers.method (request); }           \
                                                                                                   \
      private:                                                                                     \
        commerce_api_handlers_t &_handlers;                                                        \
    };

SHOPPINGMALL_HANDLER (start_order_handler_t, start_order_req_t, start_order_res_t, start_order)
SHOPPINGMALL_HANDLER (get_order_handler_t, get_order_state_req_t, get_order_state_res_t, get_order)
SHOPPINGMALL_HANDLER (pending_handler_t, pending_mapping_req_t, ok_res_t, create_pending)
SHOPPINGMALL_HANDLER (prepare_handler_t, start_order_req_t, start_order_res_t, prepare_inventory_reserved)
SHOPPINGMALL_HANDLER (continue_handler_t, continue_order_workflow_req_t, continue_order_workflow_res_t, continue_order)
SHOPPINGMALL_HANDLER (delete_projection_handler_t, delete_projection_req_t, ok_res_t, delete_projection)
SHOPPINGMALL_HANDLER (rebuild_projection_handler_t, rebuild_order_projection_req_t, rebuild_order_projection_res_t, rebuild_projection_req)
SHOPPINGMALL_HANDLER (assert_handler_t, server_assertion_req_t, server_assertion_res_t, assert_server)

} // namespace zlink::samples::shoppingmall

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::shoppingmall;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    auto instance = topology.for_api_instance (configuration.role.name);
    redis_state_store_t store{topology};
    store.seed_defaults ();
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.services ().add_singleton<sample_topology_t> (
          std::make_unique<sample_topology_t> (topology));
        options.services ().add_singleton<api_instance_topology_t> (
          std::make_unique<api_instance_topology_t> (instance));
        options.services ()
          .add_singleton<redis_state_store_t, sample_topology_t> ()
          .add_singleton<commerce_api_handlers_t,
                         route_client_t,
                         redis_state_store_t> ();
        add_shoppingmall_location_store (options, topology);
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label (instance.instance_id);
        /* 공통 sample spec §16: 서버 발견은 registry 프로세스 없이 공유 location store가 맡는다.
         * endpoint를 코드에 박지 않는다. */
        auto workflow = options.add_route_mesh (sample_names_t::order_workflow_channel);
        workflow
          .set_routing_id (zlink::routing_id_t::from (
            "shoppingmall-" + instance.instance_id + "-workflow"))
          .set_object_role (object_role_t::client)
          .listen (instance.route_endpoint);
        options.http ()
          .listen (instance.http_url)
          .map_health ("/health")
          .map_get<route_ready_http_handler_t> ("/ready")
          .map_post<start_order_handler_t> ("/orders/start")
          .map_post<get_order_handler_t> ("/orders/get")
          .map_post<pending_handler_t> ("/self-check/idempotency/pending")
          .map_post<prepare_handler_t> ("/self-check/workflow/inventory-reserved")
          .map_post<continue_handler_t> ("/self-check/workflow/continue")
          .map_post<delete_projection_handler_t> ("/self-check/projection/delete")
          .map_post<rebuild_projection_handler_t> ("/self-check/projection/rebuild")
          .map_post<assert_handler_t> ("/self-check/assert");
    });
    return app.run (argc, argv);
}
