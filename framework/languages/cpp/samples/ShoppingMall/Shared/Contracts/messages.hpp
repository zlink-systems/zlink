/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <nlohmann/json.hpp>
#include <zlink/json_profile.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::samples::shoppingmall
{

struct order_status_t
{
    static constexpr const char *created = "Created";
    static constexpr const char *inventory_reserved = "InventoryReserved";
    static constexpr const char *payment_authorized = "PaymentAuthorized";
    static constexpr const char *confirmed = "Confirmed";
    static constexpr const char *failed = "Failed";
};

struct order_line_input_t
{
    std::string sku;
    int quantity{};
};

struct start_order_req_t
{
    static constexpr const char *packet_name = "StartOrderReq";
    std::string cart_id;
    std::string shipping_address_id;
    std::string payment_method_id;
    std::string idempotency_key;
};

struct get_order_state_req_t
{
    static constexpr const char *packet_name = "GetOrderStateReq";
    std::string order_id;
};

struct order_state_t
{
    std::string order_id;
    std::string status;
    std::optional<std::string> shipping_address_id;
    std::optional<std::string> reservation_id;
    std::optional<std::string> payment_id;
    std::optional<std::string> reason;
    std::optional<double> amount;
    std::optional<std::string> currency;
    std::int64_t updated_at_unix_ms{};
};

struct start_order_res_t
{
    static constexpr const char *packet_name = "StartOrderRes";
    std::string order_id;
    order_state_t state;
};

struct get_order_state_res_t
{
    static constexpr const char *packet_name = "GetOrderStateRes";
    order_state_t state;
};

/* 공통 sample spec §11: 주문 이벤트 스트림 레코드. payload는 이벤트 종류별 필드를 담는다. */
struct stored_order_event_t
{
    std::string event_id;
    std::string source_command_id;
    std::string order_id;
    std::string event_type;
    nlohmann::json payload = nlohmann::json::object ();
    std::int64_t version{};
    std::int64_t created_at_unix_ms{};
};

struct order_event_types_t
{
    static constexpr const char *order_started = "OrderStartedEvent";
    static constexpr const char *inventory_reserved = "InventoryReservedEvent";
    static constexpr const char *inventory_reservation_failed = "InventoryReservationFailedEvent";
    static constexpr const char *payment_authorized = "PaymentAuthorizedEvent";
    static constexpr const char *payment_failed = "PaymentFailedEvent";
    static constexpr const char *inventory_released = "InventoryReleasedEvent";
    static constexpr const char *order_confirmed = "OrderConfirmedEvent";
    static constexpr const char *order_failed = "OrderFailedEvent";
};

/* 모듈 호출 계약(§11). ReservationId·PaymentId는 owner spot이 결정적으로 만들어 넘기고, 모듈은
 * 같은 id로 다시 부르면 최초 결과를 그대로 돌려준다. */
struct reserve_inventory_command_t
{
    std::string order_id;
    std::string reservation_id;
    std::vector<order_line_input_t> lines;
};

struct reserve_inventory_result_t
{
    bool accepted{};
    std::string reason;
};

struct release_inventory_command_t
{
    std::string order_id;
    std::string reservation_id;
    std::string reason;
};

struct release_inventory_result_t
{
    bool released{};
};

struct authorize_payment_command_t
{
    std::string order_id;
    std::string payment_id;
    std::string payment_method_id;
    double amount{};
    std::string currency;
};

struct authorize_payment_result_t
{
    bool accepted{};
    std::string reason;
};

/* Test/evidence-only self-check seed. These values initialize the sample's deterministic stores;
 * they are not part of the order application message contract. */
struct cart_seed_t
{
    std::string cart_id;
    std::vector<order_line_input_t> lines;
    double amount{};
    std::string currency;
};

struct inventory_seed_t
{
    std::string sku;
    int available_quantity{};
};

struct payment_method_seed_t
{
    std::string payment_method_id;
    bool should_authorize{};
    std::string failure_reason;
};

struct start_order_workflow_req_t
{
    static constexpr const char *packet_name = "StartOrderWorkflowReq";
    std::string order_id;
    std::string cart_id;
    std::string shipping_address_id;
    std::string payment_method_id;
    std::string idempotency_key;
    std::string source_command_id;
    std::vector<order_line_input_t> lines;
    double amount{};
    std::string currency;
};

struct start_order_workflow_res_t
{
    static constexpr const char *packet_name = "StartOrderWorkflowRes";
    order_state_t state;
};

/* 시작 handler가 스스로 예약하는 재개 호출(§9.3). 응답을 기다리지 않는 one-way다. */
struct continue_order_workflow_msg_t
{
    static constexpr const char *packet_name = "ContinueOrderWorkflowMsg";
    std::string order_id;
};

struct continue_order_workflow_req_t
{
    static constexpr const char *packet_name = "ContinueOrderWorkflowReq";
    std::string order_id;
    std::string source_command_id;
};

struct continue_order_workflow_res_t
{
    static constexpr const char *packet_name = "ContinueOrderWorkflowRes";
    order_state_t state;
};

struct rebuild_order_projection_req_t
{
    static constexpr const char *packet_name = "RebuildOrderProjectionReq";
    std::string order_id;
    std::string source_command_id;
};

struct rebuild_order_projection_res_t
{
    static constexpr const char *packet_name = "RebuildOrderProjectionRes";
    order_state_t state;
};

struct pending_mapping_req_t
{
    static constexpr const char *packet_name = "PendingMappingReq";
    std::string idempotency_key;
    std::string order_id;
};

struct delete_projection_req_t
{
    static constexpr const char *packet_name = "DeleteProjectionReq";
    std::string order_id;
};

struct ok_res_t
{
    static constexpr const char *packet_name = "OkRes";
    bool ok{true};
};

/* Test/evidence-only HTTP assertion messages. They expose runner evidence and are not a public
 * order API. */
struct server_assertion_req_t
{
    static constexpr const char *packet_name = "ServerAssertionReq";
    std::string successful_order_id;
    std::string pending_recovered_order_id;
    std::string concurrent_order_id;
    std::string resumed_order_id;
    std::string inventory_failure_order_id;
    std::string payment_failure_order_id;
    std::string scale_out_order_id;
};

struct server_assertion_res_t
{
    static constexpr const char *packet_name = "ServerAssertionRes";
    bool passed{};
    std::vector<std::string> evidence;
};

inline std::string json_string (const nlohmann::json &json, const char *camel, const char *snake)
{
    return json.contains (camel) ? json.value (camel, std::string{})
                                 : json.value (snake, std::string{});
}

inline std::optional<std::string>
json_nullable_string (const nlohmann::json &json, const char *camel, const char *snake)
{
    const auto read = [&json] (const char *name) -> std::optional<std::string> {
        if (!json.contains (name) || json[name].is_null ())
            return std::nullopt;
        return json.value (name, std::string{});
    };
    return json.contains (camel) ? read (camel) : read (snake);
}

}

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::order_line_input_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sku", &zlink::samples::shoppingmall::order_line_input_t::sku),
      zlink::json::field ("quantity", &zlink::samples::shoppingmall::order_line_input_t::quantity)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::continue_order_workflow_msg_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "orderId", &zlink::samples::shoppingmall::continue_order_workflow_msg_t::order_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::stored_order_event_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("eventId", &zlink::samples::shoppingmall::stored_order_event_t::event_id),
      zlink::json::field ("sourceCommandId",
                          &zlink::samples::shoppingmall::stored_order_event_t::source_command_id),
      zlink::json::field ("orderId", &zlink::samples::shoppingmall::stored_order_event_t::order_id),
      zlink::json::field ("eventType",
                          &zlink::samples::shoppingmall::stored_order_event_t::event_type),
      zlink::json::field ("payload", &zlink::samples::shoppingmall::stored_order_event_t::payload),
      zlink::json::field ("version", &zlink::samples::shoppingmall::stored_order_event_t::version),
      zlink::json::field ("createdAtUnixMs",
                          &zlink::samples::shoppingmall::stored_order_event_t::created_at_unix_ms)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::authorize_payment_command_t>
{
    using type = zlink::samples::shoppingmall::authorize_payment_command_t;
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId", &type::order_id),
      zlink::json::field ("paymentId", &type::payment_id),
      zlink::json::field ("paymentMethodId", &type::payment_method_id),
      zlink::json::field ("amount", &type::amount),
      zlink::json::field ("currency", &type::currency)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::cart_seed_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("cartId", &zlink::samples::shoppingmall::cart_seed_t::cart_id),
      zlink::json::field ("lines", &zlink::samples::shoppingmall::cart_seed_t::lines),
      zlink::json::field ("amount", &zlink::samples::shoppingmall::cart_seed_t::amount),
      zlink::json::field ("currency", &zlink::samples::shoppingmall::cart_seed_t::currency)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::inventory_seed_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sku", &zlink::samples::shoppingmall::inventory_seed_t::sku),
      zlink::json::field ("availableQuantity",
                          &zlink::samples::shoppingmall::inventory_seed_t::available_quantity)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::payment_method_seed_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("paymentMethodId",
                          &zlink::samples::shoppingmall::payment_method_seed_t::payment_method_id),
      zlink::json::field ("shouldAuthorize",
                          &zlink::samples::shoppingmall::payment_method_seed_t::should_authorize),
      zlink::json::field ("failureReason",
                          &zlink::samples::shoppingmall::payment_method_seed_t::failure_reason)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::start_order_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("cartId", &zlink::samples::shoppingmall::start_order_req_t::cart_id),
      zlink::json::field ("shippingAddressId",
                          &zlink::samples::shoppingmall::start_order_req_t::shipping_address_id),
      zlink::json::field ("paymentMethodId",
                          &zlink::samples::shoppingmall::start_order_req_t::payment_method_id),
      zlink::json::field ("idempotencyKey",
                          &zlink::samples::shoppingmall::start_order_req_t::idempotency_key)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::get_order_state_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "orderId", &zlink::samples::shoppingmall::get_order_state_req_t::order_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::order_state_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId", &zlink::samples::shoppingmall::order_state_t::order_id),
      zlink::json::field ("status", &zlink::samples::shoppingmall::order_state_t::status),
      zlink::json::field ("shippingAddressId",
                          &zlink::samples::shoppingmall::order_state_t::shipping_address_id),
      zlink::json::field ("reservationId",
                          &zlink::samples::shoppingmall::order_state_t::reservation_id),
      zlink::json::field ("paymentId", &zlink::samples::shoppingmall::order_state_t::payment_id),
      zlink::json::field ("reason", &zlink::samples::shoppingmall::order_state_t::reason),
      zlink::json::field ("currency", &zlink::samples::shoppingmall::order_state_t::currency),
      zlink::json::field ("updatedAtUnixMs",
                          &zlink::samples::shoppingmall::order_state_t::updated_at_unix_ms),
      zlink::json::field ("amount", &zlink::samples::shoppingmall::order_state_t::amount)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::start_order_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId", &zlink::samples::shoppingmall::start_order_res_t::order_id),
      zlink::json::field ("state", &zlink::samples::shoppingmall::start_order_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::get_order_state_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("state", &zlink::samples::shoppingmall::get_order_state_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::start_order_workflow_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId",
                          &zlink::samples::shoppingmall::start_order_workflow_req_t::order_id),
      zlink::json::field ("cartId",
                          &zlink::samples::shoppingmall::start_order_workflow_req_t::cart_id),
      zlink::json::field (
        "shippingAddressId",
        &zlink::samples::shoppingmall::start_order_workflow_req_t::shipping_address_id),
      zlink::json::field (
        "paymentMethodId",
        &zlink::samples::shoppingmall::start_order_workflow_req_t::payment_method_id),
      zlink::json::field (
        "idempotencyKey",
        &zlink::samples::shoppingmall::start_order_workflow_req_t::idempotency_key),
      zlink::json::field (
        "sourceCommandId",
        &zlink::samples::shoppingmall::start_order_workflow_req_t::source_command_id),
      zlink::json::field ("lines",
                          &zlink::samples::shoppingmall::start_order_workflow_req_t::lines),
      zlink::json::field ("amount",
                          &zlink::samples::shoppingmall::start_order_workflow_req_t::amount),
      zlink::json::field ("currency",
                          &zlink::samples::shoppingmall::start_order_workflow_req_t::currency)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::start_order_workflow_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "state", &zlink::samples::shoppingmall::start_order_workflow_res_t::state)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::continue_order_workflow_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId",
                          &zlink::samples::shoppingmall::continue_order_workflow_req_t::order_id),
      zlink::json::field (
        "sourceCommandId",
        &zlink::samples::shoppingmall::continue_order_workflow_req_t::source_command_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::continue_order_workflow_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "state", &zlink::samples::shoppingmall::continue_order_workflow_res_t::state)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::rebuild_order_projection_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("orderId",
                          &zlink::samples::shoppingmall::rebuild_order_projection_req_t::order_id),
      zlink::json::field (
        "sourceCommandId",
        &zlink::samples::shoppingmall::rebuild_order_projection_req_t::source_command_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::shoppingmall::rebuild_order_projection_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "state", &zlink::samples::shoppingmall::rebuild_order_projection_res_t::state)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::pending_mapping_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("idempotencyKey",
                          &zlink::samples::shoppingmall::pending_mapping_req_t::idempotency_key),
      zlink::json::field ("orderId",
                          &zlink::samples::shoppingmall::pending_mapping_req_t::order_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::delete_projection_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "orderId", &zlink::samples::shoppingmall::delete_projection_req_t::order_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::ok_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("ok", &zlink::samples::shoppingmall::ok_res_t::ok)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::server_assertion_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "successfulOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::successful_order_id),
      zlink::json::field (
        "pendingRecoveredOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::pending_recovered_order_id),
      zlink::json::field (
        "concurrentOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::concurrent_order_id),
      zlink::json::field ("resumedOrderId",
                          &zlink::samples::shoppingmall::server_assertion_req_t::resumed_order_id),
      zlink::json::field (
        "inventoryFailureOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::inventory_failure_order_id),
      zlink::json::field (
        "paymentFailureOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::payment_failure_order_id),
      zlink::json::field (
        "scaleOutOrderId",
        &zlink::samples::shoppingmall::server_assertion_req_t::scale_out_order_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::shoppingmall::server_assertion_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("passed", &zlink::samples::shoppingmall::server_assertion_res_t::passed),
      zlink::json::field ("evidence",
                          &zlink::samples::shoppingmall::server_assertion_res_t::evidence)};
};

namespace zlink::samples::shoppingmall
{

} // namespace zlink::samples::shoppingmall

// Explicit JSON state-store conversion shares the typed messaging profile.
namespace zlink::samples::shoppingmall
{
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void to_json (nlohmann::json &out, const T &value)
{
    out = zlink::detail::json_profile::encode_value (value);
}
template <typename T>
    requires zlink::detail::json_profile::has_dto<T>
void from_json (const nlohmann::json &in, T &value)
{
    value = zlink::detail::json_profile::decode_value<T> (in);
}
}
