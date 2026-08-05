/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework::runtime::stateful
{
namespace
{

constexpr std::array<std::uint8_t, 4> envelope_magic_v1{'Z', 'L', 'R', '1'};
constexpr std::array<std::uint8_t, 4> envelope_magic_v2{'Z', 'L', 'R', '2'};
constexpr std::array<std::uint8_t, 4> aggregate_envelope_magic{
  'Z', 'L', 'R', 'A'};
constexpr std::array<std::uint8_t, 4> recoverable_envelope_magic{
  'Z', 'L', 'R', 'W'};
constexpr std::array<std::uint8_t, 4> join_completion_magic{
  'Z', 'L', 'J', '1'};
constexpr std::array<std::uint8_t, 4> session_journal_magic{
  'Z', 'L', 'S', 'J'};
constexpr std::chrono::hours relocation_retention{24};
constexpr std::size_t max_envelope_bytes = 256u * 1024u * 1024u;
constexpr std::size_t max_application_state_bytes = 64u * 1024u * 1024u;
constexpr std::uint32_t max_pending_records = 4096;
constexpr std::uint32_t max_logical_timers = 4096;

struct replay_ack_effect_progress_t
{
    std::uint64_t acknowledged = 0;
    std::uint64_t acknowledged_records = 0;
};

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

bool append_bytes (std::vector<std::uint8_t> &output,
                   const std::vector<std::uint8_t> &value)
{
    if (value.size () > std::numeric_limits<std::uint32_t>::max ())
        return false;
    append_u32 (output, static_cast<std::uint32_t> (value.size ()));
    output.insert (output.end (), value.begin (), value.end ());
    return true;
}

bool append_text (std::vector<std::uint8_t> &output,
                  const std::string &value)
{
    return append_bytes (
      output, std::vector<std::uint8_t> (value.begin (), value.end ()));
}

class reader_t
{
  public:
    explicit reader_t (const std::vector<std::uint8_t> &input) :
        _input (input)
    {
    }

    std::optional<std::uint8_t> u8 ()
    {
        if (_offset == _input.size ())
            return std::nullopt;
        return _input[_offset++];
    }

    std::optional<std::uint32_t> u32 ()
    {
        if (_input.size () - _offset < 4)
            return std::nullopt;
        std::uint32_t value = 0;
        for (int count = 0; count != 4; ++count)
            value = (value << 8) | _input[_offset++];
        return value;
    }

    std::optional<std::uint64_t> u64 ()
    {
        if (_input.size () - _offset < 8)
            return std::nullopt;
        std::uint64_t value = 0;
        for (int count = 0; count != 8; ++count)
            value = (value << 8) | _input[_offset++];
        return value;
    }

    std::optional<std::vector<std::uint8_t>> bytes (
      std::size_t maximum = max_envelope_bytes)
    {
        const auto size = u32 ();
        if (!size || *size > maximum
            || *size > _input.size () - _offset)
            return std::nullopt;
        std::vector<std::uint8_t> result (
          _input.begin () + static_cast<std::ptrdiff_t> (_offset),
          _input.begin () + static_cast<std::ptrdiff_t> (_offset + *size));
        _offset += *size;
        return result;
    }

    std::optional<std::string> text ()
    {
        const auto value = bytes (255);
        return value
                 ? std::make_optional (
                     std::string (value->begin (), value->end ()))
                 : std::nullopt;
    }

    bool done () const noexcept { return _offset == _input.size (); }
    std::size_t remaining () const noexcept
    {
        return _input.size () - _offset;
    }

  private:
    const std::vector<std::uint8_t> &_input;
    std::size_t _offset = 0;
};

struct persisted_relocation_wire_t
{
    eligible_relocation_unit_t::canonical_wire_context_t context;
    std::vector<protocol::relocation_data_t> records;
};

struct recoverable_payload_t
{
    std::vector<std::uint8_t> state;
    std::optional<persisted_relocation_wire_t> wire;
};

std::vector<std::uint8_t> encode_recoverable_payload (
  std::vector<std::uint8_t> state,
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<protocol::relocation_data_t> &records)
{
    if (state.empty ()
        || (context.relocation.high == 0
            && context.relocation.low == 0)
        || context.target_attempt_generation == 0
        || context.coordinator.owner_id.empty ()
        || context.coordinator.lease_generation == 0
        || context.coordinator.node_routing_id.empty ()
        || context.coordinator.node_generation == 0
        || context.coordinator.expected_authority_store_version.empty ()
        || context.target_node_routing_id.empty ()
        || context.target_node_generation == 0
        || context.participant_ids.empty ()
        || context.participant_ids.size () > std::numeric_limits<std::uint32_t>::max ()
        || records.size () > max_pending_records) {
        return {};
    }

    std::vector<std::uint8_t> output (
      recoverable_envelope_magic.begin (),
      recoverable_envelope_magic.end ());
    if (!append_bytes (output, state))
        return {};
    append_u64 (output, context.relocation.high);
    append_u64 (output, context.relocation.low);
    append_u64 (output, context.target_attempt_generation);
    if (!append_text (output, context.coordinator.owner_id))
        return {};
    append_u64 (output, context.coordinator.lease_generation);
    if (!append_bytes (output, context.coordinator.node_routing_id))
        return {};
    append_u64 (output, context.coordinator.node_generation);
    if (!append_text (
          output,
          context.coordinator.expected_authority_store_version)
        || !append_bytes (output, context.target_node_routing_id))
        return {};
    append_u64 (output, context.target_node_generation);
    append_u32 (
      output,
      static_cast<std::uint32_t> (context.participant_ids.size ()));
    for (const auto participant : context.participant_ids) {
        if (participant == 0)
            return {};
        append_u64 (output, participant);
    }
    append_u32 (
      output, static_cast<std::uint32_t> (records.size ()));
    for (const auto &record : records) {
        std::vector<std::uint8_t> encoded;
        try {
            encoded = protocol::encode_relocation_control (record);
        }
        catch (...) {
            return {};
        }
        if (!append_bytes (output, encoded))
            return {};
    }
    return output.size () <= max_envelope_bytes
             ? std::move (output)
             : std::vector<std::uint8_t>{};
}

std::optional<recoverable_payload_t> decode_recoverable_payload (
  const std::vector<std::uint8_t> &payload) noexcept
try
{
    if (payload.size () < recoverable_envelope_magic.size ()
        || !std::equal (
          recoverable_envelope_magic.begin (),
          recoverable_envelope_magic.end (), payload.begin ())) {
        return recoverable_payload_t{payload, std::nullopt};
    }
    if (payload.size () > max_envelope_bytes)
        return std::nullopt;
    std::vector<std::uint8_t> body (
      payload.begin ()
        + static_cast<std::ptrdiff_t> (
          recoverable_envelope_magic.size ()),
      payload.end ());
    reader_t reader (body);
    auto state = reader.bytes ();
    const auto relocation_high = reader.u64 ();
    const auto relocation_low = reader.u64 ();
    const auto attempt = reader.u64 ();
    const auto coordinator_owner = reader.text ();
    const auto coordinator_lease = reader.u64 ();
    auto coordinator_node = reader.bytes (255);
    const auto coordinator_generation = reader.u64 ();
    const auto authority_version = reader.text ();
    auto target_node = reader.bytes (255);
    const auto target_generation = reader.u64 ();
    const auto participant_count = reader.u32 ();
    if (!state || !relocation_high || !relocation_low
        || (*relocation_high == 0 && *relocation_low == 0)
        || !attempt || *attempt == 0 || !coordinator_owner
        || coordinator_owner->empty () || !coordinator_lease
        || *coordinator_lease == 0 || !coordinator_node
        || coordinator_node->empty () || !coordinator_generation
        || *coordinator_generation == 0 || !authority_version
        || authority_version->empty () || !target_node
        || target_node->empty () || !target_generation
        || *target_generation == 0 || !participant_count
        || *participant_count == 0
        || reader.remaining ()
             < static_cast<std::size_t> (*participant_count) * 8u + 4u) {
        return std::nullopt;
    }

    persisted_relocation_wire_t persisted;
    persisted.context.relocation = {*relocation_high, *relocation_low};
    persisted.context.target_attempt_generation = *attempt;
    persisted.context.coordinator = {
      std::move (*coordinator_owner),
      *coordinator_lease,
      std::move (*coordinator_node),
      *coordinator_generation,
      std::move (*authority_version)};
    persisted.context.target_node_routing_id = std::move (*target_node);
    persisted.context.target_node_generation = *target_generation;
    persisted.context.participant_ids.reserve (*participant_count);
    for (std::uint32_t index = 0; index != *participant_count; ++index) {
        const auto participant = reader.u64 ();
        if (!participant || *participant == 0)
            return std::nullopt;
        persisted.context.participant_ids.push_back (*participant);
    }
    const auto record_count = reader.u32 ();
    if (!record_count || *record_count > max_pending_records
        || reader.remaining ()
             < static_cast<std::size_t> (*record_count) * 4u) {
        return std::nullopt;
    }
    persisted.records.reserve (*record_count);
    for (std::uint32_t index = 0; index != *record_count; ++index) {
        const auto encoded = reader.bytes ();
        if (!encoded)
            return std::nullopt;
        const auto control =
          protocol::decode_relocation_control (*encoded);
        const auto *record =
          std::get_if<protocol::relocation_data_t> (&control);
        if (!record
            || record->relocation != persisted.context.relocation
            || record->target_attempt_generation
                 != persisted.context.target_attempt_generation
            || record->coordinator != persisted.context.coordinator) {
            return std::nullopt;
        }
        persisted.records.push_back (*record);
    }
    if (!reader.done ())
        return std::nullopt;
    return recoverable_payload_t{
      std::move (*state), std::move (persisted)};
}
catch (...)
{
    return std::nullopt;
}

std::vector<std::uint8_t> encode_join_completion (
  const durable_join_completion_record_t &record)
{
    if ((record.operation_id_high == 0
         && record.operation_id_low == 0)
        || record.actor.kind != object_kind_t::actor
        || record.actor.key.empty ()
        || record.actor.object_generation == 0)
        throw std::invalid_argument (
          "durable Join completion identity is invalid");
    std::vector<std::uint8_t> output (
      join_completion_magic.begin (), join_completion_magic.end ());
    output.push_back (static_cast<std::uint8_t> (record.cursor));
    append_u64 (output, record.operation_id_high);
    append_u64 (output, record.operation_id_low);
    append_u64 (output, record.actor.object_generation);
    append_u64 (output, record.actor.authority_owner_generation);
    if (!append_text (output, record.actor.key)
        || !append_text (output, record.actor.mesh_name)
        || !append_text (output, record.actor.node_id)
        || !append_bytes (output, record.raw_reply))
        throw std::invalid_argument (
          "durable Join completion payload is too large");
    return output;
}

std::optional<durable_join_completion_record_t>
decode_join_completion (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () < join_completion_magic.size ()
        || !std::equal (join_completion_magic.begin (),
                        join_completion_magic.end (),
                        payload.begin ()))
        return std::nullopt;
    std::vector<std::uint8_t> body (
      payload.begin () + 4, payload.end ());
    reader_t reader (body);
    const auto cursor = reader.u8 ();
    const auto high = reader.u64 ();
    const auto low = reader.u64 ();
    const auto object_generation = reader.u64 ();
    const auto owner_generation = reader.u64 ();
    const auto key = reader.text ();
    const auto mesh = reader.text ();
    const auto node = reader.text ();
    const auto reply = reader.bytes ();
    if (!cursor || *cursor > static_cast<std::uint8_t> (
                     join_completion_cursor_t::delivered)
        || !high || !low || (*high == 0 && *low == 0)
        || !object_generation || *object_generation == 0
        || !owner_generation || !key || key->empty ()
        || !mesh || !node || !reply || !reader.done ())
        return std::nullopt;
    return durable_join_completion_record_t{
      *high, *low,
      object_ref_t{object_kind_t::actor, *key,
                   *object_generation, *owner_generation,
                   *mesh, *node},
      *reply,
      static_cast<join_completion_cursor_t> (*cursor)};
}

std::vector<std::uint8_t> encode_session_journal (
  const durable_session_journal_record_t &record)
{
    if ((record.relocation_id_high == 0
         && record.relocation_id_low == 0)
        || record.actor.kind != object_kind_t::actor
        || record.actor.key.empty ()
        || record.actor.object_generation == 0
        || record.actor.authority_owner_generation == 0
        || record.actor.node_id.empty ()
        || record.binding_generation == 0)
        throw std::invalid_argument (
          "durable Session journal identity is invalid");
    std::vector<std::uint8_t> output (
      session_journal_magic.begin (), session_journal_magic.end ());
    append_u64 (output, record.relocation_id_high);
    append_u64 (output, record.relocation_id_low);
    append_u64 (output, record.actor.object_generation);
    append_u64 (output, record.actor.authority_owner_generation);
    append_u64 (output, record.binding_generation);
    append_u64 (output, record.last_accepted_session_sequence);
    if (!append_text (output, record.actor.key)
        || !append_text (output, record.actor.mesh_name)
        || !append_text (output, record.actor.node_id)
        || !append_bytes (output, record.accepted_journal)
        || output.size () > max_envelope_bytes)
        throw std::invalid_argument (
          "durable Session journal payload is too large");
    return output;
}

std::optional<durable_session_journal_record_t>
decode_session_journal (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () < session_journal_magic.size ()
        || !std::equal (session_journal_magic.begin (),
                        session_journal_magic.end (),
                        payload.begin ()))
        return std::nullopt;
    std::vector<std::uint8_t> body (
      payload.begin () + 4, payload.end ());
    reader_t reader (body);
    const auto relocation_high = reader.u64 ();
    const auto relocation_low = reader.u64 ();
    const auto object_generation = reader.u64 ();
    const auto authority_generation = reader.u64 ();
    const auto binding_generation = reader.u64 ();
    const auto high_water = reader.u64 ();
    const auto actor_id = reader.text ();
    const auto mesh_name = reader.text ();
    const auto node_id = reader.text ();
    const auto journal = reader.bytes ();
    if (!relocation_high || !relocation_low
        || (*relocation_high == 0 && *relocation_low == 0)
        || !object_generation || *object_generation == 0
        || !authority_generation || *authority_generation == 0
        || !binding_generation || *binding_generation == 0
        || !high_water || !actor_id || actor_id->empty ()
        || !mesh_name || !node_id || node_id->empty ()
        || !journal || !reader.done ())
        return std::nullopt;
    return durable_session_journal_record_t{
      *relocation_high,
      *relocation_low,
      object_ref_t{object_kind_t::actor, *actor_id,
                   *object_generation, *authority_generation,
                   *mesh_name, *node_id},
      *binding_generation,
      *high_water,
      *journal};
}

} // namespace

durable_join_completion_store_t::durable_join_completion_store_t (
  std::shared_ptr<relocation_store_port_t> store) :
    _store (std::move (store))
{
    if (!_store)
        throw std::invalid_argument (
          "durable Join completion store is required");
}

durable_join_completion_root_t
durable_join_completion_store_t::store (
  const durable_join_completion_record_t &record)
{
    const auto payload = encode_join_completion (record);
    const auto checksum = maintenance_runtime_t::crc32c (payload);
    const auto stored = _store->put (payload, relocation_retention);
    if (stored.reference.empty ()
        || stored.checksum_crc32c != checksum)
        throw std::runtime_error (
          "durable Join completion store write failed");
    return {stored.reference, checksum};
}

durable_join_completion_root_t
durable_join_completion_store_t::prepare (
  durable_join_completion_record_t record)
{
    record.cursor = join_completion_cursor_t::prepared;
    return store (record);
}

std::optional<durable_join_completion_record_t>
durable_join_completion_store_t::recover (
  const durable_join_completion_root_t &root) const
{
    const auto payload = _store->get (root.reference);
    if (!payload
        || maintenance_runtime_t::crc32c (*payload)
             != root.checksum_crc32c)
        return std::nullopt;
    return decode_join_completion (*payload);
}

durable_join_completion_root_t
durable_join_completion_store_t::commit (
  const durable_join_completion_root_t &root,
  bool remove_previous)
{
    auto record = recover (root);
    if (!record)
        throw std::runtime_error (
          "durable Join completion root is unavailable");
    if (record->cursor != join_completion_cursor_t::prepared)
        return root;
    record->cursor = join_completion_cursor_t::committed;
    auto next = store (*record);
    if (remove_previous)
        _store->remove (root.reference);
    return next;
}

durable_join_completion_root_t
durable_join_completion_store_t::deliver (
  const durable_join_completion_root_t &root,
  const object_ref_t &expected_actor,
  const std::function<bool (
    const durable_join_completion_record_t &)> &callback,
  bool remove_previous)
{
    auto record = recover (root);
    if (!record)
        throw std::runtime_error (
          "durable Join completion root is unavailable");
    if (record->actor != expected_actor)
        throw std::invalid_argument (
          "durable Join completion Actor generation fence failed");
    if (record->cursor == join_completion_cursor_t::delivered)
        return root;
    if (record->cursor != join_completion_cursor_t::committed
        || !callback || !callback (*record))
        return root;
    record->cursor = join_completion_cursor_t::delivered;
    auto next = store (*record);
    if (remove_previous)
        _store->remove (root.reference);
    return next;
}

void durable_join_completion_store_t::cleanup (
  const durable_join_completion_root_t &root)
{
    _store->remove (root.reference);
}

durable_session_journal_store_t::durable_session_journal_store_t (
  std::shared_ptr<relocation_store_port_t> store) :
    _store (std::move (store))
{
    if (!_store)
        throw std::invalid_argument (
          "durable Session journal store is required");
}

durable_session_journal_root_t
durable_session_journal_store_t::prepare (
  const durable_session_journal_record_t &record)
{
    const auto payload = encode_session_journal (record);
    const auto checksum = maintenance_runtime_t::crc32c (payload);
    const auto stored = _store->put (payload, relocation_retention);
    if (stored.reference.empty ()
        || stored.checksum_crc32c != checksum)
        throw std::runtime_error (
          "durable Session journal store write failed");
    return {stored.reference, checksum};
}

std::optional<durable_session_journal_record_t>
durable_session_journal_store_t::recover (
  const durable_session_journal_root_t &root) const
{
    const auto payload = _store->get (root.reference);
    if (!payload
        || maintenance_runtime_t::crc32c (*payload)
             != root.checksum_crc32c)
        return std::nullopt;
    return decode_session_journal (*payload);
}

void durable_session_journal_store_t::cleanup (
  const durable_session_journal_root_t &root)
{
    _store->remove (root.reference);
}

maintenance_runtime_t::maintenance_runtime_t (
  stateful_object_runtime_t &objects,
  std::shared_ptr<authority_relocation_port_t> authority,
  std::shared_ptr<relocation_store_port_t> relocations,
  relocation_limits_t limits,
  observer_t observer,
  std::shared_ptr<aggregate_authority_port_t>
    aggregate_authority) :
    _objects (objects),
    _authority (std::move (authority)),
    _relocations (std::move (relocations)),
    _aggregate_authority (std::move (aggregate_authority)),
    _limits (limits),
    _observer (std::move (observer))
{
    if (!_authority || !_relocations
        || _limits.outbound_units == 0
        || _limits.inbound_units == 0
        || _limits.capture_callbacks == 0
        || _limits.restore_callbacks == 0
        || _limits.payload_bytes == 0) {
        throw std::invalid_argument ("maintenance runtime configuration is invalid");
    }
}

maintenance_runtime_t::maintenance_runtime_t (
  stateful_object_runtime_t &objects,
  maintenance_provider_set_t providers,
  relocation_limits_t limits,
  observer_t observer) :
    maintenance_runtime_t (
      objects, std::move (providers.authority),
      std::move (providers.relocations), limits, std::move (observer),
      std::move (providers.aggregate_authority))
{
    if (!_aggregate_authority || !providers.targets)
        throw std::invalid_argument (
          "maintenance provider set is incomplete");
}

void maintenance_runtime_t::attach_relocation_wire (
  raw_relocation_replay_coordinator_t &wire) noexcept
{
    _relocation_wire = &wire;
}

std::optional<std::vector<protocol::relocation_data_t>>
maintenance_runtime_t::build_replay_records (
  const std::vector<frozen_object_state_t> &participants,
  const eligible_relocation_unit_t::canonical_wire_context_t &context) const
{
    if (!_relocation_wire
        || (context.relocation.high == 0 && context.relocation.low == 0)
        || context.target_attempt_generation == 0
        || context.coordinator.owner_id.empty ()
        || context.coordinator.lease_generation == 0
        || context.coordinator.node_routing_id.empty ()
        || context.coordinator.node_generation == 0
        || context.coordinator.expected_authority_store_version.empty ()
        || context.target_node_routing_id.empty ()
        || context.target_node_generation == 0
        || context.participant_ids.size () != participants.size ()
        || !context.prepare_target || !context.acknowledged
        || !context.abort_target)
        return std::nullopt;

    std::vector<protocol::relocation_data_t> result;
    for (std::size_t index = 0; index != participants.size (); ++index) {
        const auto &participant = participants[index];
        const auto participant_id = context.participant_ids[index];
        if (participant_id == 0)
            return std::nullopt;
        std::uint64_t sequence = 0;
        for (const auto &pending : participant.pending_application) {
            protocol::frozen_record_t frozen;
            try {
                frozen = protocol::decode_frozen_record (pending.payload);
            }
            catch (...) {
                return std::nullopt;
            }
            if (!frozen.target
                || frozen.target->object_id != participant.owner.key
                || frozen.target->object_generation
                     != participant.owner.object_generation
                || frozen.target->authority_owner_generation
                     != participant.owner.authority_owner_generation
                || (frozen.source.owner_id.empty ()
                    || frozen.source.lease_generation == 0
                    || frozen.source.node_routing_id.empty ()
                    || frozen.source.node_generation == 0)
                || (frozen.operation.high == 0 && frozen.operation.low == 0)
                || ((frozen.kind
                       == protocol::frozen_record_kind_t::actor_request
                     || frozen.kind
                          == protocol::frozen_record_kind_t::spot_request)
                    && (!frozen.reply_route_id
                        || *frozen.reply_route_id == 0)))
                return std::nullopt;
            protocol::relocation_object_kind_t kind;
            switch (participant.owner.kind) {
            case object_kind_t::actor:
                kind = protocol::relocation_object_kind_t::actor;
                break;
            case object_kind_t::user_spot:
                kind = protocol::relocation_object_kind_t::user_spot;
                break;
            case object_kind_t::instance_spot:
                kind = protocol::relocation_object_kind_t::instance_spot;
                break;
            default:
                return std::nullopt;
            }
            result.push_back ({
              context.relocation,
              context.target_attempt_generation,
              context.coordinator,
              protocol::relocation_role_t::source,
              participant_id,
              ++sequence,
              frozen.source,
              {kind, participant.stable_type, participant.owner.key,
               participant.owner.object_generation,
               participant.owner.authority_owner_generation},
              protocol::relocation_phase_t::prepared,
              0,
              protocol::framework_error_code::none,
              std::move (frozen)});
        }
    }
    return result;
}

bool maintenance_runtime_t::prepare_replay_source (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<frozen_object_state_t> &participants,
  const std::vector<protocol::relocation_data_t> &records,
  const relocation_stored_t &stored)
{
    bool target_prepared = false;
    try {
        target_prepared =
          context.prepare_target (participants, records, stored);
    }
    catch (...) {
        target_prepared = false;
    }
    if (!target_prepared) {
        return false;
    }
    if (records.empty ())
        return true;

    std::vector<protocol::wire_operation_id_t> terminal_sources;
    for (const auto &record : records) {
        if (!record.frozen_record
            || !record.frozen_record->reply_route_id)
            continue;
        if (!context.complete_source_terminal) {
            try {
                context.abort_target ();
            }
            catch (...) {
            }
            return false;
        }
        const auto operation = record.frozen_record->operation;
        const auto sequence = record.sequence;
        const auto participant = record.participant_id;
        if (!_relocation_wire->register_terminal_source ({
              context.relocation,
              context.coordinator,
              operation,
              record.source,
              context.target_node_routing_id,
              context.target_node_generation,
              context.target_attempt_generation,
              participant,
              sequence,
              *record.frozen_record->reply_route_id,
              [callback = context.complete_source_terminal,
               participant,
               sequence] (
                const protocol::reply_relay_t &relay,
                const std::optional<protocol::application_payload_t>
                  &application_reply) {
                  return callback (
                    participant, sequence, relay, application_reply);
              }})) {
            for (const auto &registered : terminal_sources) {
                (void) _relocation_wire->unregister_terminal_source (
                  context.relocation, registered);
            }
            try {
                context.abort_target ();
            }
            catch (...) {
            }
            return false;
        }
        terminal_sources.push_back (operation);
    }

    std::map<std::uint64_t, std::vector<protocol::relocation_data_t>> grouped;
    for (const auto &record : records)
        grouped[record.participant_id].push_back (record);
    std::vector<std::uint64_t> registered;
    for (auto &[participant, participant_records] : grouped) {
        const auto high_water = participant_records.size ();
        const auto records_for_ack =
          std::make_shared<std::vector<protocol::relocation_data_t>> (
            participant_records);
        const auto effect_progress =
          std::make_shared<replay_ack_effect_progress_t> ();
        if (!_relocation_wire->register_source ({
              context.relocation,
              context.target_attempt_generation,
              context.coordinator,
              participant,
              context.target_node_routing_id,
              context.target_node_generation,
              high_water,
              [callback = context.acknowledged,
               record_callback = context.acknowledged_records,
               records_for_ack,
               participant,
               effect_progress] (std::uint64_t value) {
                  if (value > effect_progress->acknowledged) {
                      if (callback)
                          callback (participant, value);
                      effect_progress->acknowledged = value;
                  }
                  if (record_callback
                      && value > effect_progress->acknowledged_records) {
                      record_callback (
                        participant, *records_for_ack, value);
                      effect_progress->acknowledged_records = value;
                  }
              },
              std::move (participant_records)})) {
            for (const auto registered_participant : registered) {
                (void) _relocation_wire->unregister_source (
                  context.relocation, context.target_attempt_generation,
                  registered_participant);
            }
            for (const auto &registered_operation : terminal_sources) {
                (void) _relocation_wire->unregister_terminal_source (
                  context.relocation, registered_operation);
            }
            try {
                context.abort_target ();
            }
            catch (...) {
            }
            return false;
        }
        registered.push_back (participant);
    }
    return true;
}

bool maintenance_runtime_t::arm_replay_source (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<protocol::relocation_data_t> &records)
{
    std::set<std::uint64_t> participants;
    for (const auto &record : records)
        participants.insert (record.participant_id);
    for (const auto participant : participants) {
        if (!_relocation_wire->arm_source (
              context.relocation, context.target_attempt_generation,
              participant))
            return false;
    }
    (void) _relocation_wire->retry_source_replays (
      raw_relocation_replay_coordinator_t::clock_t::now ());
    return true;
}

void maintenance_runtime_t::abort_replay_source (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<protocol::relocation_data_t> &records) noexcept
{
    std::set<std::uint64_t> participants;
    for (const auto &record : records)
        participants.insert (record.participant_id);
    for (const auto participant : participants) {
        (void) _relocation_wire->unregister_source (
          context.relocation, context.target_attempt_generation,
          participant);
    }
    std::set<std::pair<std::uint64_t, std::uint64_t>> operations;
    for (const auto &record : records) {
        if (!record.frozen_record
            || !record.frozen_record->reply_route_id)
            continue;
        operations.emplace (
          record.frozen_record->operation.high,
          record.frozen_record->operation.low);
    }
    for (const auto &[high, low] : operations) {
        (void) _relocation_wire->unregister_terminal_source (
          context.relocation, {high, low});
    }
    try {
        context.abort_target ();
    }
    catch (...) {
    }
}

relocation_result_t maintenance_runtime_t::relocate (
  const object_ref_t &source,
  std::string target_node_id,
  location_owner_token_t target_owner,
  relocation_capacity_fence_t relocation_capacity_fence,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
    &canonical_wire,
  std::stop_token cancellation)
{
    auto permit = try_acquire (encoded_upper_bound);
    if (!permit) {
        return finish (
          {relocation_terminal_t::blocked,
           relocation_reason_t::permit_unavailable,
           std::nullopt});
    }

    auto [seal_error, seal] =
      _objects.try_seal_relocation (source, cancellation);
    if (seal_error != stateful_error_t::none) {
        return finish (
          {relocation_terminal_t::blocked,
           seal_error == stateful_error_t::backpressured
             ? relocation_reason_t::turn_active
             : relocation_reason_t::restore_failed,
           std::nullopt});
    }

    std::vector<protocol::relocation_data_t> replay_records;
    if (canonical_wire) {
        const auto built = build_replay_records ({seal.frozen}, *canonical_wire);
        if (!built) {
            (void) _objects.abort_relocation (seal.token);
            return finish (
              {relocation_terminal_t::blocked,
               relocation_reason_t::restore_failed,
               std::nullopt});
        }
        replay_records = *built;
    }

    std::vector<std::uint8_t> payload;
    try {
        payload = encode (seal.frozen, inventory_digest);
        if (payload.empty () || payload.size () > encoded_upper_bound) {
            (void) _objects.abort_relocation (seal.token);
            return finish (
              {relocation_terminal_t::blocked,
               relocation_reason_t::payload_bound_exceeded,
               std::nullopt});
        }
        if (canonical_wire) {
            payload = encode_recoverable_payload (
              std::move (payload), *canonical_wire, replay_records);
        }
    }
    catch (...) {
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {relocation_terminal_t::store_failed,
           relocation_reason_t::store_write_failed,
           std::nullopt});
    }
    if (payload.empty ()) {
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {relocation_terminal_t::blocked,
           relocation_reason_t::payload_bound_exceeded,
           std::nullopt});
    }
    const auto checksum = crc32c (payload);

    relocation_stored_t stored;
    try {
        stored = _relocations->put (payload, relocation_retention);
    }
    catch (...) {
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {relocation_terminal_t::store_failed,
           relocation_reason_t::store_write_failed,
           std::nullopt});
    }
    if (stored.reference.empty () || stored.checksum_crc32c != checksum) {
        if (!stored.reference.empty ()) {
            try {
                _relocations->remove (stored.reference);
            }
            catch (...) {
            }
        }
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {relocation_terminal_t::store_failed,
           relocation_reason_t::checksum_mismatch,
           std::nullopt});
    }


    if (canonical_wire
        && !prepare_replay_source (
          *canonical_wire, {seal.frozen}, replay_records, stored)) {
        try {
            _relocations->remove (stored.reference);
        }
        catch (...) {
        }
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {relocation_terminal_t::blocked,
           relocation_reason_t::restore_failed,
           std::nullopt});
    }
    authority_publish_result_t published;
    bool publish_uncertain = false;
    try {
        published = _authority->publish (
          source, std::move (target_node_id),
          std::move (target_owner),
          std::move (relocation_capacity_fence),
          stored.reference,
          checksum, inventory_digest);
    }
    catch (...) {
        published.status = authority_publish_status_t::failed;
        publish_uncertain = true;
    }
    if (published.status != authority_publish_status_t::published
        || !published.current) {
        try {
            const auto current =
              _authority->read (source.kind, source.key);
            if (current
                && current->source == source
                && current->relocation_reference == stored.reference
                && current->checksum_crc32c == checksum
                && current->inventory_digest == inventory_digest) {
                published.status = authority_publish_status_t::published;
                published.current = current;
                publish_uncertain = false;
            } else {
                publish_uncertain = false;
            }
        }
        catch (...) {
            publish_uncertain = true;
        }
    }
    if (published.status != authority_publish_status_t::published
        || !published.current) {
        if (publish_uncertain) {
            return finish (
              {relocation_terminal_t::recovery_required,
               relocation_reason_t::authority_publish_failed,
               std::nullopt,
               replay_records});
        }
        if (canonical_wire)
            abort_replay_source (*canonical_wire, replay_records);
        try {
            _relocations->remove (stored.reference);
        }
        catch (...) {
        }
        (void) _objects.abort_relocation (seal.token);
        return finish (
          {published.status == authority_publish_status_t::conflict
             ? relocation_terminal_t::conflict
             : relocation_terminal_t::store_failed,
           published.status == authority_publish_status_t::conflict
             ? relocation_reason_t::authority_conflict
             : relocation_reason_t::authority_publish_failed,
           published.current});
    }


    if (canonical_wire
        && !arm_replay_source (*canonical_wire, replay_records)) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           published.current,
           replay_records});
    }
    const auto [commit_error, committed] =
      _objects.commit_relocation (
        seal.token, published.current->target.node_id);
    if (commit_error != stateful_error_t::none
        || committed != published.current->target) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           published.current,
           replay_records});
    }
    if (canonical_wire
        && (!canonical_wire->complete_target
            || !canonical_wire->complete_target ())) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           published.current,
           replay_records});
    }
    return finish (
      {relocation_terminal_t::completed,
       relocation_reason_t::none,
       published.current,
       replay_records});
}

relocation_result_t maintenance_runtime_t::recover (
  object_kind_t kind,
  const std::string &key,
  stateful_object_runtime_t &target,
  std::stop_token cancellation)
{
    return recover_impl (
      kind, key, target, nullptr, cancellation);
}

relocation_result_t maintenance_runtime_t::recover (
  object_kind_t kind,
  const std::string &key,
  stateful_object_runtime_t &target,
  const eligible_relocation_unit_t::canonical_wire_context_t
    &recovery_callbacks,
  std::stop_token cancellation)
{
    return recover_impl (
      kind, key, target, &recovery_callbacks, cancellation);
}

relocation_result_t maintenance_runtime_t::recover_impl (
  object_kind_t kind,
  const std::string &key,
  stateful_object_runtime_t &target,
  const eligible_relocation_unit_t::canonical_wire_context_t
    *recovery_callbacks,
  std::stop_token cancellation)
{
    std::optional<authority_relocation_reference_t> authority;
    try {
        authority = _authority->read (kind, key);
    }
    catch (...) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::authority_publish_failed,
           std::nullopt});
    }
    if (!authority) {
        return finish (
          {relocation_terminal_t::conflict,
           relocation_reason_t::authority_conflict,
           std::nullopt});
    }
    std::optional<std::vector<std::uint8_t>> payload;
    try {
        payload = _relocations->get (authority->relocation_reference);
    }
    catch (...) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::store_write_failed,
           authority});
    }
    if (!payload) {
        return finish (
          {relocation_terminal_t::data_lost,
           relocation_reason_t::payload_missing,
           authority});
    }
    if (crc32c (*payload) != authority->checksum_crc32c) {
        return finish (
          {relocation_terminal_t::data_lost,
           relocation_reason_t::checksum_mismatch,
           authority});
    }
    std::optional<recoverable_payload_t> recoverable;
    std::optional<
      std::pair<frozen_object_state_t, inventory_digest_t>> decoded;
    try {
        recoverable = decode_recoverable_payload (*payload);
        if (recoverable)
            decoded = decode (recoverable->state);
    }
    catch (...) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           authority});
    }
    if (!decoded
        || decoded->second != authority->inventory_digest) {
        return finish (
          {relocation_terminal_t::data_lost,
           relocation_reason_t::inventory_mismatch,
           authority});
    }
    const auto recovery_participant =
      recoverable && recoverable->wire && recovery_callbacks
        ? std::make_optional (decoded->first)
        : std::nullopt;
    stateful_error_t restored = stateful_error_t::conflict;
    try {
        restored = target.restore_relocation (
          std::move (decoded->first), authority->target,
          {authority->relocation_reference,
           authority->checksum_crc32c,
           authority->inventory_digest},
          cancellation);
    }
    catch (...) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           authority});
    }
    if (restored != stateful_error_t::none
        && !(restored == stateful_error_t::already_exists
             && target.find (kind, key) == authority->target)) {
        return finish (
          {relocation_terminal_t::recovery_required,
           relocation_reason_t::restore_failed,
           authority});
    }
    if (recoverable && recoverable->wire && recovery_callbacks) {
        auto context = std::move (recoverable->wire->context);
        context.prepare_target = recovery_callbacks->prepare_target;
        context.acknowledged = recovery_callbacks->acknowledged;
        context.acknowledged_records =
          recovery_callbacks->acknowledged_records;
        context.complete_source_terminal =
          recovery_callbacks->complete_source_terminal;
        context.complete_target = recovery_callbacks->complete_target;
        context.abort_target = recovery_callbacks->abort_target;
        if (!prepare_replay_source (
              context, {*recovery_participant},
              recoverable->wire->records,
              {authority->relocation_reference,
               authority->checksum_crc32c})
            || !arm_replay_source (
              context, recoverable->wire->records)
            || !context.complete_target
            || !context.complete_target ()) {
            return finish (
              {relocation_terminal_t::recovery_required,
               relocation_reason_t::restore_failed,
               authority,
               recoverable->wire->records});
        }
        return finish (
          {relocation_terminal_t::completed,
           relocation_reason_t::none,
           authority,
           recoverable->wire->records});
    }
    return finish (
      {relocation_terminal_t::recovery_required,
       relocation_reason_t::restore_failed,
       authority});
}

aggregate_relocation_result_t maintenance_runtime_t::recover_aggregate (
  const std::vector<object_ref_t> &sources,
  stateful_object_runtime_t &target,
  std::stop_token cancellation)
{
    return recover_aggregate_impl (
      sources, target, nullptr, cancellation);
}

aggregate_relocation_result_t maintenance_runtime_t::recover_aggregate (
  const std::vector<object_ref_t> &sources,
  stateful_object_runtime_t &target,
  const eligible_relocation_unit_t::canonical_wire_context_t
    &recovery_callbacks,
  std::stop_token cancellation)
{
    return recover_aggregate_impl (
      sources, target, &recovery_callbacks, cancellation);
}

aggregate_relocation_result_t
maintenance_runtime_t::recover_aggregate_impl (
  const std::vector<object_ref_t> &sources,
  stateful_object_runtime_t &target,
  const eligible_relocation_unit_t::canonical_wire_context_t
    *recovery_callbacks,
  std::stop_token cancellation)
{
    if (sources.size () < 2) {
        return {
          relocation_terminal_t::conflict,
          relocation_reason_t::inventory_mismatch,
          {}};
    }
    std::vector<authority_relocation_reference_t> authority;
    try {
        auto canonical_sources = sources;
        std::sort (
          canonical_sources.begin (), canonical_sources.end (),
          [] (const object_ref_t &left, const object_ref_t &right) {
              if (left.kind != right.kind)
                  return left.kind < right.kind;
              return left.key < right.key;
          });
        for (std::size_t index = 1;
             index != canonical_sources.size (); ++index) {
            if (canonical_sources[index - 1].kind
                  == canonical_sources[index].kind
                && canonical_sources[index - 1].key
                     == canonical_sources[index].key) {
                return {
                  relocation_terminal_t::conflict,
                  relocation_reason_t::inventory_mismatch,
                  {}};
            }
        }
        authority.reserve (sources.size ());
        for (const auto &source : sources) {
            const auto current =
              _authority->read (source.kind, source.key);
            if (!current || current->source != source) {
                return {
                  relocation_terminal_t::conflict,
                  relocation_reason_t::authority_conflict,
                  {}};
            }
            authority.push_back (*current);
        }
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::authority_publish_failed,
          {}};
    }

    const auto &root = authority.front ();
    for (const auto &current : authority) {
        if (current.relocation_reference != root.relocation_reference
            || current.checksum_crc32c != root.checksum_crc32c
            || current.inventory_digest != root.inventory_digest
            || current.target.node_id != root.target.node_id
            || current.target.mesh_name != root.target.mesh_name
            || current.target_owner.owner_id
                 != root.target_owner.owner_id
            || current.target_owner.lease_generation
                 != root.target_owner.lease_generation) {
            return {
              relocation_terminal_t::data_lost,
              relocation_reason_t::inventory_mismatch,
              authority};
        }
    }

    std::optional<std::vector<std::uint8_t>> payload;
    try {
        payload = _relocations->get (root.relocation_reference);
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::store_write_failed,
          authority};
    }
    if (!payload) {
        return {
          relocation_terminal_t::data_lost,
          relocation_reason_t::payload_missing,
          authority};
    }
    if (crc32c (*payload) != root.checksum_crc32c) {
        return {
          relocation_terminal_t::data_lost,
          relocation_reason_t::checksum_mismatch,
          authority};
    }
    std::optional<recoverable_payload_t> recoverable;
    std::optional<
      std::pair<std::vector<frozen_object_state_t>,
                inventory_digest_t>> decoded;
    try {
        recoverable = decode_recoverable_payload (*payload);
        if (recoverable)
            decoded = decode_aggregate (recoverable->state);
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          authority};
    }
    if (!decoded || decoded->second != root.inventory_digest
        || decoded->first.size () != authority.size ()) {
        return {
          relocation_terminal_t::data_lost,
          relocation_reason_t::inventory_mismatch,
          authority};
    }

    std::vector<object_ref_t> targets;
    auto frozen = std::move (decoded->first);
    try {
        std::sort (
          frozen.begin (), frozen.end (),
          [] (const frozen_object_state_t &left,
              const frozen_object_state_t &right) {
              if (left.owner.kind != right.owner.kind)
                  return left.owner.kind < right.owner.kind;
              return left.owner.key < right.owner.key;
          });
        std::sort (
          authority.begin (), authority.end (),
          [] (const authority_relocation_reference_t &left,
              const authority_relocation_reference_t &right) {
              if (left.source.kind != right.source.kind)
                  return left.source.kind < right.source.kind;
              return left.source.key < right.source.key;
          });
        targets.reserve (authority.size ());
        for (std::size_t index = 0; index != authority.size (); ++index) {
            if (frozen[index].owner != authority[index].source) {
                return {
                  relocation_terminal_t::data_lost,
                  relocation_reason_t::inventory_mismatch,
                  authority};
            }
            targets.push_back (authority[index].target);
        }
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          authority};
    }

    const auto recovery_participants =
      recoverable && recoverable->wire && recovery_callbacks
        ? std::make_optional (frozen)
        : std::nullopt;
    stateful_error_t restored = stateful_error_t::conflict;
    try {
        restored = target.restore_relocation_aggregate (
          std::move (frozen), std::move (targets),
          {root.relocation_reference,
           root.checksum_crc32c,
           root.inventory_digest},
          cancellation);
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          authority};
    }
    if (restored != stateful_error_t::none
        && restored != stateful_error_t::already_exists) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          authority};
    }
    if (recoverable && recoverable->wire && recovery_callbacks) {
        auto context = std::move (recoverable->wire->context);
        context.prepare_target = recovery_callbacks->prepare_target;
        context.acknowledged = recovery_callbacks->acknowledged;
        context.acknowledged_records =
          recovery_callbacks->acknowledged_records;
        context.complete_source_terminal =
          recovery_callbacks->complete_source_terminal;
        context.complete_target = recovery_callbacks->complete_target;
        context.abort_target = recovery_callbacks->abort_target;
        if (!prepare_replay_source (
              context, *recovery_participants,
              recoverable->wire->records,
              {root.relocation_reference, root.checksum_crc32c})
            || !arm_replay_source (
              context, recoverable->wire->records)
            || !context.complete_target
            || !context.complete_target ()) {
            return {
              relocation_terminal_t::recovery_required,
              relocation_reason_t::restore_failed,
              authority,
              recoverable->wire->records};
        }
        return {
          relocation_terminal_t::completed,
          relocation_reason_t::none,
          authority,
          recoverable->wire->records};
    }
    return {
      relocation_terminal_t::recovery_required,
      relocation_reason_t::restore_failed,
      authority};
}

aggregate_relocation_result_t maintenance_runtime_t::relocate_aggregate (
  const std::vector<object_ref_t> &sources,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::vector<relocation_capacity_fence_t>
    relocation_capacity_fences,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
    &canonical_wire,
  std::stop_token cancellation)
{
    if (!_aggregate_authority || sources.size () < 2) {
        return {
          relocation_terminal_t::blocked,
          relocation_reason_t::restore_failed,
          {}};
    }
    auto permit = try_acquire (encoded_upper_bound);
    if (!permit) {
        return {
          relocation_terminal_t::blocked,
          relocation_reason_t::permit_unavailable,
          {}};
    }
    auto [seal_error, seal] =
      _objects.try_seal_relocation_aggregate (
        sources, cancellation);
    if (seal_error != stateful_error_t::none) {
        return {
          relocation_terminal_t::blocked,
          seal_error == stateful_error_t::backpressured
            ? relocation_reason_t::turn_active
            : relocation_reason_t::restore_failed,
          {}};
    }
    std::vector<protocol::relocation_data_t> replay_records;
    if (canonical_wire) {
        const auto built =
          build_replay_records (seal.participants, *canonical_wire);
        if (!built) {
            (void) _objects.abort_relocation (seal.token);
            return {
              relocation_terminal_t::blocked,
              relocation_reason_t::restore_failed,
              {}};
        }
        replay_records = *built;
    }
    std::vector<std::uint8_t> payload;
    try {
        payload = encode_aggregate (
          seal.participants, inventory_digest);
        if (payload.empty () || payload.size () > encoded_upper_bound) {
            (void) _objects.abort_relocation (seal.token);
            return {
              relocation_terminal_t::blocked,
              relocation_reason_t::payload_bound_exceeded,
              {}};
        }
        if (canonical_wire) {
            payload = encode_recoverable_payload (
              std::move (payload), *canonical_wire, replay_records);
        }
    }
    catch (...) {
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::store_failed,
          relocation_reason_t::store_write_failed,
          {}};
    }
    if (payload.empty ()) {
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::blocked,
          relocation_reason_t::payload_bound_exceeded,
          {}};
    }
    const auto checksum = crc32c (payload);
    relocation_stored_t stored;
    try {
        stored = _relocations->put (payload, relocation_retention);
    }
    catch (...) {
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::store_failed,
          relocation_reason_t::store_write_failed,
          {}};
    }
    if (stored.reference.empty () || stored.checksum_crc32c != checksum) {
        if (!stored.reference.empty ()) {
            try {
                _relocations->remove (stored.reference);
            }
            catch (...) {
            }
        }
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::store_failed,
          relocation_reason_t::checksum_mismatch,
          {}};
    }


    if (canonical_wire
        && !prepare_replay_source (
          *canonical_wire, seal.participants, replay_records,
          stored)) {
        try {
            _relocations->remove (stored.reference);
        }
        catch (...) {
        }
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::blocked,
          relocation_reason_t::restore_failed,
          {}};
    }
    aggregate_publish_result_t prepared;
    try {
        prepared = _aggregate_authority->prepare (
          sources, target_node_id, std::move (target_owner),
          std::move (relocation_capacity_fences),
          stored.reference, checksum,
          inventory_digest);
    }
    catch (...) {
        if (canonical_wire)
            abort_replay_source (*canonical_wire, replay_records);
        try {
            _relocations->remove (stored.reference);
        }
        catch (...) {
        }
        (void) _objects.abort_relocation (seal.token);
        return {
          relocation_terminal_t::store_failed,
          relocation_reason_t::authority_publish_failed,
          {}};
    }
    if (prepared.status != aggregate_publish_status_t::prepared
        || prepared.fence.value == 0) {
        if (canonical_wire)
            abort_replay_source (*canonical_wire, replay_records);
        try {
            _relocations->remove (stored.reference);
        }
        catch (...) {
        }
        (void) _objects.abort_relocation (seal.token);
        return {
          prepared.status == aggregate_publish_status_t::conflict
            ? relocation_terminal_t::conflict
            : relocation_terminal_t::store_failed,
          prepared.status == aggregate_publish_status_t::conflict
            ? relocation_reason_t::authority_conflict
            : relocation_reason_t::authority_publish_failed,
          prepared.current};
    }

    aggregate_publish_result_t committed;
    try {
        committed = _aggregate_authority->commit (prepared.fence);
    }
    catch (...) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::authority_publish_failed,
          prepared.current,
          replay_records};
    }
    if (committed.status != aggregate_publish_status_t::committed
        || committed.current.size () != sources.size ()) {
        if (committed.status == aggregate_publish_status_t::conflict) {
            if (canonical_wire)
                abort_replay_source (*canonical_wire, replay_records);
            try {
                _aggregate_authority->abort (prepared.fence);
            }
            catch (...) {
            }
            try {
                _relocations->remove (stored.reference);
            }
            catch (...) {
            }
            (void) _objects.abort_relocation (seal.token);
            return {
              relocation_terminal_t::conflict,
              relocation_reason_t::authority_conflict,
              committed.current};
        }
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::authority_publish_failed,
          committed.current,
          replay_records};
    }

    if (canonical_wire
        && !arm_replay_source (*canonical_wire, replay_records)) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          committed.current,
          replay_records};
    }
    const auto [commit_error, local] =
      _objects.commit_relocation_aggregate (
        seal.token, std::move (target_node_id));
    if (commit_error != stateful_error_t::none
        || local.size () != committed.current.size ()) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          committed.current,
          replay_records};
    }
    for (const auto &current : committed.current) {
        const auto match = std::find (
          local.begin (), local.end (), current.target);
        if (match == local.end ()) {
            return {
              relocation_terminal_t::recovery_required,
              relocation_reason_t::restore_failed,
              committed.current,
              replay_records};
        }
    }
    if (canonical_wire
        && (!canonical_wire->complete_target
            || !canonical_wire->complete_target ())) {
        return {
          relocation_terminal_t::recovery_required,
          relocation_reason_t::restore_failed,
          committed.current,
          replay_records};
    }
    return {
      relocation_terminal_t::completed,
      relocation_reason_t::none,
      committed.current,
      replay_records};
}

relocation_gate_snapshot_t maintenance_runtime_t::gate_snapshot () const
{
    std::lock_guard lock (_gate_mutex);
    return _gate;
}

std::uint32_t maintenance_runtime_t::crc32c (
  const std::vector<std::uint8_t> &payload) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : payload) {
        crc ^= byte;
        for (int bit = 0; bit != 8; ++bit)
            crc = (crc >> 1)
                  ^ (0x82f63b78u
                     & static_cast<std::uint32_t> (
                       -static_cast<std::int32_t> (crc & 1u)));
    }
    return ~crc;
}

std::vector<std::uint8_t> maintenance_runtime_t::encode (
  const frozen_object_state_t &frozen,
  const inventory_digest_t &inventory_digest)
{
    if (frozen.owner.key.empty ()
        || frozen.owner.mesh_name.empty ()
        || frozen.owner.node_id.empty ()
        || frozen.stable_type.empty ()
        || frozen.application_state.size () > max_application_state_bytes
        || frozen.pending_application.size () > max_pending_records
        || frozen.timers.size () > max_logical_timers) {
        return {};
    }
    std::vector<std::uint8_t> output (
      envelope_magic_v2.begin (), envelope_magic_v2.end ());
    output.push_back (static_cast<std::uint8_t> (frozen.owner.kind));
    if (!append_text (output, frozen.owner.key)
        || !append_text (output, frozen.stable_type)
        || !append_text (output, frozen.owner.mesh_name)
        || !append_text (output, frozen.owner.node_id)) {
        return {};
    }
    append_u64 (output, frozen.owner.object_generation);
    append_u64 (output, frozen.owner.authority_owner_generation);
    if (!append_bytes (output, frozen.application_state))
        return {};
    append_u32 (
      output,
      static_cast<std::uint32_t> (frozen.pending_application.size ()));
    std::uint64_t previous_sequence = 0;
    for (const auto &record : frozen.pending_application) {
        if (record.sequence == 0
            || record.sequence <= previous_sequence)
            return {};
        previous_sequence = record.sequence;
        append_u64 (output, record.sequence);
        if (!append_bytes (output, record.payload))
            return {};
    }
    append_u32 (
      output, static_cast<std::uint32_t> (frozen.timers.size ()));
    std::uint64_t previous_timer = 0;
    for (const auto &timer : frozen.timers) {
        if (timer.timer_id == 0 || timer.timer_id <= previous_timer
            || timer.due_after_milliseconds == 0
            || timer.next_tick_sequence == 0) {
            return {};
        }
        previous_timer = timer.timer_id;
        append_u64 (output, timer.timer_id);
        append_u64 (output, timer.due_after_milliseconds);
        append_u64 (output, timer.period_milliseconds);
        append_u64 (output, timer.next_tick_sequence);
    }
    output.insert (
      output.end (), inventory_digest.begin (), inventory_digest.end ());
    return output.size () <= max_envelope_bytes
             ? std::move (output)
             : std::vector<std::uint8_t>{};
}

std::optional<std::pair<frozen_object_state_t, inventory_digest_t>>
maintenance_runtime_t::decode (
  const std::vector<std::uint8_t> &payload) noexcept
try
{
    if (payload.size () >= recoverable_envelope_magic.size ()
        && std::equal (
          recoverable_envelope_magic.begin (),
          recoverable_envelope_magic.end (), payload.begin ())) {
        const auto recoverable =
          decode_recoverable_payload (payload);
        return recoverable
                 ? decode (recoverable->state)
                 : std::nullopt;
    }
    const auto legacy_v1 =
      payload.size () >= envelope_magic_v1.size ()
      && std::equal (
        envelope_magic_v1.begin (), envelope_magic_v1.end (),
        payload.begin ());
    const auto current_v2 =
      payload.size () >= envelope_magic_v2.size ()
      && std::equal (
        envelope_magic_v2.begin (), envelope_magic_v2.end (),
        payload.begin ());
    if (payload.size () > max_envelope_bytes
        || payload.size () < envelope_magic_v2.size ()
                           + 1 + inventory_digest_t{}.size ()
        || (!legacy_v1 && !current_v2)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> encoded (
      payload.begin ()
        + static_cast<std::ptrdiff_t> (envelope_magic_v2.size ()),
      payload.end ());
    reader_t reader (encoded);
    const auto kind = reader.u8 ();
    const auto key = reader.text ();
    const auto stable_type = reader.text ();
    const auto mesh_name = reader.text ();
    const auto node_id = reader.text ();
    const auto object_generation = reader.u64 ();
    const auto owner_generation = reader.u64 ();
    std::optional<std::vector<std::uint8_t>> application_state =
      legacy_v1
        ? std::optional<std::vector<std::uint8_t>>{
            std::vector<std::uint8_t>{}}
        : reader.bytes ();
    const auto pending_count = reader.u32 ();
    if (!kind || *kind > static_cast<std::uint8_t> (object_kind_t::instance_spot)
        || !key || key->empty () || !stable_type || stable_type->empty ()
        || !mesh_name || mesh_name->empty () || !node_id || node_id->empty ()
        || !object_generation || *object_generation == 0
        || !owner_generation || *owner_generation == 0
        || !application_state
        || application_state->size () > max_application_state_bytes
        || !pending_count
        || *pending_count > max_pending_records
        || reader.remaining ()
             < static_cast<std::size_t> (*pending_count) * 12u
                 + 4u + inventory_digest_t{}.size ()) {
        return std::nullopt;
    }

    frozen_object_state_t frozen{
      .owner =
        object_ref_t{
          .kind = static_cast<object_kind_t> (*kind),
          .key = *key,
          .object_generation = *object_generation,
          .authority_owner_generation = *owner_generation,
          .mesh_name = *mesh_name,
          .node_id = *node_id},
      .stable_type = *stable_type,
      .application_state = std::move (*application_state),
      .pending_application = {},
      .timers = {}};
    frozen.pending_application.reserve (*pending_count);
    std::uint64_t previous_sequence = 0;
    for (std::uint32_t index = 0; index != *pending_count; ++index) {
        const auto sequence = reader.u64 ();
        auto bytes = reader.bytes ();
        if (!sequence || *sequence == 0
            || *sequence <= previous_sequence || !bytes)
            return std::nullopt;
        previous_sequence = *sequence;
        frozen.pending_application.push_back (
          turn_record_t{*sequence, std::move (*bytes)});
    }
    const auto timer_count = reader.u32 ();
    if (!timer_count || *timer_count > max_logical_timers
        || reader.remaining ()
             < static_cast<std::size_t> (*timer_count) * 32u
                 + inventory_digest_t{}.size ())
        return std::nullopt;
    frozen.timers.reserve (*timer_count);
    std::uint64_t previous_timer = 0;
    for (std::uint32_t index = 0; index != *timer_count; ++index) {
        const auto timer_id = reader.u64 ();
        const auto due = reader.u64 ();
        const auto period = reader.u64 ();
        const auto next = reader.u64 ();
        if (!timer_id || *timer_id == 0
            || *timer_id <= previous_timer || !due || *due == 0
            || !period || !next || *next == 0) {
            return std::nullopt;
        }
        previous_timer = *timer_id;
        frozen.timers.push_back (
          logical_timer_t{*timer_id, *due, *period, *next});
    }
    inventory_digest_t digest{};
    for (auto &byte : digest) {
        const auto value = reader.u8 ();
        if (!value)
            return std::nullopt;
        byte = *value;
    }
    if (!reader.done ())
        return std::nullopt;
    return std::make_pair (std::move (frozen), digest);
}
catch (...)
{
    return std::nullopt;
}

std::vector<std::uint8_t> maintenance_runtime_t::encode_aggregate (
  const std::vector<frozen_object_state_t> &participants,
  const inventory_digest_t &inventory_digest)
{
    if (participants.size () < 2
        || participants.size () > std::numeric_limits<std::uint32_t>::max ())
        return {};
    std::vector<std::uint8_t> output (
      aggregate_envelope_magic.begin (),
      aggregate_envelope_magic.end ());
    append_u32 (
      output, static_cast<std::uint32_t> (participants.size ()));
    for (const auto &participant : participants) {
        const auto encoded = encode (participant, inventory_digest);
        if (encoded.empty () || !append_bytes (output, encoded))
            return {};
    }
    output.insert (
      output.end (), inventory_digest.begin (), inventory_digest.end ());
    return output;
}

std::optional<
  std::pair<std::vector<frozen_object_state_t>, inventory_digest_t>>
maintenance_runtime_t::decode_aggregate (
  const std::vector<std::uint8_t> &payload) noexcept
try
{
    if (payload.size () >= recoverable_envelope_magic.size ()
        && std::equal (
          recoverable_envelope_magic.begin (),
          recoverable_envelope_magic.end (), payload.begin ())) {
        const auto recoverable =
          decode_recoverable_payload (payload);
        return recoverable
                 ? decode_aggregate (recoverable->state)
                 : std::nullopt;
    }
    if (payload.size () > max_envelope_bytes
        || payload.size () < aggregate_envelope_magic.size () + 4
                           + inventory_digest_t{}.size ()
        || !std::equal (
          aggregate_envelope_magic.begin (),
          aggregate_envelope_magic.end (), payload.begin ())) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> encoded (
      payload.begin ()
        + static_cast<std::ptrdiff_t> (aggregate_envelope_magic.size ()),
      payload.end ());
    reader_t reader (encoded);
    const auto participant_count = reader.u32 ();
    if (!participant_count || *participant_count < 2
        || reader.remaining ()
             < static_cast<std::size_t> (*participant_count) * 4u
                 + inventory_digest_t{}.size ()) {
        return std::nullopt;
    }

    std::vector<frozen_object_state_t> participants;
    participants.reserve (*participant_count);
    std::optional<inventory_digest_t> participant_digest;
    for (std::uint32_t index = 0; index != *participant_count; ++index) {
        const auto participant_payload = reader.bytes ();
        if (!participant_payload)
            return std::nullopt;
        auto participant = decode (*participant_payload);
        if (!participant)
            return std::nullopt;
        if (participant_digest
            && *participant_digest != participant->second) {
            return std::nullopt;
        }
        participant_digest = participant->second;
        participants.push_back (std::move (participant->first));
    }
    inventory_digest_t root_digest{};
    for (auto &byte : root_digest) {
        const auto value = reader.u8 ();
        if (!value)
            return std::nullopt;
        byte = *value;
    }
    if (!reader.done () || !participant_digest
        || *participant_digest != root_digest) {
        return std::nullopt;
    }

    std::sort (
      participants.begin (), participants.end (),
      [] (const frozen_object_state_t &left,
          const frozen_object_state_t &right) {
          if (left.owner.kind != right.owner.kind)
              return left.owner.kind < right.owner.kind;
          return left.owner.key < right.owner.key;
      });
    for (std::size_t index = 1; index != participants.size (); ++index) {
        if (participants[index - 1].owner.kind
              == participants[index].owner.kind
            && participants[index - 1].owner.key
                 == participants[index].owner.key) {
            return std::nullopt;
        }
    }
    return std::make_pair (
      std::move (participants), root_digest);
}
catch (...)
{
    return std::nullopt;
}

maintenance_runtime_t::permit_t::permit_t (
  maintenance_runtime_t *owner,
  std::size_t payload) :
    _owner (owner), _payload (payload)
{
}

maintenance_runtime_t::permit_t::~permit_t ()
{
    if (_owner)
        _owner->release (_payload);
}

maintenance_runtime_t::permit_t::permit_t (
  permit_t &&other) noexcept :
    _owner (std::exchange (other._owner, nullptr)),
    _payload (std::exchange (other._payload, 0))
{
}

maintenance_runtime_t::permit_t &
maintenance_runtime_t::permit_t::operator= (
  permit_t &&other) noexcept
{
    if (this == &other)
        return *this;
    if (_owner)
        _owner->release (_payload);
    _owner = std::exchange (other._owner, nullptr);
    _payload = std::exchange (other._payload, 0);
    return *this;
}

maintenance_runtime_t::permit_t::operator bool () const noexcept
{
    return _owner != nullptr;
}

maintenance_runtime_t::permit_t
maintenance_runtime_t::try_acquire (std::size_t payload)
{
    if (payload == 0)
        return {};
    std::lock_guard lock (_gate_mutex);
    const bool empty = _gate.outbound_units == 0
                       && _gate.inbound_units == 0
                       && _gate.capture_callbacks == 0
                       && _gate.restore_callbacks == 0
                       && _gate.payload_bytes == 0;
    const bool oversized = payload > _limits.payload_bytes;
    if (_gate.outbound_units >= _limits.outbound_units
        || _gate.inbound_units >= _limits.inbound_units
        || _gate.capture_callbacks >= _limits.capture_callbacks
        || _gate.restore_callbacks >= _limits.restore_callbacks
        || (oversized ? !empty
                      : payload > _limits.payload_bytes - _gate.payload_bytes)) {
        return {};
    }
    ++_gate.outbound_units;
    ++_gate.inbound_units;
    ++_gate.capture_callbacks;
    ++_gate.restore_callbacks;
    _gate.payload_bytes += payload;
    return permit_t (this, payload);
}

void maintenance_runtime_t::release (std::size_t payload) noexcept
{
    std::lock_guard lock (_gate_mutex);
    --_gate.outbound_units;
    --_gate.inbound_units;
    --_gate.capture_callbacks;
    --_gate.restore_callbacks;
    _gate.payload_bytes -= payload;
}

relocation_result_t maintenance_runtime_t::finish (
  relocation_result_t result)
{
    if (_observer) {
        try {
            _observer (result);
        }
        catch (...) {
        }
    }
    return result;
}

host_maintenance_runtime_t::host_maintenance_runtime_t (
  stateful_object_runtime_t &objects,
  stream_session_registry_t &sessions,
  maintenance_runtime_t &relocation,
  std::shared_ptr<target_preflight_port_t> targets,
  observer_t observer) :
    _objects (objects),
    _sessions (sessions),
    _relocation (relocation),
    _targets (std::move (targets)),
    _observer (std::move (observer))
{
    if (!_targets)
        throw std::invalid_argument ("target preflight provider is missing");
}

void host_maintenance_runtime_t::mark_serving ()
{
    std::lock_guard lock (_mutex);
    if (_state != host_runtime_state_t::preparing)
        throw std::logic_error ("host can become serving only from preparing");
    _state = host_runtime_state_t::serving;
}

void host_maintenance_runtime_t::mark_error ()
{
    std::lock_guard lock (_mutex);
    if (_state != host_runtime_state_t::stopped)
        _state = host_runtime_state_t::error;
}

host_runtime_state_t host_maintenance_runtime_t::state () const
{
    std::lock_guard lock (_mutex);
    return _state;
}

std::optional<termination_result_t>
host_maintenance_runtime_t::terminal_result () const
{
    std::lock_guard lock (_mutex);
    return _terminal;
}

std::optional<termination_intent_t>
host_maintenance_runtime_t::intent_snapshot () const
{
    std::lock_guard lock (_mutex);
    if (_shutdown_claimed)
        return termination_intent_t::shutdown;
    return _effective_intent;
}

termination_result_t host_maintenance_runtime_t::terminate (
  termination_intent_t intent)
{
    std::uint64_t attempt = 0;
    {
        std::unique_lock lock (_mutex);
        if (_terminal)
            return *_terminal;
        if (_state == host_runtime_state_t::stopped) {
            return {
              intent, termination_outcome_t::stopped,
              termination_reason_t::none};
        }
        if (_active) {
            attempt = _active_attempt;
            if (intent == termination_intent_t::shutdown
                && !_effective_intent) {
                _shutdown_claimed = true;
            }
            _changed.wait (
              lock,
              [&] { return _attempt_results.contains (attempt); });
            return _attempt_results.at (attempt);
        }
        if (intent == termination_intent_t::retire
            && _state != host_runtime_state_t::serving) {
            return {
              intent, termination_outcome_t::blocked,
              termination_reason_t::runtime_not_ready};
        }
        _active = true;
        _shutdown_claimed = false;
        _effective_intent.reset ();
        attempt = _next_attempt++;
        _active_attempt = attempt;
        if (intent == termination_intent_t::shutdown)
            _effective_intent = termination_intent_t::shutdown;
    }

    const auto result =
      intent == termination_intent_t::retire
        ? run_retire ()
        : run_shutdown (termination_intent_t::shutdown);
    complete_attempt (attempt, result);
    return result;
}

std::vector<relocation_unit_t>
host_maintenance_runtime_t::inventory_units (
  std::vector<object_inventory_t> inventory)
{
    std::sort (
      inventory.begin (), inventory.end (),
      [] (const object_inventory_t &left,
          const object_inventory_t &right) {
          if (left.owner.kind != right.owner.kind)
              return left.owner.kind < right.owner.kind;
          return left.owner.key < right.owner.key;
      });
    std::vector<relocation_unit_t> units;
    std::map<std::string, std::size_t> user_spots;
    for (const auto &object : inventory) {
        if (object.owner.kind != object_kind_t::user_spot)
            continue;
        user_spots.emplace (object.owner.key, units.size ());
        units.push_back ({{object.owner}});
    }
    for (const auto &object : inventory) {
        if (object.owner.kind == object_kind_t::actor) {
            const auto spot = user_spots.find (object.membership);
            if (spot != user_spots.end ())
                units[spot->second].participants.push_back (object.owner);
            else
                units.push_back ({{object.owner}});
        } else if (object.owner.kind == object_kind_t::instance_spot) {
            units.push_back ({{object.owner}});
        }
    }
    for (auto &unit : units) {
        std::sort (
          unit.participants.begin (), unit.participants.end (),
          [] (const object_ref_t &left, const object_ref_t &right) {
              if (left.kind != right.kind)
                  return left.kind < right.kind;
              return left.key < right.key;
          });
    }
    return units;
}

termination_result_t host_maintenance_runtime_t::run_retire ()
{
    auto inventory = _objects.try_begin_maintenance_inventory ();
    if (!inventory) {
        return {
          termination_intent_t::retire,
          termination_outcome_t::blocked,
          termination_reason_t::runtime_not_ready};
    }
    {
        std::lock_guard lock (_mutex);
        _inventory_sealed = true;
    }
    for (const auto &object : *inventory) {
        if (object.state != object_state_t::ready) {
            _objects.end_maintenance_inventory ();
            {
                std::lock_guard lock (_mutex);
                _inventory_sealed = false;
            }
            return {
              termination_intent_t::retire,
              termination_outcome_t::blocked,
              termination_reason_t::state_incompatible};
        }
    }
    const auto units = inventory_units (std::move (*inventory));
    target_preflight_result_t preflight;
    try {
        preflight = _targets->preflight (units);
    }
    catch (...) {
        preflight.status = target_preflight_status_t::store_unavailable;
    }

    bool exact_preflight = preflight.units.size () == units.size ();
    if (exact_preflight) {
        for (std::size_t index = 0; index != units.size (); ++index) {
            if (preflight.units[index].unit != units[index]) {
                exact_preflight = false;
                break;
            }
        }
    }
    {
        std::lock_guard lock (_mutex);
        if (_shutdown_claimed) {
            _effective_intent = termination_intent_t::shutdown;
            _state = host_runtime_state_t::draining;
        } else if (preflight.status
                   == target_preflight_status_t::eligible
                   && exact_preflight) {
            _effective_intent = termination_intent_t::retire;
            _state = host_runtime_state_t::retiring;
        } else {
            termination_reason_t reason =
              termination_reason_t::target_unavailable;
            switch (preflight.status) {
            case target_preflight_status_t::store_unavailable:
                reason = termination_reason_t::store_unavailable;
                break;
            case target_preflight_status_t::relocation_disabled:
                reason = termination_reason_t::relocation_disabled;
                break;
            case target_preflight_status_t::state_incompatible:
                reason = termination_reason_t::state_incompatible;
                break;
            case target_preflight_status_t::eligible:
                reason = termination_reason_t::state_incompatible;
                break;
            default:
                break;
            }
            _objects.end_maintenance_inventory ();
            _inventory_sealed = false;
            return {
              termination_intent_t::retire,
              termination_outcome_t::blocked, reason};
        }
    }
    if (_effective_intent == termination_intent_t::shutdown)
        return run_shutdown (termination_intent_t::shutdown);

    std::size_t committed_units = 0;
    const auto fail_relocation =
      [&] (termination_reason_t blocked_reason, bool irreversible = false) {
          if (committed_units == 0 && !irreversible) {
              return termination_result_t{
                termination_intent_t::retire,
                termination_outcome_t::blocked, blocked_reason};
          }
          {
              std::lock_guard lock (_mutex);
              _state = host_runtime_state_t::draining;
          }
          _sessions.force_close_all ();
          {
              std::lock_guard lock (_mutex);
              _state = host_runtime_state_t::stopped;
          }
          return termination_result_t{
            termination_intent_t::retire,
            termination_outcome_t::force_stopped,
            termination_reason_t::relocation_failed};
      };
    for (const auto &eligible : preflight.units) {
        if (eligible.unit.participants.empty ()
            || eligible.target_node_id.empty ()
            || eligible.relocation_capacity_fences.size ()
                 != eligible.unit.participants.size ()
            || eligible.encoded_upper_bound == 0) {
            return fail_relocation (
              termination_reason_t::state_incompatible);
        }

        std::vector<stream_barrier_t> barriers;
        bool barrier_ready = true;
        for (const auto &participant : eligible.unit.participants) {
            if (participant.kind != object_kind_t::actor)
                continue;
            auto [error, barrier] =
              _sessions.try_seal_actor (participant);
            if (error != stateful_error_t::none) {
                barrier_ready = false;
                break;
            }
            barriers.push_back (barrier);
        }
        if (!barrier_ready) {
            for (const auto &barrier : barriers)
                (void) _sessions.abort_barrier (barrier);
            return fail_relocation (
              termination_reason_t::state_incompatible);
        }

        std::vector<authority_relocation_reference_t> current;
        relocation_terminal_t terminal = relocation_terminal_t::blocked;
        if (eligible.unit.participants.size () == 1) {
            const auto result = _relocation.relocate (
              eligible.unit.participants.front (),
              eligible.target_node_id, eligible.target_owner,
              eligible.relocation_capacity_fences.front (),
              eligible.encoded_upper_bound,
              eligible.inventory_digest,
              eligible.canonical_wire);
            terminal = result.terminal;
            if (result.authority)
                current.push_back (*result.authority);
        } else {
            const auto result = _relocation.relocate_aggregate (
              eligible.unit.participants, eligible.target_node_id,
              eligible.target_owner,
              eligible.relocation_capacity_fences,
              eligible.encoded_upper_bound,
              eligible.inventory_digest,
              eligible.canonical_wire);
            terminal = result.terminal;
            current = result.authority;
        }
        if (terminal != relocation_terminal_t::completed) {
            for (const auto &barrier : barriers)
                (void) _sessions.abort_barrier (barrier);
            return fail_relocation (
              termination_reason_t::store_unavailable,
              terminal == relocation_terminal_t::recovery_required
                || terminal == relocation_terminal_t::data_lost);
        }
        for (const auto &barrier : barriers) {
            const auto target = std::find_if (
              current.begin (), current.end (),
              [&] (const authority_relocation_reference_t &reference) {
                  return reference.source == barrier.actor;
              });
            if (target == current.end ()
                || _sessions.commit_barrier (
                     barrier, target->target)
                     != stateful_error_t::none) {
                return fail_relocation (
                  termination_reason_t::relocation_failed, true);
            }
        }
        ++committed_units;
    }
    {
        std::lock_guard lock (_mutex);
        _state = host_runtime_state_t::draining;
    }
    if (!_sessions.try_seal_all ()) {
        _sessions.force_close_all ();
        {
            std::lock_guard lock (_mutex);
            _state = host_runtime_state_t::stopped;
        }
        return {
          termination_intent_t::retire,
          termination_outcome_t::force_stopped,
          termination_reason_t::relocation_failed};
    }
    {
        std::lock_guard lock (_mutex);
        _state = host_runtime_state_t::stopped;
    }
    return {
      termination_intent_t::retire,
      termination_outcome_t::stopped,
      termination_reason_t::none};
}

termination_result_t host_maintenance_runtime_t::run_shutdown (
  termination_intent_t effective_intent)
{
    bool acquire_inventory = false;
    {
        std::lock_guard lock (_mutex);
        _effective_intent = effective_intent;
        _state = host_runtime_state_t::draining;
        acquire_inventory = !_inventory_sealed;
    }
    if (acquire_inventory) {
        const auto inventory =
          _objects.try_begin_maintenance_inventory ();
        if (!inventory) {
            {
                std::lock_guard lock (_mutex);
                _state = host_runtime_state_t::stopped;
            }
            return {
              effective_intent,
              termination_outcome_t::force_stopped,
              termination_reason_t::teardown_failed};
        }
        std::lock_guard lock (_mutex);
        _inventory_sealed = true;
    }
    const auto sealed = _sessions.try_seal_all ();
    if (!sealed)
        _sessions.force_close_all ();
    {
        std::lock_guard lock (_mutex);
        _state = host_runtime_state_t::stopped;
    }
    return {
      effective_intent,
      sealed ? termination_outcome_t::stopped
             : termination_outcome_t::force_stopped,
      sealed ? termination_reason_t::none
             : termination_reason_t::teardown_failed};
}

void host_maintenance_runtime_t::complete_attempt (
  std::uint64_t attempt,
  const termination_result_t &result)
{
    bool release_inventory = false;
    {
        std::lock_guard lock (_mutex);
        _attempt_results[attempt] = result;
        _active = false;
        _active_attempt = 0;
        _shutdown_claimed = false;
        _effective_intent.reset ();
        if (result.outcome != termination_outcome_t::blocked)
            _terminal = result;
        else {
            release_inventory = _inventory_sealed;
            _inventory_sealed = false;
            if (_state == host_runtime_state_t::retiring)
                _state = host_runtime_state_t::serving;
        }
    }
    if (release_inventory)
        _objects.end_maintenance_inventory ();
    _changed.notify_all ();
    if (_observer) {
        try {
            _observer (result);
        }
        catch (...) {
        }
    }
}

} // namespace zlink::framework::runtime::stateful

namespace zlink::framework::runtime::host
{

void public_host_runtime_t::configure_relocation (
  std::shared_ptr<stateful::authority_relocation_port_t> authority,
  std::shared_ptr<stateful::relocation_store_port_t> relocations,
  std::shared_ptr<stateful::aggregate_authority_port_t>
    aggregate_authority,
  stateful::relocation_limits_t limits,
  stateful::maintenance_runtime_t::observer_t relocation_observer)
{
    if (!authority || !relocations)
        throw std::invalid_argument (
          "relocation providers must not be null");
    std::lock_guard lock (_mutex);
    if (_started || _maintenance || _termination) {
        throw std::logic_error (
          "relocation providers must be configured once before host start");
    }
    _relocation_authority = authority;
    _session_relocations = relocations;
    auto maintenance =
      std::make_unique<stateful::maintenance_runtime_t> (
        _objects, std::move (authority), std::move (relocations),
        limits, std::move (relocation_observer),
        std::move (aggregate_authority));
    maintenance->attach_relocation_wire (*_relocation_wire);
    _maintenance = std::move (maintenance);
}

void public_host_runtime_t::configure_maintenance (
  stateful::maintenance_provider_set_t providers,
  stateful::relocation_limits_t limits,
  stateful::maintenance_runtime_t::observer_t relocation_observer,
  stateful::host_maintenance_runtime_t::observer_t
    termination_observer)
{
    std::lock_guard lock (_mutex);
    if (_started || _maintenance || _termination) {
        throw std::logic_error (
          "maintenance providers must be configured once before host start");
    }
    auto targets = providers.targets;
    _relocation_authority = providers.authority;
    _session_relocations = providers.relocations;
    auto maintenance =
      std::make_unique<stateful::maintenance_runtime_t> (
      _objects, std::move (providers), limits,
      std::move (relocation_observer));
    maintenance->attach_relocation_wire (*_relocation_wire);
    auto termination =
      std::make_unique<stateful::host_maintenance_runtime_t> (
        _objects, _sessions, *maintenance, std::move (targets),
        std::move (termination_observer));
    _maintenance = std::move (maintenance);
    _termination = std::move (termination);
    _maintenance_started = [this] {
        _termination->mark_serving ();
    };
    _maintenance_closing = [this] {
        (void) _termination->terminate (
          stateful::termination_intent_t::shutdown);
    };
}

stateful::maintenance_runtime_t *
public_host_runtime_t::maintenance () noexcept
{
    std::lock_guard lock (_mutex);
    return _maintenance.get ();
}

stateful::host_maintenance_runtime_t *
public_host_runtime_t::termination () noexcept
{
    std::lock_guard lock (_mutex);
    return _termination.get ();
}

} // namespace zlink::framework::runtime::host
