/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework.hpp>

namespace zlink::framework::test
{
inline void wait_for_serving (service_provider_t &services)
{
    auto serving = std::make_shared<task_completion_source_t<void>> ();
    auto observation =
      services.get_required<framework_runtime_t> ().observe (1, [serving] (const auto &event) {
          if (event.status.state == framework_runtime_state_t::serving)
              serving->complete (result_t<void>::success ());
          else if (event.status.state == framework_runtime_state_t::error
                   || event.status.state == framework_runtime_state_t::stopped)
              serving->complete (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                          "Host startup ended before Serving"));
      });
    auto result = serving->task ().result ();
    observation->close ();
    result.value ();
}

} // namespace zlink::framework::test
