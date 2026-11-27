/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/locations/location_repository_values.hpp"
#include <zlink/framework/contracts/locations/stores.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime
{
class provider_location_repository_t;

namespace relocation_completion
{
enum class delivery_t : std::uint8_t
{
    accepted,
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
};

struct row_t
{
    std::string relocation_id;
    std::string object_key;
    source_t source;
    std::string operation_id;
    std::uint64_t reply_route_id = 0;
    location_owner_token_t writer;
    delivery_t state = delivery_t::accepted;
    std::vector<std::byte> reply;
};

std::string prefix (std::string_view relocation_id);
store_key_t key (const row_t &row);
store_put_t accepted (const row_t &row);
bool append_accepted (store_write_request_t &batch, std::span<const store_put_t> rows);

class repository_t final
{
  public:
    explicit repository_t (provider_location_repository_t &locations) : _locations (locations) {}

    // A missing row or ended writer lease retires local delivery work.
    task_t<std::optional<row_t>>
    record_result (row_t identity, std::vector<std::byte> reply, std::vector<std::byte> failure);
    task_t<bool> confirm (row_t identity, delivery_t state);
    task_t<bool> source_lease_live (const source_t &source);
    task_t<std::vector<row_t>> pending (std::string relocation_id);

  private:
    task_t<std::optional<row_t>> update (row_t identity,
                                         delivery_t state,
                                         std::vector<std::byte> reply = {},
                                         std::vector<std::byte> failure = {});

    provider_location_repository_t &_locations;
};
}
}
