/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/codecs/serializer.hpp>

namespace zlink::framework::detail
{

struct serializer_registry_test_access_t
{
    static void set_resolved_serializer_cache_capacity (
      serializer_registry_t &registry,
      std::size_t capacity) noexcept
    {
        registry.set_resolved_serializer_cache_capacity_for_tests (capacity);
    }
};

} // namespace zlink::framework::detail
