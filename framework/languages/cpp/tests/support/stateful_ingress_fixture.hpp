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

inline runtime::mesh::service_mailbox_claim_t
publish_stateful_claims (runtime::mesh::raw_mesh_node_owner_t &transport,
                         const runtime::stateful::object_ref_t &owner,
                         std::vector<runtime::mesh::service_mailbox_record_t> records,
                         std::function<void ()> released = {})
{
    auto &mailbox = transport.mailbox ();
    const auto key = runtime::mesh::service_mailbox_t::application_owner (
      owner.kind == runtime::stateful::object_kind_t::actor ? runtime::host::owner_kind_t::actor
                                                            : runtime::host::owner_kind_t::spot,
      owner.key);
    mailbox.begin_application_receive_turn ();
    for (auto &record : records) {
        record.owner = key;
        record.domain = runtime::mesh::service_mailbox_domain_t::application;
        if (!mailbox.try_enqueue (std::move (record)))
            throw std::runtime_error ("fixture mailbox publication failed");
    }
    auto claim =
      mailbox.try_claim_owner (runtime::mesh::service_mailbox_domain_t::application, key,
                               records.size (), runtime::dispatch_limits::receive_batch_bytes);
    mailbox.end_application_receive_turn ();
    if (!claim || claim->records.size () != records.size ())
        throw std::runtime_error ("fixture mailbox claim failed");
    for (auto &slot : claim->records) {
        auto release = std::exchange (slot->transferred, {});
        slot->transferred = [release = std::move (release), released] {
            release ();
            if (released)
                released ();
        };
    }
    return std::move (*claim);
}

inline runtime::mesh::service_mailbox_t::claim_handle_t
publish_stateful_claim (runtime::mesh::raw_mesh_node_owner_t &transport,
                        const runtime::stateful::object_ref_t &owner,
                        runtime::mesh::service_mailbox_record_t record,
                        std::function<void ()> released = {})
{
    std::vector<runtime::mesh::service_mailbox_record_t> records;
    records.push_back (std::move (record));
    return publish_stateful_claims (transport, owner, std::move (records), std::move (released))
      .records.front ();
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
    co_return co_await dispatch.ingest_claimed (owner, claim->records.front (),
                                                retained_ingress_snapshot (objects, owner));
}
} // namespace zlink::framework::test
