/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"
#include <exception>
#include <memory>

namespace zlink::framework::test::spot_close
{
class empty_callback_failure_t final : public std::exception
{
  public:
    explicit empty_callback_failure_t (std::shared_ptr<const int> marker) :
        marker (std::move (marker))
    {
    }
    const char *what () const noexcept override { return ""; }
    std::shared_ptr<const int> marker;
};

// Spot timer §1: a native expiry must remain queued while creation is preparing.
// A FIFO worker marker follows the expiry's runnable queue consumer. All fixtures
// using this marker configure one worker, so observation does not stop the worker.
void observe_scheduled_expiry (
  const std::shared_ptr<zlink::framework::runtime::offload_executor_t> &executor);
} // namespace zlink::framework::test::spot_close
