/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/spots/spot_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/state_lane.hpp"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>

namespace zlink::framework::test::execution
{
class serial_test_blocker_t
{
  public:
    using queue_t = zlink::framework::runtime::serial_execution_queue_t;

    ~serial_test_blocker_t ();

    queue_t::async_work_t work (std::function<void ()> entered = {});

    bool wait_for_entry (std::chrono::milliseconds timeout = std::chrono::seconds (1));

    void release (std::function<void ()> finish = {});

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::optional<queue_t::async_completion_t> _completion;
    std::function<void ()> _finish;
    bool _entered = false;
    bool _release_requested = false;
};

class serial_executor_test_fixture_t
{
  public:
    using queue_t = zlink::framework::runtime::serial_execution_queue_t;

    serial_executor_test_fixture_t (
      zlink::framework::runtime::serial_lane_policy_t policy,
      zlink::framework::runtime::serial_execution_queue_options_t spot_options = {},
      std::size_t workers = 4);

    ~serial_executor_test_fixture_t ();

    // Drains the Spot queue, then every Actor and Timer queue that exists now.
    void drain ();

    std::shared_ptr<zlink::framework::runtime::offload_executor_t> worker;
    zlink::framework::runtime::state_lane_t lane;
    std::shared_ptr<queue_t> spot_queue;
    zlink::framework::detail::spot_serial_executor_t serial;
};
}
