/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace zlink::framework::runtime
{

enum class runtime_event_kind_t
{
    channel_readable,
    stream_readable,
    spot_readable,
    timer_readable
};

struct runtime_event_t
{
    runtime_event_kind_t kind;
    std::uint64_t sequence;
};

class dispatch_projection_t
{
  public:
    using handler_t = std::function<void (const runtime_event_t &)>;

    explicit dispatch_projection_t (offload_executor_t &offload);

    void
    register_handler (runtime_event_kind_t kind, handler_execution_t execution, handler_t handler);
    void project (runtime_event_kind_t kind);
    const std::vector<runtime_event_t> &projected_events () const noexcept;

  private:
    struct registered_handler_t
    {
        handler_execution_t execution;
        handler_t handler;
    };

    offload_executor_t &_offload;
    std::map<runtime_event_kind_t, registered_handler_t> _handlers;
    std::vector<runtime_event_t> _events;
    std::uint64_t _next_sequence = 1;
};

} // namespace zlink::framework::runtime
