/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Canonical relocation-envelope-v1 logical stream (28-relocation-flow §4.2,
 * framework/runtime/protocol/golden/relocation-envelope-v1.json).
 *
 * The direct-transfer payload is this schema stream and nothing else: a
 * big-endian field stream with no provider envelope, in declaration order
 * `relocation`, `object`, `applicationVersion`, `applicationStates`,
 * `savedWork`, `timerRegistrations`, `pendingTimerTicks`.  Participant
 * identity is deliberately absent: `participantId` is the zero-based index
 * plus one into the target's canonical participant inventory, which the
 * target reconstructs from Location Store authority keys sorted by UTF-8
 * authority-key bytes.  Integrity rides the command-40 manifest CRC-32C
 * over the complete stream; the stream itself carries no checksum.
 *
 * This codec owns the bytes only.  Mapping between the runtime's frozen
 * object model and this stream belongs to the relocation runtime
 * (maintenance_runtime), not here. */

#include "runtime/protocol/service_wire_codec.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zlink::framework::runtime::protocol
{

struct relocation_envelope_application_state_t
{
    std::uint64_t participant_id = 0;
    /* Discriminator 0: no state.  1: state payload.  2: state payload plus
     * an opaque canonical participant-recovery body (durable direct-join);
     * this codec retains the recovery bytes without interpreting them. */
    std::uint8_t state_encoding = 0;
    std::vector<std::uint8_t> state;
    std::vector<std::uint8_t> recovery;

    friend bool operator== (const relocation_envelope_application_state_t &,
                            const relocation_envelope_application_state_t &)
      = default;
};

struct relocation_envelope_saved_work_t
{
    std::uint64_t participant_id = 0;
    std::uint64_t order = 0;
    /* One self-delimiting canonical frozen record (service-wire-v1). The
     * decoded summary retains its exact bytes in canonical_bytes. */
    frozen_record_t record;

    friend bool operator== (const relocation_envelope_saved_work_t &,
                            const relocation_envelope_saved_work_t &)
      = default;
};

struct relocation_envelope_timer_t
{
    std::uint64_t participant_id = 0;
    std::string name;
    std::string handler_type;
    std::uint64_t period_milliseconds = 0;
    std::uint8_t overrun_policy = 0;
    std::uint64_t max_catch_up_ticks = 0;
    bool stop_on_unhandled_exception = false;
    std::uint64_t last_completed_delivery_index = 0;
    std::uint64_t last_completed_scheduled_index = 0;
    std::uint64_t next_scheduled_at_unix_milliseconds = 0;

    friend bool operator== (const relocation_envelope_timer_t &,
                            const relocation_envelope_timer_t &) = default;
};

struct relocation_envelope_pending_tick_t
{
    std::uint64_t participant_id = 0;
    std::uint64_t order = 0;
    std::string timer_name;
    std::uint64_t delivery_index = 0;
    std::uint64_t scheduled_index = 0;
    std::uint64_t scheduled_at_unix_milliseconds = 0;
    std::uint64_t skipped_ticks = 0;

    friend bool operator== (const relocation_envelope_pending_tick_t &,
                            const relocation_envelope_pending_tick_t &)
      = default;
};

struct relocation_envelope_t
{
    relocation_id_t relocation;
    /* object.stable_type is on the wire only for instanceSpot (kind 3);
     * actor/userSpot object identities carry id + generations only.  For
     * kind 3 the stream has no owner generation; decode reports 1. */
    relocation_object_t object;
    std::int64_t application_version = 1;
    std::vector<relocation_envelope_application_state_t> application_states;
    std::vector<relocation_envelope_saved_work_t> saved_work;
    std::vector<relocation_envelope_timer_t> timer_registrations;
    std::vector<relocation_envelope_pending_tick_t> pending_timer_ticks;

    friend bool operator== (const relocation_envelope_t &,
                            const relocation_envelope_t &) = default;
};

/* Both throw service_wire_error_t on any contract violation. Encoding
 * revalidates the same invariants decoding enforces, so an encoded stream
 * is always decodable and byte-identical on re-encode. */
std::vector<std::uint8_t> encode_relocation_envelope (
  const relocation_envelope_t &envelope);
relocation_envelope_t decode_relocation_envelope (
  std::span<const std::uint8_t> bytes);

} // namespace zlink::framework::runtime::protocol
