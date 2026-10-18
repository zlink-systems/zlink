/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"

namespace zlink::framework::test
{
inline std::optional<runtime::stateful::retained_relocation_source_t>
retained_ingress_snapshot (runtime::stateful::stateful_object_runtime_t &objects,
                           const runtime::stateful::object_ref_t &owner)
{
    std::optional<runtime::stateful::retained_relocation_source_t> retained;
    (void) objects.find (owner.kind, owner.key, std::nullopt, &retained);
    return retained;
}

inline task_t<runtime::stateful::stateful_error_t>
ingest_stateful_mailbox_record (runtime::stateful::stateful_object_runtime_t &objects,
                                runtime::mesh::raw_mesh_node_owner_t &transport,
                                runtime::stateful::raw_stateful_dispatch_t &dispatch,
                                runtime::stateful::object_ref_t owner)
{
    const auto key = runtime::mesh::service_mailbox_t::application_owner (
      owner.kind == runtime::stateful::object_kind_t::actor ? runtime::host::owner_kind_t::actor
                                                            : runtime::host::owner_kind_t::spot,
      owner.key);
    auto claim =
      transport.mailbox ().try_claim_owner (runtime::mesh::service_mailbox_domain_t::application,
                                            key, 1, runtime::dispatch_limits::receive_batch_bytes);
    if (!claim)
        co_return runtime::stateful::stateful_error_t::not_found;
    auto reservation =
      std::make_shared<runtime::mesh::service_mailbox_claim_t> (std::move (*claim));
    co_return co_await dispatch.ingest_claimed (owner, std::move (reservation->records.front ()),
                                                reservation,
                                                retained_ingress_snapshot (objects, owner));
}
} // namespace zlink::framework::test
