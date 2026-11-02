/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "../utils/available_processor_count.hpp"

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
    constexpr unsigned minimum_worker_count = 2;
    constexpr std::size_t maximum_worker_count = 8;
    constexpr auto worker_idle_timeout = std::chrono::seconds (30);
    const auto processors =
      std::max (minimum_worker_count, ::zlink::framework::runtime::available_processor_count ());
    const auto maximum = std::min<std::size_t> (maximum_worker_count, processors);
    static runtime::offload_executor_t dispatcher (minimum_worker_count, maximum,
                                                   worker_idle_timeout, "zlink-observation");
    return dispatcher;
}

} // namespace

bool schedule_runtime_observation_work (std::function<void ()> work) noexcept
{
    try {
        return runtime_observation_dispatcher ().try_submit_internal (std::move (work));
    }
    catch (...) {
        return false;
    }
}

} // namespace zlink::framework::observation_detail
