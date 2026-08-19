/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/protocol/relocation_envelope_codec.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime::protocol
{
namespace
{

constexpr std::size_t max_state_bytes = 64u * 1024u * 1024u;
constexpr std::uint32_t max_timer_items = 65536;
constexpr std::size_t min_state_entry_bytes = 8 + 1 + 8;
constexpr std::size_t min_saved_work_entry_bytes = 8 + 8 + 2;

class envelope_reader_t
{
  public:
    explicit envelope_reader_t (std::span<const std::uint8_t> input) :
        _input (input)
    {
    }

    std::size_t offset () const noexcept { return _offset; }
    std::size_t remaining () const noexcept
    {
        return _input.size () - _offset;
    }

    std::uint8_t u8 (const char *field)
    {
        require (1, field);
        return _input[_offset++];
    }

    std::uint16_t u16 (const char *field)
    {
        require (2, field);
        std::uint16_t value = 0;
        for (int count = 0; count != 2; ++count)
            value = static_cast<std::uint16_t> (
              (value << 8) | _input[_offset++]);
        return value;
    }

    std::uint32_t u32 (const char *field)
    {
        require (4, field);
        std::uint32_t value = 0;
        for (int count = 0; count != 4; ++count)
            value = (value << 8) | _input[_offset++];
        return value;
    }

    std::uint64_t u64 (const char *field)
    {
        require (8, field);
        std::uint64_t value = 0;
        for (int count = 0; count != 8; ++count)
            value = (value << 8) | _input[_offset++];
        return value;
    }

    std::span<const std::uint8_t> bytes (std::size_t size, const char *field)
    {
        require (size, field);
        const auto value = _input.subspan (_offset, size);
        _offset += size;
        return value;
    }

    std::string text8 (const char *field)
    {
        const auto size = u8 (field);
        if (size == 0)
            throw service_wire_error_t (
              "relocation envelope text field is empty");
        const auto value = bytes (size, field);
        return {value.begin (), value.end ()};
    }

    std::size_t length64 (std::size_t maximum, const char *field)
    {
        const auto value = u64 (field);
        if (value > maximum || value > remaining ())
            throw service_wire_error_t (
              "relocation envelope length field exceeds its bound");
        return static_cast<std::size_t> (value);
    }

    void require_end (const char *field) const
    {
        if (_offset != _input.size ())
            throw service_wire_error_t (
              std::string ("relocation envelope ") + field
              + " contains trailing bytes");
    }

  private:
    void require (std::size_t size, const char *field) const
    {
        if (remaining () < size)
            throw service_wire_error_t (
              std::string ("relocation envelope ") + field
              + " is truncated");
    }

    std::span<const std::uint8_t> _input;
    std::size_t _offset = 0;
};

void append_u8 (std::vector<std::uint8_t> &output, std::uint8_t value)
{
    output.push_back (value);
}

void append_u16 (std::vector<std::uint8_t> &output, std::uint16_t value)
{
    output.push_back (static_cast<std::uint8_t> (value >> 8));
    output.push_back (static_cast<std::uint8_t> (value));
}

void append_u32 (std::vector<std::uint8_t> &output, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        output.push_back (static_cast<std::uint8_t> (value >> shift));
}

void append_u64 (std::vector<std::uint8_t> &output, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back (static_cast<std::uint8_t> (value >> shift));
}

void append_text8 (std::vector<std::uint8_t> &output,
                   const std::string &value,
                   const char *field)
{
    if (value.empty () || value.size () > 255)
        throw service_wire_error_t (
          std::string ("relocation envelope ") + field
          + " must contain 1..255 UTF-8 bytes");
    append_u8 (output, static_cast<std::uint8_t> (value.size ()));
    output.insert (output.end (), value.begin (), value.end ());
}

void validate_envelope_shape (const relocation_envelope_t &envelope)
{
    if (envelope.relocation.high == 0 && envelope.relocation.low == 0)
        throw service_wire_error_t (
          "relocation envelope id must not be zero");
    const auto kind = static_cast<std::uint8_t> (envelope.object.kind);
    if (kind < 1 || kind > 3)
        throw service_wire_error_t (
          "relocation envelope object kind is invalid");
    if (envelope.object.object_generation == 0)
        throw service_wire_error_t (
          "relocation envelope object generation must not be zero");
    if (envelope.object.kind != relocation_object_kind_t::instance_spot
        && envelope.object.expected_authority_owner_generation == 0)
        throw service_wire_error_t (
          "relocation envelope owner generation must not be zero");
    if (envelope.application_version < 0)
        throw service_wire_error_t (
          "relocation envelope application version is invalid");
    if (envelope.application_states.empty ())
        throw service_wire_error_t (
          "relocation envelope requires at least one participant state");
}

} // namespace

std::vector<std::uint8_t> encode_relocation_envelope (
  const relocation_envelope_t &envelope)
{
    validate_envelope_shape (envelope);
    std::vector<std::uint8_t> output;
    append_u64 (output, envelope.relocation.high);
    append_u64 (output, envelope.relocation.low);
    append_u8 (output, static_cast<std::uint8_t> (envelope.object.kind));
    std::vector<std::uint8_t> object_body;
    if (envelope.object.kind == relocation_object_kind_t::instance_spot)
        append_text8 (object_body, envelope.object.stable_type,
                      "object stable type");
    append_text8 (object_body, envelope.object.object_id, "object id");
    append_u64 (object_body, envelope.object.object_generation);
    if (envelope.object.kind != relocation_object_kind_t::instance_spot)
        append_u64 (object_body,
                    envelope.object.expected_authority_owner_generation);
    if (object_body.size () > std::numeric_limits<std::uint16_t>::max ())
        throw service_wire_error_t (
          "relocation envelope object body exceeds 65535 bytes");
    append_u16 (output, static_cast<std::uint16_t> (object_body.size ()));
    output.insert (output.end (), object_body.begin (), object_body.end ());
    append_u64 (output,
                static_cast<std::uint64_t> (envelope.application_version));

    append_u32 (output, static_cast<std::uint32_t> (
                          envelope.application_states.size ()));
    for (const auto &state : envelope.application_states) {
        append_u64 (output, state.participant_id);
        append_u8 (output, state.state_encoding);
        std::vector<std::uint8_t> body;
        if (state.state_encoding >= 1) {
            append_u64 (body, state.state.size ());
            body.insert (body.end (), state.state.begin (),
                         state.state.end ());
        }
        if (state.state_encoding == 2) {
            append_u64 (body, state.recovery.size ());
            body.insert (body.end (), state.recovery.begin (),
                         state.recovery.end ());
        }
        append_u64 (output, body.size ());
        output.insert (output.end (), body.begin (), body.end ());
    }

    append_u32 (output,
                static_cast<std::uint32_t> (envelope.saved_work.size ()));
    for (const auto &entry : envelope.saved_work) {
        append_u64 (output, entry.participant_id);
        append_u64 (output, entry.order);
        const auto canonical = encode_frozen_record (entry.record);
        output.insert (output.end (), canonical.begin (), canonical.end ());
    }

    append_u32 (output, static_cast<std::uint32_t> (
                          envelope.timer_registrations.size ()));
    for (const auto &timer : envelope.timer_registrations) {
        append_u64 (output, timer.participant_id);
        append_text8 (output, timer.name, "timer name");
        append_text8 (output, timer.handler_type, "timer handler type");
        append_u64 (output, timer.period_milliseconds);
        append_u8 (output, timer.overrun_policy);
        append_u64 (output, timer.max_catch_up_ticks);
        append_u8 (output, timer.stop_on_unhandled_exception ? 1 : 0);
        append_u64 (output, timer.last_completed_delivery_index);
        append_u64 (output, timer.last_completed_scheduled_index);
        append_u64 (output, timer.next_scheduled_at_unix_milliseconds);
    }

    append_u32 (output, static_cast<std::uint32_t> (
                          envelope.pending_timer_ticks.size ()));
    for (const auto &tick : envelope.pending_timer_ticks) {
        append_u64 (output, tick.participant_id);
        append_u64 (output, tick.order);
        append_text8 (output, tick.timer_name, "pending timer name");
        append_u64 (output, tick.delivery_index);
        append_u64 (output, tick.scheduled_index);
        append_u64 (output, tick.scheduled_at_unix_milliseconds);
        append_u64 (output, tick.skipped_ticks);
    }

    /* One shared contract: the decoder is the validator, so an encoded
     * stream is guaranteed decodable and byte-identical on re-encode. */
    (void) decode_relocation_envelope (output);
    return output;
}

relocation_envelope_t decode_relocation_envelope (
  std::span<const std::uint8_t> bytes)
{
    envelope_reader_t reader (bytes);
    relocation_envelope_t envelope;
    envelope.relocation.high = reader.u64 ("relocation id");
    envelope.relocation.low = reader.u64 ("relocation id");
    if (envelope.relocation.high == 0 && envelope.relocation.low == 0)
        throw service_wire_error_t (
          "relocation envelope id must not be zero");

    const auto object_kind = reader.u8 ("object kind");
    if (object_kind < 1 || object_kind > 3)
        throw service_wire_error_t (
          "relocation envelope object kind is invalid");
    envelope.object.kind =
      static_cast<relocation_object_kind_t> (object_kind);
    const auto object_size = reader.u16 ("object body");
    envelope_reader_t object_body (
      reader.bytes (object_size, "object body"));
    if (envelope.object.kind == relocation_object_kind_t::instance_spot) {
        envelope.object.stable_type =
          object_body.text8 ("object stable type");
        envelope.object.object_id = object_body.text8 ("object id");
        envelope.object.object_generation =
          object_body.u64 ("object generation");
        envelope.object.expected_authority_owner_generation = 1;
    }
    else {
        envelope.object.object_id = object_body.text8 ("object id");
        envelope.object.object_generation =
          object_body.u64 ("object generation");
        envelope.object.expected_authority_owner_generation =
          object_body.u64 ("owner generation");
        if (envelope.object.expected_authority_owner_generation == 0)
            throw service_wire_error_t (
              "relocation envelope owner generation must not be zero");
    }
    object_body.require_end ("object body");
    if (envelope.object.object_generation == 0)
        throw service_wire_error_t (
          "relocation envelope object generation must not be zero");

    const auto application_version = reader.u64 ("application version");
    if (application_version
        > static_cast<std::uint64_t> (
            std::numeric_limits<std::int64_t>::max ()))
        throw service_wire_error_t (
          "relocation envelope application version is invalid");
    envelope.application_version =
      static_cast<std::int64_t> (application_version);

    const auto state_count = reader.u32 ("application state count");
    if (state_count == 0
        || state_count > reader.remaining () / min_state_entry_bytes)
        throw service_wire_error_t (
          "relocation envelope application-state count exceeds its bound");
    envelope.application_states.reserve (state_count);
    std::set<std::uint64_t> participant_ids;
    std::uint64_t previous_participant = 0;
    for (std::uint32_t index = 0; index != state_count; ++index) {
        relocation_envelope_application_state_t state;
        state.participant_id = reader.u64 ("state participant");
        if (state.participant_id == 0
            || state.participant_id <= previous_participant)
            throw service_wire_error_t (
              "relocation envelope participant ids must be strictly increasing");
        previous_participant = state.participant_id;
        state.state_encoding = reader.u8 ("state discriminator");
        if (state.state_encoding > 2)
            throw service_wire_error_t (
              "relocation envelope state discriminator is invalid");
        const auto body_size =
          reader.length64 (2 * (8 + max_state_bytes), "state body");
        envelope_reader_t body (reader.bytes (body_size, "state body"));
        if (state.state_encoding >= 1) {
            const auto state_size =
              body.length64 (max_state_bytes, "state payload");
            const auto payload = body.bytes (state_size, "state payload");
            state.state.assign (payload.begin (), payload.end ());
        }
        if (state.state_encoding == 2) {
            const auto recovery_size =
              body.length64 (max_state_bytes, "state recovery");
            const auto recovery = body.bytes (recovery_size,
                                              "state recovery");
            state.recovery.assign (recovery.begin (), recovery.end ());
        }
        body.require_end ("application state");
        participant_ids.insert (state.participant_id);
        envelope.application_states.push_back (std::move (state));
    }

    const auto saved_work_count = reader.u32 ("saved work count");
    if (saved_work_count
        > reader.remaining () / min_saved_work_entry_bytes)
        throw service_wire_error_t (
          "relocation envelope saved-work count exceeds the remaining payload");
    envelope.saved_work.reserve (saved_work_count);
    std::set<std::pair<std::uint64_t, std::uint64_t>> accepted_orders;
    std::set<std::tuple<std::string, std::uint64_t, std::uint64_t,
                        std::uint64_t, std::uint64_t, std::string>>
      operations;
    std::uint64_t previous_work_participant = 0;
    std::uint64_t previous_order = 0;
    for (std::uint32_t index = 0; index != saved_work_count; ++index) {
        relocation_envelope_saved_work_t entry;
        entry.participant_id = reader.u64 ("saved work participant");
        entry.order = reader.u64 ("saved work order");
        if (!participant_ids.contains (entry.participant_id)
            || entry.participant_id < previous_work_participant
            || (entry.participant_id == previous_work_participant
                && entry.order <= previous_order)
            || entry.order == 0
            || !accepted_orders
                  .emplace (entry.participant_id, entry.order)
                  .second)
            throw service_wire_error_t (
              "relocation envelope saved-work order is invalid");
        previous_work_participant = entry.participant_id;
        previous_order = entry.order;
        const auto record_bytes = bytes.subspan (reader.offset ());
        std::size_t consumed = 0;
        try {
            entry.record =
              decode_frozen_record_prefix (record_bytes, consumed);
        }
        catch (const service_wire_error_t &) {
            throw;
        }
        (void) reader.bytes (consumed, "saved work record");
        if (entry.record.operation.high != 0
            || entry.record.operation.low != 0) {
            if (!operations
                   .emplace (entry.record.source.owner_id,
                             entry.record.source.lease_generation,
                             entry.record.operation.high,
                             entry.record.operation.low,
                             entry.record.source.node_generation,
                             std::string (
                               entry.record.source.node_routing_id.begin (),
                               entry.record.source.node_routing_id.end ()))
                   .second)
                throw service_wire_error_t (
                  "relocation envelope saved work duplicates an operation identity");
        }
        envelope.saved_work.push_back (std::move (entry));
    }

    const auto timer_count = reader.u32 ("timer registration count");
    if (timer_count > max_timer_items)
        throw service_wire_error_t (
          "relocation envelope timer registration count exceeds its bound");
    envelope.timer_registrations.reserve (timer_count);
    std::set<std::pair<std::uint64_t, std::string>> timer_names;
    std::uint64_t previous_timer_participant = 0;
    std::string previous_timer_name;
    for (std::uint32_t index = 0; index != timer_count; ++index) {
        relocation_envelope_timer_t timer;
        timer.participant_id = reader.u64 ("timer participant");
        timer.name = reader.text8 ("timer name");
        timer.handler_type = reader.text8 ("timer handler type");
        timer.period_milliseconds = reader.u64 ("timer period");
        timer.overrun_policy = reader.u8 ("timer overrun policy");
        timer.max_catch_up_ticks = reader.u64 ("timer catch-up limit");
        const auto stop = reader.u8 ("timer exception policy");
        timer.last_completed_delivery_index =
          reader.u64 ("timer delivery index");
        timer.last_completed_scheduled_index =
          reader.u64 ("timer scheduled index");
        timer.next_scheduled_at_unix_milliseconds =
          reader.u64 ("timer schedule cursor");
        if (!participant_ids.contains (timer.participant_id)
            || timer.participant_id < previous_timer_participant
            || (timer.participant_id == previous_timer_participant
                && timer.name <= previous_timer_name)
            || timer.period_milliseconds == 0
            || timer.overrun_policy < 1 || timer.overrun_policy > 3
            || timer.max_catch_up_ticks == 0 || stop > 1
            || timer.next_scheduled_at_unix_milliseconds
                 > static_cast<std::uint64_t> (
                     std::numeric_limits<std::int64_t>::max ()))
            throw service_wire_error_t (
              "relocation envelope timer registration is invalid");
        timer.stop_on_unhandled_exception = stop == 1;
        previous_timer_participant = timer.participant_id;
        previous_timer_name = timer.name;
        timer_names.emplace (timer.participant_id, timer.name);
        envelope.timer_registrations.push_back (std::move (timer));
    }

    const auto pending_count = reader.u32 ("pending timer tick count");
    if (pending_count > max_timer_items)
        throw service_wire_error_t (
          "relocation envelope pending tick count exceeds its bound");
    envelope.pending_timer_ticks.reserve (pending_count);
    std::uint64_t previous_tick_participant = 0;
    std::uint64_t previous_tick_order = 0;
    for (std::uint32_t index = 0; index != pending_count; ++index) {
        relocation_envelope_pending_tick_t tick;
        tick.participant_id = reader.u64 ("pending tick participant");
        tick.order = reader.u64 ("pending tick order");
        tick.timer_name = reader.text8 ("pending tick timer name");
        tick.delivery_index = reader.u64 ("pending tick delivery index");
        tick.scheduled_index = reader.u64 ("pending tick scheduled index");
        tick.scheduled_at_unix_milliseconds =
          reader.u64 ("pending tick timestamp");
        tick.skipped_ticks = reader.u64 ("pending tick skip count");
        if (!participant_ids.contains (tick.participant_id)
            || tick.participant_id < previous_tick_participant
            || (tick.participant_id == previous_tick_participant
                && tick.order <= previous_tick_order)
            || tick.order == 0
            || !accepted_orders
                  .emplace (tick.participant_id, tick.order)
                  .second
            || tick.delivery_index == 0 || tick.scheduled_index == 0
            || !timer_names.contains (
                 {tick.participant_id, tick.timer_name})
            || tick.scheduled_at_unix_milliseconds
                 > static_cast<std::uint64_t> (
                     std::numeric_limits<std::int64_t>::max ()))
            throw service_wire_error_t (
              "relocation envelope pending timer tick is invalid");
        previous_tick_participant = tick.participant_id;
        previous_tick_order = tick.order;
        envelope.pending_timer_ticks.push_back (std::move (tick));
    }

    reader.require_end ("root");
    return envelope;
}

} // namespace zlink::framework::runtime::protocol
