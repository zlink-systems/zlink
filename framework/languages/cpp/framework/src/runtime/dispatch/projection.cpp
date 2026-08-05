/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/dispatch/projection.hpp"

namespace zlink::framework::runtime
{

dispatch_projection_t::dispatch_projection_t (offload_executor_t &offload) : _offload (offload)
{
}

void dispatch_projection_t::register_handler (runtime_event_kind_t kind,
                                              handler_execution_t execution,
                                              handler_t handler)
{
    _handlers[kind] = registered_handler_t{execution, std::move (handler)};
}

void dispatch_projection_t::project (runtime_event_kind_t kind)
{
    runtime_event_t event{kind, _next_sequence++};
    _events.push_back (event);

    const auto found = _handlers.find (kind);
    if (found == _handlers.end ()) {
        return;
    }

    auto handler = found->second.handler;
    if (found->second.execution == handler_execution_t::offload) {
        _offload.submit ([handler = std::move (handler), event] { handler (event); });
        return;
    }

    handler (event);
}

const std::vector<runtime_event_t> &dispatch_projection_t::projected_events () const noexcept
{
    return _events;
}

} // namespace zlink::framework::runtime
