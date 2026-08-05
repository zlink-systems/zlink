/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/pending_operation.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace zlink::framework::detail
{

struct pending_operation_state_t
{
    std::atomic_bool completed{false};
    std::atomic_bool cancelled{false};

    bool try_complete () noexcept
    {
        bool expected = false;
        return completed.compare_exchange_strong (expected, true);
    }

    bool try_cancel () noexcept
    {
        if (!try_complete ()) {
            return false;
        }
        cancelled.store (true);
        return true;
    }

    bool try_fail (std::exception_ptr) noexcept { return try_complete (); }
};

} // namespace zlink::framework::detail
