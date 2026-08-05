/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>
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

inline void to_json (nlohmann::json &json, const create_delivery_req_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"customerId", value.customer_id},
            {"pickupAddress", value.pickup_address},
            {"dropoffAddress", value.dropoff_address}};
}

inline void from_json (const nlohmann::json &json, create_delivery_req_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.customer_id = json_string (json, "customerId", "customer_id");
    value.pickup_address = json_string (json, "pickupAddress", "pickup_address");
    value.dropoff_address = json_string (json, "dropoffAddress", "dropoff_address");
}

inline void to_json (nlohmann::json &json, const create_delivery_res_t &value)
{
    json = {{"deliveryId", value.delivery_id}};
}

inline void from_json (const nlohmann::json &json, create_delivery_res_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
}

inline void to_json (nlohmann::json &json, const ensure_customer_actor_req_t &value)
{
    json = {{"customerId", value.customer_id}};
}

inline void from_json (const nlohmann::json &json, ensure_customer_actor_req_t &value)
{
    value.customer_id = json_string (json, "customerId", "customer_id");
}

inline void to_json (nlohmann::json &json, const bind_courier_session_req_t &value)
{
    json = {{"courierId", value.courier_id}};
}

inline void from_json (const nlohmann::json &json, bind_courier_session_req_t &value)
{
    value.courier_id = json_string (json, "courierId", "courier_id");
}

inline void to_json (nlohmann::json &json, const bind_courier_session_res_t &value)
{
    json = {{"courierId", value.courier_id}};
}

inline void from_json (const nlohmann::json &json, bind_courier_session_res_t &value)
{
    value.courier_id = json_string (json, "courierId", "courier_id");
}

inline void to_json (nlohmann::json &json, const ensure_courier_actor_req_t &value)
{
    json = {{"courierId", value.courier_id}};
}

inline void from_json (const nlohmann::json &json, ensure_courier_actor_req_t &value)
{
    value.courier_id = json_string (json, "courierId", "courier_id");
}

inline void to_json (nlohmann::json &json, const subscribe_delivery_req_t &value)
{
    json = {{"deliveryId", value.delivery_id}};
}

inline void from_json (const nlohmann::json &json, subscribe_delivery_req_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
}

inline void to_json (nlohmann::json &json, const subscribe_delivery_res_t &value)
{
    json = {{"deliveryId", value.delivery_id}};
}

inline void from_json (const nlohmann::json &json, subscribe_delivery_res_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
}

inline void to_json (nlohmann::json &json, const assign_delivery_msg_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"customerId", value.customer_id},
            {"pickupAddress", value.pickup_address},
            {"dropoffAddress", value.dropoff_address}};
}

inline void from_json (const nlohmann::json &json, assign_delivery_msg_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.customer_id = json_string (json, "customerId", "customer_id");
    value.pickup_address = json_string (json, "pickupAddress", "pickup_address");
    value.dropoff_address = json_string (json, "dropoffAddress", "dropoff_address");
}

inline void to_json (nlohmann::json &json, const offer_delivery_msg_t &value)
{
    json = {{"courierId", value.courier_id},
            {"deliveryId", value.delivery_id},
            {"attempt", value.attempt},
            {"pickupAddress", value.pickup_address},
            {"dropoffAddress", value.dropoff_address}};
}

inline void from_json (const nlohmann::json &json, offer_delivery_msg_t &value)
{
    value.courier_id = json_string (json, "courierId", "courier_id");
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.attempt = json.value ("attempt", 0);
    value.pickup_address = json_string (json, "pickupAddress", "pickup_address");
    value.dropoff_address = json_string (json, "dropoffAddress", "dropoff_address");
}

inline void to_json (nlohmann::json &json, const offer_delivery_result_msg_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"courierId", value.courier_id},
            {"attempt", value.attempt},
            {"accepted", value.accepted},
            {"reason", value.reason ? nlohmann::json (*value.reason) : nlohmann::json (nullptr)}};
}

inline void from_json (const nlohmann::json &json, offer_delivery_result_msg_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.courier_id = json_string (json, "courierId", "courier_id");
    value.attempt = json.value ("attempt", 0);
    value.accepted = json.value ("accepted", false);
    value.reason = json_optional_string (json, "reason", "reason");
}

inline void to_json (nlohmann::json &json, const offer_delivery_notify_t &value)
{
    json = {{"courierId", value.courier_id},
            {"deliveryId", value.delivery_id},
            {"pickupAddress", value.pickup_address},
            {"dropoffAddress", value.dropoff_address}};
}

inline void from_json (const nlohmann::json &json, offer_delivery_notify_t &value)
{
    value.courier_id = json_string (json, "courierId", "courier_id");
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.pickup_address = json_string (json, "pickupAddress", "pickup_address");
    value.dropoff_address = json_string (json, "dropoffAddress", "dropoff_address");
}

inline void to_json (nlohmann::json &json, const courier_decision_msg_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"courierId", value.courier_id},
            {"accepted", value.accepted},
            {"reason", value.reason ? nlohmann::json (*value.reason) : nlohmann::json (nullptr)}};
}

inline void from_json (const nlohmann::json &json, courier_decision_msg_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.courier_id = json_string (json, "courierId", "courier_id");
    value.accepted = json.value ("accepted", false);
    value.reason = json_optional_string (json, "reason", "reason");
}

inline void to_json (nlohmann::json &json, const delivery_status_changed_req_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"customerId", value.customer_id},
            {"status", value.status},
            {"courierId", value.courier_id ? nlohmann::json (*value.courier_id)
                                             : nlohmann::json (nullptr)},
            {"occurredAtUnixMs", value.occurred_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, delivery_status_changed_req_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.customer_id = json_string (json, "customerId", "customer_id");
    value.status = json.value ("status", "");
    value.courier_id = json_optional_string (json, "courierId", "courier_id");
    value.occurred_at_unix_ms = json.value ("occurredAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const delivery_status_updated_msg_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"customerId", value.customer_id},
            {"status", value.status},
            {"courierId", value.courier_id ? nlohmann::json (*value.courier_id)
                                             : nlohmann::json (nullptr)},
            {"occurredAtUnixMs", value.occurred_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, delivery_status_updated_msg_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.customer_id = json_string (json, "customerId", "customer_id");
    value.status = json.value ("status", "");
    value.courier_id = json_optional_string (json, "courierId", "courier_id");
    value.occurred_at_unix_ms = json.value ("occurredAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const delivery_status_changed_res_t &value)
{
    json = {{"deliveryId", value.delivery_id}, {"status", value.status}};
}

inline void from_json (const nlohmann::json &json, delivery_status_changed_res_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.status = json.value ("status", "");
}

inline void to_json (nlohmann::json &json, const delivery_status_notify_t &value)
{
    json = {{"deliveryId", value.delivery_id},
            {"status", value.status},
            {"courierId", value.courier_id ? nlohmann::json (*value.courier_id)
                                             : nlohmann::json (nullptr)},
            {"occurredAtUnixMs", value.occurred_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, delivery_status_notify_t &value)
{
    value.delivery_id = json_string (json, "deliveryId", "delivery_id");
    value.status = json.value ("status", "");
    value.courier_id = json_optional_string (json, "courierId", "courier_id");
    value.occurred_at_unix_ms = json.value ("occurredAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const server_assertion_req_t &value)
{
    json = {{"successfulDeliveryId", value.successful_delivery_id},
            {"reassignedDeliveryId", value.reassigned_delivery_id}};
}

inline void from_json (const nlohmann::json &json, server_assertion_req_t &value)
{
    value.successful_delivery_id =
      json_string (json, "successfulDeliveryId", "successful_delivery_id");
    value.reassigned_delivery_id =
      json_string (json, "reassignedDeliveryId", "reassigned_delivery_id");
}

inline void to_json (nlohmann::json &json, const server_assertion_res_t &value)
{
    json = {{"passed", value.passed}, {"evidence", value.evidence}};
}

inline void from_json (const nlohmann::json &json, server_assertion_res_t &value)
{
    value.passed = json.value ("passed", false);
    value.evidence = json.value ("evidence", std::vector<std::string>{});
}

} // namespace zlink::samples::deliverydispatch
