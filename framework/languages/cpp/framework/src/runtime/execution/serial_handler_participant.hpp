/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/dispatch/task.hpp>

namespace zlink::framework::runtime
{
// Readiness observes H and retains M through its release completion.
class serial_handler_participant_t
{
  public:
    virtual ~serial_handler_participant_t () = default;
    virtual task_t<void> run () = 0;
    virtual task_t<void> terminal () = 0;
    virtual void cancel () noexcept = 0;
};
} // namespace zlink::framework::runtime
