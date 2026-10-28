/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <zlink/json_profile.hpp>
#include <optional>
#include <string>
#include <vector>

namespace zlink::samples::deliverydispatch
{

struct delivery_status_t
{
    static constexpr const char *created = "Created";
    static constexpr const char *assigned = "Assigned";
    static constexpr const char *accepted = "Accepted";
    static constexpr const char *reassigned = "Reassigned";
    static constexpr const char *picked_up = "PickedUp";
    static constexpr const char *delivered = "Delivered";
    static constexpr const char *failed = "Failed";
};

struct create_delivery_req_t
{
    static constexpr const char *packet_name = "CreateDeliveryReq";
    std::string delivery_id;
    std::string customer_id;
    std::string pickup_address;
    std::string dropoff_address;
};

struct create_delivery_res_t
{
    static constexpr const char *packet_name = "CreateDeliveryRes";
    std::string delivery_id;
};

struct ensure_customer_actor_req_t
{
    static constexpr const char *packet_name = "EnsureCustomerActorReq";
    std::string customer_id;
};

struct bind_courier_session_req_t
{
    static constexpr const char *packet_name = "BindCourierSessionReq";
    std::string courier_id;
};

struct bind_courier_session_res_t
{
    static constexpr const char *packet_name = "BindCourierSessionRes";
    std::string courier_id;
};

struct ensure_courier_actor_req_t
{
    static constexpr const char *packet_name = "EnsureCourierActorReq";
    std::string courier_id;
};

struct subscribe_delivery_req_t
{
    static constexpr const char *packet_name = "SubscribeDeliveryReq";
    std::string delivery_id;
};

struct subscribe_delivery_res_t
{
    static constexpr const char *packet_name = "SubscribeDeliveryRes";
    std::string delivery_id;
};

/* 공통 sample spec: 배차 투입은 응답 없는 one-way send다(`AssignDeliveryMsg`). */
struct assign_delivery_msg_t
{
    static constexpr const char *packet_name = "AssignDeliveryMsg";
    std::string delivery_id;
    std::string customer_id;
    std::string pickup_address;
    std::string dropoff_address;
};

/* 공통 sample spec §7.4: 제안도 결정 결과도 응답 없는 one-way다. 사람이 버튼을 누르는 시간을
 * 요청의 응답 시간에 묶지 않는다 — 묶으면 그 요청을 처리하던 실행 줄이 결정이 올 때까지 잡힌다.
 * `attempt`는 이 배송의 몇 번째 제안인지이며, 늦게 도착한 결정을 버리는 데 쓴다. */
struct offer_delivery_msg_t
{
    static constexpr const char *packet_name = "OfferDeliveryMsg";
    std::string courier_id;
    std::string delivery_id;
    int attempt{0};
    std::string pickup_address;
    std::string dropoff_address;
};

struct offer_delivery_result_msg_t
{
    static constexpr const char *packet_name = "OfferDeliveryResultMsg";
    std::string delivery_id;
    std::string courier_id;
    int attempt{0};
    bool accepted{false};
    std::optional<std::string> reason;
};

struct offer_delivery_notify_t
{
    static constexpr const char *packet_name = "OfferDeliveryNotify";
    std::string courier_id;
    std::string delivery_id;
    std::string pickup_address;
    std::string dropoff_address;
};

struct courier_decision_msg_t
{
    static constexpr const char *packet_name = "CourierDecisionMsg";
    std::string delivery_id;
    std::string courier_id;
    bool accepted{false};
    std::optional<std::string> reason;
};

/* 공통 sample spec §10: 상태 변경에는 알림 대상 고객을 함께 전달한다. Tracking은 이 값을
 * 사용해 해당 고객 actor를 찾으므로 delivery와 고객의 관계를 별도로 추측하지 않는다. */
struct delivery_status_changed_req_t
{
    static constexpr const char *packet_name = "DeliveryStatusChangedReq";
    std::string delivery_id;
    std::string customer_id;
    std::string status;
    std::optional<std::string> courier_id;
    std::int64_t occurred_at_unix_ms{0};
};

struct delivery_status_updated_msg_t
{
    static constexpr const char *packet_name = "DeliveryStatusUpdatedMsg";
    std::string delivery_id;
    std::string customer_id;
    std::string status;
    std::optional<std::string> courier_id;
    std::int64_t occurred_at_unix_ms{0};
};

struct delivery_status_changed_res_t
{
    static constexpr const char *packet_name = "DeliveryStatusChangedRes";
    std::string delivery_id;
    std::string status;
};

struct delivery_status_notify_t
{
    static constexpr const char *packet_name = "DeliveryStatusNotify";
    std::string delivery_id;
    std::string status;
    std::optional<std::string> courier_id;
    std::int64_t occurred_at_unix_ms{0};
};

/* Test/evidence-only HTTP assertion messages. They expose the runner's server evidence and do
 * not belong to the DeliveryDispatch client or role-to-role contract. */
struct server_assertion_req_t
{
    static constexpr const char *packet_name = "ServerAssertionReq";
    std::string successful_delivery_id;
    std::string reassigned_delivery_id;
};

struct server_assertion_res_t
{
    static constexpr const char *packet_name = "ServerAssertionRes";
    bool passed{false};
    std::vector<std::string> evidence;
};

inline std::string json_string (const nlohmann::json &json,
                                const char *camel,
                                const char *snake,
                                std::string fallback = {})
{
    if (json.contains (camel)) {
        return json.value (camel, fallback);
    }
    return json.value (snake, fallback);
}

inline std::optional<std::string>
json_optional_string (const nlohmann::json &json, const char *camel, const char *snake)
{
    const auto name = json.contains (camel) ? camel : snake;
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<std::string> ();
}

}

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::create_delivery_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::create_delivery_req_t::delivery_id),
      zlink::json::field ("customerId",
                          &zlink::samples::deliverydispatch::create_delivery_req_t::customer_id),
      zlink::json::field ("pickupAddress",
                          &zlink::samples::deliverydispatch::create_delivery_req_t::pickup_address),
      zlink::json::field (
        "dropoffAddress",
        &zlink::samples::deliverydispatch::create_delivery_req_t::dropoff_address)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::create_delivery_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "deliveryId", &zlink::samples::deliverydispatch::create_delivery_res_t::delivery_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::ensure_customer_actor_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "customerId", &zlink::samples::deliverydispatch::ensure_customer_actor_req_t::customer_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::bind_courier_session_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "courierId", &zlink::samples::deliverydispatch::bind_courier_session_req_t::courier_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::bind_courier_session_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "courierId", &zlink::samples::deliverydispatch::bind_courier_session_res_t::courier_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::ensure_courier_actor_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "courierId", &zlink::samples::deliverydispatch::ensure_courier_actor_req_t::courier_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::subscribe_delivery_req_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "deliveryId", &zlink::samples::deliverydispatch::subscribe_delivery_req_t::delivery_id)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::subscribe_delivery_res_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "deliveryId", &zlink::samples::deliverydispatch::subscribe_delivery_res_t::delivery_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::assign_delivery_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::assign_delivery_msg_t::delivery_id),
      zlink::json::field ("customerId",
                          &zlink::samples::deliverydispatch::assign_delivery_msg_t::customer_id),
      zlink::json::field ("pickupAddress",
                          &zlink::samples::deliverydispatch::assign_delivery_msg_t::pickup_address),
      zlink::json::field (
        "dropoffAddress",
        &zlink::samples::deliverydispatch::assign_delivery_msg_t::dropoff_address)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::offer_delivery_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("courierId",
                          &zlink::samples::deliverydispatch::offer_delivery_msg_t::courier_id),
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::offer_delivery_msg_t::delivery_id),
      zlink::json::field ("attempt",
                          &zlink::samples::deliverydispatch::offer_delivery_msg_t::attempt),
      zlink::json::field ("pickupAddress",
                          &zlink::samples::deliverydispatch::offer_delivery_msg_t::pickup_address),
      zlink::json::field (
        "dropoffAddress",
        &zlink::samples::deliverydispatch::offer_delivery_msg_t::dropoff_address)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::offer_delivery_result_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "deliveryId", &zlink::samples::deliverydispatch::offer_delivery_result_msg_t::delivery_id),
      zlink::json::field (
        "courierId", &zlink::samples::deliverydispatch::offer_delivery_result_msg_t::courier_id),
      zlink::json::field ("attempt",
                          &zlink::samples::deliverydispatch::offer_delivery_result_msg_t::attempt),
      zlink::json::field ("accepted",
                          &zlink::samples::deliverydispatch::offer_delivery_result_msg_t::accepted),
      zlink::json::field ("reason",
                          &zlink::samples::deliverydispatch::offer_delivery_result_msg_t::reason)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::offer_delivery_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("courierId",
                          &zlink::samples::deliverydispatch::offer_delivery_notify_t::courier_id),
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::offer_delivery_notify_t::delivery_id),
      zlink::json::field (
        "pickupAddress",
        &zlink::samples::deliverydispatch::offer_delivery_notify_t::pickup_address),
      zlink::json::field (
        "dropoffAddress",
        &zlink::samples::deliverydispatch::offer_delivery_notify_t::dropoff_address)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::courier_decision_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::courier_decision_msg_t::delivery_id),
      zlink::json::field ("courierId",
                          &zlink::samples::deliverydispatch::courier_decision_msg_t::courier_id),
      zlink::json::field ("accepted",
                          &zlink::samples::deliverydispatch::courier_decision_msg_t::accepted),
      zlink::json::field ("reason",
                          &zlink::samples::deliverydispatch::courier_decision_msg_t::reason)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::delivery_status_changed_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "deliveryId",
        &zlink::samples::deliverydispatch::delivery_status_changed_req_t::delivery_id),
      zlink::json::field (
        "customerId",
        &zlink::samples::deliverydispatch::delivery_status_changed_req_t::customer_id),
      zlink::json::field ("status",
                          &zlink::samples::deliverydispatch::delivery_status_changed_req_t::status),
      zlink::json::field (
        "courierId", &zlink::samples::deliverydispatch::delivery_status_changed_req_t::courier_id),
      zlink::json::field (
        "occurredAtUnixMs",
        &zlink::samples::deliverydispatch::delivery_status_changed_req_t::occurred_at_unix_ms)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::delivery_status_updated_msg_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "deliveryId",
        &zlink::samples::deliverydispatch::delivery_status_updated_msg_t::delivery_id),
      zlink::json::field (
        "customerId",
        &zlink::samples::deliverydispatch::delivery_status_updated_msg_t::customer_id),
      zlink::json::field ("status",
                          &zlink::samples::deliverydispatch::delivery_status_updated_msg_t::status),
      zlink::json::field (
        "courierId", &zlink::samples::deliverydispatch::delivery_status_updated_msg_t::courier_id),
      zlink::json::field (
        "occurredAtUnixMs",
        &zlink::samples::deliverydispatch::delivery_status_updated_msg_t::occurred_at_unix_ms)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::delivery_status_changed_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "deliveryId",
        &zlink::samples::deliverydispatch::delivery_status_changed_res_t::delivery_id),
      zlink::json::field (
        "status", &zlink::samples::deliverydispatch::delivery_status_changed_res_t::status)};
};

template <>
struct zlink::json::dto_traits<zlink::samples::deliverydispatch::delivery_status_notify_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("deliveryId",
                          &zlink::samples::deliverydispatch::delivery_status_notify_t::delivery_id),
      zlink::json::field ("status",
                          &zlink::samples::deliverydispatch::delivery_status_notify_t::status),
      zlink::json::field ("courierId",
                          &zlink::samples::deliverydispatch::delivery_status_notify_t::courier_id),
      zlink::json::field (
        "occurredAtUnixMs",
        &zlink::samples::deliverydispatch::delivery_status_notify_t::occurred_at_unix_ms)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::server_assertion_req_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field (
        "successfulDeliveryId",
        &zlink::samples::deliverydispatch::server_assertion_req_t::successful_delivery_id),
      zlink::json::field (
        "reassignedDeliveryId",
        &zlink::samples::deliverydispatch::server_assertion_req_t::reassigned_delivery_id)};
};

template <> struct zlink::json::dto_traits<zlink::samples::deliverydispatch::server_assertion_res_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("passed",
                          &zlink::samples::deliverydispatch::server_assertion_res_t::passed),
      zlink::json::field ("evidence",
                          &zlink::samples::deliverydispatch::server_assertion_res_t::evidence)};
};

namespace zlink::samples::deliverydispatch
{

} // namespace zlink::samples::deliverydispatch

// Explicit JSON state-store conversion shares the typed messaging profile.
namespace zlink::samples::deliverydispatch
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
