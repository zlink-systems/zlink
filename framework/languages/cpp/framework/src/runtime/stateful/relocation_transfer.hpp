/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

/* Relocation direct-transfer payload chunking and reassembly.
 *
 * The captured relocation payload is held only in source memory and travels
 * to the target as ordered relocationState(52) chunks on the mesh
 * connection.  This module owns the two sides of that transfer:
 *
 * - the source chunk plan: how one encoded payload maps onto a manifest
 *   (total length, chunk count, CRC-32C) and onto individual wire chunks;
 * - the target assembly: ordered reassembly of chunks bound to one exact
 *   relocation identity, with the manifest checks the contract requires
 *   (exact identity match, in-order ordinals, declared length, and a full
 *   payload CRC-32C comparison; a mismatch is an explicit, non-retryable
 *   failure).
 */

#include "runtime/protocol/service_wire_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zlink::framework::runtime::stateful
{

struct relocation_payload_manifest_t
{
    std::uint64_t total_length = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t checksum_crc32c = 0;

    friend bool operator== (const relocation_payload_manifest_t &,
                            const relocation_payload_manifest_t &) = default;
};

/* Incremental CRC-32C (Castagnoli) matching
 * protocol::relocation_checksum_crc32c.  Used for the cutover boundary
 * checksum, which covers the concatenated canonical bytes of the
 * pre-boundary relay batch as the records stream in. */
class relocation_crc32c_accumulator_t
{
  public:
    void update (std::span<const std::uint8_t> bytes) noexcept
    {
        for (const auto byte : bytes) {
            _state ^= byte;
            for (int bit = 0; bit < 8; ++bit) {
                const auto mask = static_cast<std::uint32_t> (
                  -static_cast<std::int32_t> (_state & 1u));
                _state = (_state >> 1u) ^ (0x82f63b78u & mask);
            }
        }
    }

    std::uint32_t value () const noexcept { return ~_state; }

  private:
    std::uint32_t _state = 0xffffffffu;
};

/* The number of chunks one payload occupies under the effective chunk
 * limit.  An empty payload still occupies zero chunks; the manifest then
 * declares length zero and count zero. */
inline std::uint32_t relocation_chunk_count (std::uint64_t total_length,
                                             std::uint64_t chunk_limit) noexcept
{
    if (total_length == 0 || chunk_limit == 0)
        return 0;
    return static_cast<std::uint32_t> (
      (total_length + chunk_limit - 1) / chunk_limit);
}

inline relocation_payload_manifest_t plan_relocation_payload (
  std::span<const std::uint8_t> payload, std::uint64_t chunk_limit)
{
    relocation_payload_manifest_t manifest;
    manifest.total_length = payload.size ();
    manifest.chunk_count = relocation_chunk_count (payload.size (), chunk_limit);
    manifest.checksum_crc32c = protocol::relocation_checksum_crc32c (payload);
    return manifest;
}

/* One chunk of the payload as a relocationState wire record.  The chunk
 * bytes are copied out of the retained source payload; the identity fields
 * bind the chunk to exactly one relocation attempt. */
inline protocol::relocation_state_t make_relocation_state_chunk (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_coordinator_fence_t &coordinator,
  const protocol::relocation_object_t &object,
  std::span<const std::uint8_t> payload,
  std::uint32_t chunk_ordinal,
  std::uint64_t chunk_limit)
{
    protocol::relocation_state_t chunk;
    chunk.relocation = relocation;
    chunk.target_attempt_generation = target_attempt_generation;
    chunk.coordinator = coordinator;
    chunk.sender_role = protocol::relocation_role_t::source;
    chunk.object = object;
    // TODO(schema-atomic): payload_stage is encoded as its default (final)
    // until the wire field is removed in the atomic schema commit.
    chunk.chunk_ordinal = chunk_ordinal;
    const auto offset =
      static_cast<std::uint64_t> (chunk_ordinal) * chunk_limit;
    const auto remaining = payload.size () - offset;
    const auto length = remaining < chunk_limit ? remaining : chunk_limit;
    chunk.chunk_data.assign (payload.begin () + static_cast<std::ptrdiff_t> (offset),
                             payload.begin ()
                               + static_cast<std::ptrdiff_t> (offset + length));
    return chunk;
}

enum class relocation_assembly_result_t
{
    /* The chunk was copied into the assembly and more chunks remain. */
    accepted,
    /* The chunk completed the payload and the manifest CRC-32C matched. */
    completed,
    /* The chunk does not carry this assembly's exact identity; the
     * contract discards it without touching the assembly. */
    ignored,
    /* The chunk carries the exact identity but violates the declared
     * manifest (out-of-order ordinal, overrunning length, or a final
     * CRC-32C mismatch).  This is an explicit failure: the partially
     * assembled payload is discarded and never restored. */
    conflict
};

/* Ordered reassembly of one relocation payload.  Bound to a single exact
 * identity (RelocationId + targetAttemptGeneration + coordinator fence +
 * object) and one declared manifest.  Chunks are copied into the assembly
 * buffer immediately so the caller can release its transport ownership of
 * the arriving record right after the call. */
class relocation_state_assembly_t
{
  public:
    relocation_state_assembly_t (protocol::relocation_id_t relocation,
                                 std::uint64_t target_attempt_generation,
                                 protocol::relocation_coordinator_fence_t coordinator,
                                 protocol::relocation_object_t object,
                                 relocation_payload_manifest_t manifest) :
        _relocation (relocation),
        _target_attempt_generation (target_attempt_generation),
        _coordinator (std::move (coordinator)),
        _object (std::move (object)),
        _manifest (manifest)
    {
        _payload.reserve (static_cast<std::size_t> (manifest.total_length));
    }

    bool complete () const noexcept
    {
        return _next_ordinal == _manifest.chunk_count
               && _payload.size () == _manifest.total_length;
    }

    bool failed () const noexcept { return _failed; }

    const relocation_payload_manifest_t &manifest () const noexcept
    {
        return _manifest;
    }

    relocation_assembly_result_t accept (
      const protocol::relocation_state_t &chunk)
    {
        /* Exact identity is RelocationId + targetAttemptGeneration +
         * coordinator fence; a chunk with a different identity is discarded
         * without touching this assembly. */
        if (chunk.relocation != _relocation
            || chunk.target_attempt_generation != _target_attempt_generation
            || chunk.coordinator != _coordinator)
            return relocation_assembly_result_t::ignored;
        if (_failed || complete ()
            || chunk.chunk_ordinal != _next_ordinal
            || _payload.size () + chunk.chunk_data.size ()
                 > _manifest.total_length) {
            _failed = true;
            _payload.clear ();
            return relocation_assembly_result_t::conflict;
        }
        _payload.insert (_payload.end (), chunk.chunk_data.begin (),
                         chunk.chunk_data.end ());
        ++_next_ordinal;
        if (_next_ordinal != _manifest.chunk_count)
            return relocation_assembly_result_t::accepted;
        if (_payload.size () != _manifest.total_length
            || protocol::relocation_checksum_crc32c (_payload)
                 != _manifest.checksum_crc32c) {
            _failed = true;
            _payload.clear ();
            return relocation_assembly_result_t::conflict;
        }
        return relocation_assembly_result_t::completed;
    }

    /* The fully assembled payload. Valid only after completion. */
    std::vector<std::uint8_t> take_payload () noexcept
    {
        return std::move (_payload);
    }

  private:
    protocol::relocation_id_t _relocation;
    std::uint64_t _target_attempt_generation = 0;
    protocol::relocation_coordinator_fence_t _coordinator;
    protocol::relocation_object_t _object;
    relocation_payload_manifest_t _manifest;
    std::vector<std::uint8_t> _payload;
    std::uint32_t _next_ordinal = 0;
    bool _failed = false;
};

} // namespace zlink::framework::runtime::stateful
