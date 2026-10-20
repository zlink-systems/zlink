/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/timers/async_delay.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"

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

bool participant_order_less (const object_ref_t &, const object_ref_t &) noexcept;

std::vector<const frozen_object_state_t *>
canonical_participants (const std::vector<frozen_object_state_t> &participants)
{
    std::vector<const frozen_object_state_t *> ordered;
    ordered.reserve (participants.size ());
    for (const auto &participant : participants)
        ordered.push_back (&participant);
    std::sort (ordered.begin (), ordered.end (), [] (const auto left, const auto right) {
        return participant_order_less (left->owner, right->owner);
    });
    return ordered;
}

constexpr std::array<std::uint8_t, 4> session_journal_magic{'Z', 'L', 'S', 'J'};
constexpr std::chrono::hours relocation_retention{24};
constexpr std::size_t max_envelope_bytes = 256u * 1024u * 1024u;
constexpr std::size_t max_application_state_bytes = 64u * 1024u * 1024u;
constexpr std::uint32_t max_pending_records = 4096;
constexpr std::uint32_t max_logical_timers = 4096;

protocol::relocation_object_kind_t to_wire_object_kind (object_kind_t kind)
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

bool append_bytes (std::vector<std::uint8_t> &output, const std::vector<std::uint8_t> &value)
{
    if (value.size () > std::numeric_limits<std::uint32_t>::max ())
        return false;
    append_u32 (output, static_cast<std::uint32_t> (value.size ()));
    output.insert (output.end (), value.begin (), value.end ());
    return true;
}

bool append_text (std::vector<std::uint8_t> &output, const std::string &value)
{
    return append_bytes (output, std::vector<std::uint8_t> (value.begin (), value.end ()));
}

class reader_t
{
  public:
    explicit reader_t (const std::vector<std::uint8_t> &input) : _input (input) {}

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

    std::optional<std::vector<std::uint8_t>> bytes (std::size_t maximum = max_envelope_bytes)
    {
        const auto size = u32 ();
        if (!size || *size > maximum || *size > _input.size () - _offset)
            return std::nullopt;
        std::vector<std::uint8_t> result (_input.begin () + static_cast<std::ptrdiff_t> (_offset),
                                          _input.begin ()
                                            + static_cast<std::ptrdiff_t> (_offset + *size));
        _offset += *size;
        return result;
    }

    std::optional<std::string> text ()
    {
        const auto value = bytes (255);
        return value ? std::make_optional (std::string (value->begin (), value->end ()))
                     : std::nullopt;
    }

    bool done () const noexcept { return _offset == _input.size (); }
    std::size_t remaining () const noexcept { return _input.size () - _offset; }
    std::size_t checkpoint () const noexcept { return _offset; }
    void rewind (std::size_t checkpoint) noexcept { _offset = checkpoint; }

  private:
    const std::vector<std::uint8_t> &_input;
    std::size_t _offset = 0;
};

std::vector<std::uint8_t> encode_session_journal (const durable_session_journal_record_t &record)
{
    if ((record.relocation_id_high == 0 && record.relocation_id_low == 0)
        || record.actor.kind != object_kind_t::actor || record.actor.key.empty ()
        || record.actor.object_generation == 0 || record.actor.authority_owner_generation == 0
        || record.actor.node_id.empty () || record.binding_generation == 0)
        throw std::invalid_argument ("durable Session journal identity is invalid");
    std::vector<std::uint8_t> output (session_journal_magic.begin (), session_journal_magic.end ());
    append_u64 (output, record.relocation_id_high);
    append_u64 (output, record.relocation_id_low);
    append_u64 (output, record.actor.object_generation);
    append_u64 (output, record.actor.authority_owner_generation);
    append_u64 (output, record.binding_generation);
    append_u64 (output, record.last_accepted_session_sequence);
    if (!append_text (output, record.actor.key) || !append_text (output, record.actor.mesh_name)
        || !append_text (output, record.actor.node_id)
        || !append_bytes (output, record.accepted_journal) || output.size () > max_envelope_bytes)
        throw std::invalid_argument ("durable Session journal payload is too large");
    return output;
}

std::optional<durable_session_journal_record_t>
decode_session_journal (const std::vector<std::uint8_t> &payload)
{
    if (payload.size () < session_journal_magic.size ()
        || !std::equal (session_journal_magic.begin (), session_journal_magic.end (),
                        payload.begin ()))
        return std::nullopt;
    std::vector<std::uint8_t> body (payload.begin () + 4, payload.end ());
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
    if (!relocation_high || !relocation_low || (*relocation_high == 0 && *relocation_low == 0)
        || !object_generation || *object_generation == 0 || !authority_generation
        || *authority_generation == 0 || !binding_generation || *binding_generation == 0
        || !high_water || !actor_id || actor_id->empty () || !mesh_name || !node_id
        || node_id->empty () || !journal || !reader.done ())
        return std::nullopt;
    return durable_session_journal_record_t{*relocation_high,
                                            *relocation_low,
                                            object_ref_t{object_kind_t::actor, *actor_id,
                                                         *object_generation, *authority_generation,
                                                         *mesh_name, *node_id},
                                            *binding_generation,
                                            *high_water,
                                            *journal};
}

} // namespace

namespace
{
class relocation_metric_operation_t
{
  public:
    relocation_metric_operation_t (
      const eligible_relocation_unit_t::canonical_wire_context_t &context, object_kind_t kind) :
        _metrics (context.monitoring)
    {
        using policy_t = framework::detail::factory_relocation_kind_t;
        if (!_metrics.enabled ()
            || (context.policy != policy_t::recreate && context.policy != policy_t::preserve_state))
            return;
        const auto *object_kind = runtime::metric_object_kind (kind);
        _tags = {{"mesh_name", context.mesh_name},
                 {"object_kind", object_kind},
                 {"policy", context.policy == policy_t::preserve_state ? "snapshot" : "recreate"}};
        _started = std::chrono::steady_clock::now ();
        _metrics.counter ("zlink.relocation.started", "{relocation}", 1, _tags);
    }

    relocation_metric_operation_t (relocation_metric_operation_t &&other) noexcept :
        _metrics (std::move (other._metrics)),
        _tags (std::move (other._tags)),
        _started (std::exchange (other._started, {}))
    {
    }
    relocation_metric_operation_t (const relocation_metric_operation_t &) = delete;
    ~relocation_metric_operation_t () { complete (std::nullopt); }

    void record_bytes (std::size_t bytes) const
    {
        if (_started != std::chrono::steady_clock::time_point{})
            _metrics.histogram ("zlink.relocation.bytes", "By", static_cast<double> (bytes), _tags);
    }

    void complete (std::optional<relocation_authority_t> settlement)
    {
        const auto started = std::exchange (_started, {});
        if (started == std::chrono::steady_clock::time_point{})
            return;
        const char *outcome = "failed";
        if (settlement) {
            switch (*settlement) {
                case relocation_authority_t::target_committed:
                    outcome = "completed";
                    break;
                case relocation_authority_t::source_preserved:
                    outcome = "aborted";
                    break;
                // Only the host-stop deadline return at maintenance_runtime.cpp:687 reaches
                // this case; ordinary unsettled observations stay in settle_relocation.
                // Host stop installs that deadline at host/app.cpp:3959,4261.
                case relocation_authority_t::unsettled:
                    outcome = "shutdown";
                    break;
                default:
                    break;
            }
        }
        _tags.emplace ("outcome", outcome);
        _metrics.counter ("zlink.relocation.completed", "{relocation}", 1, _tags);
        _metrics.histogram (
          "zlink.relocation.duration", "s",
          std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count (),
          _tags);
    }

  private:
    runtime_metrics_t _metrics;
    std::map<std::string, std::string> _tags;
    std::chrono::steady_clock::time_point _started{};
};
}

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
     * (begin_pending_relocation_unit), released explicitly at S4 in
     * retain_message_follow, or implicitly by destruction of this state
     * when the unit settles any other way. */
    std::shared_ptr<void> pending_unit_token;
    std::optional<target_only_cas_t> handoff;
    relocation_ingress_batch_t batch;
    relocation_reason_t failure = relocation_reason_t::restore_failed;
    std::optional<std::vector<protocol::relocation_data_t>> records;
    std::optional<relocation_result_t> result;
    std::optional<protocol::relocation_cutover_t> cutover_record;
    std::uint64_t submitted_route_generation = 0;
    /* S1 (cutover submit terminal), on the source clock. Used as the start
     * of the route_convergence window (25 §"zlink.relocation"), which ends
     * at S4 (Message Follow route removable). */
    std::chrono::steady_clock::time_point cutover_terminal_at{};
    std::optional<relocation_metric_operation_t> metric;
};

durable_session_journal_store_t::durable_session_journal_store_t (
  std::shared_ptr<relocation_store_port_t> store) :
    _store (std::move (store))
{
    if (!_store)
        throw std::invalid_argument ("durable Session journal store is required");
}

durable_session_journal_root_t
durable_session_journal_store_t::prepare (const durable_session_journal_record_t &record,
                                          std::chrono::steady_clock::time_point operation_deadline)
{
    const auto payload = encode_session_journal (record);
    const auto checksum = maintenance_runtime_t::crc32c (payload);
    const auto stored = _store->put (payload, relocation_retention, operation_deadline);
    if (stored.reference.empty () || stored.checksum_crc32c != checksum)
        throw std::runtime_error ("durable Session journal store write failed");
    return {stored.reference, checksum};
}

std::optional<durable_session_journal_record_t>
durable_session_journal_store_t::recover (const durable_session_journal_root_t &root) const
{
    const auto payload = _store->get (root.reference);
    if (!payload || maintenance_runtime_t::crc32c (*payload) != root.checksum_crc32c)
        return std::nullopt;
    return decode_session_journal (*payload);
}

void durable_session_journal_store_t::cleanup (const durable_session_journal_root_t &root)
{
    _store->remove (root.reference);
}

maintenance_runtime_t::maintenance_runtime_t (
  stateful_object_runtime_t &objects,
  std::shared_ptr<authority_relocation_port_t> authority,
  std::shared_ptr<relocation_store_port_t> relocations,
  relocation_limits_t limits,
  observer_t observer,
  std::shared_ptr<aggregate_authority_port_t> aggregate_authority) :
    _objects (objects),
    _authority (std::move (authority)),
    _relocations (std::move (relocations)),
    _aggregate_authority (std::move (aggregate_authority)),
    _limits (limits),
    _observer (std::move (observer))
{
    if (!_authority || !_relocations || _limits.outbound_units == 0 || _limits.inbound_units == 0
        || _limits.capture_callbacks == 0 || _limits.restore_callbacks == 0) {
        throw std::invalid_argument ("maintenance runtime configuration is invalid");
    }
}

maintenance_runtime_t::maintenance_runtime_t (stateful_object_runtime_t &objects,
                                              maintenance_provider_set_t providers,
                                              relocation_limits_t limits,
                                              observer_t observer) :
    maintenance_runtime_t (objects,
                           std::move (providers.authority),
                           std::move (providers.relocations),
                           limits,
                           std::move (observer),
                           std::move (providers.aggregate_authority))
{
    if (!_aggregate_authority)
        throw std::invalid_argument ("maintenance provider set is incomplete");
}

void maintenance_runtime_t::attach_relocation_wire (
  raw_relocation_replay_coordinator_t &wire,
  std::function<raw_stateful_dispatch_t *()> dispatch) noexcept
{
    _relocation_wire = &wire;
    _relocation_dispatch = std::move (dispatch);
}

void maintenance_runtime_t::attach_committed_terminal (committed_terminal_t terminal) noexcept
{
    _committed_terminal = std::move (terminal);
}

void maintenance_runtime_t::attach_unavailable_terminal (
  std::function<void (const object_ref_t &)> terminal) noexcept
{
    _unavailable_terminal = std::move (terminal);
}

void maintenance_runtime_t::configure_route_convergence_metric (
  std::function<void (double)> metric) noexcept
{
    std::lock_guard lock (_shutdown_tracking->metric_mutex);
    _shutdown_tracking->route_convergence_metric = std::move (metric);
}

bool maintenance_runtime_t::relocation_units_settled () const noexcept
{
    return _shutdown_tracking->pending_units.load (std::memory_order_acquire) <= 0;
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
        || context.target_attempt_generation == 0 || context.coordinator.owner_id.empty ()
        || context.coordinator.lease_generation == 0 || context.coordinator.node_routing_id.empty ()
        || context.coordinator.node_generation == 0
        || context.coordinator.expected_authority_store_version.empty ()
        || context.target_node_routing_id.empty () || context.target_node_generation == 0
        || !context.prepare_target || !context.send_state_chunk || !context.send_relocation_data
        || !context.send_cutover || !context.abort_target_before_cutover)
        return std::nullopt;

    const auto ordered = canonical_participants (participants);
    std::vector<protocol::relocation_data_t> records;
    try {
        for (const auto &batch_participant : batch.participants) {
            const auto frozen = std::find_if (participants.begin (), participants.end (),
                                              [&] (const frozen_object_state_t &candidate) {
                                                  return candidate.owner == batch_participant.owner;
                                              });
            if (frozen == participants.end ())
                return std::nullopt;
            const protocol::relocation_object_t object{
              to_wire_object_kind (frozen->owner.kind), frozen->stable_type, frozen->owner.key,
              frozen->owner.object_generation, frozen->owner.authority_owner_generation};
            const auto participant_id =
              static_cast<std::uint64_t> (std::find (ordered.begin (), ordered.end (), &*frozen)
                                          - ordered.begin ())
              + 1;
            for (const auto &turn : batch_participant.records) {
                if (!turn.application_record)
                    return std::nullopt;
                if (!register_terminal_source (context, frozen->owner, participant_id, turn))
                    return std::nullopt;
                auto record = protocol::encode_frozen_application_record (*turn.application_record);
                records.push_back ({context.relocation, context.target_attempt_generation,
                                    context.coordinator, protocol::relocation_role_t::source,
                                    object, std::move (record)});
            }
        }
    }
    catch (const protocol::service_wire_error_t &) {
        return std::nullopt;
    }
    return records;
}

protocol::reply_relay_t maintenance_runtime_t::terminal_identity (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_coordinator_fence_t &coordinator,
  std::uint64_t participant_id,
  const protocol::frozen_record_t &record)
{
    return {record.operation,
            record.reply_route_id.value (),
            relocation,
            target_attempt_generation,
            coordinator,
            participant_id,
            record.operation.low != 0 ? record.operation.low : record.operation.high,
            0,
            protocol::framework_error_code::none};
}

bool maintenance_runtime_t::register_terminal_source (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const object_ref_t &owner,
  std::uint64_t participant_id,
  const turn_record_t &turn) const
{
    const auto dispatch = _relocation_dispatch ? _relocation_dispatch () : nullptr;
    if (!_relocation_wire || !dispatch)
        return true;
    const auto record = turn.application_record
                          ? protocol::summarize_frozen_application_record (*turn.application_record)
                        : turn.frozen_record ? *turn.frozen_record
                                             : protocol::decode_frozen_record (turn.payload);
    if (!record.application || !record.reply_route_id)
        return true;
    if (!_relocation_wire->owns_request_source (record.source))
        return true;
    auto terminal = terminal_identity (context.relocation, context.target_attempt_generation,
                                       context.coordinator, participant_id, record);
    return _relocation_wire->register_terminal_source (
      {terminal.relocation, std::move (terminal.coordinator), terminal.operation, record.source,
       context.target_node_routing_id, context.target_node_generation,
       terminal.target_attempt_generation, terminal.participant_id, terminal.sequence,
       terminal.reply_route_id,
       [dispatch, owner,
        sequence = turn.sequence] (const protocol::reply_relay_t &relay,
                                   const std::optional<protocol::application_payload_t> &reply) {
           return dispatch->complete_relocated_source_async (owner, sequence, relay, reply)
             .result ()
             .value ();
       }});
}

task_t<stateful_error_t> maintenance_runtime_t::abort_relocation_before_cutover (
  std::uint64_t token, const eligible_relocation_unit_t::canonical_wire_context_t &context)
{
    const auto aborted = co_await _objects.abort_relocation_before_cutover (token);
    if (aborted == stateful_error_t::none && _relocation_wire)
        co_await _relocation_wire->unregister_terminal_sources (context.relocation);
    co_return aborted;
}

task_t<relocation_reason_t> maintenance_runtime_t::prepare_target (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const std::vector<frozen_object_state_t> &participants,
  const relocation_payload_manifest_t &manifest)
{
    try {
        const auto ordered = canonical_participants (participants);
        for (std::size_t index = 0; index != ordered.size (); ++index)
            for (const auto &turn : ordered[index]->pending_application)
                if (!register_terminal_source (context, ordered[index]->owner, index + 1, turn))
                    co_return relocation_reason_t::restore_failed;
        co_return co_await context.prepare_target (participants, manifest);
    }
    catch (...) {
        co_return relocation_reason_t::restore_failed;
    }
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

task_t<void> maintenance_runtime_t::acquire_transfer_budget (std::uint64_t bytes)
{
    if (bytes == 0)
        co_return;
    const auto budget = effective_in_flight_budget ();
    for (;;) {
        std::shared_ptr<task_completion_source_t<bool>> waiter;
        {
            std::lock_guard lock (_budget_mutex);
            if (_budget_in_flight_bytes == 0 || _budget_in_flight_bytes + bytes <= budget) {
                _budget_in_flight_bytes += bytes;
                co_return;
            }
            waiter = std::make_shared<task_completion_source_t<bool>> ();
            _budget_waiters.emplace_back (bytes, waiter);
        }
        (void) co_await waiter->task ();
    }
}

void maintenance_runtime_t::release_transfer_budget (std::uint64_t bytes) noexcept
{
    std::vector<std::shared_ptr<task_completion_source_t<bool>>> released;
    {
        std::lock_guard lock (_budget_mutex);
        _budget_in_flight_bytes =
          _budget_in_flight_bytes > bytes ? _budget_in_flight_bytes - bytes : 0;
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
  std::shared_ptr<relocation_terminal_state_t> state, std::function<bool ()> target_failed)
{
    if (!state->context.send_state_chunk)
        co_return false;
    const auto &participants = state->seal_attempt.seal.participants;
    const auto found = std::find_if (
      participants.begin (), participants.end (), [] (const frozen_object_state_t &candidate) {
          return candidate.owner.kind == object_kind_t::user_spot
                 || candidate.owner.kind == object_kind_t::instance_spot;
      });
    const auto &principal = found != participants.end () ? *found : participants.front ();
    const protocol::relocation_object_t object{
      to_wire_object_kind (principal.owner.kind), principal.stable_type, principal.owner.key,
      principal.owner.object_generation, principal.owner.authority_owner_generation};
    for (std::uint32_t ordinal = 0; ordinal != state->manifest.chunk_count; ++ordinal) {
        if (target_failed && target_failed ())
            co_return false;
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
        if (target_failed && target_failed ())
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

task_t<void> maintenance_runtime_t::submit_cutover (
  const eligible_relocation_unit_t::canonical_wire_context_t &context,
  const protocol::relocation_cutover_t &cutover)
{
    try {
        (void) co_await context.send_cutover (cutover);
        co_return;
    }
    catch (const framework_exception_t &error) {
        if (error.kind () != framework_error_kind_t::shutting_down)
            throw;
        // Relocation flow §4.4: a cancelled submit does not decide authority.
        co_return;
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
std::uint32_t boundary_batch_checksum (const std::vector<protocol::relocation_data_t> &records)
{
    relocation_crc32c_accumulator_t accumulator;
    for (const auto &record : records) {
        const auto bytes = protocol::encode_relocation_control (record);
        accumulator.update (bytes);
    }
    return accumulator.value ();
}
} // namespace

std::shared_ptr<void> maintenance_runtime_t::begin_pending_relocation_unit () noexcept
{
    auto tracking = _shutdown_tracking;
    tracking->pending_units.fetch_add (1, std::memory_order_acq_rel);
    /* The deleter (not `this`) owns the decrement, so it runs correctly
     * even if this maintenance_runtime_t is torn down while the token is
     * still held (Message Follow retention outliving the runtime). */
    return std::shared_ptr<void> (static_cast<void *> (nullptr), [tracking] (void *) {
        tracking->pending_units.fetch_sub (1, std::memory_order_acq_rel);
    });
}

void maintenance_runtime_t::retain_message_follow (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    /* Confirmed target commit (28 §4.4, §10): the source releases its payload
     * and boundary copies and keeps only the Message Follow route. S4, the
     * route becoming removable (18 §2.4), is MessageFollowDuration after the
     * cutover submit terminal; route_convergence and the SafeToShutdown
     * pending-unit token both end there. */
    state->payload.clear ();
    state->payload.shrink_to_fit ();
    state->records.reset ();
    state->cutover_record.reset ();
    auto tracking = _shutdown_tracking;
    const auto cutover_terminal_at = state->cutover_terminal_at;
    auto token = std::move (state->pending_unit_token);
    const auto deadline = cutover_terminal_at + _limits.message_follow_duration;
    const auto now = std::chrono::steady_clock::now ();
    auto retention = std::make_shared<task_t<void>> (
      deadline > now
        ? ::zlink::framework::detail::delay (
            _delay_owner, std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now))
        : task_t<void> (result_t<void>::success ()));
    detail::observe_task_terminal (
      *retention,
      [retention, tracking, cutover_terminal_at, metrics = _metrics, kind = state->source.kind,
       token = std::move (token)] (const result_t<void> &result) mutable {
          {
              std::lock_guard lock (tracking->metric_mutex);
              if (result && tracking->route_convergence_metric) {
                  const auto elapsed = std::chrono::duration<double> (
                    std::chrono::steady_clock::now () - cutover_terminal_at);
                  tracking->route_convergence_metric (elapsed.count ());
              }
          }
          if (result && metrics)
              metrics->convergence (kind, std::chrono::duration<double> (
                                            std::chrono::steady_clock::now () - cutover_terminal_at)
                                            .count ());
          token.reset ();
      });
}

task_t<source_relocation_settlement_t>
maintenance_runtime_t::settle_relocation (std::shared_ptr<relocation_terminal_state_t> state,
                                          std::vector<object_ref_t> participants)
{
    /* Source authority settlement (28 §4.4, 01 §6.1/§10), the only place that
     * ends a unit after relay-ready: the cutover submit terminal is neither
     * success nor failure. Before the Restore deadline the source reads the
     * primary authority row; from it on, it runs the Preserve fence. An
     * indeterminate result repeats under the Store-failure policy. */
    const auto &primary = relocation_primary (participants);
    const auto &coordinator = state->context.coordinator;
    const relocation_authority_fence_t fence{
      primary.kind,
      primary.key,
      coordinator.expected_authority_store_version,
      location_owner_token_t{coordinator.owner_id,
                             static_cast<std::int64_t> (coordinator.lease_generation)},
      state->target_owner,
      state->context.relocation};
    for (;;) {
        const auto preserve = std::chrono::steady_clock::now () >= state->context.restore_deadline;
        const auto settled = _authority->settle_source_relocation (fence, participants, preserve);
        if (settled.outcome != relocation_authority_t::unsettled)
            co_return settled;
        const auto stop_at = _delay_owner ? _delay_owner->stop_deadline () : std::nullopt;
        if (stop_at && std::chrono::steady_clock::now () >= *stop_at)
            co_return source_relocation_settlement_t{};
        // Relocation flow §4.4: only a different selected route can
        // require another complete boundary batch. Wait for its admission.
        const auto route = state->context.target_route ? state->context.target_route ()
                                                       : std::pair<std::uint64_t, bool>{0, false};
        if (!preserve && route.second && route.first != state->submitted_route_generation) {
            state->submitted_route_generation = route.first;
            if (co_await send_boundary_records (state->context, *state->records, state->batch))
                co_await submit_cutover (state->context, *state->cutover_record);
        }
        try {
            co_await ::zlink::framework::detail::delay (
              _delay_owner, dispatch_limits::management_retry_interval, stop_at.has_value ());
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::shutting_down)
                throw;
            // A cancelled management delay does not decide authority. Read it
            // again before the existing host teardown deadline can end the unit.
        }
    }
}

task_t<std::pair<relocation_terminal_t, relocation_reason_t>>
maintenance_runtime_t::apply_relocation_settlement (
  const std::shared_ptr<relocation_terminal_state_t> &state,
  const std::vector<object_ref_t> &participants,
  const source_relocation_settlement_t &settled)
{
    const auto token = state->seal_attempt.seal.token;
    const auto deliver = [forward = state->context.send_application,
                          follows = std::make_shared<const decltype (settled.follow_generations)> (
                            settled.follow_generations)] (const object_ref_t &owner,
                                                          const turn_record_t &record) {
        if (!forward)
            return;
        // Heterogeneous lookup avoids allocating a key on each message.
        const auto found = follows->find (std::pair{owner.kind, std::string_view{owner.key}});
        forward (owner, record,
                 found == follows->end () ? std::nullopt
                                          : std::optional<std::uint64_t>{found->second});
    };
    switch (settled.outcome) {
        case relocation_authority_t::target_committed: {
            state->metric->complete (settled.outcome);
            relocation_ingress_batch_t committed;
            (void) co_await _objects.finalize_relocation_cutover (token, deliver, &committed);
            if (_committed_terminal)
                for (const auto &participant : committed.participants)
                    _committed_terminal (participant.owner, participant.records);
            co_return std::pair{relocation_terminal_t::completed, relocation_reason_t::none};
        }
        case relocation_authority_t::source_preserved:
            /* The captured queue and timers, then the retained boundary and
             * post-boundary records, return to the source queue in original
             * acceptance order and dispatch reopens. */
            if (co_await abort_relocation_before_cutover (token, state->context)
                == stateful_error_t::none)
                state->metric->complete (settled.outcome);
            co_return std::pair{relocation_terminal_t::blocked,
                                relocation_reason_t::restore_failed};
        default:
            state->metric->complete (settled.outcome);
            /* Expired-owner terminal: dispatch and Store changes stop for
             * good; each pending request gets Unavailable once. */
            (void) co_await _objects.finalize_relocation_cutover (token, deliver);
            if (_unavailable_terminal) {
                for (const auto &participant : participants) {
                    try {
                        _unavailable_terminal (participant);
                    }
                    catch (...) {
                    }
                }
            }
            co_return settled.outcome == relocation_authority_t::data_lost
              ? std::pair{relocation_terminal_t::data_lost, relocation_reason_t::authority_conflict}
              : std::pair{relocation_terminal_t::recovery_required,
                          relocation_reason_t::owner_lease_expired};
    }
}

task_t<relocation_result_t> maintenance_runtime_t::relocate (
  const object_ref_t &source,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t> &canonical_wire,
  std::stop_token cancellation,
  std::uint64_t advertised_receive_chunk_limit_bytes)
{
    auto prepared = co_await relocate_to_ready (
      source, std::move (target_node_id), std::move (target_owner), encoded_upper_bound,
      inventory_digest, canonical_wire, cancellation, advertised_receive_chunk_limit_bytes);
    if (const auto *failed = std::get_if<relocation_result_t> (&prepared))
        co_return *failed;
    co_return co_await std::get<relocation_source_cleanup_t> (prepared) ();
}

task_t<relocation_preparation_t> maintenance_runtime_t::relocate_to_ready (
  const object_ref_t &source,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t> &canonical_wire,
  std::stop_token cancellation,
  std::uint64_t advertised_receive_chunk_limit_bytes)
{
    if (!canonical_wire) {
        return task_t<relocation_preparation_t> (
          result_t<relocation_preparation_t>::success (relocation_preparation_t{finish (
            {relocation_terminal_t::blocked, relocation_reason_t::restore_failed, std::nullopt})}));
    }
    auto state = std::make_shared<relocation_terminal_state_t> (
      relocation_terminal_state_t{source, std::move (target_node_id), std::move (target_owner),
                                  encoded_upper_bound, inventory_digest, *canonical_wire,
                                  cancellation, std::make_shared<permit_t> (try_acquire ())});
    state->advertised_receive_chunk_limit_bytes = advertised_receive_chunk_limit_bytes;
    if (!*state->permit) {
        return task_t<relocation_preparation_t> (
          result_t<relocation_preparation_t>::success (relocation_preparation_t{
            finish ({relocation_terminal_t::blocked, relocation_reason_t::permit_unavailable,
                     std::nullopt})}));
    }
    state->metric.emplace (*canonical_wire, source.kind);
    return relocate_prepare_ready (std::move (state));
}

task_t<relocation_preparation_t>
maintenance_runtime_t::relocate_prepare_ready (std::shared_ptr<relocation_terminal_state_t> state)
{
    /* The in-flight payload budget is acquired before the source admission
     * seal so a unit waiting on the budget keeps serving messages. Chunk
     * sends are awaited one at a time, so one reservation of the effective
     * chunk size bounds this unit's in-flight bytes. */
    state->effective_chunk_limit =
      std::min<std::uint64_t> (_limits.payload_chunk_limit_bytes ? _limits.payload_chunk_limit_bytes
                                                                 : protocol::relocationChunkBytes,
                               protocol::relocationChunkBytes);
    state->effective_chunk_limit = apply_advertised_receive_chunk_limit (
      state->effective_chunk_limit, state->advertised_receive_chunk_limit_bytes);
    state->budget_reserved = std::min (state->effective_chunk_limit, effective_in_flight_budget ());
    co_await acquire_transfer_budget (state->budget_reserved);
    const auto release_reservation = [this, state] {
        if (state->budget_reserved != 0) {
            release_transfer_budget (state->budget_reserved);
            state->budget_reserved = 0;
        }
    };
    if (!co_await relocate_seal (state)) {
        release_reservation ();
        co_return relocation_preparation_t{std::move (*state->result)};
    }
    if (!co_await relocate_encode (state)) {
        release_reservation ();
        co_return relocation_preparation_t{std::move (*state->result)};
    }
    /* Start the Restore request first: the eager task enqueues the request
     * frame on the ordered connection before its first suspension, so the
     * relocationState chunks sent next arrive after it. A target can reply
     * with relocationFailed while chunks are still in flight; observe that
     * existing terminal between sends so it stops the outbound transfer.
     * Relay-ready still arrives only after every chunk was assembled and
     * restored, and is awaited after the chunk sends complete. */
    auto prepared = relocate_prepare_target (state);
    const auto chunks_sent = co_await relocate_send_state_chunks (state, [&prepared] {
        if (!prepared.await_ready ())
            return false;
        const auto &result = prepared.result ();
        return !result || !result.value ();
    });
    release_reservation ();
    const auto target_ready = co_await prepared;
    if (!target_ready)
        co_return relocation_preparation_t{std::move (*state->result)};
    if (!chunks_sent) {
        /* The target replied ready without every chunk — treat it as an
         * exact target failure before cutover. */
        if (abort_target_before_cutover (state->context))
            (void) co_await abort_relocation_before_cutover (state->seal_attempt.seal.token,
                                                             state->context);
        state->result.emplace (finish (
          {relocation_terminal_t::blocked, relocation_reason_t::restore_failed, std::nullopt}));
        co_return relocation_preparation_t{std::move (*state->result)};
    }
    co_return relocation_preparation_t{
      relocation_source_cleanup_t{[this, state] { return relocate_source_cleanup (state); }}};
}

task_t<relocation_result_t>
maintenance_runtime_t::relocate_source_cleanup (std::shared_ptr<relocation_terminal_state_t> state)
{
    if (!co_await relocate_boundary_and_send (state))
        co_return std::move (*state->result);
    (void) co_await relocate_cutover (state);
    co_return std::move (*state->result);
}

task_t<bool> maintenance_runtime_t::capture_relocation_session_bindings (
  eligible_relocation_unit_t::canonical_wire_context_t &context)
{
    if (!context.capture_session_bindings)
        co_return true;
    auto bindings = co_await context.capture_session_bindings ();
    if (!bindings)
        co_return false;
    context.session_bindings = std::move (*bindings);
    co_return true;
}

task_t<bool>
maintenance_runtime_t::relocate_seal (std::shared_ptr<relocation_terminal_state_t> state)
{
    std::vector<object_ref_t> participants{state->source};
    std::function<task_t<bool> ()> before_capture = [this, state] {
        return capture_relocation_session_bindings (state->context);
    };
    auto seal_task =
      _objects.try_seal_relocation_aggregate (participants, state->cancellation, before_capture);
    auto seal_attempt = co_await seal_task;
    state->seal_attempt = std::move (seal_attempt);
    if (state->seal_attempt.error != stateful_error_t::none
        || state->seal_attempt.seal.participants.size () != 1) {
        state->result.emplace (finish ({relocation_terminal_t::blocked,
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

task_t<bool>
maintenance_runtime_t::relocate_encode (std::shared_ptr<relocation_terminal_state_t> state)
{
    /* The captured payload stays only in source memory: it is chunked onto
     * the wire and retained until authority settles. Nothing is
     * written to the Relocation Store on this path. */
    std::optional<std::pair<relocation_terminal_t, relocation_reason_t>> failed;
    try {
        if (state->context.augment_frozen
            && !state->context.augment_frozen (state->seal_attempt.seal.participants)) {
            failed.emplace (relocation_terminal_t::blocked, relocation_reason_t::restore_failed);
        } else {
            state->payload =
              encode_envelope (state->seal_attempt.seal.participants, state->context.relocation,
                               state->context.application_version, state->context.session_bindings);
            if (!state->payload.empty () && state->payload.size () <= state->encoded_upper_bound)
                state->manifest =
                  plan_relocation_payload (state->payload, state->effective_chunk_limit);
        }
    }
    catch (...) {
        failed.emplace (relocation_terminal_t::store_failed,
                        relocation_reason_t::store_write_failed);
    }
    if (!failed
        && (state->payload.empty () || state->payload.size () > state->encoded_upper_bound
            || state->manifest.chunk_count > protocol::relocationChunkCount
            || state->manifest.total_length > protocol::relocationLogicalBytes))
        failed.emplace (relocation_terminal_t::blocked,
                        relocation_reason_t::payload_bound_exceeded);
    if (!failed) {
        state->metric->record_bytes (state->payload.size ());
        co_return true;
    }
    (void) co_await _objects.abort_relocation_before_cutover (state->seal_attempt.seal.token);
    state->result.emplace (finish ({failed->first, failed->second, std::nullopt}));
    co_return false;
}

task_t<bool>
maintenance_runtime_t::relocate_prepare_target (std::shared_ptr<relocation_terminal_state_t> state)
{
    state->handoff.emplace (target_only_cas_t{{state->source},
                                              state->target_node_id,
                                              state->target_owner,
                                              state->inventory_digest,
                                              state->manifest});
    const std::vector<frozen_object_state_t> participants{
      state->seal_attempt.seal.participants.front ()};
    const auto prepared = co_await runtime::await_result (
      prepare_target (state->context, participants, state->manifest));
    if (!prepared)
        co_return detail::propagate_failure<bool> (prepared,
                                                   "relocation target preparation failed");
    if (prepared.value () != relocation_reason_t::none) {
        (void) co_await abort_relocation_before_cutover (state->seal_attempt.seal.token,
                                                         state->context);
        state->result.emplace (
          finish ({relocation_terminal_t::blocked, prepared.value (), std::nullopt}));
        co_return false;
    }
    co_return true;
}

task_t<bool> maintenance_runtime_t::relocate_boundary_and_send (
  std::shared_ptr<relocation_terminal_state_t> state)
{
    const auto boundary = _objects.begin_relocation_boundary (
      state->seal_attempt.seal.token,
      [forward = state->context.send_application] (const object_ref_t &owner,
                                                   const turn_record_t &record) {
          if (forward)
              forward (owner, record, owner.authority_owner_generation);
      },
      {state->context.coordinator.owner_id, state->context.coordinator.lease_generation,
       state->context.coordinator.node_routing_id, state->context.coordinator.node_generation});
    const auto boundary_error = boundary.first;
    state->batch = boundary.second;
    if (boundary_error != stateful_error_t::none) {
        (void) abort_target_before_cutover (state->context);
        (void) co_await abort_relocation_before_cutover (state->seal_attempt.seal.token,
                                                         state->context);
        state->result.emplace (finish (
          {relocation_terminal_t::blocked, relocation_reason_t::restore_failed, std::nullopt}));
        co_return false;
    }
    state->records = build_boundary_records ({state->seal_attempt.seal.participants.front ()},
                                             state->context, state->batch, state->failure);
    if (state->context.target_route) {
        const auto route = state->context.target_route ();
        state->submitted_route_generation = route.second ? route.first : 0;
    }
    bool records_sent = false;
    if (state->records)
        records_sent =
          co_await send_boundary_records (state->context, *state->records, state->batch);
    if (!records_sent) {
        if (abort_target_before_cutover (state->context))
            (void) co_await abort_relocation_before_cutover (state->seal_attempt.seal.token,
                                                             state->context);
        state->result.emplace (finish (
          {relocation_terminal_t::recovery_required, state->failure, std::nullopt,
           state->records.value_or (std::vector<protocol::relocation_data_t>{}), state->handoff}));
        co_return false;
    }
    co_return true;
}

task_t<bool>
maintenance_runtime_t::relocate_cutover (std::shared_ptr<relocation_terminal_state_t> state)
{
    const protocol::relocation_object_t object{
      to_wire_object_kind (state->source.kind),
      state->seal_attempt.seal.participants.front ().stable_type, state->source.key,
      state->source.object_generation, state->source.authority_owner_generation};
    auto cutover = protocol::relocation_cutover_t{
      state->context.relocation, state->context.target_attempt_generation,
      state->context.coordinator, protocol::relocation_role_t::source, object};
    cutover.boundary_record_count = state->records->size ();
    cutover.boundary_checksum_crc32c = boundary_batch_checksum (*state->records);
    const auto cutover_result = co_await await_result (submit_cutover (state->context, cutover));
    const auto terminal_now = std::chrono::steady_clock::now ();
    state->cutover_terminal_at = terminal_now;
    if (_metrics && state->sealed_at != std::chrono::steady_clock::time_point{})
        _metrics->interruption (
          state->source.kind,
          std::chrono::duration<double> (terminal_now - state->sealed_at).count ());
    const auto stall = state->sealed_at != std::chrono::steady_clock::time_point{}
                         ? terminal_now - state->sealed_at
                         : std::chrono::steady_clock::duration::zero ();
    cutover_result.value ();
    state->cutover_record = cutover;
    // The relocation execution permit ends at the cutover submit terminal.
    state->permit.reset ();
    const std::vector<object_ref_t> participants{state->source};
    const auto settled = co_await settle_relocation (state, participants);
    const auto applied = co_await apply_relocation_settlement (state, participants, settled);
    const auto terminal = applied.first;
    const auto reason = applied.second;
    state->result.emplace (
      finish ({terminal, reason, std::nullopt, *state->records, state->handoff, stall, settled}));
    if (terminal == relocation_terminal_t::completed)
        retain_message_follow (state);
    co_return terminal == relocation_terminal_t::completed;
}

relocation_result_t maintenance_runtime_t::recover (object_kind_t kind,
                                                    const std::string &key,
                                                    stateful_object_runtime_t &target,
                                                    std::stop_token cancellation)
{
    std::optional<authority_relocation_reference_t> authority;
    try {
        authority = _authority->read (kind, key);
    }
    catch (...) {
        return finish ({relocation_terminal_t::recovery_required,
                        relocation_reason_t::authority_publish_failed, std::nullopt});
    }
    if (!authority) {
        return finish (
          {relocation_terminal_t::conflict, relocation_reason_t::authority_conflict, std::nullopt});
    }
    /* Direct transfer keeps the payload's only original in the source
     * process memory. A store-mediated payload recovery path no longer
     * exists; an interrupted transfer whose source is gone is data loss.
     * The unused arguments stay for the recovery-orchestration callers. */
    (void) target;
    (void) cancellation;
    return finish (
      {relocation_terminal_t::data_lost, relocation_reason_t::payload_missing, authority});
}

aggregate_relocation_result_t
maintenance_runtime_t::recover_aggregate (const std::vector<object_ref_t> &sources,
                                          stateful_object_runtime_t &target,
                                          std::stop_token cancellation)
{
    if (sources.size () < 2) {
        return {relocation_terminal_t::conflict, relocation_reason_t::inventory_mismatch, {}};
    }
    std::vector<authority_relocation_reference_t> authority;
    try {
        auto canonical_sources = sources;
        std::sort (canonical_sources.begin (), canonical_sources.end (),
                   [] (const object_ref_t &left, const object_ref_t &right) {
                       if (left.kind != right.kind)
                           return left.kind < right.kind;
                       return left.key < right.key;
                   });
        for (std::size_t index = 1; index != canonical_sources.size (); ++index) {
            if (canonical_sources[index - 1].kind == canonical_sources[index].kind
                && canonical_sources[index - 1].key == canonical_sources[index].key) {
                return {
                  relocation_terminal_t::conflict, relocation_reason_t::inventory_mismatch, {}};
            }
        }
        authority.reserve (sources.size ());
        for (const auto &source : sources) {
            const auto current = _authority->read (source.kind, source.key);
            if (!current || current->source != source) {
                return {
                  relocation_terminal_t::conflict, relocation_reason_t::authority_conflict, {}};
            }
            authority.push_back (*current);
        }
    }
    catch (...) {
        return {relocation_terminal_t::recovery_required,
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
            || current.target_owner.owner_id != root.target_owner.owner_id
            || current.target_owner.lease_generation != root.target_owner.lease_generation) {
            return {relocation_terminal_t::data_lost, relocation_reason_t::inventory_mismatch,
                    authority};
        }
    }

    /* Direct transfer keeps the payload's only original in the source
     * process memory; there is no store-mediated aggregate payload to
     * recover from. */
    (void) target;
    (void) cancellation;
    return {relocation_terminal_t::data_lost, relocation_reason_t::payload_missing, authority};
}

task_t<aggregate_relocation_result_t> maintenance_runtime_t::relocate_aggregate (
  const std::vector<object_ref_t> &sources,
  std::string target_node_id,
  location_owner_token_t target_owner,
  std::size_t encoded_upper_bound,
  inventory_digest_t inventory_digest,
  const std::optional<eligible_relocation_unit_t::canonical_wire_context_t> &canonical_wire,
  std::stop_token cancellation,
  std::uint64_t advertised_receive_chunk_limit_bytes)
{
    if (sources.size () < 2 || !canonical_wire || !_aggregate_authority) {
        co_return aggregate_relocation_result_t{
          relocation_terminal_t::blocked, relocation_reason_t::restore_failed, {}};
    }
    auto persisted_context = *canonical_wire;
    auto permit = std::make_shared<permit_t> (try_acquire ());
    if (!*permit) {
        co_return aggregate_relocation_result_t{
          relocation_terminal_t::blocked, relocation_reason_t::permit_unavailable, {}};
    }
    auto state = std::make_shared<relocation_terminal_state_t> ();
    state->source = relocation_primary (sources);
    state->metric.emplace (persisted_context, state->source.kind);
    const auto seal_attempt = co_await _objects.try_seal_relocation_aggregate (
      sources, cancellation, [this, &persisted_context] {
          return capture_relocation_session_bindings (persisted_context);
      });
    const auto seal_error = seal_attempt.error;
    const auto &seal = seal_attempt.seal;
    if (seal_error != stateful_error_t::none) {
        co_return aggregate_relocation_result_t{
          relocation_terminal_t::blocked, relocation_reason_t::restore_failed, {}};
    }
    state->context = persisted_context;
    state->advertised_receive_chunk_limit_bytes = advertised_receive_chunk_limit_bytes;
    state->effective_chunk_limit =
      std::min<std::uint64_t> (_limits.payload_chunk_limit_bytes ? _limits.payload_chunk_limit_bytes
                                                                 : protocol::relocationChunkBytes,
                               protocol::relocationChunkBytes);
    state->effective_chunk_limit = apply_advertised_receive_chunk_limit (
      state->effective_chunk_limit, state->advertised_receive_chunk_limit_bytes);
    state->seal_attempt = seal_attempt;
    state->sealed_at = std::chrono::steady_clock::now ();
    state->pending_unit_token = begin_pending_relocation_unit ();
    try {
        if (persisted_context.augment_frozen
            && !persisted_context.augment_frozen (state->seal_attempt.seal.participants)) {
            (void) co_await abort_relocation_before_cutover (seal.token, persisted_context);
            co_return aggregate_relocation_result_t{
              relocation_terminal_t::blocked, relocation_reason_t::restore_failed, {}};
        }
        state->payload = encode_envelope (
          state->seal_attempt.seal.participants, persisted_context.relocation,
          persisted_context.application_version, persisted_context.session_bindings);
        state->manifest = plan_relocation_payload (state->payload, state->effective_chunk_limit);
        state->metric->record_bytes (state->payload.size ());
    }
    catch (...) {
    }
    if (state->payload.empty () || state->payload.size () > encoded_upper_bound
        || state->manifest.chunk_count > protocol::relocationChunkCount) {
        (void) co_await abort_relocation_before_cutover (seal.token, persisted_context);
        co_return aggregate_relocation_result_t{
          relocation_terminal_t::blocked, relocation_reason_t::payload_bound_exceeded, {}};
    }
    state->budget_reserved = std::min (state->effective_chunk_limit, effective_in_flight_budget ());
    co_await acquire_transfer_budget (state->budget_reserved);
    auto prepared = std::make_shared<task_t<relocation_reason_t>> (
      prepare_target (persisted_context, state->seal_attempt.seal.participants, state->manifest));
    const auto chunks_sent = co_await relocate_send_state_chunks (state, [prepared] {
        if (!prepared->await_ready ())
            return false;
        const auto &result = prepared->result ();
        return !result || result.value () != relocation_reason_t::none;
    });
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
        (void) co_await abort_relocation_before_cutover (seal.token, persisted_context);
        co_return aggregate_relocation_result_t{relocation_terminal_t::blocked,
                                                target_prepare_reason != relocation_reason_t::none
                                                  ? target_prepare_reason
                                                  : relocation_reason_t::restore_failed,
                                                {}};
    }
    const target_only_cas_t handoff{sources, target_node_id, target_owner, inventory_digest,
                                    state->manifest};
    const auto [boundary_error, batch] = _objects.begin_relocation_boundary (
      seal.token,
      [forward = canonical_wire->send_application] (const object_ref_t &owner,
                                                    const turn_record_t &record) {
          if (forward)
              forward (owner, record, owner.authority_owner_generation);
      },
      {state->context.coordinator.owner_id, state->context.coordinator.lease_generation,
       state->context.coordinator.node_routing_id, state->context.coordinator.node_generation});
    auto failure = relocation_reason_t::restore_failed;
    const auto records =
      boundary_error == stateful_error_t::none
        ? build_boundary_records (seal.participants, *canonical_wire, batch, failure)
        : std::nullopt;
    if (canonical_wire->target_route) {
        const auto route = canonical_wire->target_route ();
        state->submitted_route_generation = route.second ? route.first : 0;
    }
    bool records_sent = false;
    if (records)
        records_sent = co_await send_boundary_records (*canonical_wire, *records, batch);
    if (!records_sent) {
        if (abort_target_before_cutover (*canonical_wire))
            (void) co_await abort_relocation_before_cutover (seal.token, persisted_context);
        co_return aggregate_relocation_result_t{
          relocation_terminal_t::recovery_required,
          failure,
          {},
          records.value_or (std::vector<protocol::relocation_data_t>{}),
          handoff};
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
    const auto principal =
      std::find_if (seal.participants.begin (), seal.participants.end (),
                    [] (const frozen_object_state_t &candidate) {
                        return candidate.owner.kind == object_kind_t::user_spot
                               || candidate.owner.kind == object_kind_t::instance_spot;
                    });
    const auto &root =
      principal != seal.participants.end () ? *principal : seal.participants.front ();
    auto cutover = protocol::relocation_cutover_t{
      canonical_wire->relocation,
      canonical_wire->target_attempt_generation,
      canonical_wire->coordinator,
      protocol::relocation_role_t::source,
      {to_wire_object_kind (root.owner.kind), root.stable_type, root.owner.key,
       root.owner.object_generation, root.owner.authority_owner_generation}};
    cutover.boundary_record_count = records->size ();
    cutover.boundary_checksum_crc32c = boundary_batch_checksum (*records);
    const auto cutover_result = co_await await_result (submit_cutover (*canonical_wire, cutover));
    state->cutover_terminal_at = std::chrono::steady_clock::now ();
    if (_metrics)
        _metrics->interruption (
          state->source.kind,
          std::chrono::duration<double> (state->cutover_terminal_at - state->sealed_at).count ());
    cutover_result.value ();
    state->target_owner = target_owner;
    state->batch = batch;
    state->records = *records;
    state->cutover_record = cutover;
    const auto settled = co_await settle_relocation (state, sources);
    const auto applied = co_await apply_relocation_settlement (state, sources, settled);
    const auto terminal = applied.first;
    const auto reason = applied.second;
    if (terminal == relocation_terminal_t::completed)
        retain_message_follow (state);
    co_return aggregate_relocation_result_t{terminal, reason, {}, *records, handoff, settled};
}

relocation_gate_snapshot_t maintenance_runtime_t::gate_snapshot () const
{
    std::lock_guard lock (_gate_mutex);
    return _gate;
}

std::uint64_t maintenance_runtime_t::apply_advertised_receive_chunk_limit (
  std::uint64_t local_limit_bytes, std::uint64_t advertised_receive_chunk_limit_bytes) noexcept
{
    if (advertised_receive_chunk_limit_bytes == 0)
        return local_limit_bytes;
    return std::min (local_limit_bytes, advertised_receive_chunk_limit_bytes);
}

std::uint32_t maintenance_runtime_t::crc32c (const std::vector<std::uint8_t> &payload) noexcept
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto byte : payload) {
        crc ^= byte;
        for (int bit = 0; bit != 8; ++bit)
            crc =
              (crc >> 1)
              ^ (0x82f63b78u & static_cast<std::uint32_t> (-static_cast<std::int32_t> (crc & 1u)));
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
bool participant_order_less (const object_ref_t &left, const object_ref_t &right) noexcept
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
  const protocol::relocation_id_t &relocation,
  std::uint64_t application_version,
  const std::vector<relocation_session_binding_t> &session_bindings)
{
    if (participants.empty () || (relocation.high == 0 && relocation.low == 0))
        return {};
    const auto ordered = canonical_participants (participants);
    for (std::size_t index = 0; index != ordered.size (); ++index) {
        const auto &frozen = *ordered[index];
        if (frozen.owner.key.empty () || frozen.stable_type.empty ()
            || frozen.owner.object_generation == 0 || frozen.owner.authority_owner_generation == 0
            || frozen.application_state.size () > max_application_state_bytes
            || frozen.pending_application.size () > max_pending_records
            || frozen.timers.size () > max_logical_timers)
            return {};
        if (index != 0 && !participant_order_less (ordered[index - 1]->owner, frozen.owner))
            return {};
    }
    const auto principal = std::find_if (
      participants.begin (), participants.end (), [] (const frozen_object_state_t &candidate) {
          return candidate.owner.kind == object_kind_t::user_spot
                 || candidate.owner.kind == object_kind_t::instance_spot;
      });
    const auto &root = principal != participants.end () ? *principal : participants.front ();

    protocol::relocation_envelope_t envelope;
    envelope.relocation = relocation;
    envelope.object = {to_wire_object_kind (root.owner.kind), root.stable_type, root.owner.key,
                       root.owner.object_generation, root.owner.authority_owner_generation};
    if (application_version
        > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ()))
        return {};
    envelope.application_version = static_cast<std::int64_t> (application_version);
    for (std::size_t index = 0; index != ordered.size (); ++index) {
        const auto &participant = *ordered[index];
        const auto binding = std::find_if (
          session_bindings.begin (), session_bindings.end (),
          [&participant] (const auto &candidate) {
              return participant.owner.kind == object_kind_t::actor
                     && candidate.first.actor_id == participant.owner.key
                     && candidate.first.object_generation == participant.owner.object_generation;
          });
        if (binding != session_bindings.end ()) {
            auto token = binding->second;
            token.participant_id = static_cast<std::uint64_t> (index + 1);
            envelope.session_bindings.push_back (std::move (token));
        }
    }
    if (envelope.session_bindings.size () != session_bindings.size ())
        throw protocol::service_wire_error_t (
          "relocation Session binding does not match the participant inventory");


    try {
        for (std::size_t index = 0; index != ordered.size (); ++index) {
            const auto &frozen = *ordered[index];
            const auto participant_id = static_cast<std::uint64_t> (index) + 1;
            envelope.application_states.push_back (
              {participant_id, true, frozen.application_state});

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
                        canonical =
                          protocol::encode_frozen_application_record (*record.application_record);
                    else
                        canonical = protocol::decode_frozen_record (record.payload);
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

            std::vector<std::pair<std::string, const logical_timer_t *>> timers;
            timers.reserve (frozen.timers.size ());
            for (const auto &timer : frozen.timers) {
                auto name = timer.name.empty () ? std::to_string (timer.timer_id) : timer.name;
                timers.emplace_back (std::move (name), &timer);
            }
            std::sort (timers.begin (), timers.end (), [] (const auto &left, const auto &right) {
                return left.first < right.first;
            });
            for (std::size_t position = 1; position < timers.size (); ++position) {
                if (timers[position - 1].first == timers[position].first)
                    return {};
            }
            for (const auto &[name, timer] : timers) {
                const auto bare = timer->name.empty ();
                protocol::relocation_envelope_timer_t registration;
                registration.participant_id = participant_id;
                registration.name = name;
                registration.handler_type = timer->handler_type.empty ()
                                              ? std::string (bare_timer_handler_type)
                                              : timer->handler_type;
                registration.period_milliseconds =
                  timer->period_milliseconds != 0
                    ? timer->period_milliseconds
                    : (timer->due_after_milliseconds != 0 ? timer->due_after_milliseconds : 1);
                registration.overrun_policy =
                  timer->overrun_policy >= 1 && timer->overrun_policy <= 3 ? timer->overrun_policy
                                                                           : 1;
                registration.max_catch_up_ticks =
                  timer->max_catch_up_ticks != 0 ? timer->max_catch_up_ticks : 1;
                registration.stop_on_unhandled_exception = timer->stop_on_unhandled_exception;
                if (bare) {
                    const auto completed =
                      timer->next_tick_sequence != 0 ? timer->next_tick_sequence - 1 : 0;
                    registration.last_completed_delivery_index = completed;
                    registration.last_completed_scheduled_index = completed;
                    registration.next_scheduled_at_unix_milliseconds =
                      timer->due_after_milliseconds;
                } else {
                    registration.last_completed_delivery_index =
                      timer->last_completed_delivery_index;
                    registration.last_completed_scheduled_index =
                      timer->last_completed_scheduled_index;
                    registration.next_scheduled_at_unix_milliseconds =
                      timer->next_scheduled_at_unix_milliseconds != 0
                        ? timer->next_scheduled_at_unix_milliseconds
                        : timer->due_after_milliseconds;
                }
                envelope.timer_registrations.push_back (std::move (registration));
                for (const auto &tick : timer->pending_ticks) {
                    ++boundary;
                    envelope.pending_timer_ticks.push_back (
                      {participant_id, boundary, name, tick.delivery_index, tick.scheduled_index,
                       tick.scheduled_at_unix_milliseconds, tick.skipped_ticks});
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
maintenance_runtime_t::decode_envelope (const std::vector<std::uint8_t> &payload) noexcept
try {
    if (payload.empty () || payload.size () > max_envelope_bytes)
        return std::nullopt;
    return protocol::decode_relocation_envelope (payload);
}
catch (...) {
    return std::nullopt;
}

std::optional<std::vector<frozen_object_state_t>> maintenance_runtime_t::materialize_envelope (
  const protocol::relocation_envelope_t &envelope,
  std::vector<relocation_participant_identity_t> inventory) noexcept
try {
    if (inventory.empty () || inventory.size () != envelope.application_states.size ())
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
        if (index != 0 && !participant_order_less (inventory[index - 1].owner, identity.owner))
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
        participants.push_back (frozen_object_state_t{.owner = identity.owner,
                                                      .stable_type = identity.stable_type,
                                                      .application_state = state.state,
                                                      .pending_application = {},
                                                      .timers = {}});
    }

    for (const auto &entry : envelope.saved_work) {
        if (entry.participant_id == 0 || entry.participant_id > participants.size ())
            return std::nullopt;
        auto &participant = participants[static_cast<std::size_t> (entry.participant_id) - 1];
        std::optional<std::size_t> application_payload_bytes;
        if (entry.record.application)
            application_payload_bytes =
              protocol::application_payload_hwm_bytes (*entry.record.application);
        participant.pending_application.push_back (
          turn_record_t{entry.order, entry.record.canonical_bytes, application_payload_bytes,
                        std::nullopt, entry.record});
    }

    /* Rebuild the runtime timer model. A schema registration whose handler
     * type is the bare sentinel and whose name is the decimal timer id was
     * written from a bare C++ registration and reconstructs it exactly;
     * anything else keeps the enriched fields and derives the local timer
     * id from the name when it is numeric, or sequentially otherwise. */
    std::map<std::uint64_t, std::set<std::uint64_t>> used_ids;
    for (const auto &registration : envelope.timer_registrations) {
        if (registration.participant_id == 0 || registration.participant_id > participants.size ())
            return std::nullopt;
        auto &participant =
          participants[static_cast<std::size_t> (registration.participant_id) - 1];
        auto &ids = used_ids[registration.participant_id];
        std::uint64_t timer_id = 0;
        if (all_digits (registration.name)) {
            std::uint64_t parsed = 0;
            const auto *first = registration.name.data ();
            const auto *last = first + registration.name.size ();
            if (std::from_chars (first, last, parsed).ec == std::errc{} && parsed != 0
                && !ids.contains (parsed))
                timer_id = parsed;
        }
        if (timer_id == 0) {
            timer_id = ids.empty () ? 1 : *ids.rbegin () + 1;
            while (ids.contains (timer_id))
                ++timer_id;
        }
        ids.insert (timer_id);
        const auto bare =
          registration.handler_type == bare_timer_handler_type && all_digits (registration.name);
        logical_timer_t timer;
        timer.timer_id = timer_id;
        timer.period_milliseconds = registration.period_milliseconds;
        timer.due_after_milliseconds = registration.next_scheduled_at_unix_milliseconds != 0
                                         ? registration.next_scheduled_at_unix_milliseconds
                                         : registration.period_milliseconds;
        timer.next_tick_sequence = registration.last_completed_delivery_index + 1;
        if (!bare) {
            timer.name = registration.name;
            timer.handler_type = registration.handler_type;
            timer.overrun_policy = registration.overrun_policy;
            timer.max_catch_up_ticks = registration.max_catch_up_ticks;
            timer.stop_on_unhandled_exception = registration.stop_on_unhandled_exception;
            timer.last_completed_delivery_index = registration.last_completed_delivery_index;
            timer.last_completed_scheduled_index = registration.last_completed_scheduled_index;
            timer.next_scheduled_at_unix_milliseconds =
              registration.next_scheduled_at_unix_milliseconds;
        }
        participant.timers.push_back (std::move (timer));
    }
    for (const auto &tick : envelope.pending_timer_ticks) {
        if (tick.participant_id == 0 || tick.participant_id > participants.size ())
            return std::nullopt;
        auto &participant = participants[static_cast<std::size_t> (tick.participant_id) - 1];
        const auto registration =
          std::find_if (envelope.timer_registrations.begin (), envelope.timer_registrations.end (),
                        [&tick] (const protocol::relocation_envelope_timer_t &candidate) {
                            return candidate.participant_id == tick.participant_id
                                   && candidate.name == tick.timer_name;
                        });
        if (registration == envelope.timer_registrations.end ())
            return std::nullopt;
        const auto position = static_cast<std::size_t> (std::distance (
          std::find_if (envelope.timer_registrations.begin (), envelope.timer_registrations.end (),
                        [&tick] (const auto &candidate) {
                            return candidate.participant_id == tick.participant_id;
                        }),
          registration));
        if (position >= participant.timers.size ())
            return std::nullopt;
        participant.timers[position].pending_ticks.push_back (
          {tick.delivery_index, tick.scheduled_index, tick.scheduled_at_unix_milliseconds,
           tick.skipped_ticks});
    }
    return participants;
}
catch (...) {
    return std::nullopt;
}

inventory_digest_t
maintenance_runtime_t::compute_inventory_digest (const std::vector<object_ref_t> &participants)
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
            seed.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
        for (int shift = 56; shift >= 0; shift -= 8) {
            seed.push_back (static_cast<std::byte> (source.object_generation >> shift));
            seed.push_back (static_cast<std::byte> (source.authority_owner_generation >> shift));
        }
    }
    const auto digest = runtime::sha256 (seed);
    inventory_digest_t output{};
    for (std::size_t index = 0; index != output.size (); ++index)
        output[index] = std::to_integer<std::uint8_t> (digest[index]);
    return output;
}

maintenance_runtime_t::permit_t::permit_t (maintenance_runtime_t *owner) : _owner (owner)
{
}

maintenance_runtime_t::permit_t::~permit_t ()
{
    if (_owner)
        _owner->release ();
}

maintenance_runtime_t::permit_t::permit_t (permit_t &&other) noexcept :
    _owner (std::exchange (other._owner, nullptr))
{
}

maintenance_runtime_t::permit_t &
maintenance_runtime_t::permit_t::operator= (permit_t &&other) noexcept
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

maintenance_runtime_t::permit_t maintenance_runtime_t::try_acquire ()
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

relocation_result_t maintenance_runtime_t::finish (relocation_result_t result)
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
  std::function<task_t<void> ()> seal_targets) :
    _objects (objects),
    _sessions (sessions),
    _seal_targets (std::move (seal_targets)),
    _lane_executor (),
    _lane (_lane_executor)
{
}

task_t<termination_result_t> host_maintenance_runtime_t::terminate ()
{
    struct start_t
    {
        std::shared_ptr<task_completion_source_t<termination_result_t>> completion;
        std::optional<termination_result_t> immediate;
        bool started = false;
    };
    auto start = _lane
                   .run ([this] {
                       start_t start;
                       if (_terminal)
                           start.immediate = *_terminal;
                       else if (_active_completion)
                           start.completion = _active_completion;
                       else {
                           start.completion =
                             std::make_shared<task_completion_source_t<termination_result_t>> ();
                           _active_completion = start.completion;
                           start.started = true;
                       }
                       return start;
                   })
                   .get ();
    if (start.immediate) {
        return task_t<termination_result_t> (
          result_t<termination_result_t>::success (*start.immediate));
    }
    auto output = start.completion->task ();
    if (!start.started)
        return output;
    auto running = std::make_shared<task_t<termination_result_t>> (run_termination ());
    detail::observe_task_completion (*running, [completion = start.completion, running] (
                                                 const result_t<termination_result_t> &settled) {
        completion->complete (settled);
    });
    return output;
}

task_t<termination_result_t> host_maintenance_runtime_t::run_termination ()
{
    const auto result = co_await run_shutdown ();
    complete_termination (result);
    co_return result;
}

task_t<termination_result_t> host_maintenance_runtime_t::run_shutdown ()
{
    if (_seal_targets)
        co_await _seal_targets ();
    const auto inventory = co_await _objects.try_begin_maintenance_inventory ();
    if (!inventory) {
        co_return termination_result_t{termination_outcome_t::force_stopped,
                                       termination_reason_t::teardown_failed};
    }
    const auto sealed = _sessions.try_seal_all ();
    if (!sealed)
        _sessions.force_close_all ();
    co_return termination_result_t{
      sealed ? termination_outcome_t::stopped : termination_outcome_t::force_stopped,
      sealed ? termination_reason_t::none : termination_reason_t::teardown_failed};
}

void host_maintenance_runtime_t::complete_termination (const termination_result_t &result)
{
    _lane
      .run ([this, &result] {
          _terminal = result;
          _active_completion.reset ();
      })
      .get ();
}

} // namespace zlink::framework::runtime::stateful

namespace zlink::framework::runtime::host
{

void public_host_runtime_t::attach_relocation_terminals (
  stateful::maintenance_runtime_t &maintenance)
{
    if (_delay_owner)
        maintenance.bind_delay_owner (*_delay_owner);
    maintenance.attach_relocation_wire (*_relocation_wire,
                                        [this] { return _stateful_dispatch.get (); });
    maintenance.attach_committed_terminal (
      [this] (const stateful::object_ref_t &owner,
              const std::vector<stateful::turn_record_t> &committed) {
          if (_stateful_dispatch)
              _stateful_dispatch->release_relocated_payloads (owner, committed);
      });
    maintenance.attach_unavailable_terminal ([this] (const stateful::object_ref_t &owner) {
        if (_stateful_dispatch)
            (void) _stateful_dispatch->fail_pending_unavailable (owner);
    });
}

void public_host_runtime_t::configure_relocation (
  std::shared_ptr<stateful::authority_relocation_port_t> authority,
  std::shared_ptr<stateful::relocation_store_port_t> relocations,
  std::shared_ptr<stateful::aggregate_authority_port_t> aggregate_authority,
  stateful::relocation_limits_t limits,
  stateful::maintenance_runtime_t::observer_t relocation_observer)
{
    if (!authority || !relocations)
        throw std::invalid_argument ("relocation providers must not be null");
    std::lock_guard lock (_mutex);
    if (_started || _maintenance || _termination) {
        throw std::logic_error ("relocation providers must be configured once before host start");
    }
    _relocation_authority = authority;
    _aggregate_relocation_authority = aggregate_authority;
    _session_relocations = relocations;
    _relocation_cutover_wait = limits.cutover_wait_timeout;
    auto maintenance = std::make_unique<stateful::maintenance_runtime_t> (
      _objects, std::move (authority), std::move (relocations), limits,
      std::move (relocation_observer), std::move (aggregate_authority));
    maintenance->configure_metrics (_relocation_metrics);
    attach_relocation_terminals (*maintenance);
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
        maintenance_ptr->configure_route_convergence_metric (std::move (route_convergence_metric));
}

void public_host_runtime_t::configure_maintenance (
  stateful::maintenance_provider_set_t providers,
  stateful::relocation_limits_t limits,
  stateful::maintenance_runtime_t::observer_t relocation_observer)
{
    std::lock_guard lock (_mutex);
    if (_started || _maintenance || _termination) {
        throw std::logic_error ("maintenance providers must be configured once before host start");
    }
    _relocation_authority = providers.authority;
    _aggregate_relocation_authority = providers.aggregate_authority;
    _session_relocations = providers.relocations;
    _relocation_cutover_wait = limits.cutover_wait_timeout;
    auto maintenance = std::make_unique<stateful::maintenance_runtime_t> (
      _objects, std::move (providers), limits, std::move (relocation_observer));
    maintenance->configure_metrics (_relocation_metrics);
    attach_relocation_terminals (*maintenance);
    auto termination = std::make_unique<stateful::host_maintenance_runtime_t> (
      _objects, _sessions, [this] { return seal_relocation_targets (); });
    _maintenance = std::move (maintenance);
    _termination = std::move (termination);
    _maintenance_closing = [this] { _termination->terminate ().result ().value (); };
}

stateful::maintenance_runtime_t *public_host_runtime_t::maintenance () noexcept
{
    std::lock_guard lock (_mutex);
    return _maintenance.get ();
}

stateful::host_maintenance_runtime_t *public_host_runtime_t::termination () noexcept
{
    std::lock_guard lock (_mutex);
    return _termination.get ();
}

} // namespace zlink::framework::runtime::host
