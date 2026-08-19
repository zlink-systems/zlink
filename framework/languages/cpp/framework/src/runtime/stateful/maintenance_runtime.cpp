/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/timers/async_delay.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime::stateful
{
namespace
{

constexpr std::array<std::uint8_t, 4> session_journal_magic{
  'Z', 'L', 'S', 'J'};
constexpr std::chrono::hours relocation_retention{24};
constexpr std::size_t max_envelope_bytes = 256u * 1024u * 1024u;
constexpr std::size_t max_application_state_bytes = 64u * 1024u * 1024u;
constexpr std::uint32_t max_pending_records = 4096;
constexpr std::uint32_t max_logical_timers = 4096;

protocol::relocation_object_kind_t to_wire_object_kind (
  object_kind_t kind)
{
    switch (kind) {
        case object_kind_t::actor:
            return protocol::relocation_object_kind_t::actor;
        case object_kind_t::user_spot:
            return protocol::relocation_object_kind_t::user_spot;
        case object_kind_t::instance_spot:
            return protocol::relocation_object_kind_t::instance_spot;
    }
    throw std::invalid_argument ("invalid stateful object kind");
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
    std::size_t checkpoint () const noexcept { return _offset; }
    void rewind (std::size_t checkpoint) noexcept { _offset = checkpoint; }

  private:
    const std::vector<std::uint8_t> &_input;
    std::size_t _offset = 0;
};

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

struct maintenance_runtime_t::relocation_terminal_state_t
{
    object_ref_t source;
    std::string target_node_id;
    location_owner_token_t target_owner;
    std::size_t encoded_upper_bound = 0;
    inventory_digest_t inventory_digest{};
    eligible_relocation_unit_t::canonical_wire_context_t context;
    std::stop_token cancellation;
    std::shared_ptr<permit_t> permit;
    aggregate_relocation_seal_attempt_t seal_attempt;
    std::vector<std::uint8_t> payload;
    relocation_payload_manifest_t manifest;
    std::uint64_t effective_chunk_limit = 0;
    /* Target-advertised inbound chunk-size cap (bytes); 0 = not
     * advertised. */
    std::uint64_t advertised_receive_chunk_limit_bytes = 0;
    std::uint64_t budget_reserved = 0;
    std::chrono::steady_clock::time_point sealed_at{};
    /* SafeToShutdown pending-unit accounting: set once the unit is sealed
     * (begin_pending_relocation_unit), released explicitly at S4+window
     * close in retain_retransmission_copies, or implicitly by destruction
     * of this state on any earlier failure return. */
    std::shared_ptr<void> pending_unit_token;
    std::optional<target_only_cas_t> handoff;
    relocation_ingress_batch_t batch;
    relocation_reason_t failure = relocation_reason_t::restore_failed;
    std::optional<std::vector<protocol::relocation_data_t>> records;
    std::optional<relocation_result_t> result;
    std::optional<protocol::relocation_cutover_t> cutover_record;
    bool cutover_enqueued = false;
    /* S1 (cutover submit terminal), on the source clock. Used as the start
     * of the route_convergence window (25 §"zlink.relocation"), which ends
     * when the retransmission-window copies are released. */
    std::chrono::steady_clock::time_point cutover_terminal_at{};
};

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
        || _limits.restore_callbacks == 0) {
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

void maintenance_runtime_t::configure_route_convergence_metric (
  std::function<void (double)> metric) noexcept
{
    std::lock_guard lock (_shutdown_tracking->metric_mutex);
    _shutdown_tracking->route_convergence_metric = std::move (metric);
}

bool maintenance_runtime_t::relocation_units_settled () const noexcept
{
    return _shutdown_tracking->pending_units.load (std::memory_order_acquire)
        <= 0;
}

std::optional<std::vector<protocol::relocation_data_t>>
maintenance_runtime_t::build_boundary_records (
  const std::vector<frozen_object_state_t> &participants,
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const relocation_ingress_batch_t &batch,
  relocation_reason_t &failure_reason) const
{
    failure_reason = relocation_reason_t::restore_failed;
    if ((context.relocation.high == 0 && context.relocation.low == 0)
        || context.target_attempt_generation == 0
        || context.coordinator.owner_id.empty ()
        || context.coordinator.lease_generation == 0
        || context.coordinator.node_routing_id.empty ()
        || context.coordinator.node_generation == 0
        || context.coordinator.expected_authority_store_version.empty ()
        || context.target_node_routing_id.empty ()
        || context.target_node_generation == 0
        || !context.prepare_target || !context.send_state_chunk
        || !context.send_relocation_data
        || !context.send_cutover || !context.abort_target_before_cutover)
        return std::nullopt;

    std::vector<protocol::relocation_data_t> records;
    try {
        for (const auto &batch_participant : batch.participants) {
            const auto frozen = std::find_if (
              participants.begin (), participants.end (),
              [&] (const frozen_object_state_t &candidate) {
                  return candidate.owner == batch_participant.owner;
              });
            if (frozen == participants.end ())
                return std::nullopt;
            const protocol::relocation_object_t object{
              to_wire_object_kind (frozen->owner.kind),
              frozen->stable_type, frozen->owner.key,
              frozen->owner.object_generation,
              frozen->owner.authority_owner_generation};
            for (const auto &turn : batch_participant.records) {
                if (!turn.application_record)
                    return std::nullopt;
                const auto &input = *turn.application_record;
                protocol::frozen_record_t record{
                  .kind = input.kind,
                  .source_kind = input.source_kind,
                  .source = input.source,
                  .source_spot_id = input.source_spot_id,
                  .source_actor = input.source_actor,
                  .source_session_routing_id = input.source_session_routing_id,
                  .source_binding_generation = input.source_binding_generation,
                  .source_session_sequence = input.source_session_sequence,
                  .has_metadata = !input.metadata.empty (),
                  .operation = input.operation,
                  .operation_kind = input.operation_kind,
                  .reply_route_id = input.reply_route_id};
                if (const auto *spot = std::get_if<
                      protocol::frozen_spot_application_body_t> (&input.body)) {
                    record.target = protocol::frozen_target_identity_t{
                      to_wire_object_kind (object_kind_t::user_spot),
                      spot->target.spot_id, spot->target.object_generation,
                      spot->target.target_node_routing_id,
                      spot->target.target_node_generation,
                      spot->target.authority_owner_generation,
                      spot->expected_owner_lease_generation};
                    record.application = spot->application;
                } else {
                    const auto &actor = std::get<
                      protocol::frozen_actor_application_body_t> (input.body);
                    record.target = protocol::frozen_target_identity_t{
                      to_wire_object_kind (object_kind_t::actor),
                      actor.target.actor_id, actor.target.object_generation,
                      actor.target.target_node_routing_id,
                      actor.target.target_node_generation,
                      actor.target.authority_owner_generation,
                      actor.target.owner_lease_generation};
                    record.application = actor.application;
                }
                records.push_back ({context.relocation,
                  context.target_attempt_generation, context.coordinator,
                  protocol::relocation_role_t::source, object,
                  std::move (record)});
            }
        }
    }
    catch (...) {
        return std::nullopt;
    }
    return records;
}

task_t<relocation_reason_t> maintenance_runtime_t::prepare_target (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<frozen_object_state_t> &participants,
  const relocation_payload_manifest_t &manifest)
{
    auto reason = relocation_reason_t::restore_failed;
    try {
        reason = co_await context.prepare_target (
          participants, manifest, context.session_routes);
    }
    catch (...) {
        reason = relocation_reason_t::restore_failed;
    }
    co_return reason;
}

std::uint64_t maintenance_runtime_t::effective_in_flight_budget () const noexcept
{
    /* Phase 1: min of the configured connection and node budgets (zero
     * disables a budget) and the fixed conservative constant, pending a
     * Core observation API for per-pipe effective HWM. */
    auto budget = relocation_conservative_in_flight_budget_bytes;
    if (_limits.in_flight_payload_budget_bytes != 0)
        budget = std::min (budget, _limits.in_flight_payload_budget_bytes);
    if (_limits.node_in_flight_payload_budget_bytes != 0)
        budget = std::min (budget, _limits.node_in_flight_payload_budget_bytes);
    return budget;
}

task_t<void> maintenance_runtime_t::acquire_transfer_budget (
  std::uint64_t bytes)
{
    if (bytes == 0)
        co_return;
    const auto budget = effective_in_flight_budget ();
    for (;;) {
        std::shared_ptr<detail::task_completion_source_t<bool>> waiter;
        {
            std::lock_guard lock (_budget_mutex);
            if (_budget_in_flight_bytes == 0
                || _budget_in_flight_bytes + bytes <= budget) {
                _budget_in_flight_bytes += bytes;
                co_return;
            }
            waiter =
              std::make_shared<detail::task_completion_source_t<bool>> ();
            _budget_waiters.emplace_back (bytes, waiter);
        }
        (void) co_await waiter->task ();
    }
}

void maintenance_runtime_t::release_transfer_budget (
  std::uint64_t bytes) noexcept
{
    std::vector<std::shared_ptr<detail::task_completion_source_t<bool>>>
      released;
    {
        std::lock_guard lock (_budget_mutex);
        _budget_in_flight_bytes =
          _budget_in_flight_bytes > bytes ? _budget_in_flight_bytes - bytes
                                          : 0;
        while (!_budget_waiters.empty ()) {
            released.push_back (std::move (_budget_waiters.front ().second));
            _budget_waiters.pop_front ();
        }
    }
    for (auto &waiter : released) {
        try {
            waiter->complete (result_t<bool>::success (true));
        }
        catch (...) {
        }
    }
}

task_t<bool> maintenance_runtime_t::relocate_send_state_chunks (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    if (!state->context.send_state_chunk)
        co_return false;
    const auto &participants = state->seal_attempt.seal.participants;
    const auto found = std::find_if (
      participants.begin (), participants.end (),
      [] (const frozen_object_state_t &candidate) {
          return candidate.owner.kind == object_kind_t::user_spot;
      });
    const auto &principal =
      found != participants.end () ? *found : participants.front ();
    const protocol::relocation_object_t object{
      to_wire_object_kind (principal.owner.kind), principal.stable_type,
      principal.owner.key, principal.owner.object_generation,
      principal.owner.authority_owner_generation};
    for (std::uint32_t ordinal = 0; ordinal != state->manifest.chunk_count;
         ++ordinal) {
        auto chunk = make_relocation_state_chunk (
          state->context.relocation, state->context.target_attempt_generation,
          state->context.coordinator, object, state->payload, ordinal,
          state->effective_chunk_limit);
        bool sent = false;
        try {
            sent = co_await state->context.send_state_chunk (chunk);
        }
        catch (...) {
            sent = false;
        }
        if (!sent)
            co_return false;
    }
    co_return true;
}

task_t<bool> maintenance_runtime_t::send_boundary_records (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<protocol::relocation_data_t> &records,
  const relocation_ingress_batch_t &batch)
{
    try {
        co_return co_await context.send_relocation_data (records, batch);
    }
    catch (...) {
        co_return false;
    }
}

bool maintenance_runtime_t::abort_target_before_cutover (
  const eligible_relocation_unit_t::canonical_wire_context_t &context) noexcept
{
    try {
        return context.abort_target_before_cutover ();
    }
    catch (...) {
        return false;
    }
}

namespace
{
/* CRC-32C over the concatenated canonical wire bytes of the pre-boundary
 * relay batch, in send order. The target accumulates the same value over
 * the relocationData records it stages and compares it at cutover. */
std::uint32_t boundary_batch_checksum (
  const std::vector<protocol::relocation_data_t> &records)
{
    relocation_crc32c_accumulator_t accumulator;
    for (const auto &record : records) {
        const auto bytes = protocol::encode_relocation_control (record);
        accumulator.update (bytes);
    }
    return accumulator.value ();
}
} // namespace

std::shared_ptr<void> maintenance_runtime_t::begin_pending_relocation_unit ()
  noexcept
{
    auto tracking = _shutdown_tracking;
    tracking->pending_units.fetch_add (1, std::memory_order_acq_rel);
    /* The deleter (not `this`) owns the decrement, so it runs correctly
     * even if this maintenance_runtime_t is torn down while the token is
     * still held (retransmission window outliving the runtime). */
    return std::shared_ptr<void> (
      static_cast<void *> (nullptr),
      [tracking] (void *) {
          tracking->pending_units.fetch_sub (1, std::memory_order_acq_rel);
      });
}

void maintenance_runtime_t::retain_retransmission_copies (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    /* The payload and boundary batch copies survive the submit terminal for
     * one retransmission window (the cutover wait timeout), then are
     * released exactly once. These copies are Framework memory and are not
     * charged to the in-flight budget. When the cutover submit did not
     * reach the wire, the window is also used to retry the one-way cutover
     * on the (possibly re-established) connection. */
    const auto window = _limits.cutover_wait_timeout;
    /* 18 §2.4 / 28 §-: S4 is the point the Message Follow route becomes
     * removable, i.e. MessageFollowDuration after this source's cutover
     * terminal — not the (much shorter) retransmission window. The
     * retransmission window still bounds the payload/records retention and
     * the cutover retry attempts below; the S4 wait is layered on top of
     * it, on the same clock (cutover_terminal_at), so it always dominates
     * with the default settings (30s follow duration vs. 1s cutover wait)
     * while still degrading correctly if a deployment configures them the
     * other way around. */
    const auto follow_duration = _limits.message_follow_duration;
    /* Captured by value (not `this`): this coroutine is detached and
     * self-keeping, so it can outlive the maintenance_runtime_t that
     * started it. */
    auto tracking = _shutdown_tracking;
    auto retention = std::make_shared<task_t<void>> (
      [] (std::shared_ptr<relocation_terminal_state_t> retained,
          std::chrono::milliseconds duration,
          std::chrono::milliseconds follow) -> task_t<void> {
          const auto deadline = std::chrono::steady_clock::now () + duration;
          constexpr auto retry_interval = std::chrono::milliseconds (100);
          while (std::chrono::steady_clock::now () < deadline) {
              const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds> (
                  deadline - std::chrono::steady_clock::now ());
              if (retained->cutover_enqueued || !retained->cutover_record) {
                  co_await ::zlink::framework::detail::delay (
                    std::max (remaining, std::chrono::milliseconds (1)));
                  break;
              }
              co_await ::zlink::framework::detail::delay (
                std::min (retry_interval,
                          std::max (remaining, std::chrono::milliseconds (1))));
              try {
                  /* 28 §4.4/§9: the cutover submit reaching the wire does
                   * not by itself prove the target still has every
                   * boundary record staged — a reconnect on either side
                   * can have dropped mid-batch state. Resend the whole
                   * retained boundary batch ahead of each cutover retry
                   * (not just the cutover itself), on the same connection,
                   * so a target that lost partial staging gets the full
                   * batch again before the cutover comparison runs. */
                  if (retained->records
                      && !co_await retained->context.send_relocation_data (
                           *retained->records, retained->batch))
                      continue;
                  const auto retried = co_await retained->context.send_cutover (
                    *retained->cutover_record);
                  if (retried
                      == eligible_relocation_unit_t::canonical_wire_context_t::
                        cutover_enqueue_t::enqueued)
                      retained->cutover_enqueued = true;
              }
              catch (...) {
              }
          }
          retained->payload.clear ();
          retained->payload.shrink_to_fit ();
          retained->records.reset ();
          retained->cutover_record.reset ();
          /* S4 (Message Follow route removable): wait out whatever remains
           * of the follow duration measured from the cutover terminal, on
           * top of the retransmission window handled above. A unit that
           * never reached a cutover terminal (never sealed / no handoff)
           * has nothing to converge, so it skips this wait. */
          if (retained->cutover_terminal_at
              != std::chrono::steady_clock::time_point{}) {
              const auto follow_deadline =
                retained->cutover_terminal_at + follow;
              const auto now = std::chrono::steady_clock::now ();
              if (follow_deadline > now) {
                  co_await ::zlink::framework::detail::delay (
                    std::chrono::duration_cast<std::chrono::milliseconds> (
                      follow_deadline - now));
              }
          }
      } (state, window, follow_duration));
    /* route_convergence (25 §"zlink.relocation") measures S1 (cutover
     * submit terminal) to S4 (follow duration elapsed) specifically. It is
     * timed by its own wait, independent of the SafeToShutdown release
     * below: the coroutine above waits out max(retransmission window,
     * follow duration) serially, so when the window is configured longer
     * than the follow duration (an unusual, but legal, deployment), taking
     * the metric timestamp from that coroutine's completion would inflate
     * it by the extra window wait past S4. This wait only ever needs to
     * cover the follow duration itself. */
    if (state->cutover_terminal_at != std::chrono::steady_clock::time_point{}) {
        auto metric_wait = std::make_shared<task_t<void>> (
          [] (std::shared_ptr<relocation_terminal_state_t> retained,
              std::chrono::milliseconds follow) -> task_t<void> {
              const auto deadline = retained->cutover_terminal_at + follow;
              const auto now = std::chrono::steady_clock::now ();
              if (deadline > now) {
                  co_await ::zlink::framework::detail::delay (
                    std::chrono::duration_cast<std::chrono::milliseconds> (
                      deadline - now));
              }
          } (state, follow_duration));
        detail::observe_task_completion (
          *metric_wait, [metric_wait, tracking, state] (const result_t<void> &) {
              std::lock_guard lock (tracking->metric_mutex);
              if (tracking->route_convergence_metric) {
                  const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now ()
                    - state->cutover_terminal_at);
                  tracking->route_convergence_metric (elapsed.count ());
              }
          });
    }
    detail::observe_task_completion (
      *retention, [retention, state] (const result_t<void> &) {
          /* Fires on any completion (normal or exceptional): this is the
           * SafeToShutdown obligation, which is "S4 reached AND the
           * retransmission window is closed" — satisfied by the time this
           * observer runs, since the coroutine above waits out the window
           * first and then the remainder of the follow duration. Releases
           * the pending-unit token acquired at seal time
           * (begin_pending_relocation_unit). */
          state->pending_unit_token.reset ();
      });
}

task_t<relocation_result_t> maintenance_runtime_t::relocate (
  const object_ref_t &source,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
    &canonical_wire,
  std::stop_token cancellation,
  std::uint64_t advertised_receive_chunk_limit_bytes)
{
    if (!canonical_wire) {
        return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
          finish ({relocation_terminal_t::blocked,
                   relocation_reason_t::restore_failed,
                   std::nullopt})));
    }
    auto state = std::make_shared<relocation_terminal_state_t> (
      relocation_terminal_state_t{
        source, std::move (target_node_id), std::move (target_owner),
        encoded_upper_bound, inventory_digest, *canonical_wire, cancellation,
        std::make_shared<permit_t> (try_acquire ())});
    state->advertised_receive_chunk_limit_bytes =
      advertised_receive_chunk_limit_bytes;
    if (!*state->permit) {
        return task_t<relocation_result_t> (result_t<relocation_result_t>::success (
          finish ({relocation_terminal_t::blocked,
                   relocation_reason_t::permit_unavailable,
                   std::nullopt})));
    }
    return relocate_terminal (std::move (state));
}

task_t<relocation_result_t> maintenance_runtime_t::relocate_terminal (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    /* The in-flight payload budget is acquired before the source admission
     * seal so a unit waiting on the budget keeps serving messages. Chunk
     * sends are awaited one at a time, so one reservation of the effective
     * chunk size bounds this unit's in-flight bytes. */
    state->effective_chunk_limit =
      std::min<std::uint64_t> (_limits.payload_chunk_limit_bytes
                                 ? _limits.payload_chunk_limit_bytes
                                 : protocol::relocationChunkBytes,
                               protocol::relocationChunkBytes);
    state->effective_chunk_limit = apply_advertised_receive_chunk_limit (
      state->effective_chunk_limit,
      state->advertised_receive_chunk_limit_bytes);
    state->budget_reserved =
      std::min (state->effective_chunk_limit, effective_in_flight_budget ());
    co_await acquire_transfer_budget (state->budget_reserved);
    const auto release_reservation = [this, state] {
        if (state->budget_reserved != 0) {
            release_transfer_budget (state->budget_reserved);
            state->budget_reserved = 0;
        }
    };
    if (!co_await relocate_seal (state)) {
        release_reservation ();
        co_return std::move (*state->result);
    }
    if (!relocate_encode (state)) {
        release_reservation ();
        co_return std::move (*state->result);
    }
    /* Start the Restore request first: the eager task enqueues the request
     * frame on the ordered connection before its first suspension, so the
     * relocationState chunks sent next arrive after it. The relay-ready
     * reply arrives only after the target assembled and restored every
     * chunk, so the reply is awaited after the chunk sends complete. */
    auto prepared = relocate_prepare_target (state);
    const auto chunks_sent = co_await relocate_send_state_chunks (state);
    release_reservation ();
    const auto target_ready = co_await prepared;
    if (!target_ready)
        co_return std::move (*state->result);
    if (!chunks_sent) {
        /* The target replied ready without every chunk — treat it as an
         * exact target failure before cutover. */
        if (abort_target_before_cutover (state->context))
            (void) _objects.abort_relocation_before_cutover (
              state->seal_attempt.seal.token);
        state->result.emplace (finish ({relocation_terminal_t::blocked,
                                        relocation_reason_t::restore_failed,
                                        std::nullopt}));
        co_return std::move (*state->result);
    }
    if (!co_await relocate_boundary_and_send (state))
        co_return std::move (*state->result);
    (void) co_await relocate_cutover (state);
    co_return std::move (*state->result);
}

task_t<bool> maintenance_runtime_t::capture_relocation_session_routes (
  eligible_relocation_unit_t::canonical_wire_context_t &context)
{
    if (!context.capture_session_routes)
        co_return true;
    const auto routes = co_await context.capture_session_routes ();
    if (!routes)
        co_return false;
    context.session_routes = *routes;
    co_return true;
}

task_t<bool> maintenance_runtime_t::relocate_seal (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    std::vector<object_ref_t> participants{state->source};
    std::function<task_t<bool> ()> before_capture = [this, state] {
        return capture_relocation_session_routes (state->context);
    };
    auto seal_task = _objects.try_seal_relocation_aggregate (
      participants, state->cancellation, before_capture);
    auto seal_attempt = co_await seal_task;
    state->seal_attempt = std::move (seal_attempt);
    if (state->seal_attempt.error != stateful_error_t::none
        || state->seal_attempt.seal.participants.size () != 1) {
        state->result.emplace (finish (
          {relocation_terminal_t::blocked,
           state->seal_attempt.error == stateful_error_t::backpressured
             ? relocation_reason_t::turn_active
             : relocation_reason_t::restore_failed,
           std::nullopt}));
        co_return false;
    }
    state->sealed_at = std::chrono::steady_clock::now ();
    state->pending_unit_token = begin_pending_relocation_unit ();
    co_return true;
}

bool maintenance_runtime_t::relocate_encode (
  const std::shared_ptr<relocation_terminal_state_t> &state)
{
    /* The captured payload stays only in source memory: it is chunked onto
     * the wire and retained through the retransmission window. Nothing is
     * written to the Relocation Store on this path. */
    try {
        state->payload = encode_envelope (
          state->seal_attempt.seal.participants, state->context.relocation);
        if (state->payload.empty () || state->payload.size () > state->encoded_upper_bound) {
            (void) _objects.abort_relocation (state->seal_attempt.seal.token);
            state->result.emplace (finish (
              {relocation_terminal_t::blocked,
               relocation_reason_t::payload_bound_exceeded,
               std::nullopt}));
            return false;
        }
        state->manifest = plan_relocation_payload (
          state->payload, state->effective_chunk_limit);
    }
    catch (...) {
        (void) _objects.abort_relocation (state->seal_attempt.seal.token);
        state->result.emplace (finish (
          {relocation_terminal_t::store_failed,
           relocation_reason_t::store_write_failed,
           std::nullopt}));
        return false;
    }
    if (state->payload.empty ()
        || state->manifest.chunk_count > protocol::relocationChunkCount
        || state->manifest.total_length > protocol::relocationLogicalBytes) {
        (void) _objects.abort_relocation (state->seal_attempt.seal.token);
        state->result.emplace (finish (
          {relocation_terminal_t::blocked,
           relocation_reason_t::payload_bound_exceeded,
           std::nullopt}));
        return false;
    }
    return true;
}

task_t<bool> maintenance_runtime_t::relocate_prepare_target (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    state->handoff.emplace (target_only_cas_t{
      {state->source}, state->target_node_id, state->target_owner,
      state->inventory_digest, state->manifest});
    auto completion = std::make_shared<detail::task_completion_source_t<bool>> ();
    auto output = completion->task ();
    auto prepared = std::make_shared<task_t<relocation_reason_t>> (prepare_target (
      state->context, {state->seal_attempt.seal.participants.front ()},
      state->manifest));
    detail::observe_task_completion (
      *prepared, [this, state, completion, prepared] (
                   const result_t<relocation_reason_t> &settled) {
          if (!settled) {
              completion->complete (detail::propagate_failure<bool> (
                settled, "relocation target preparation failed"));
              return;
          }
          if (settled.value () != relocation_reason_t::none) {
              (void) _objects.abort_relocation_before_cutover (
                state->seal_attempt.seal.token);
              state->result.emplace (finish ({relocation_terminal_t::blocked,
                                      settled.value (),
                                      std::nullopt}));
              completion->complete (result_t<bool>::success (false));
              return;
          }
          completion->complete (result_t<bool>::success (true));
      });
    return output;
}

task_t<bool> maintenance_runtime_t::relocate_boundary_and_send (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    const auto boundary =
      _objects.begin_relocation_boundary (state->seal_attempt.seal.token);
    const auto boundary_error = boundary.first;
    state->batch = boundary.second;
    if (boundary_error != stateful_error_t::none) {
        (void) abort_target_before_cutover (state->context);
        (void) _objects.abort_relocation_before_cutover (state->seal_attempt.seal.token);
        state->result.emplace (finish ({relocation_terminal_t::blocked,
                        relocation_reason_t::restore_failed,
                        std::nullopt}));
        co_return false;
    }
    state->records = build_boundary_records (
      {state->seal_attempt.seal.participants.front ()}, state->context,
      state->batch, state->failure);
    if (!state->records
        || !co_await send_boundary_records (
          state->context, *state->records, state->batch)) {
        if (abort_target_before_cutover (state->context))
            (void) _objects.abort_relocation_before_cutover (state->seal_attempt.seal.token);
        state->result.emplace (finish ({relocation_terminal_t::recovery_required, state->failure,
                        std::nullopt,
                        state->records.value_or (std::vector<protocol::relocation_data_t>{}),
                        state->handoff}));
        co_return false;
    }
    co_return true;
}

task_t<bool> maintenance_runtime_t::relocate_cutover (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    const protocol::relocation_object_t object{
      to_wire_object_kind (state->source.kind),
      state->seal_attempt.seal.participants.front ().stable_type,
      state->source.key, state->source.object_generation,
      state->source.authority_owner_generation};
    auto cutover = protocol::relocation_cutover_t{
      state->context.relocation, state->context.target_attempt_generation,
      state->context.coordinator, protocol::relocation_role_t::source, object};
    cutover.boundary_record_count = state->records->size ();
    cutover.boundary_checksum_crc32c =
      boundary_batch_checksum (*state->records);
    const auto outcome = co_await state->context.send_cutover (cutover);
    const auto terminal_now = std::chrono::steady_clock::now ();
    state->cutover_terminal_at = terminal_now;
    const auto stall = state->sealed_at
                           != std::chrono::steady_clock::time_point{}
                         ? terminal_now - state->sealed_at
                         : std::chrono::steady_clock::duration::zero ();
    /* The target's relay-ready reply was already accepted: source dispatch
     * never reopens from here, whatever the submit outcome (28 §4.4/§9). */
    const auto finalized =
      _objects.finalize_relocation_cutover (state->seal_attempt.seal.token)
      == stateful_error_t::none;
    const auto enqueued =
      outcome
      == eligible_relocation_unit_t::canonical_wire_context_t::cutover_enqueue_t::enqueued;
    state->cutover_record = cutover;
    state->cutover_enqueued = enqueued;
    retain_retransmission_copies (state);
    if (!enqueued || !finalized) {
        state->result.emplace (finish ({relocation_terminal_t::recovery_required,
                        relocation_reason_t::restore_failed, std::nullopt,
                        *state->records, state->handoff, stall}));
        co_return false;
    }
    state->result.emplace (finish ({relocation_terminal_t::completed,
                    relocation_reason_t::none,
                    std::nullopt, *state->records, state->handoff, stall}));
    co_return true;
}

relocation_result_t maintenance_runtime_t::recover (
  object_kind_t kind,
  const std::string &key,
  stateful_object_runtime_t &target,
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
    /* Direct transfer keeps the payload's only original in the source
     * process memory. A store-mediated payload recovery path no longer
     * exists; an interrupted transfer whose source is gone is data loss.
     * The unused arguments stay for the recovery-orchestration callers. */
    (void) target;
    (void) cancellation;
    return finish (
      {relocation_terminal_t::data_lost,
       relocation_reason_t::payload_missing,
       authority});
}

aggregate_relocation_result_t maintenance_runtime_t::recover_aggregate (
  const std::vector<object_ref_t> &sources,
  stateful_object_runtime_t &target,
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

    /* Direct transfer keeps the payload's only original in the source
     * process memory; there is no store-mediated aggregate payload to
     * recover from. */
    (void) target;
    (void) cancellation;
    return {
      relocation_terminal_t::data_lost,
      relocation_reason_t::payload_missing,
      authority};
}

task_t<aggregate_relocation_result_t> maintenance_runtime_t::relocate_aggregate (
  const std::vector<object_ref_t> &sources,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t>
    &canonical_wire,
  std::stop_token cancellation,
  std::uint64_t advertised_receive_chunk_limit_bytes)
{
    if (sources.size () < 2 || !canonical_wire || !_aggregate_authority) {
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                relocation_reason_t::restore_failed, {}};
    }
    auto persisted_context = *canonical_wire;
    auto permit = std::make_shared<permit_t> (try_acquire ());
    if (!*permit) {
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                relocation_reason_t::permit_unavailable, {}};
    }
    const auto seal_attempt =
      co_await _objects.try_seal_relocation_aggregate (
        sources, cancellation,
        [this, &persisted_context] {
            return capture_relocation_session_routes (persisted_context);
        });
    const auto seal_error = seal_attempt.error;
    const auto &seal = seal_attempt.seal;
    if (seal_error != stateful_error_t::none) {
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                relocation_reason_t::restore_failed, {}};
    }
    auto state = std::make_shared<relocation_terminal_state_t> ();
    state->context = persisted_context;
    state->advertised_receive_chunk_limit_bytes =
      advertised_receive_chunk_limit_bytes;
    state->effective_chunk_limit =
      std::min<std::uint64_t> (_limits.payload_chunk_limit_bytes
                                 ? _limits.payload_chunk_limit_bytes
                                 : protocol::relocationChunkBytes,
                               protocol::relocationChunkBytes);
    state->effective_chunk_limit = apply_advertised_receive_chunk_limit (
      state->effective_chunk_limit,
      state->advertised_receive_chunk_limit_bytes);
    state->seal_attempt = seal_attempt;
    state->sealed_at = std::chrono::steady_clock::now ();
    state->pending_unit_token = begin_pending_relocation_unit ();
    try {
        state->payload = encode_envelope (
          seal.participants, persisted_context.relocation);
        state->manifest = plan_relocation_payload (
          state->payload, state->effective_chunk_limit);
    }
    catch (...) {
    }
    if (state->payload.empty ()
        || state->payload.size () > encoded_upper_bound
        || state->manifest.chunk_count > protocol::relocationChunkCount) {
        (void) _objects.abort_relocation_before_cutover (seal.token);
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                relocation_reason_t::payload_bound_exceeded, {}};
    }
    state->budget_reserved =
      std::min (state->effective_chunk_limit, effective_in_flight_budget ());
    co_await acquire_transfer_budget (state->budget_reserved);
    auto prepared = std::make_shared<task_t<relocation_reason_t>> (prepare_target (
      persisted_context, seal.participants, state->manifest));
    const auto chunks_sent =
      co_await relocate_send_state_chunks (state);
    release_transfer_budget (state->budget_reserved);
    state->budget_reserved = 0;
    auto target_prepare_reason = relocation_reason_t::restore_failed;
    try {
        target_prepare_reason = co_await *prepared;
    }
    catch (...) {
        target_prepare_reason = relocation_reason_t::restore_failed;
    }
    if (target_prepare_reason != relocation_reason_t::none || !chunks_sent) {
        (void) _objects.abort_relocation_before_cutover (seal.token);
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                target_prepare_reason != relocation_reason_t::none
                  ? target_prepare_reason
                  : relocation_reason_t::restore_failed, {}};
    }
    const target_only_cas_t handoff{
      sources, target_node_id, target_owner,
      inventory_digest, state->manifest};
    const auto [boundary_error, batch] =
      _objects.begin_relocation_boundary (seal.token);
    auto failure = relocation_reason_t::restore_failed;
    const auto records = boundary_error == stateful_error_t::none
      ? build_boundary_records (seal.participants, *canonical_wire, batch, failure)
      : std::nullopt;
    if (!records || !co_await send_boundary_records (*canonical_wire, *records, batch)) {
        if (abort_target_before_cutover (*canonical_wire))
            (void) _objects.abort_relocation_before_cutover (seal.token);
        co_return aggregate_relocation_result_t{relocation_terminal_t::recovery_required, failure, {},
                records.value_or (std::vector<protocol::relocation_data_t>{}), handoff};
    }
    /* 28 §6 (target-only owner CAS): the multi-object location update's
     * atomic authority CAS is the target's decision, driven by
     * commit_relocation_target_authority /
     * _aggregate_relocation_authority on the public host layer (after
     * cutover is observed there, or on the ready-fallback timeout) — same
     * as the single-object path, which never runs a source-side
     * prepare/commit at all. Running a second 2-phase prepare+commit here,
     * on the source, ahead of cutover, would let the source (and any peer
     * observing its store) see a committed authority before the target
     * has verified anything, which is exactly the ordering 28 §6
     * forbids. The source's only remaining job is to hand the boundary
     * records to the wire and send cutover; the target owns the CAS. */
    /* Cutover carries the same principal object identity as the Restore
     * request so the target binds it to the prepared attempt. */
    const auto principal = std::find_if (
      seal.participants.begin (), seal.participants.end (),
      [] (const frozen_object_state_t &candidate) {
          return candidate.owner.kind == object_kind_t::user_spot;
      });
    const auto &root = principal != seal.participants.end ()
                         ? *principal
                         : seal.participants.front ();
    auto cutover = protocol::relocation_cutover_t{
      canonical_wire->relocation, canonical_wire->target_attempt_generation,
      canonical_wire->coordinator, protocol::relocation_role_t::source,
      {to_wire_object_kind (root.owner.kind),
       root.stable_type, root.owner.key, root.owner.object_generation,
       root.owner.authority_owner_generation}};
    cutover.boundary_record_count = records->size ();
    cutover.boundary_checksum_crc32c = boundary_batch_checksum (*records);
    const auto outcome = co_await canonical_wire->send_cutover (cutover);
    state->cutover_terminal_at = std::chrono::steady_clock::now ();
    /* The relay-ready reply was already accepted: source dispatch never
     * reopens from here, whatever the submit outcome (28 §4.4/§9). */
    const auto finalized =
      _objects.finalize_relocation_cutover (seal.token) == stateful_error_t::none;
    const auto enqueued =
      outcome == eligible_relocation_unit_t::canonical_wire_context_t::cutover_enqueue_t::enqueued;
    state->records = *records;
    state->cutover_record = cutover;
    state->cutover_enqueued = enqueued;
    retain_retransmission_copies (state);
    co_return aggregate_relocation_result_t{enqueued && finalized
              ? relocation_terminal_t::completed
              : relocation_terminal_t::recovery_required,
            enqueued && finalized
              ? relocation_reason_t::none
              : relocation_reason_t::restore_failed,
            {}, *records, handoff};
}

relocation_gate_snapshot_t maintenance_runtime_t::gate_snapshot () const
{
    std::lock_guard lock (_gate_mutex);
    return _gate;
}

std::uint64_t maintenance_runtime_t::apply_advertised_receive_chunk_limit (
  std::uint64_t local_limit_bytes,
  std::uint64_t advertised_receive_chunk_limit_bytes) noexcept
{
    if (advertised_receive_chunk_limit_bytes == 0)
        return local_limit_bytes;
    return std::min (local_limit_bytes, advertised_receive_chunk_limit_bytes);
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

namespace
{

std::string_view participant_sort_word (object_kind_t kind) noexcept
{
    return kind == object_kind_t::actor ? "actor" : "spot";
}

/* Canonical participant inventory order (28 §4.2): UTF-8 authority-key
 * bytes. The cross-language authority-key preimage is
 * "authority\0{actor|spot}\0{Id}", so the order is the kind word first and
 * the raw object-id bytes second. */
bool participant_order_less (const object_ref_t &left,
                             const object_ref_t &right) noexcept
{
    const auto left_word = participant_sort_word (left.kind);
    const auto right_word = participant_sort_word (right.kind);
    if (left_word != right_word)
        return left_word < right_word;
    return left.key < right.key;
}

bool all_digits (std::string_view value) noexcept
{
    if (value.empty () || value.size () > 20)
        return false;
    for (const auto character : value) {
        if (character < '0' || character > '9')
            return false;
    }
    return true;
}

/* Synthetic identity for a logical timer that was registered without the
 * enriched schema fields. Its schema name is the decimal timer id and the
 * handler type is this sentinel, which lets a C++ target reconstruct the
 * original bare registration exactly. */
constexpr std::string_view bare_timer_handler_type = "LogicalTimer";

} // namespace

std::vector<std::uint8_t> maintenance_runtime_t::encode_envelope (
  const std::vector<frozen_object_state_t> &participants,
  const protocol::relocation_id_t &relocation)
{
    if (participants.empty ()
        || (relocation.high == 0 && relocation.low == 0))
        return {};
    std::vector<const frozen_object_state_t *> ordered;
    ordered.reserve (participants.size ());
    for (const auto &participant : participants)
        ordered.push_back (&participant);
    std::sort (ordered.begin (), ordered.end (),
               [] (const frozen_object_state_t *left,
                   const frozen_object_state_t *right) {
                   return participant_order_less (left->owner, right->owner);
               });
    for (std::size_t index = 0; index != ordered.size (); ++index) {
        const auto &frozen = *ordered[index];
        if (frozen.owner.key.empty () || frozen.stable_type.empty ()
            || frozen.owner.object_generation == 0
            || frozen.owner.authority_owner_generation == 0
            || frozen.application_state.size () > max_application_state_bytes
            || frozen.pending_application.size () > max_pending_records
            || frozen.timers.size () > max_logical_timers)
            return {};
        if (index != 0
            && !participant_order_less (ordered[index - 1]->owner,
                                        frozen.owner))
            return {};
    }
    const auto principal = std::find_if (
      participants.begin (), participants.end (),
      [] (const frozen_object_state_t &candidate) {
          return candidate.owner.kind == object_kind_t::user_spot;
      });
    const auto &root =
      principal != participants.end () ? *principal : participants.front ();

    protocol::relocation_envelope_t envelope;
    envelope.relocation = relocation;
    envelope.object = {to_wire_object_kind (root.owner.kind),
                       root.stable_type, root.owner.key,
                       root.owner.object_generation,
                       root.owner.authority_owner_generation};
    envelope.application_version = 1;

    try {
        for (std::size_t index = 0; index != ordered.size (); ++index) {
            const auto &frozen = *ordered[index];
            const auto participant_id =
              static_cast<std::uint64_t> (index) + 1;
            envelope.application_states.push_back (
              {participant_id, 1, frozen.application_state, {}});

            std::uint64_t boundary = 0;
            for (const auto &record : frozen.pending_application) {
                if (record.sequence == 0 || record.sequence <= boundary)
                    return {};
                boundary = record.sequence;
                protocol::frozen_record_t canonical;
                try {
                    if (record.frozen_record)
                        canonical = *record.frozen_record;
                    else if (record.application_record)
                        canonical = protocol::encode_frozen_application_record (
                          *record.application_record);
                    else
                        canonical =
                          protocol::decode_frozen_record (record.payload);
                }
                catch (const protocol::service_wire_error_t &) {
                    /* Only canonical service-wire-v1 frozen records exist as
                     * schema saved work. A non-canonical retained payload is
                     * not encodable; its accepted boundary still counts. */
                    continue;
                }
                envelope.saved_work.push_back (
                  {participant_id, record.sequence, std::move (canonical)});
            }

            std::vector<std::pair<std::string, const logical_timer_t *>>
              timers;
            timers.reserve (frozen.timers.size ());
            for (const auto &timer : frozen.timers) {
                auto name = timer.name.empty ()
                              ? std::to_string (timer.timer_id)
                              : timer.name;
                timers.emplace_back (std::move (name), &timer);
            }
            std::sort (timers.begin (), timers.end (),
                       [] (const auto &left, const auto &right) {
                           return left.first < right.first;
                       });
            for (std::size_t position = 1; position < timers.size ();
                 ++position) {
                if (timers[position - 1].first == timers[position].first)
                    return {};
            }
            for (const auto &[name, timer] : timers) {
                const auto bare = timer->name.empty ();
                protocol::relocation_envelope_timer_t registration;
                registration.participant_id = participant_id;
                registration.name = name;
                registration.handler_type =
                  timer->handler_type.empty ()
                    ? std::string (bare_timer_handler_type)
                    : timer->handler_type;
                registration.period_milliseconds =
                  timer->period_milliseconds != 0
                    ? timer->period_milliseconds
                    : (timer->due_after_milliseconds != 0
                         ? timer->due_after_milliseconds
                         : 1);
                registration.overrun_policy =
                  timer->overrun_policy >= 1 && timer->overrun_policy <= 3
                    ? timer->overrun_policy
                    : 1;
                registration.max_catch_up_ticks =
                  timer->max_catch_up_ticks != 0 ? timer->max_catch_up_ticks
                                                 : 1;
                registration.stop_on_unhandled_exception =
                  timer->stop_on_unhandled_exception;
                if (bare) {
                    const auto completed =
                      timer->next_tick_sequence != 0
                        ? timer->next_tick_sequence - 1
                        : 0;
                    registration.last_completed_delivery_index = completed;
                    registration.last_completed_scheduled_index = completed;
                    registration.next_scheduled_at_unix_milliseconds =
                      timer->due_after_milliseconds;
                }
                else {
                    registration.last_completed_delivery_index =
                      timer->last_completed_delivery_index;
                    registration.last_completed_scheduled_index =
                      timer->last_completed_scheduled_index;
                    registration.next_scheduled_at_unix_milliseconds =
                      timer->next_scheduled_at_unix_milliseconds != 0
                        ? timer->next_scheduled_at_unix_milliseconds
                        : timer->due_after_milliseconds;
                }
                envelope.timer_registrations.push_back (
                  std::move (registration));
                for (const auto &tick : timer->pending_ticks) {
                    ++boundary;
                    envelope.pending_timer_ticks.push_back (
                      {participant_id, boundary, name,
                       tick.delivery_index, tick.scheduled_index,
                       tick.scheduled_at_unix_milliseconds,
                       tick.skipped_ticks});
                }
            }
        }
        auto encoded = protocol::encode_relocation_envelope (envelope);
        if (encoded.size () > max_envelope_bytes)
            return {};
        return encoded;
    }
    catch (...) {
        return {};
    }
}

std::optional<protocol::relocation_envelope_t>
maintenance_runtime_t::decode_envelope (
  const std::vector<std::uint8_t> &payload) noexcept
try
{
    if (payload.empty () || payload.size () > max_envelope_bytes)
        return std::nullopt;
    return protocol::decode_relocation_envelope (payload);
}
catch (...)
{
    return std::nullopt;
}

std::optional<std::vector<frozen_object_state_t>>
maintenance_runtime_t::materialize_envelope (
  const protocol::relocation_envelope_t &envelope,
  std::vector<relocation_participant_identity_t> inventory) noexcept
try
{
    if (inventory.empty ()
        || inventory.size () != envelope.application_states.size ())
        return std::nullopt;
    std::sort (inventory.begin (), inventory.end (),
               [] (const relocation_participant_identity_t &left,
                   const relocation_participant_identity_t &right) {
                   return participant_order_less (left.owner, right.owner);
               });
    for (std::size_t index = 0; index != inventory.size (); ++index) {
        const auto &identity = inventory[index];
        if (identity.owner.key.empty () || identity.stable_type.empty ()
            || identity.owner.object_generation == 0
            || identity.owner.authority_owner_generation == 0)
            return std::nullopt;
        if (index != 0
            && !participant_order_less (inventory[index - 1].owner,
                                        identity.owner))
            return std::nullopt;
        /* participantId is deliberately absent from the stream: it is this
         * sorted inventory's zero-based index plus one. */
        if (envelope.application_states[index].participant_id
            != static_cast<std::uint64_t> (index) + 1)
            return std::nullopt;
    }

    std::vector<frozen_object_state_t> participants;
    participants.reserve (inventory.size ());
    for (std::size_t index = 0; index != inventory.size (); ++index) {
        const auto &identity = inventory[index];
        const auto &state = envelope.application_states[index];
        participants.push_back (frozen_object_state_t{
          .owner = identity.owner,
          .stable_type = identity.stable_type,
          .application_state = state.state,
          .pending_application = {},
          .timers = {}});
    }

    for (const auto &entry : envelope.saved_work) {
        if (entry.participant_id == 0
            || entry.participant_id > participants.size ())
            return std::nullopt;
        auto &participant =
          participants[static_cast<std::size_t> (entry.participant_id) - 1];
        std::optional<std::size_t> application_payload_bytes;
        if (entry.record.application)
            application_payload_bytes =
              protocol::application_payload_hwm_bytes (
                *entry.record.application);
        participant.pending_application.push_back (
          turn_record_t{entry.order, entry.record.canonical_bytes,
                        application_payload_bytes, std::nullopt,
                        entry.record});
    }

    /* Rebuild the runtime timer model. A schema registration whose handler
     * type is the bare sentinel and whose name is the decimal timer id was
     * written from a bare C++ registration and reconstructs it exactly;
     * anything else keeps the enriched fields and derives the local timer
     * id from the name when it is numeric, or sequentially otherwise. */
    std::map<std::uint64_t, std::set<std::uint64_t>> used_ids;
    for (const auto &registration : envelope.timer_registrations) {
        if (registration.participant_id == 0
            || registration.participant_id > participants.size ())
            return std::nullopt;
        auto &participant = participants[static_cast<std::size_t> (
                              registration.participant_id) - 1];
        auto &ids = used_ids[registration.participant_id];
        std::uint64_t timer_id = 0;
        if (all_digits (registration.name)) {
            std::uint64_t parsed = 0;
            const auto *first = registration.name.data ();
            const auto *last = first + registration.name.size ();
            if (std::from_chars (first, last, parsed).ec == std::errc{}
                && parsed != 0 && !ids.contains (parsed))
                timer_id = parsed;
        }
        if (timer_id == 0) {
            timer_id = ids.empty () ? 1 : *ids.rbegin () + 1;
            while (ids.contains (timer_id))
                ++timer_id;
        }
        ids.insert (timer_id);
        const auto bare =
          registration.handler_type == bare_timer_handler_type
          && all_digits (registration.name);
        logical_timer_t timer;
        timer.timer_id = timer_id;
        timer.period_milliseconds = registration.period_milliseconds;
        timer.due_after_milliseconds =
          registration.next_scheduled_at_unix_milliseconds != 0
            ? registration.next_scheduled_at_unix_milliseconds
            : registration.period_milliseconds;
        timer.next_tick_sequence =
          registration.last_completed_delivery_index + 1;
        if (!bare) {
            timer.name = registration.name;
            timer.handler_type = registration.handler_type;
            timer.overrun_policy = registration.overrun_policy;
            timer.max_catch_up_ticks = registration.max_catch_up_ticks;
            timer.stop_on_unhandled_exception =
              registration.stop_on_unhandled_exception;
            timer.last_completed_delivery_index =
              registration.last_completed_delivery_index;
            timer.last_completed_scheduled_index =
              registration.last_completed_scheduled_index;
            timer.next_scheduled_at_unix_milliseconds =
              registration.next_scheduled_at_unix_milliseconds;
        }
        participant.timers.push_back (std::move (timer));
    }
    for (const auto &tick : envelope.pending_timer_ticks) {
        if (tick.participant_id == 0
            || tick.participant_id > participants.size ())
            return std::nullopt;
        auto &participant = participants[static_cast<std::size_t> (
                              tick.participant_id) - 1];
        const auto registration = std::find_if (
          envelope.timer_registrations.begin (),
          envelope.timer_registrations.end (),
          [&tick] (const protocol::relocation_envelope_timer_t &candidate) {
              return candidate.participant_id == tick.participant_id
                     && candidate.name == tick.timer_name;
          });
        if (registration == envelope.timer_registrations.end ())
            return std::nullopt;
        const auto position = static_cast<std::size_t> (std::distance (
          std::find_if (envelope.timer_registrations.begin (),
                        envelope.timer_registrations.end (),
                        [&tick] (const auto &candidate) {
                            return candidate.participant_id
                                   == tick.participant_id;
                        }),
          registration));
        if (position >= participant.timers.size ())
            return std::nullopt;
        participant.timers[position].pending_ticks.push_back (
          {tick.delivery_index, tick.scheduled_index,
           tick.scheduled_at_unix_milliseconds, tick.skipped_ticks});
    }
    return participants;
}
catch (...)
{
    return std::nullopt;
}

inventory_digest_t maintenance_runtime_t::compute_inventory_digest (
  const std::vector<object_ref_t> &participants)
{
    auto sorted = participants;
    std::sort (sorted.begin (), sorted.end (),
               [] (const object_ref_t &left, const object_ref_t &right) {
                   if (left.kind != right.kind)
                       return left.kind < right.kind;
                   return left.key < right.key;
               });
    std::vector<std::byte> seed;
    for (const auto &source : sorted) {
        for (const auto value : source.key)
            seed.push_back (static_cast<std::byte> (
              static_cast<unsigned char> (value)));
        for (int shift = 56; shift >= 0; shift -= 8) {
            seed.push_back (
              static_cast<std::byte> (source.object_generation >> shift));
            seed.push_back (static_cast<std::byte> (
              source.authority_owner_generation >> shift));
        }
    }
    const auto digest = runtime::sha256 (seed);
    inventory_digest_t output{};
    for (std::size_t index = 0; index != output.size (); ++index)
        output[index] = std::to_integer<std::uint8_t> (digest[index]);
    return output;
}

maintenance_runtime_t::permit_t::permit_t (maintenance_runtime_t *owner) :
    _owner (owner)
{
}

maintenance_runtime_t::permit_t::~permit_t ()
{
    if (_owner)
        _owner->release ();
}

maintenance_runtime_t::permit_t::permit_t (
  permit_t &&other) noexcept :
    _owner (std::exchange (other._owner, nullptr))
{
}

maintenance_runtime_t::permit_t &
maintenance_runtime_t::permit_t::operator= (
  permit_t &&other) noexcept
{
    if (this == &other)
        return *this;
    if (_owner)
        _owner->release ();
    _owner = std::exchange (other._owner, nullptr);
    return *this;
}

maintenance_runtime_t::permit_t::operator bool () const noexcept
{
    return _owner != nullptr;
}

maintenance_runtime_t::permit_t
maintenance_runtime_t::try_acquire ()
{
    std::lock_guard lock (_gate_mutex);
    if (_gate.outbound_units >= _limits.outbound_units
        || _gate.inbound_units >= _limits.inbound_units
        || _gate.capture_callbacks >= _limits.capture_callbacks
        || _gate.restore_callbacks >= _limits.restore_callbacks) {
        return {};
    }
    ++_gate.outbound_units;
    ++_gate.inbound_units;
    ++_gate.capture_callbacks;
    ++_gate.restore_callbacks;
    return permit_t (this);
}

void maintenance_runtime_t::release () noexcept
{
    std::lock_guard lock (_gate_mutex);
    --_gate.outbound_units;
    --_gate.inbound_units;
    --_gate.capture_callbacks;
    --_gate.restore_callbacks;
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
    if (_state != maintenance_admission_state_t::preparing)
        throw std::logic_error ("host can become serving only from preparing");
    _state = maintenance_admission_state_t::serving;
}

void host_maintenance_runtime_t::mark_error ()
{
    std::lock_guard lock (_mutex);
    if (_state != maintenance_admission_state_t::stopped)
        _state = maintenance_admission_state_t::error;
}

maintenance_admission_state_t host_maintenance_runtime_t::state () const
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

task_t<termination_result_t> host_maintenance_runtime_t::terminate (
  termination_intent_t intent)
{
    std::uint64_t attempt = 0;
    std::shared_ptr<detail::task_completion_source_t<termination_result_t>> completion;
    {
        std::unique_lock lock (_mutex);
        if (_terminal)
            return task_t<termination_result_t> (
              result_t<termination_result_t>::success (*_terminal));
        if (_state == maintenance_admission_state_t::stopped) {
            return task_t<termination_result_t> (
              result_t<termination_result_t>::success ({
                intent, termination_outcome_t::stopped,
                termination_reason_t::none}));
        }
        if (_active) {
            attempt = _active_attempt;
            if (intent == termination_intent_t::shutdown
                && !_effective_intent) {
                _shutdown_claimed = true;
            }
            return _active_completion->task ();
        }
        if (intent == termination_intent_t::retire
            && _state != maintenance_admission_state_t::serving) {
            return task_t<termination_result_t> (
              result_t<termination_result_t>::success ({
                intent, termination_outcome_t::blocked,
                termination_reason_t::runtime_not_ready}));
        }
        _active = true;
        _shutdown_claimed = false;
        _effective_intent.reset ();
        attempt = _next_attempt++;
        _active_attempt = attempt;
        if (intent == termination_intent_t::shutdown)
            _effective_intent = termination_intent_t::shutdown;
        completion = std::make_shared<
          detail::task_completion_source_t<termination_result_t>> ();
        _active_completion = completion;
    }
    auto output = completion->task ();
    auto running = std::make_shared<task_t<termination_result_t>> (
      run_termination_attempt (intent, attempt));
    detail::observe_task_completion (
      *running, [completion, running] (
                  const result_t<termination_result_t> &settled) {
          completion->complete (settled);
      });
    return output;
}

task_t<termination_result_t>
host_maintenance_runtime_t::run_termination_attempt (
  termination_intent_t intent, std::uint64_t attempt)
{
    const auto result = intent == termination_intent_t::retire
                        ? co_await run_retire ()
                        : run_shutdown (termination_intent_t::shutdown);
    complete_attempt (attempt, result);
    co_return result;
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

task_t<termination_result_t> host_maintenance_runtime_t::run_retire ()
{
    auto inventory = _objects.try_begin_maintenance_inventory ();
    if (!inventory) {
        co_return termination_result_t{
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
            co_return termination_result_t{
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
            _state = maintenance_admission_state_t::draining;
        } else if (preflight.status
                   == target_preflight_status_t::eligible
                   && exact_preflight) {
            _effective_intent = termination_intent_t::retire;
            _state = maintenance_admission_state_t::retiring;
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
            co_return termination_result_t{
              termination_intent_t::retire,
              termination_outcome_t::blocked, reason};
        }
    }
    if (_effective_intent == termination_intent_t::shutdown)
        co_return run_shutdown (termination_intent_t::shutdown);

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
              _state = maintenance_admission_state_t::draining;
          }
          _sessions.force_close_all ();
          {
              std::lock_guard lock (_mutex);
              _state = maintenance_admission_state_t::stopped;
          }
          return termination_result_t{
            termination_intent_t::retire,
            termination_outcome_t::force_stopped,
            termination_reason_t::relocation_failed};
      };
    for (const auto &eligible : preflight.units) {
        if (eligible.unit.participants.empty ()
            || eligible.target_node_id.empty ()
            || eligible.encoded_upper_bound == 0) {
            co_return fail_relocation (
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
            co_return fail_relocation (
              termination_reason_t::state_incompatible);
        }

        std::vector<authority_relocation_reference_t> current;
        relocation_terminal_t terminal = relocation_terminal_t::blocked;
        if (eligible.unit.participants.size () == 1) {
            const auto result = co_await _relocation.relocate (
              eligible.unit.participants.front (),
              eligible.target_node_id, eligible.target_owner,
              eligible.encoded_upper_bound,
              eligible.inventory_digest,
              eligible.canonical_wire);
            terminal = result.terminal;
            if (result.authority)
                current.push_back (*result.authority);
        } else {
            const auto result = co_await _relocation.relocate_aggregate (
              eligible.unit.participants, eligible.target_node_id,
              eligible.target_owner,
              eligible.encoded_upper_bound,
              eligible.inventory_digest,
              eligible.canonical_wire);
            terminal = result.terminal;
            current = result.authority;
        }
        if (terminal != relocation_terminal_t::completed) {
            for (const auto &barrier : barriers)
                (void) _sessions.abort_barrier (barrier);
            co_return fail_relocation (
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
                co_return fail_relocation (
                  termination_reason_t::relocation_failed, true);
            }
        }
        ++committed_units;
    }
    {
        std::lock_guard lock (_mutex);
        _state = maintenance_admission_state_t::draining;
    }
    if (!_sessions.try_seal_all ()) {
        _sessions.force_close_all ();
        {
            std::lock_guard lock (_mutex);
            _state = maintenance_admission_state_t::stopped;
        }
        co_return termination_result_t{
          termination_intent_t::retire,
          termination_outcome_t::force_stopped,
          termination_reason_t::relocation_failed};
    }
    {
        std::lock_guard lock (_mutex);
        _state = maintenance_admission_state_t::stopped;
    }
    co_return termination_result_t{
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
        _state = maintenance_admission_state_t::draining;
        acquire_inventory = !_inventory_sealed;
    }
    if (acquire_inventory) {
        const auto inventory =
          _objects.try_begin_maintenance_inventory ();
        if (!inventory) {
            {
                std::lock_guard lock (_mutex);
                _state = maintenance_admission_state_t::stopped;
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
        _state = maintenance_admission_state_t::stopped;
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
        _active_completion.reset ();
        _shutdown_claimed = false;
        _effective_intent.reset ();
        if (result.outcome != termination_outcome_t::blocked)
            _terminal = result;
        else {
            release_inventory = _inventory_sealed;
            _inventory_sealed = false;
            if (_state == maintenance_admission_state_t::retiring)
                _state = maintenance_admission_state_t::serving;
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
    _aggregate_relocation_authority = aggregate_authority;
    _session_relocations = relocations;
    _relocation_cutover_wait = limits.cutover_wait_timeout;
    auto maintenance =
      std::make_unique<stateful::maintenance_runtime_t> (
        _objects, std::move (authority), std::move (relocations),
        limits, std::move (relocation_observer),
        std::move (aggregate_authority));
    maintenance->attach_relocation_wire (*_relocation_wire);
    _maintenance = std::move (maintenance);
}

void public_host_runtime_t::configure_relocation_target_metrics (
  relocation_target_metrics_t metrics)
{
    std::lock_guard lock (_mutex);
    _relocation_target_metrics = std::move (metrics);
}

void public_host_runtime_t::configure_relocation_source_metrics (
  std::function<void (double)> route_convergence_metric)
{
    stateful::maintenance_runtime_t *maintenance_ptr;
    {
        std::lock_guard lock (_mutex);
        maintenance_ptr = _maintenance.get ();
    }
    if (maintenance_ptr)
        maintenance_ptr->configure_route_convergence_metric (
          std::move (route_convergence_metric));
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
    _aggregate_relocation_authority = providers.aggregate_authority;
    _session_relocations = providers.relocations;
    _relocation_cutover_wait = limits.cutover_wait_timeout;
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
