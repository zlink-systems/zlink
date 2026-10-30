/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace zlink::framework::detail
{
class mesh_node_runtime_t;
}

namespace zlink::framework::runtime
{
// Owns every resource charged to one admitted application dispatch until its
// synchronous or deferred terminal settles. This internal value is separate
// from the host object so a late terminal never needs to retain or dereference
// the host service itself.
class application_dispatch_terminal_owner_t final
{
  public:
    application_dispatch_terminal_owner_t (std::shared_ptr<detail::mesh_node_runtime_t> node = {},
                                           std::function<void ()> complete_stateful_dispatch = {},
                                           std::function<void ()> release_mailbox_reservation = {});
    ~application_dispatch_terminal_owner_t () noexcept;
    application_dispatch_terminal_owner_t (application_dispatch_terminal_owner_t &&other) noexcept;

    application_dispatch_terminal_owner_t (const application_dispatch_terminal_owner_t &) = delete;
    application_dispatch_terminal_owner_t &
    operator= (const application_dispatch_terminal_owner_t &) = delete;

    void settle () noexcept;
    void retain_physical_work (offload_executor_t::retained_work_t work) noexcept;
    std::function<void ()> take_mailbox_reservation () noexcept;

  private:
    static void invoke (const std::function<void ()> &callback) noexcept;

    std::atomic_bool _settled{false};
    std::shared_ptr<detail::mesh_node_runtime_t> _node;
    offload_executor_t::retained_work_t _physical_work;
    std::function<void ()> _complete_stateful_dispatch;
    std::function<void ()> _release_mailbox_reservation;
};

}
