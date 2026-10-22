/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>
#include <memory>

namespace zlink::framework::detail
{
// Configuration owns a callable, never the runtime that installs it. An invocation
// holds the runtime only for its accepted work and uses the shutdown boundary on expiry.
template <typename T> std::shared_ptr<T> lock_runtime_owner (const std::weak_ptr<T> &owner)
{
    auto live = owner.lock ();
    if (!live)
        throw make_boundary_exception (boundary_error_t::shutdown, "Framework runtime is stopped");
    return live;
}
} // namespace zlink::framework::detail
