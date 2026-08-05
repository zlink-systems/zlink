/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "store.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::samples::shoppingmall
{

/* ---------------------------------------------------------------------------
 * OrderEventStore — 주문 상태의 기준이 되는 append-only 스트림.
 * 기록은 기대 Version 검사를 통과해야 한다(§9.4: 이전 owner 차단).
 * ------------------------------------------------------------------------- */

inline std::vector<stored_order_event_t> read_event_stream (const nlohmann::json &state,
                                                            const std::string &order_id)
{
    std::vector<stored_order_event_t> stream;
    const auto events = state.find ("events");
    if (events == state.end ()) {
        return stream;
    }
    const auto order_stream = events->find (order_id);
    if (order_stream == events->end ()) {
        return stream;
    }
    for (const auto &event : *order_stream) {
        stream.push_back (event.get<stored_order_event_t> ());
    }
    return stream;
}

inline void append_event (nlohmann::json &state,
                          const std::string &order_id,
                          const std::string &event_type,
                          const std::string &source_command_id,
                          nlohmann::json payload,
                          std::int64_t expected_version)
{
    auto &stream = state["events"][order_id];
    const auto current_version = static_cast<std::int64_t> (stream.size ());
    if (current_version != expected_version) {
        throw std::runtime_error ("Order event stream version conflict: " + order_id);
    }
    stored_order_event_t event;
    event.event_id = order_id + ":" + std::to_string (expected_version + 1);
    event.source_command_id = source_command_id;
    event.order_id = order_id;
    event.event_type = event_type;
    event.payload = std::move (payload);
    event.version = expected_version + 1;
    event.created_at_unix_ms = now_unix_ms ();
    stream.push_back (event);
}

/* ---------------------------------------------------------------------------
 * OrderAggregate — 상태는 이벤트를 접은 결과이고, 다음 단계도 그 접은 결과가 정한다.
 * ------------------------------------------------------------------------- */

struct order_aggregate_t
{
    std::string order_id;
    std::string status;
    std::string cart_id;
    std::string shipping_address_id;
    std::string payment_method_id;
    std::string reservation_id;
    std::string payment_id;
    std::string reason;
    std::vector<order_line_input_t> lines;
    decimal_t amount;
    std::string currency;
    std::int64_t version{};

    bool exists () const { return version > 0; }

    bool terminal () const
    {
        return status == order_status_t::confirmed || status == order_status_t::failed;
    }
};

inline order_aggregate_t fold (const std::vector<stored_order_event_t> &stream,
                               const std::string &order_id)
{
    order_aggregate_t aggregate;
    aggregate.order_id = order_id;
    for (const auto &event : stream) {
        const auto &payload = event.payload;
        if (event.event_type == order_event_types_t::order_started) {
            aggregate.status = order_status_t::created;
            aggregate.cart_id = payload.value ("cartId", "");
            aggregate.shipping_address_id = payload.value ("shippingAddressId", "");
            aggregate.payment_method_id = payload.value ("paymentMethodId", "");
            aggregate.lines = payload.value ("lines", std::vector<order_line_input_t>{});
            aggregate.amount = json_decimal (payload, "amount", "amount");
            aggregate.currency = payload.value ("currency", "");
        } else if (event.event_type == order_event_types_t::inventory_reserved) {
            aggregate.status = order_status_t::inventory_reserved;
            aggregate.reservation_id = payload.value ("reservationId", "");
        } else if (event.event_type == order_event_types_t::inventory_reservation_failed) {
            aggregate.status = order_event_types_t::inventory_reservation_failed;
            aggregate.reason = payload.value ("reason", "");
        } else if (event.event_type == order_event_types_t::payment_authorized) {
            aggregate.status = order_status_t::payment_authorized;
            aggregate.payment_id = payload.value ("paymentId", "");
        } else if (event.event_type == order_event_types_t::payment_failed) {
            aggregate.status = order_event_types_t::payment_failed;
            aggregate.reason = payload.value ("reason", "");
        } else if (event.event_type == order_event_types_t::inventory_released) {
            aggregate.status = order_event_types_t::inventory_released;
        } else if (event.event_type == order_event_types_t::order_confirmed) {
            aggregate.status = order_status_t::confirmed;
        } else if (event.event_type == order_event_types_t::order_failed) {
            aggregate.status = order_status_t::failed;
            if (aggregate.reason.empty ()) {
                aggregate.reason = payload.value ("reason", "");
            }
        }
        aggregate.version = event.version;
    }
    return aggregate;
}

/* 조회 모델은 fold의 파생물이다. 삭제해도 스트림 재생으로 그대로 복원된다.
 * 보상 구간(실패 기록 ~ OrderFailed 사이)은 아직 종료가 아니다 — 종료는 OrderFailedEvent가 정한다. */
inline order_state_t projection_of (const order_aggregate_t &aggregate)
{
    order_state_t state;
    state.order_id = aggregate.order_id;
    if (aggregate.status == order_event_types_t::inventory_reservation_failed) {
        state.status = order_status_t::created;
    } else if (aggregate.status == order_event_types_t::payment_failed
               || aggregate.status == order_event_types_t::inventory_released) {
        state.status = order_status_t::inventory_reserved;
    } else {
        state.status = aggregate.status;
    }
    state.shipping_address_id = aggregate.shipping_address_id;
    state.reservation_id = aggregate.reservation_id;
    state.payment_id = aggregate.payment_id;
    state.reason = aggregate.reason;
    state.amount = aggregate.amount;
    state.currency = aggregate.currency;
    state.updated_at_unix_ms = now_unix_ms ();
    return state;
}

inline order_state_t save_projection (nlohmann::json &state, const order_aggregate_t &aggregate)
{
    auto projection = projection_of (aggregate);
    state["readModels"][aggregate.order_id] = projection;
    return projection;
}

inline order_state_t rebuild_projection (nlohmann::json &state, const std::string &order_id)
{
    const auto stream = read_event_stream (state, order_id);
    if (stream.empty ()) {
        throw std::runtime_error ("Order event stream does not exist: " + order_id);
    }
    return save_projection (state, fold (stream, order_id));
}

/* ---------------------------------------------------------------------------
 * 재고·결제 모듈 — 같은 id로 다시 부르면 최초 결과를 그대로 돌려주는 멱등 연산이다(§9.4).
 * 바깥 상태(CommerceStateStore)에 쓰고 결과를 돌려준다.
 * ------------------------------------------------------------------------- */

class inventory_module_t
{
  public:
    static reserve_inventory_result_t reserve (nlohmann::json &state,
                                               const reserve_inventory_command_t &command)
    {
        auto &reservations = state["reservations"];
        if (reservations.contains (command.reservation_id)) {
            const auto &saved = reservations[command.reservation_id];
            return {saved.value ("accepted", false), saved.value ("reason", std::string{})};
        }

        reserve_inventory_result_t result{true, ""};
        for (const auto &line : command.lines) {
            const auto available =
              state["inventory"].contains (line.sku)
                ? state["inventory"][line.sku].get<inventory_seed_t> ().available_quantity
                : 0;
            if (available < line.quantity) {
                result = {false, "inventory unavailable for " + line.sku};
                break;
            }
        }
        if (result.accepted) {
            for (const auto &line : command.lines) {
                auto seed = state["inventory"][line.sku].get<inventory_seed_t> ();
                seed.available_quantity -= line.quantity;
                state["inventory"][line.sku] = seed;
            }
        }
        reservations[command.reservation_id] =
          nlohmann::json{{"orderId", command.order_id},
                         {"accepted", result.accepted},
                         {"reason", result.reason},
                         {"lines", command.lines}};
        return result;
    }

    static release_inventory_result_t release (nlohmann::json &state,
                                               const release_inventory_command_t &command)
    {
        auto &reservations = state["reservations"];
        if (!reservations.contains (command.reservation_id)
            || state["releasedReservations"].contains (command.reservation_id)) {
            return {false};
        }
        const auto lines =
          reservations[command.reservation_id].value ("lines", std::vector<order_line_input_t>{});
        for (const auto &line : lines) {
            auto seed = state["inventory"][line.sku].get<inventory_seed_t> ();
            seed.available_quantity += line.quantity;
            state["inventory"][line.sku] = seed;
        }
        state["releasedReservations"][command.reservation_id] = command.reason;
        return {true};
    }
};

class payment_module_t
{
  public:
    static authorize_payment_result_t authorize (nlohmann::json &state,
                                                 const authorize_payment_command_t &command)
    {
        auto &payments = state["payments"];
        if (payments.contains (command.payment_id)) {
            const auto &saved = payments[command.payment_id];
            return {saved.value ("accepted", false), saved.value ("reason", std::string{})};
        }

        const auto method =
          state["paymentMethods"].contains (command.payment_method_id)
            ? state["paymentMethods"][command.payment_method_id].get<payment_method_seed_t> ()
            : payment_method_seed_t{command.payment_method_id, false, "unknown payment method"};
        authorize_payment_result_t result{method.should_authorize,
                                          method.should_authorize ? std::string{}
                                                                  : method.failure_reason};
        payments[command.payment_id] = nlohmann::json{{"orderId", command.order_id},
                                                      {"accepted", result.accepted},
                                                      {"reason", result.reason},
                                                      {"amount", command.amount},
                                                      {"currency", command.currency}};
        if (!result.accepted) {
            state["paymentAttempts"][command.order_id] = result.reason;
        }
        return result;
    }
};

/* ---------------------------------------------------------------------------
 * 처리 루프(§9.1) — 시작이든 재개든 같은 코드가 돈다. 다음 단계는 fold 결과가 정한다.
 * ------------------------------------------------------------------------- */

inline std::string reservation_id_for (const std::string &order_id)
{
    return "reservation-" + order_id;
}

inline std::string payment_id_for (const std::string &order_id)
{
    return "payment-" + order_id;
}

/* 한 단계를 진행한다. 진행할 게 없으면 false. */
inline bool advance_once (nlohmann::json &state,
                          const std::string &order_id,
                          const std::string &source_command_id,
                          const start_order_workflow_req_t *start_command)
{
    auto aggregate = fold (read_event_stream (state, order_id), order_id);

    if (!aggregate.exists ()) {
        if (start_command == nullptr) {
            return false;
        }
        append_event (state, order_id, order_event_types_t::order_started, source_command_id,
                      nlohmann::json{{"cartId", start_command->cart_id},
                                     {"shippingAddressId", start_command->shipping_address_id},
                                     {"paymentMethodId", start_command->payment_method_id},
                                     {"lines", start_command->lines},
                                     {"amount", start_command->amount},
                                     {"currency", start_command->currency}},
                      aggregate.version);
        return true;
    }

    if (aggregate.terminal ()) {
        return false;
    }

    if (aggregate.status == order_status_t::created) {
        const auto reservation_id = reservation_id_for (order_id);
        const auto result = inventory_module_t::reserve (
          state, reserve_inventory_command_t{order_id, reservation_id, aggregate.lines});
        if (result.accepted) {
            append_event (state, order_id, order_event_types_t::inventory_reserved,
                          source_command_id,
                          nlohmann::json{{"reservationId", reservation_id}}, aggregate.version);
        } else {
            append_event (state, order_id, order_event_types_t::inventory_reservation_failed,
                          source_command_id, nlohmann::json{{"reason", result.reason}},
                          aggregate.version);
        }
        return true;
    }

    if (aggregate.status == order_status_t::inventory_reserved) {
        const auto payment_id = payment_id_for (order_id);
        const auto result = payment_module_t::authorize (
          state, authorize_payment_command_t{order_id, payment_id, aggregate.payment_method_id,
                                             aggregate.amount, aggregate.currency});
        if (result.accepted) {
            append_event (state, order_id, order_event_types_t::payment_authorized,
                          source_command_id, nlohmann::json{{"paymentId", payment_id}},
                          aggregate.version);
        } else {
            append_event (state, order_id, order_event_types_t::payment_failed, source_command_id,
                          nlohmann::json{{"reason", result.reason}}, aggregate.version);
        }
        return true;
    }

    if (aggregate.status == order_status_t::payment_authorized) {
        append_event (state, order_id, order_event_types_t::order_confirmed, source_command_id,
                      nlohmann::json{{"confirmedAtUnixMs", now_unix_ms ()}}, aggregate.version);
        return true;
    }

    if (aggregate.status == order_event_types_t::payment_failed) {
        (void) inventory_module_t::release (
          state, release_inventory_command_t{order_id, aggregate.reservation_id, aggregate.reason});
        append_event (state, order_id, order_event_types_t::inventory_released, source_command_id,
                      nlohmann::json{{"reservationId", aggregate.reservation_id},
                                     {"reason", aggregate.reason}},
                      aggregate.version);
        return true;
    }

    if (aggregate.status == order_event_types_t::inventory_released
        || aggregate.status == order_event_types_t::inventory_reservation_failed) {
        append_event (state, order_id, order_event_types_t::order_failed, source_command_id,
                      nlohmann::json{{"reason", aggregate.reason},
                                     {"failedAtUnixMs", now_unix_ms ()}},
                      aggregate.version);
        return true;
    }

    return false;
}

/* self-check hook: 배경 재개를 이 상태에서 멈춘다(중단 지점 생성). */
inline bool stop_hook_reached (const nlohmann::json &state,
                               const std::string &order_id,
                               const std::string &status)
{
    const auto hooks = state.find ("testHooks");
    if (hooks == state.end ()) {
        return false;
    }
    const auto stop_at = hooks->find ("stopAt");
    if (stop_at == hooks->end ()) {
        return false;
    }
    const auto stop_status = stop_at->find (order_id);
    return stop_status != stop_at->end () && stop_status->get<std::string> () == status;
}

/* 명령 중복 제거: 같은 SourceCommandId가 이미 스트림에 있으면 진행하지 않는다. */
inline bool command_already_applied (const nlohmann::json &state,
                                     const std::string &order_id,
                                     const std::string &source_command_id)
{
    if (source_command_id.empty ()) {
        return false;
    }
    const auto stream = read_event_stream (state, order_id);
    return std::any_of (stream.begin (), stream.end (), [&] (const stored_order_event_t &event) {
        return event.source_command_id == source_command_id;
    });
}

inline order_state_t run_workflow (nlohmann::json &state,
                                   const std::string &order_id,
                                   const std::string &source_command_id,
                                   const start_order_workflow_req_t *start_command,
                                   int max_steps)
{
    /* step 2·3(§9.1): 중복 명령이거나 이미 종료면 조회 모델만 fold와 맞추고 끝낸다. 종료 이벤트를
     * 기록한 직후 조회 모델 갱신 전에 죽어도, 재진입이 조회 모델을 치유한다. */
    if (command_already_applied (state, order_id, source_command_id)) {
        return save_projection (state, fold (read_event_stream (state, order_id), order_id));
    }

    for (int step = 0; step < max_steps; ++step) {
        auto before = fold (read_event_stream (state, order_id), order_id);
        if (before.terminal ()) {
            break;
        }
        if (before.exists () && stop_hook_reached (state, order_id, before.status)) {
            break;
        }
        if (!advance_once (state, order_id, source_command_id, start_command)) {
            break;
        }
        save_projection (state, fold (read_event_stream (state, order_id), order_id));
    }

    return save_projection (state, fold (read_event_stream (state, order_id), order_id));
}

} // namespace zlink::samples::shoppingmall
