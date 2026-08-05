/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace zlink::framework
{

// Framework-private values used by the domain repository above the opaque
// public Store SPI.

enum class location_write_intent_t
{
    new_claim = 1,
    renew = 2,
    takeover = 3
};

enum class location_write_status_t
{
    stored = 1,
    ignored_stale = 2,
    rejected_conflict = 3
};

struct location_write_result_t
{
    location_write_status_t status = location_write_status_t::ignored_stale;
    std::int64_t generation = 0;
    std::chrono::system_clock::time_point updated_at{};

    static location_write_result_t stored (std::int64_t generation,
                                           std::chrono::system_clock::time_point updated_at)
    {
        return location_write_result_t{location_write_status_t::stored, generation,
                                       std::move (updated_at)};
    }
};

struct location_owner_token_t
{
    std::string owner_id;
    std::int64_t lease_generation = 0;
};

struct owner_lease_claimed_t
{
    location_owner_token_t token;
    std::chrono::system_clock::time_point lease_expires_at{};
    std::chrono::system_clock::time_point store_now{};
};
struct owner_lease_conflict_t
{
};
struct owner_lease_generation_exhausted_t
{
};
using owner_lease_claim_result_t = std::variant<
  owner_lease_claimed_t,
  owner_lease_conflict_t,
  owner_lease_generation_exhausted_t>;

struct owner_lease_renewed_t
{
    std::chrono::system_clock::time_point lease_expires_at{};
    std::chrono::system_clock::time_point store_now{};
};
struct owner_lease_stale_t
{
};
using owner_lease_renew_result_t =
  std::variant<owner_lease_renewed_t, owner_lease_stale_t>;

struct owner_lease_released_t
{
};
using owner_lease_release_result_t =
  std::variant<owner_lease_released_t, owner_lease_stale_t>;

struct owner_lease_found_t
{
    location_owner_token_t token;
    std::chrono::system_clock::time_point lease_expires_at{};
    std::chrono::system_clock::time_point store_now{};
};
struct owner_lease_missing_t
{
};
using owner_lease_read_result_t =
  std::variant<owner_lease_found_t, owner_lease_missing_t>;

} // namespace zlink::framework
