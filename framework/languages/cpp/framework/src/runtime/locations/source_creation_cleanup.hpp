/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <runtime/locations/location_repository.hpp>

namespace zlink::framework::runtime
{

enum class source_creation_cleanup_t
{
    not_owned,
    aborted,
    already_aborted,
    stale,
    conflict,
    store_failed
};

inline source_creation_cleanup_t
cleanup_source_created_reservation (
  const std::shared_ptr<location_repository_t> &store,
  const object_creation_key_t &key,
  const object_reservation_fence_t &fence,
  bool source_created) noexcept
{
    if (!source_created)
        return source_creation_cleanup_t::not_owned;
    try {
        const auto result =
          store->abort ({key, fence}).result ().value ();
        if (std::holds_alternative<object_aborted_t> (result))
            return source_creation_cleanup_t::aborted;
        if (std::holds_alternative<object_already_aborted_t> (
              result))
            return source_creation_cleanup_t::already_aborted;
        if (std::holds_alternative<object_abort_stale_t> (result))
            return source_creation_cleanup_t::stale;
        return source_creation_cleanup_t::conflict;
    }
    catch (...) {
        return source_creation_cleanup_t::store_failed;
    }
}

} // namespace zlink::framework::runtime
