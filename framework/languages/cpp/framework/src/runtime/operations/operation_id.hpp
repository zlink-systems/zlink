/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace zlink::framework::runtime
{

// Framework completion, transport, location and wire paths use one runtime
// identity. The aliases in the owning subsystems are naming views, not
// separate two-word representations.
struct operation_id_t
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    friend bool operator== (const operation_id_t &, const operation_id_t &) = default;
};

struct operation_id_hash_t
{
    std::size_t operator() (const operation_id_t &id) const noexcept
    {
        std::size_t value = 1469598103934665603ULL;
        value ^= static_cast<std::size_t> (id.high);
        value *= 1099511628211ULL;
        value ^= static_cast<std::size_t> (id.high >> 32);
        value *= 1099511628211ULL;
        value ^= static_cast<std::size_t> (id.low);
        value *= 1099511628211ULL;
        value ^= static_cast<std::size_t> (id.low >> 32);
        value *= 1099511628211ULL;
        return value;
    }
};

} // namespace zlink::framework::runtime
