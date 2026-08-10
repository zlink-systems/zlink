/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/diagnostics/runtime_observation.hpp"

#include "runtime/dispatch/offload_executor.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace zlink::framework::observation_detail
{

namespace
{

runtime::offload_executor_t &runtime_observation_dispatcher ()
{
    const auto hardware = std::max (2u, std::thread::hardware_concurrency ());
    const auto maximum = std::min<std::size_t> (8, hardware);
    static runtime::offload_executor_t dispatcher (
      2,
      maximum,
      0,
      std::chrono::seconds (30),
      "zlink-observation");
    return dispatcher;
}

} // namespace

bool schedule_runtime_observation_work (
  std::function<void ()> work) noexcept
{
    try {
        return runtime_observation_dispatcher ().try_submit_internal (
          std::move (work));
    }
    catch (...) {
        return false;
    }
}

} // namespace zlink::framework::observation_detail
