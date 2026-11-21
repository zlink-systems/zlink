/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "manual_deadlines.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>

namespace framework_instance_spot_activation_fixture
{
// Keep deadline callbacks on their production worker, including an already-due callback.
inline std::future<bool> advance_activation_deadline (std::uint64_t deadline_unix_ms)
{
    namespace fw = zlink::framework;
    auto advanced = std::make_shared<std::promise<bool>> ();
    auto result = advanced->get_future ();
    fw::detail::deadline_scheduler_t::instance ().schedule (
      std::chrono::steady_clock::now (), std::make_shared<fw::detail::deadline_state_t> (),
      [deadline_unix_ms, advanced] {
          fw::detail::deadline_scheduler_test_access_t::advance_to (
            fw::runtime::messaging::request_deadline_from_unix_ms (deadline_unix_ms)
            + std::chrono::milliseconds (1));
          advanced->set_value (true);
      });
    return result;
}
} // namespace framework_instance_spot_activation_fixture
