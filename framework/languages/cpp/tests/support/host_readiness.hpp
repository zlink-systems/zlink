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
      });
    serving->task ().result ().value ();
    observation->close ();
}

} // namespace zlink::framework::test
