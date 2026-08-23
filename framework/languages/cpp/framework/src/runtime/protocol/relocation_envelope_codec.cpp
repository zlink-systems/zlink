/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/relocation_envelope_codec.hpp"

#include "service_wire_pilot_codec.hpp"

#include <utility>

namespace zlink::framework::runtime::protocol
{
namespace
{

using generated_envelope_t = service_wire_pilot_relocation_envelope_v1;
using generated_object_t = service_wire_pilot_relocation_object_identity;
using generated_object_kind_t = service_wire_pilot_relocation_object_kind;

generated_object_t to_generated (const relocation_object_t &object)
{
    generated_object_t result;
    result.object_generation = object.object_generation;
    result.expected_authority_owner_generation =
      object.expected_authority_owner_generation;
    switch (object.kind) {
        case relocation_object_kind_t::actor:
            result.kind = generated_object_kind_t::actor;
            result.primary_id = object.object_id;
            break;
        case relocation_object_kind_t::user_spot:
            result.kind = generated_object_kind_t::user_spot;
            result.spot_id = object.object_id;
            break;
        case relocation_object_kind_t::instance_spot:
            result.kind = generated_object_kind_t::instance_spot;
            result.primary_id = object.stable_type;
            result.spot_id = object.object_id;
            break;
        default:
            throw service_wire_error_t ("relocation envelope object kind is invalid");
    }
    return result;
}

relocation_object_t from_generated (const generated_object_t &object)
{
    relocation_object_t result;
    result.object_generation = object.object_generation;
    result.expected_authority_owner_generation =
      object.expected_authority_owner_generation;
    switch (object.kind) {
        case generated_object_kind_t::actor:
            result.kind = relocation_object_kind_t::actor;
            result.object_id = object.primary_id;
            break;
        case generated_object_kind_t::user_spot:
            result.kind = relocation_object_kind_t::user_spot;
            result.object_id = object.spot_id;
            break;
        case generated_object_kind_t::instance_spot:
            result.kind = relocation_object_kind_t::instance_spot;
            result.stable_type = object.primary_id;
            result.object_id = object.spot_id;
            result.expected_authority_owner_generation = 1;
            break;
        default:
            throw service_wire_error_t ("relocation envelope object kind is invalid");
    }
    return result;
}

generated_envelope_t to_generated (const relocation_envelope_t &envelope)
{
    generated_envelope_t result;
    result.relocation_high = envelope.relocation.high;
    result.relocation_low = envelope.relocation.low;
    result.object = to_generated (envelope.object);
    result.application_version = envelope.application_version;
    result.application_states.reserve (envelope.application_states.size ());
    for (const auto &state : envelope.application_states) {
        result.application_states.push_back (
          {state.participant_id, state.has_state, state.state});
    }
    result.saved_work.reserve (envelope.saved_work.size ());
    for (const auto &work : envelope.saved_work) {
        result.saved_work.push_back (
          {work.participant_id, work.order, encode_frozen_record (work.record)});
    }
    result.timer_registrations.reserve (envelope.timer_registrations.size ());
    for (const auto &timer : envelope.timer_registrations) {
        result.timer_registrations.push_back (
          {timer.participant_id, timer.name, timer.handler_type,
           timer.period_milliseconds, timer.overrun_policy,
           timer.max_catch_up_ticks, timer.stop_on_unhandled_exception,
           timer.last_completed_delivery_index,
           timer.last_completed_scheduled_index,
           timer.next_scheduled_at_unix_milliseconds});
    }
    result.pending_timer_ticks.reserve (envelope.pending_timer_ticks.size ());
    for (const auto &tick : envelope.pending_timer_ticks) {
        result.pending_timer_ticks.push_back (
          {tick.participant_id, tick.order, tick.timer_name,
           tick.delivery_index, tick.scheduled_index,
           tick.scheduled_at_unix_milliseconds, tick.skipped_ticks});
    }
    return result;
}

relocation_envelope_t from_generated (const generated_envelope_t &envelope)
{
    relocation_envelope_t result;
    result.relocation = {envelope.relocation_high, envelope.relocation_low};
    result.object = from_generated (envelope.object);
    result.application_version = envelope.application_version;
    result.application_states.reserve (envelope.application_states.size ());
    for (const auto &state : envelope.application_states) {
        result.application_states.push_back (
          {state.participant_id, state.has_state, state.payload});
    }
    result.saved_work.reserve (envelope.saved_work.size ());
    for (const auto &work : envelope.saved_work) {
        result.saved_work.push_back (
          {work.participant_id, work.order,
           decode_frozen_record (work.frozen_record)});
    }
    result.timer_registrations.reserve (envelope.timer_registrations.size ());
    for (const auto &timer : envelope.timer_registrations) {
        result.timer_registrations.push_back (
          {timer.participant_id, timer.name, timer.handler_type,
           timer.period_milliseconds, timer.overrun_policy,
           timer.max_catch_up_ticks, timer.stop_on_unhandled_exception,
           timer.last_completed_delivery_index,
           timer.last_completed_scheduled_index,
           timer.next_scheduled_at_unix_milliseconds});
    }
    result.pending_timer_ticks.reserve (envelope.pending_timer_ticks.size ());
    for (const auto &tick : envelope.pending_timer_ticks) {
        result.pending_timer_ticks.push_back (
          {tick.participant_id, tick.order, tick.timer_name,
           tick.delivery_index, tick.scheduled_index,
           tick.scheduled_at_unix_milliseconds, tick.skipped_ticks});
    }
    return result;
}

template <class operation_t>
auto generated (operation_t &&operation)
{
    try {
        return std::forward<operation_t> (operation) ();
    }
    catch (const std::invalid_argument &error) {
        throw service_wire_error_t (error.what ());
    }
}

} // namespace

std::vector<std::uint8_t> encode_relocation_envelope (
  const relocation_envelope_t &envelope)
{
    return generated ([&] {
        return encode_relocation_envelope_v1 (to_generated (envelope));
    });
}

relocation_envelope_t decode_relocation_envelope (
  std::span<const std::uint8_t> bytes)
{
    return generated ([&] {
        return from_generated (decode_relocation_envelope_v1 (
          {{bytes.begin (), bytes.end ()}}));
    });
}

} // namespace zlink::framework::runtime::protocol
