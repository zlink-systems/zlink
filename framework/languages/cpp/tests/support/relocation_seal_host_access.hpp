/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "stateful_ingress_fixture.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static stateful::raw_stateful_dispatch_t &dispatch (public_host_runtime_t &host)
    {
        return *host._stateful_dispatch;
    }

    static auto dispatch_claim (public_host_runtime_t &host,
                                std::shared_ptr<mesh::service_mailbox_claim_t> claim,
                                mesh::service_mailbox_t::application_dispatch_t dispatch)
    {
        return host.dispatch_application_claim (std::move (claim), std::move (dispatch));
    }
    static auto dispatch_owner (public_host_runtime_t &host,
                                stateful::object_ref_t owner,
                                mesh::service_mailbox_t::application_dispatch_t dispatch)
    {
        return host.dispatch_stateful_owner (owner, dispatch);
    }

    static auto cut (public_host_runtime_t &host, std::vector<stateful::object_ref_t> owners)
    {
        return host.cut_relocation_source (
          std::move (owners), std::make_shared<stateful::relocation_seal_binding_t> ());
    }
    static auto ingest_cut (public_host_runtime_t &host,
                            stateful::object_ref_t owner,
                            mesh::service_mailbox_record_t original)
    {
        return host.ingest_cut_original (std::move (owner), std::move (original));
    }

    static void stage_relay (public_host_runtime_t &host,
                             const protocol::relocation_prepare_t &prepare,
                             const stateful::object_ref_t &source,
                             const stateful::object_ref_t &target,
                             stateful::relocation_restore_identity_t verified_restore = {})
    {
        public_host_runtime_t::relocation_target_attempt_t attempt;
        attempt.prepare = prepare;
        attempt.cutover_received = !verified_restore.reference.empty ();
        attempt.restore_identity = std::move (verified_restore);
        attempt.sources = {source};
        attempt.targets = {target};
        host._relocation_target_attempts[{1, 2, 1}] = std::move (attempt);
    }

    template <typename Header>
    static auto accept_relay (public_host_runtime_t &host,
                              const stateful::object_ref_t &target,
                              const Header &header,
                              std::vector<std::uint8_t> source)
    {
        auto retained = zlink::framework::test::retained_ingress_snapshot (host._objects, target);
        if constexpr (std::is_same_v<Header, protocol::actor_message_header_t>)
            return host._stateful_dispatch
              ->accepted_actor_authority (target, header, std::move (source), 1, std::nullopt,
                                          false, std::move (retained))
              .result ()
              .value ();
        else
            return host._stateful_dispatch
              ->accepted_spot_authority (target, header, std::move (source), 1, std::nullopt,
                                         std::move (retained))
              .result ()
              .value ();
    }
};
} // namespace zlink::framework::runtime::host
