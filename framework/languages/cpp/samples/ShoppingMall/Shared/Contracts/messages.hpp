/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <nlohmann/json.hpp>

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

class decimal_t
{
  public:
    decimal_t () = default;
    explicit decimal_t (std::string_view value) : _value (normalize (value)) {}

    const std::string &value () const noexcept { return _value; }

    friend bool operator== (const decimal_t &, const decimal_t &) = default;

  private:
    static std::string normalize (std::string_view value)
    {
        if (value.empty ()) {
            throw std::invalid_argument ("decimal value must not be empty");
        }
        bool negative = false;
        if (value.front () == '-' || value.front () == '+') {
            negative = value.front () == '-';
            value.remove_prefix (1);
        }
        const auto point = value.find ('.');
        auto integral = std::string (value.substr (0, point));
        auto fractional = point == std::string_view::npos
                            ? std::string{}
                            : std::string (value.substr (point + 1));
        if (integral.empty () || (point != std::string_view::npos && fractional.empty ())
            || !std::all_of (integral.begin (), integral.end (), [] (unsigned char ch) {
                   return std::isdigit (ch) != 0;
               })
            || !std::all_of (fractional.begin (), fractional.end (), [] (unsigned char ch) {
                   return std::isdigit (ch) != 0;
               })) {
            throw std::invalid_argument ("decimal value must use ordinary JSON number syntax");
        }
        const auto first_digit = integral.find_first_not_of ('0');
        integral = first_digit == std::string::npos ? "0" : integral.substr (first_digit);
        while (!fractional.empty () && fractional.back () == '0') {
            fractional.pop_back ();
        }
        const auto significant_digits =
          (integral == "0" ? std::size_t{0} : integral.size ()) + fractional.size ();
        if (significant_digits > 29 || fractional.size () > 28) {
            throw std::out_of_range ("decimal value exceeds the .NET decimal range");
        }
        if (integral == "0" && fractional.empty ()) {
            negative = false;
        }
        return std::string (negative ? "-" : "") + integral
               + (fractional.empty () ? std::string{} : "." + fractional);
    }

    std::string _value{"0"};
};

inline void to_json (nlohmann::json &json, const decimal_t &value)
{
    json = nlohmann::json::parse (value.value ());
}

inline void from_json (const nlohmann::json &json, decimal_t &value)
{
    if (!json.is_number ()) {
        throw nlohmann::json::type_error::create (302, "decimal must be a JSON number", &json);
    }
    value = decimal_t (json.dump ());
}

inline decimal_t json_decimal (const nlohmann::json &json,
                               const char *camel,
                               const char *snake)
{
    const auto name = json.contains (camel) ? camel : snake;
    return json.contains (name) ? json.at (name).get<decimal_t> () : decimal_t{};
}

inline std::optional<decimal_t> json_nullable_decimal (const nlohmann::json &json,
                                                       const char *camel,
                                                       const char *snake)
{
    const auto name = json.contains (camel) ? camel : snake;
    if (!json.contains (name) || json.at (name).is_null ()) {
        return std::nullopt;
    }
    return json.at (name).get<decimal_t> ();
}

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
    std::optional<decimal_t> amount;
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
    decimal_t amount;
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
    decimal_t amount;
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
    decimal_t amount;
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

inline std::string json_string (const nlohmann::json &json,
                                const char *camel,
                                const char *snake)
{
    return json.contains (camel) ? json.value (camel, std::string{})
                                 : json.value (snake, std::string{});
}

inline std::optional<std::string> json_nullable_string (const nlohmann::json &json,
                                                        const char *camel,
                                                        const char *snake)
{
    const auto read = [&json] (const char *name) -> std::optional<std::string> {
        if (!json.contains (name) || json[name].is_null ()) return std::nullopt;
        return json.value (name, std::string{});
    };
    return json.contains (camel) ? read (camel) : read (snake);
}

inline void to_json (nlohmann::json &json, const order_line_input_t &value)
{
    json = {{"sku", value.sku}, {"quantity", value.quantity}};
}

inline void from_json (const nlohmann::json &json, order_line_input_t &value)
{
    value.sku = json.value ("sku", "");
    value.quantity = json.value ("quantity", 0);
}

inline void to_json (nlohmann::json &json, const continue_order_workflow_msg_t &value)
{
    json = {{"orderId", value.order_id}};
}

inline void from_json (const nlohmann::json &json, continue_order_workflow_msg_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
}

inline void to_json (nlohmann::json &json, const stored_order_event_t &value)
{
    json = {{"eventId", value.event_id},
            {"sourceCommandId", value.source_command_id},
            {"orderId", value.order_id},
            {"eventType", value.event_type},
            {"payload", value.payload},
            {"version", value.version},
            {"createdAtUnixMs", value.created_at_unix_ms}};
}

inline void from_json (const nlohmann::json &json, stored_order_event_t &value)
{
    value.event_id = json.value ("eventId", "");
    value.source_command_id = json.value ("sourceCommandId", "");
    value.order_id = json.value ("orderId", "");
    value.event_type = json.value ("eventType", "");
    value.payload = json.contains ("payload") ? json.at ("payload") : nlohmann::json::object ();
    value.version = json.value ("version", std::int64_t{0});
    value.created_at_unix_ms = json.value ("createdAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const cart_seed_t &value)
{
    json = {{"cartId", value.cart_id},
            {"lines", value.lines},
            {"amount", value.amount},
            {"currency", value.currency}};
}

inline void from_json (const nlohmann::json &json, cart_seed_t &value)
{
    value.cart_id = json.value ("cartId", "");
    value.lines = json.value ("lines", std::vector<order_line_input_t>{});
    value.amount = json_decimal (json, "amount", "amount");
    value.currency = json.value ("currency", "");
}

inline void to_json (nlohmann::json &json, const inventory_seed_t &value)
{
    json = {{"sku", value.sku}, {"availableQuantity", value.available_quantity}};
}

inline void from_json (const nlohmann::json &json, inventory_seed_t &value)
{
    value.sku = json.value ("sku", "");
    value.available_quantity = json.value ("availableQuantity", 0);
}

inline void to_json (nlohmann::json &json, const payment_method_seed_t &value)
{
    json = {{"paymentMethodId", value.payment_method_id},
            {"shouldAuthorize", value.should_authorize},
            {"failureReason", value.failure_reason}};
}

inline void from_json (const nlohmann::json &json, payment_method_seed_t &value)
{
    value.payment_method_id = json.value ("paymentMethodId", "");
    value.should_authorize = json.value ("shouldAuthorize", false);
    value.failure_reason = json.value ("failureReason", "");
}


inline void to_json (nlohmann::json &json, const start_order_req_t &value)
{
    json = {{"cartId", value.cart_id},
            {"shippingAddressId", value.shipping_address_id},
            {"paymentMethodId", value.payment_method_id},
            {"idempotencyKey", value.idempotency_key}};
}

inline void from_json (const nlohmann::json &json, start_order_req_t &value)
{
    value.cart_id = json_string (json, "cartId", "cart_id");
    value.shipping_address_id = json_string (json, "shippingAddressId", "shipping_address_id");
    value.payment_method_id = json_string (json, "paymentMethodId", "payment_method_id");
    value.idempotency_key = json_string (json, "idempotencyKey", "idempotency_key");
}

inline void to_json (nlohmann::json &json, const get_order_state_req_t &value)
{
    json = {{"orderId", value.order_id}};
}

inline void from_json (const nlohmann::json &json, get_order_state_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
}

inline void to_json (nlohmann::json &json, const order_state_t &value)
{
    json = {{"orderId", value.order_id},
            {"status", value.status},
            {"shippingAddressId", value.shipping_address_id
                                      ? nlohmann::json (*value.shipping_address_id)
                                      : nlohmann::json (nullptr)},
            {"reservationId", value.reservation_id ? nlohmann::json (*value.reservation_id)
                                                     : nlohmann::json (nullptr)},
            {"paymentId", value.payment_id ? nlohmann::json (*value.payment_id)
                                             : nlohmann::json (nullptr)},
            {"reason", value.reason ? nlohmann::json (*value.reason)
                                      : nlohmann::json (nullptr)},
            {"currency", value.currency ? nlohmann::json (*value.currency)
                                          : nlohmann::json (nullptr)},
            {"updatedAtUnixMs", value.updated_at_unix_ms}};
    json["amount"] = value.amount ? nlohmann::json (*value.amount) : nlohmann::json (nullptr);
}

inline void from_json (const nlohmann::json &json, order_state_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
    value.status = json.value ("status", "");
    value.shipping_address_id = json_nullable_string (json, "shippingAddressId", "shipping_address_id");
    value.reservation_id = json_nullable_string (json, "reservationId", "reservation_id");
    value.payment_id = json_nullable_string (json, "paymentId", "payment_id");
    value.reason = json_nullable_string (json, "reason", "reason");
    value.amount = json_nullable_decimal (json, "amount", "amount");
    value.currency = json_nullable_string (json, "currency", "currency");
    value.updated_at_unix_ms = json.value ("updatedAtUnixMs", std::int64_t{0});
}

inline void to_json (nlohmann::json &json, const start_order_res_t &value)
{
    json = {{"orderId", value.order_id}, {"state", value.state}};
}

inline void from_json (const nlohmann::json &json, start_order_res_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
    value.state = json.value ("state", order_state_t{});
}

inline void to_json (nlohmann::json &json, const get_order_state_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, get_order_state_res_t &value)
{
    value.state = json.value ("state", order_state_t{});
}

inline void to_json (nlohmann::json &json, const start_order_workflow_req_t &value)
{
    json = {{"orderId", value.order_id},
            {"cartId", value.cart_id},
            {"shippingAddressId", value.shipping_address_id},
            {"paymentMethodId", value.payment_method_id},
            {"idempotencyKey", value.idempotency_key},
            {"sourceCommandId", value.source_command_id},
            {"lines", value.lines},
            {"amount", value.amount},
            {"currency", value.currency}};
}

inline void from_json (const nlohmann::json &json, start_order_workflow_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
    value.cart_id = json_string (json, "cartId", "cart_id");
    value.shipping_address_id = json_string (json, "shippingAddressId", "shipping_address_id");
    value.payment_method_id = json_string (json, "paymentMethodId", "payment_method_id");
    value.idempotency_key = json_string (json, "idempotencyKey", "idempotency_key");
    value.source_command_id = json_string (json, "sourceCommandId", "source_command_id");
    value.lines = json.value ("lines", std::vector<order_line_input_t>{});
    value.amount = json_decimal (json, "amount", "amount");
    value.currency = json.value ("currency", "");
}

inline void to_json (nlohmann::json &json, const start_order_workflow_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, start_order_workflow_res_t &value)
{
    value.state = json.value ("state", order_state_t{});
}

inline void to_json (nlohmann::json &json, const continue_order_workflow_req_t &value)
{
    json = {{"orderId", value.order_id}, {"sourceCommandId", value.source_command_id}};
}

inline void from_json (const nlohmann::json &json, continue_order_workflow_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
    value.source_command_id = json_string (json, "sourceCommandId", "source_command_id");
}

inline void to_json (nlohmann::json &json, const continue_order_workflow_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, continue_order_workflow_res_t &value)
{
    value.state = json.value ("state", order_state_t{});
}

inline void to_json (nlohmann::json &json, const rebuild_order_projection_req_t &value)
{
    json = {{"orderId", value.order_id}, {"sourceCommandId", value.source_command_id}};
}

inline void from_json (const nlohmann::json &json, rebuild_order_projection_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
    value.source_command_id = json_string (json, "sourceCommandId", "source_command_id");
}

inline void to_json (nlohmann::json &json, const rebuild_order_projection_res_t &value)
{
    json = {{"state", value.state}};
}

inline void from_json (const nlohmann::json &json, rebuild_order_projection_res_t &value)
{
    value.state = json.value ("state", order_state_t{});
}

inline void to_json (nlohmann::json &json, const pending_mapping_req_t &value)
{
    json = {{"idempotencyKey", value.idempotency_key},
            {"orderId", value.order_id}};
}

inline void from_json (const nlohmann::json &json, pending_mapping_req_t &value)
{
    value.idempotency_key = json_string (json, "idempotencyKey", "idempotency_key");
    value.order_id = json_string (json, "orderId", "order_id");
}

inline void to_json (nlohmann::json &json, const delete_projection_req_t &value)
{
    json = {{"orderId", value.order_id}};
}

inline void from_json (const nlohmann::json &json, delete_projection_req_t &value)
{
    value.order_id = json_string (json, "orderId", "order_id");
}

inline void to_json (nlohmann::json &json, const ok_res_t &value)
{
    json = {{"ok", value.ok}};
}

inline void from_json (const nlohmann::json &json, ok_res_t &value)
{
    value.ok = json.value ("ok", false);
}

inline void to_json (nlohmann::json &json, const server_assertion_req_t &value)
{
    json = {{"successfulOrderId", value.successful_order_id},
            {"pendingRecoveredOrderId", value.pending_recovered_order_id},
            {"concurrentOrderId", value.concurrent_order_id},
            {"resumedOrderId", value.resumed_order_id},
            {"inventoryFailureOrderId", value.inventory_failure_order_id},
            {"paymentFailureOrderId", value.payment_failure_order_id},
            {"scaleOutOrderId", value.scale_out_order_id}};
}

inline void from_json (const nlohmann::json &json, server_assertion_req_t &value)
{
    value.successful_order_id = json_string (json, "successfulOrderId", "successful_order_id");
    value.pending_recovered_order_id =
      json_string (json, "pendingRecoveredOrderId", "pending_recovered_order_id");
    value.concurrent_order_id = json_string (json, "concurrentOrderId", "concurrent_order_id");
    value.resumed_order_id = json_string (json, "resumedOrderId", "resumed_order_id");
    value.inventory_failure_order_id =
      json_string (json, "inventoryFailureOrderId", "inventory_failure_order_id");
    value.payment_failure_order_id =
      json_string (json, "paymentFailureOrderId", "payment_failure_order_id");
    value.scale_out_order_id = json_string (json, "scaleOutOrderId", "scale_out_order_id");
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

} // namespace zlink::samples::shoppingmall
