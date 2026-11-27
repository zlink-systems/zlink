/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <runtime/locations/location_repository.hpp>
#include <service_wire_constants.hpp>

namespace zlink::framework::runtime
{
class provider_relocation_repository_t;

namespace relocation_completion
{

enum class delivery_t : std::uint8_t
{
    pending,
    terminal_received,
    already_terminal,
    source_lease_expired
};

struct source_t
{
    std::string owner_id;
    std::uint64_t lease_generation = 0;
    std::string node_rid;
    std::uint64_t node_generation = 0;

    auto operator<=> (const source_t &) const = default;
};

struct entry_t
{
    std::string object_key;
    source_t source;
    std::string operation_id;
    std::uint64_t reply_route_id = 0;
    std::vector<std::byte> reply;
    delivery_t delivery = delivery_t::pending;
};

struct record_t
{
    std::string relocation_id;
    std::vector<entry_t> entries;

    std::size_t pending_deliveries () const;
};

std::string encode (const record_t &record);
record_t decode (std::string_view bytes);

// Supply the serialized InternalFailure replies for the accepted requests
// that have yet to complete. The same check also serves precommit admission.
void require_fits (const record_t &record,
                   std::span<const entry_t> remaining_failures,
                   std::uint64_t limit = protocol::relocationLogicalBytes);

class store_t final
{
  public:
    store_t (location_repository_t &authorities, provider_relocation_repository_t &blobs) :
        _authorities (authorities), _blobs (blobs)
    {
    }

    task_t<record_t> read (authority_snapshot_t authority, std::stop_token cancellation = {});

    // The coordinator supplies the full replacement in canonical object and
    // acceptance order, together with the reservations for unfinished requests.
    task_t<authority_compare_exchange_result_t>
    publish (authority_key_t key,
             authority_snapshot_t expected,
             record_t record,
             std::span<const entry_t> remaining_failures,
             std::stop_token cancellation = {});

  private:
    task_t<std::vector<std::byte>> read_blob (std::string reference, std::stop_token cancellation);

    location_repository_t &_authorities;
    provider_relocation_repository_t &_blobs;
};

} // namespace relocation_completion
} // namespace zlink::framework::runtime
