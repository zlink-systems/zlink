/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/codecs/serializer.hpp>

namespace zlink::framework::detail
{

struct serializer_registry_test_access_t
{
    template <typename T>
    static bool has_resolved_serializer (const serializer_registry_t &registry)
    {
        return static_cast<bool> (registry.cached_serializer (std::type_index (typeid (T))));
    }

    static void set_resolved_serializer_cache_capacity (serializer_registry_t &registry,
                                                        std::size_t capacity) noexcept
    {
        registry.set_resolved_serializer_cache_capacity_for_tests (capacity);
    }
};

} // namespace zlink::framework::detail
