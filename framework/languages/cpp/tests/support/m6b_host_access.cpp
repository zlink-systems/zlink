/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6b_host_access.hpp"
#include <memory>
#include <utility>

namespace zlink::framework::runtime::host
{

std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>
public_host_runtime_test_access_t::admit_seal (public_host_runtime_t &host,
                                               const protocol::session_relocation_seal_t &seal)
{
    return host.admit_session_relocation_seal (
      seal,
      {seal.session_owner_id, static_cast<std::int64_t> (seal.session_owner_lease_generation)}, {},
      {}, {});
}

void public_host_runtime_test_access_t::expire_seal_results (public_host_runtime_t &host)
{
    host._relocation_session_terminal_lane
      .run ([&] {
          for (auto &[key, record] : host._session_seal_terminals)
              record.expires_at = std::chrono::steady_clock::time_point::min ();
      })
      .get ();
    host.flush_pending_session_relocation_seals ();
}

void public_host_runtime_test_access_t::set_dispatch (public_host_runtime_t &host,
                                                      dispatch_options_t options)
{
    host._options.mesh.dispatch = std::move (options);
}

bool public_host_runtime_test_access_t::post_terminal_turn (public_host_runtime_t &host,
                                                            std::function<void ()> work)
{
    return host._relocation_session_terminal_lane.try_post (std::move (work));
}

bool public_host_runtime_test_access_t::on_terminal_lane (public_host_runtime_t &host)
{
    return host._relocation_session_terminal_lane.is_on_lane ();
}

void public_host_runtime_test_access_t::enqueue_lifetime_probe (public_host_runtime_t &host)
{
    host._local_dispatch_completion_lane
      .run ([&] {
          local_application_dispatch_t record;
          record.owner.owner_kind = owner_kind_t::node;
          record.owner.domain = ready_domain_t::application;
          record.record.kind = record_kind_t::node_send;
          record.record.domain = ready_domain_t::application;
          host._local_application_dispatches.push_back (std::move (record));
      })
      .get ();
}

bool public_host_runtime_test_access_t::restore_saved (
  public_host_runtime_t &host,
  const protocol::relocation_prepare_t &prepare,
  std::vector<stateful::frozen_object_state_t> frozen,
  std::vector<stateful::object_ref_t> targets,
  const stateful::relocation_restore_identity_t &identity)
{
    auto pending = std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
      public_host_runtime_t::pending_relocation_assembly_t{
        prepare,
        {},
        stateful::relocation_state_assembly_t{prepare.relocation,
                                              prepare.target_attempt_generation,
                                              prepare.coordinator,
                                              prepare.object,
                                              {}}});
    auto staging = std::make_shared<public_host_runtime_t::relocation_assembly_staging_t> ();
    staging->frozen = std::move (frozen);
    staging->targets = std::move (targets);
    staging->restore_identity = identity;
    for (const auto &saved : staging->frozen) {
        staging->sources.push_back (saved.owner);
        staging->wire_objects.push_back ({saved.owner.kind == stateful::object_kind_t::actor
                                            ? protocol::relocation_object_kind_t::actor
                                            : protocol::relocation_object_kind_t::user_spot,
                                          saved.stable_type, saved.owner.key,
                                          saved.owner.object_generation,
                                          saved.owner.authority_owner_generation});
    }
    return host.restore_relocation_assembly (pending, staging).result ().value ();
}

bool public_host_runtime_test_access_t::complete_saved (
  public_host_runtime_t &host,
  const stateful::object_ref_t &owner,
  const protocol::application_payload_t &reply)
{
    auto claim = host._stateful_dispatch->try_claim (owner);
    return claim.first == stateful::stateful_error_t::none && claim.second
           && host._stateful_dispatch->complete (*claim.second, reply)
                == stateful::stateful_error_t::none;
}

bool public_host_runtime_test_access_t::dispatch_saved (
  public_host_runtime_t &host,
  stateful::stateful_delivery_t delivery,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch)
{
    const auto owner = delivery.owner;
    return host.dispatch_stateful_delivery (owner, std::move (delivery), {}, "player", dispatch);
}

void public_host_runtime_test_access_t::insert_unscheduled_target_attempt (
  public_host_runtime_t &host, const key_t &key)
{
    host._relocation_session_terminal_lane
      .run ([&] {
          host.lower_relocation_next_due_on_lane (public_host_runtime_t::relocation_due_now);
          (void) host._relocation_target_attempts.try_emplace (key);
      })
      .get ();
}

void public_host_runtime_test_access_t::publish_relocation_bound (public_host_runtime_t &host)
{
    host._relocation_session_terminal_lane
      .run ([&] { host.publish_relocation_next_due_on_lane (); })
      .get ();
}

void public_host_runtime_test_access_t::schedule_finalize (public_host_runtime_t &host,
                                                           const key_t &key,
                                                           std::chrono::steady_clock::time_point at)
{
    host._relocation_session_terminal_lane
      .run ([&] {
          host.set_relocation_finalize_at_on_lane (host._relocation_target_attempts.at (key), at);
      })
      .get ();
}

bool public_host_runtime_test_access_t::relocation_due (const public_host_runtime_t &host,
                                                        std::chrono::steady_clock::time_point now)
{
    return host.relocation_management_due (now);
}

}
