/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include <array>
#include <atomic>
#include <iostream>

namespace zlink::framework::test
{
namespace rt = runtime;
namespace mesh = runtime::mesh;
inline mesh::service_mailbox_record_t control_record (std::string owner, unsigned sequence)
{
    return mesh::service_mailbox_record_t::from_parts (
      std::move (owner), mesh::service_mailbox_domain_t::application,
      {zlink::message_t::from (std::to_string (sequence))});
}

// The same observer sees the production owner drain and deliberately broken
// publication/submission adapters. Every focused test checks these controls.
enum class publication_fault_t
{
    none,
    lost_ready,
    duplicate_ready,
    per_record_wrapper
};

inline bool same_owner_drain_observation (const std::string &owner, publication_fault_t fault)
{
    rt::offload_executor_t executor (1);
    mesh::service_mailbox_t mailbox;
    constexpr unsigned count = 32;
    std::atomic_uint publications{0}, submissions{0}, delivered{0};
    std::atomic_bool single{true}, operations_ok{true};
    std::array<std::atomic_uint, count> seen{};
    std::function<void (const std::string &)> drain = [&] (const auto &key) {
        if (!mailbox.begin_application_drain (key))
            return;
        for (;;) {
            std::optional<mesh::service_mailbox_record_t> original;
            auto claim =
              mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, key, count,
                                       65536, [&] (const auto &claim) {
                                           single = single && claim->claimed_messages == 1;
                                           original.emplace (std::move (*claim->records.front ()));
                                           claim->records.front ()->application.reset ();
                                       });
            if (!claim)
                break;
            const auto id = std::stoul (original->parts ().front ().to_string ());
            auto enter = [&, id] {
                ++seen[id];
                ++delivered;
            };
            if (fault == publication_fault_t::per_record_wrapper) {
                ++submissions;
                operations_ok = executor.try_submit_internal (enter) && operations_ok;
            } else
                enter ();
            operations_ok = original->transferred () && operations_ok;
        }
        mailbox.end_application_drain (key);
    };
    mailbox.bind_application_dispatch ({}, [&] (const auto &key) {
        ++publications;
        if (fault == publication_fault_t::lost_ready)
            return;
        const auto copies = fault == publication_fault_t::duplicate_ready ? 2u : 1u;
        for (unsigned i = 0; i != copies; ++i) {
            ++submissions;
            operations_ok =
              executor.try_submit_internal ([&, key] { drain (key); }) && operations_ok;
        }
    });
    mailbox.begin_application_receive_turn ();
    for (unsigned i = 0; i != count; ++i)
        operations_ok = mailbox.try_enqueue (control_record (owner, i)) && operations_ok;
    mailbox.end_application_receive_turn ();
    executor.drain ();
    bool exactly_once = true;
    for (const auto &value : seen)
        exactly_once = exactly_once && value == 1;
    const auto pending = mailbox.pending_messages (mesh::service_mailbox_domain_t::application);
    std::cout << "owner-drain owner=" << owner << " fault=" << static_cast<int> (fault)
              << " delivered=" << delivered << "/" << count << " publications=" << publications
              << " submissions=" << submissions << " single=" << single
              << " exactly_once=" << exactly_once << " pending=" << pending << '\n';
    return operations_ok && delivered == count && single && exactly_once && publications <= 2
           && submissions <= 2 && pending == 0;
}

inline bool same_owner_claim_controls (const std::string &owner = "node")
{
    bool passed = same_owner_drain_observation (owner, publication_fault_t::none);
    for (const auto fault : {publication_fault_t::lost_ready, publication_fault_t::duplicate_ready,
                             publication_fault_t::per_record_wrapper}) {
        const bool detected = !same_owner_drain_observation (owner, fault);
        passed = detected && passed;
        std::cout << "negative control fault=" << static_cast<int> (fault)
                  << " detected=" << detected << '\n';
    }
    return passed;
}
}
