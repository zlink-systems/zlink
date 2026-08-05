/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>

namespace zlink::framework
{

class location_runtime_query_t
{
  public:
    virtual ~location_runtime_query_t () = default;
    virtual task_t<location_runtime_status_t> get_status () = 0;
    virtual task_t<location_page_t<location_topology_entry_t>>
    list_topology (location_topology_filter_t filter, location_page_request_t page = {}) = 0;
    virtual task_t<location_page_t<location_service_summary_t>>
    list_service_summaries (location_service_summary_filter_t filter,
                            location_page_request_t page = {}) = 0;
};

} // namespace zlink::framework
