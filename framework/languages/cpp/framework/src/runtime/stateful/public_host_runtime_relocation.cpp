/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/stateful/public_host_runtime_internal.hpp"

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <limits>
#include <runtime/locations/location_repository.hpp>
#include <service_wire_codec.hpp>
#include <service_wire_constants.hpp>
#include <service_wire_pilot_codec.hpp>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <zlink/framework/detail/deadline_scheduler.hpp>

namespace zlink::framework::runtime::host
{

std::tuple<std::uint64_t, std::uint64_t, std::string>
public_host_runtime_detail::session_relocation_key (
  const protocol::session_relocation_route_t &route)
{
    return std::tuple{route.relocation.high, route.relocation.low, route.actor.actor_id};
}

using public_host_runtime_detail::is_stateful_application;
using public_host_runtime_detail::local_application_query;
using public_host_runtime_detail::stateful_failure_pair;
using public_host_runtime_detail::trace_mesh_host_enabled;
using ::zlink::framework::detail::mesh_trace_enabled;

namespace
{
using public_host_runtime_detail::session_relocation_key;

template <typename Peer> bool peer_route_ready (const Peer &peer, std::uint64_t generation)
{
    return peer && peer->ready && peer->descriptor.lifecycle_generation == generation;
}

bool same_relocation_source_fence (
  const protocol::request_source_fence_t &source,
  const protocol::relocation_coordinator_fence_t &coordinator) noexcept
{
    return source.owner_id == coordinator.owner_id
           && source.lease_generation == coordinator.lease_generation
           && source.node_routing_id == coordinator.node_routing_id
           && source.node_generation == coordinator.node_generation;
}

auto session_relocation_key (const protocol::session_relocation_seal_t &seal)
{
    return std::tuple{seal.relocation.high, seal.relocation.low, seal.actor.actor_id};
}

} // namespace

void public_host_runtime_t::forward_relocation_application (
  const stateful::object_ref_t &owner,
  const stateful::turn_record_t &record,
  const std::vector<std::uint8_t> &target_routing_id,
  std::uint64_t target_generation,
  std::uint64_t target_lease_generation,
  std::chrono::milliseconds window,
  std::optional<std::uint64_t> target_authority_generation)
{
    if (!_stateful_dispatch)
        return;
    auto pending = std::make_shared<task_t<bool>> (_stateful_dispatch->forward_accepted (
      owner, record, target_routing_id, target_generation, target_lease_generation, window,
      target_authority_generation));
    detail::observe_task_completion (
      *pending, [host = shared_from_this (), pending] (const result_t<bool> &) {});
}

void public_host_runtime_t::configure_message_follow_handler (
  std::function<void (const protocol::message_follow_notice_t &)> handler)
{
    _lifecycle_configuration_lane.run_sync ([&] { _message_follow_handler = std::move (handler); });
}

void public_host_runtime_t::configure_actor_join_relocation (
  actor_join_relocation_prepare_validator_t prepare_validator,
  actor_join_recovery_consumer_t recovery_consumer,
  actor_relocation_authority_resolver_t authority_spot_resolver,
  actor_join_recovery_rollback_t recovery_rollback,
  actor_join_committed_authority_adopter_t authority_adopter)
{
    if (!prepare_validator || !recovery_consumer || !authority_spot_resolver || !authority_adopter)
        throw std::invalid_argument ("Actor Join relocation callbacks must not be empty");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("Actor Join relocation must be configured before start");
        _actor_join_relocation_prepare_validator = std::move (prepare_validator);
        _actor_join_recovery_consumer = std::move (recovery_consumer);
        _actor_relocation_authority_resolver = std::move (authority_spot_resolver);
        _actor_join_recovery_rollback = std::move (recovery_rollback);
        _actor_join_committed_authority_adopter = std::move (authority_adopter);
    });
}

void public_host_runtime_t::configure_late_session_route_update (
  std::function<void (const protocol::session_relocation_route_t &)> reporter)
{
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("late Session route reporter must be configured before start");
        _late_session_route_update_reporter = std::move (reporter);
    });
}

task_t<bool>
public_host_runtime_t::send_message_follow (const std::vector<std::uint8_t> &target_routing_id,
                                            const protocol::message_follow_notice_t &notice)
{
    co_return co_await _transport->send_message_follow (target_routing_id, notice);
}

stateful::raw_relocation_replay_coordinator_t &public_host_runtime_t::relocation_wire () noexcept
{
    return *_relocation_wire;
}

void public_host_runtime_t::configure_session_route_owner (
  std::function<std::optional<location_owner_token_t> ()> owner_resolver)
{
    if (!owner_resolver)
        throw std::invalid_argument ("Session route owner resolver is required");
    _lifecycle_configuration_lane.run_sync (
      [&] { _session_route_owner_resolver = std::move (owner_resolver); });
}

void public_host_runtime_t::configure_session_relocation_store (
  std::shared_ptr<stateful::relocation_store_port_t> relocations)
{
    if (!relocations)
        throw std::invalid_argument ("Session relocation Store is required");
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started)
            throw std::logic_error ("Session relocation Store must be configured before start");
        _session_relocations = std::move (relocations);
    });
}

std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>
public_host_runtime_t::admit_session_relocation_seal (
  const protocol::session_relocation_seal_t &seal,
  const location_owner_token_t &session_owner,
  std::vector<std::uint8_t> response_routing_id,
  session_seal_local_completion_t local_completion,
  const std::function<void (const std::vector<std::uint8_t> &)> &reconcile)
{
    // Session-Actor binding §4: compare with this host's current identity exactly.
    const auto local = status ();
    if (session_owner.lease_generation <= 0
        || !protocol::same_session_owner_identity (
          {seal.session_owner_node_routing_id,
           seal.session_owner_node_generation,
           seal.session_owner_id,
           seal.session_owner_lease_generation,
           {},
           0},
          {local.routing_id ().to_bytes (),
           local.lifecycle_generation (),
           session_owner.owner_id,
           static_cast<std::uint64_t> (session_owner.lease_generation),
           {},
           0})) {
        return {false, std::nullopt};
    }

    flush_pending_session_relocation_seals ();
    const auto relocation_key = session_relocation_key (seal);
    struct existing_session_seal_result_t
    {
        bool accepted;
        std::optional<protocol::session_relocation_sealed_t> immediate;
        bool late_consumed;
    };
    const auto existing_result = _relocation_session_terminal_lane.run_sync (
      [&] () -> std::optional<existing_session_seal_result_t> {
          const auto cached = _session_seal_terminals.find (relocation_key);
          if (cached != _session_seal_terminals.end ()) {
              if (cached->second.seal != seal)
                  return existing_session_seal_result_t{false, std::nullopt, false};
              if (cached->second.consumed)
                  return existing_session_seal_result_t{true, std::nullopt, true};
              if (!response_routing_id.empty ())
                  cached->second.response_routing_id = std::move (response_routing_id);
              if (cached->second.ready)
                  return existing_session_seal_result_t{true, cached->second.sealed, false};
              if (local_completion)
                  cached->second.local_completions.push_back (std::move (local_completion));
              return existing_session_seal_result_t{true, std::nullopt, false};
          }
          return std::nullopt;
      });
    if (existing_result) {
        if (existing_result->late_consumed) {
            const auto &logger = detail::dispatch_options_access_t::logger (_options.mesh.dispatch);
            if (logger)
                detail::diagnostic_event_sink_t::log_if_configured (
                  logger, log_level_t::warn, "late_session_relocation_seal", {});
        }
        return {existing_result->accepted, std::move (existing_result->immediate)};
    }

    const auto session_id = stateful::stream_session_registry_t::connection_key (
      zlink::routing_id_t::from (seal.session_routing_id));
    const auto admission =
      _sessions.seal_remote_route (session_id, seal.binding_generation, seal.actor.actor_id,
                                   seal.actor.object_generation, seal.actor.owner_lease_generation);
    if (admission.error != stateful::stateful_error_t::none
        && admission.error != stateful::stateful_error_t::backpressured) {
        return {false, std::nullopt};
    }

    const protocol::session_relocation_sealed_t ack{seal.relocation,
                                                    seal.coordinator,
                                                    seal.actor,
                                                    seal.session_owner_node_routing_id,
                                                    seal.session_owner_node_generation,
                                                    seal.session_owner_id,
                                                    seal.session_owner_lease_generation,
                                                    seal.session_routing_id,
                                                    seal.binding_generation,
                                                    admission.seal_result};
    const auto ready =
      admission.seal_result == protocol::session_relocation_seal_result_t::binding_absent
      || _sessions.remote_route_seal_ready (admission.barrier);
    bool inserted = false;
    std::optional<protocol::session_relocation_sealed_t> immediate;
    _relocation_session_terminal_lane.run_sync ([&] {
        session_seal_terminal_record_t record{seal,
                                              ack,
                                              admission.barrier,
                                              std::chrono::steady_clock::now ()
                                                + _options.session_relocation_seal_timeout,
                                              false,
                                              ready,
                                              response_routing_id,
                                              {}};
        if (local_completion && !ready)
            record.local_completions.push_back (local_completion);
        // A binding retains only its last closed record. The new seal owns its successor.
        for (auto it = _session_seal_terminals.begin (); it != _session_seal_terminals.end ();) {
            if (it->second.consumed && it->second.seal.actor.actor_id == seal.actor.actor_id)
                it = _session_seal_terminals.erase (it);
            else
                ++it;
        }
        const auto [stored, was_inserted] =
          _session_seal_terminals.emplace (relocation_key, std::move (record));
        lower_relocation_next_due_on_lane (relocation_due_now);
        inserted = was_inserted;
        if (inserted)
            _transport->signal_activity ();
        if (!inserted) {
            if (stored->second.seal != seal) {
                immediate.reset ();
            } else if (stored->second.ready) {
                if (!response_routing_id.empty ())
                    stored->second.response_routing_id = response_routing_id;
                immediate = stored->second.sealed;
            } else {
                if (!response_routing_id.empty ())
                    stored->second.response_routing_id = response_routing_id;
                if (local_completion)
                    stored->second.local_completions.push_back (local_completion);
            }
        } else if (ready) {
            immediate = ack;
        }
    });
    if (!inserted) {
        (void) _sessions.abort_barrier (admission.barrier);
        const auto stored_matches = _relocation_session_terminal_lane.run_sync ([&] {
            const auto stored = _session_seal_terminals.find (relocation_key);
            return stored != _session_seal_terminals.end () && stored->second.seal == seal;
        });
        if (!stored_matches)
            return {false, std::nullopt};
    }
    request_peer_reconcile (seal.target_node_routing_id, seal.target_node_generation, reconcile);
    return {true, std::move (immediate)};
}

task_t<bool> public_host_runtime_t::seal_session_remote (
  zlink::routing_id_t session_owner_node,
  protocol::session_relocation_seal_t seal,
  std::chrono::milliseconds timeout,
  std::chrono::steady_clock::time_point operation_deadline,
  session_relocation_journal_capture_t capture_journal,
  session_relocation_seal_completion_t completion)
{
    if (!capture_journal || !completion)
        throw std::invalid_argument (
          "Session relocation seal requires journal capture and completion callbacks");
    const auto relocation_key = session_relocation_key (seal);
    std::optional<session_relocation_seal_result_t> cached_result;
    std::shared_ptr<stateful::relocation_store_port_t> relocations;
    const auto journal_admitted = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto cached = _session_journal_terminals.find (relocation_key);
        if (cached != _session_journal_terminals.end ()) {
            if (cached->second.first != seal)
                return false;
            cached_result = cached->second.second;
        }
        relocations = _session_relocations;
        return true;
    });
    if (!journal_admitted)
        co_return false;
    if (cached_result) {
        completion (foundation::operation_terminal_t::completed, std::move (cached_result));
        co_return true;
    }
    if (!relocations)
        co_return false;
    const auto expected = seal;
    const auto weak_host = weak_from_this ();
    auto response =
      std::make_shared<std::function<void (foundation::operation_terminal_t, zlink::message_t)>> (
        [weak_host, expected, relocation_key, operation_deadline,
         relocations = std::move (relocations), capture_journal = std::move (capture_journal),
         completion = std::move (completion)] (foundation::operation_terminal_t terminal,
                                               zlink::message_t payload) mutable {
            if (terminal != foundation::operation_terminal_t::completed) {
                completion (terminal, std::nullopt);
                return;
            }
            try {
                const auto host = weak_host.lock ();
                if (!host) {
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                const auto sealed =
                  protocol::decode_session_relocation_sealed (protocol::message_bytes (payload));
                if (sealed.relocation != expected.relocation
                    || sealed.coordinator != expected.coordinator || sealed.actor != expected.actor
                    || sealed.session_owner_node_routing_id
                         != expected.session_owner_node_routing_id
                    || sealed.session_owner_node_generation
                         != expected.session_owner_node_generation
                    || sealed.session_owner_id != expected.session_owner_id
                    || sealed.session_owner_lease_generation
                         != expected.session_owner_lease_generation
                    || sealed.session_routing_id != expected.session_routing_id
                    || sealed.binding_generation != expected.binding_generation) {
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                if (sealed.seal_result
                    == protocol::session_relocation_seal_result_t::binding_absent) {
                    completion (terminal, session_relocation_seal_result_t{sealed, {}});
                    return;
                }
                stateful::durable_session_journal_record_t record{
                  expected.relocation.high,
                  expected.relocation.low,
                  stateful::object_ref_t{
                    stateful::object_kind_t::actor,
                    expected.actor.actor_id,
                    expected.actor.object_generation,
                    expected.actor.authority_owner_generation,
                    {},
                    zlink::routing_id_t::from (expected.actor.target_node_routing_id).to_string ()},
                  expected.binding_generation,
                  0,
                  capture_journal ()};
                stateful::durable_session_journal_store_t journal_store (relocations);
                const auto root = journal_store.prepare (record, operation_deadline);
                const auto recovered = journal_store.recover (root);
                if (!recovered || *recovered != record) {
                    journal_store.cleanup (root);
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                const session_relocation_seal_result_t result{sealed, root};
                std::optional<session_relocation_seal_result_t> existing_result;
                bool conflicting_terminal = false;
                host->_relocation_session_terminal_lane.run_sync ([&] {
                    const auto [stored, inserted] = host->_session_journal_terminals.emplace (
                      relocation_key, std::pair{expected, result});
                    if (!inserted) {
                        journal_store.cleanup (root);
                        conflicting_terminal = stored->second.first != expected;
                        if (!conflicting_terminal)
                            existing_result = stored->second.second;
                    }
                });
                if (conflicting_terminal) {
                    completion (foundation::operation_terminal_t::transport_failed, std::nullopt);
                    return;
                }
                if (existing_result) {
                    completion (terminal, std::move (existing_result));
                    return;
                }
                completion (terminal, result);
            }
            catch (...) {
                const auto failure = detail::current_exception_result<void> ();
                const auto *error = failure.error ();
                completion (error && error->kind () == framework_error_kind_t::deadline_exceeded
                              ? foundation::operation_terminal_t::timed_out
                              : foundation::operation_terminal_t::transport_failed,
                            std::nullopt);
            }
        });

    const auto local = status ();
    if (local.routing_id ().to_bytes () == session_owner_node.to_bytes ()) {
        std::function<std::optional<location_owner_token_t> ()> owner_resolver;
        std::function<void (const std::vector<std::uint8_t> &)> reconcile;
        co_await _lifecycle_configuration_lane.run_task (
          [&] { owner_resolver = _session_route_owner_resolver; });
        reconcile = _transport->peer_reconcile ();
        if (local.lifecycle_generation () != seal.session_owner_node_generation || !owner_resolver)
            co_return false;
        const auto owner = owner_resolver ();
        if (!owner)
            co_return false;
        auto local_completion =
          [response] (foundation::operation_terminal_t terminal,
                      std::optional<protocol::session_relocation_sealed_t> sealed) mutable {
              (*response) (terminal, sealed
                                       ? zlink::message_t::from (
                                           protocol::encode_session_relocation_sealed (*sealed))
                                       : zlink::message_t{});
          };
        auto [accepted, immediate] =
          admit_session_relocation_seal (seal, *owner, {}, local_completion, reconcile);
        if (!accepted)
            co_return false;
        if (immediate)
            local_completion (foundation::operation_terminal_t::completed, std::move (*immediate));
        co_return true;
    }
    co_return co_await _transport->request_session_relocation_seal (
      session_owner_node.to_bytes (), std::move (seal), timeout,
      [response] (foundation::operation_terminal_t terminal,
                  ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto payload = std::move (completion_payload.value ());
          (*response) (terminal, std::move (payload));
      });
}

task_t<stateful::relocation_reason_t>
public_host_runtime_t::prepare_relocation_remote (const zlink::routing_id_t &target_node,
                                                  protocol::relocation_prepare_t prepare,
                                                  std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ())
        co_return stateful::relocation_reason_t::restore_failed;
    const auto response =
      co_await _transport->request_relocation_prepare (target_node.to_bytes (), prepare, timeout);
    if (response.failed) {
        // Exact-identity fencing already ran in request_relocation_prepare
        // — this is not a timeout, it is the target's own explicit,
        // matching-identity rejection. Map and surface its failure_code
        // instead of letting it collapse into the same "no result" a
        // timeout produces.
        const auto kind = classify_relocation_failure_code (response.failed->failure_code);
        trace_mesh_host ("relocation-prepare-failed",
                         "wire_failure_code=" + std::to_string (response.failed->failure_code)
                           + " kind=" + std::to_string (static_cast<int> (kind)));
        co_return classify_relocation_failure_reason (response.failed->failure_code);
    }
    if (!response.ready)
        co_return stateful::relocation_reason_t::restore_failed;
    const auto &ready = *response.ready;
    if (ready.relocation != prepare.relocation
        || ready.target_attempt_generation != prepare.target_attempt_generation
        || ready.coordinator != prepare.coordinator || ready.target != prepare.target
        || ready.object != prepare.object
        || ready.sender_role != protocol::relocation_role_t::target)
        co_return stateful::relocation_reason_t::restore_failed;
    co_return stateful::relocation_reason_t::none;
}

task_t<bool>
public_host_runtime_t::cutover_relocation_remote (const zlink::routing_id_t &target_node,
                                                  protocol::relocation_cutover_t cutover)
{
    co_return co_await _transport->send_relocation_control (target_node.to_bytes (), cutover);
}

task_t<bool>
public_host_runtime_t::route_session_remote (const zlink::routing_id_t &session_owner_node,
                                             protocol::session_relocation_route_t route)
{
    co_return co_await _transport->send_session_relocation_route (session_owner_node.to_bytes (),
                                                                  route);
}

spot_handle_t public_host_runtime_t::bind_relocation_spot (stateful::object_ref_t object)
{
    return spot_handle_t (shared_from_this (), std::move (object));
}

bool public_host_runtime_t::relocation_target_wire_ready (
  const relocation_target_attempt_t &attempt) noexcept
{
    return attempt.next_finalize_at != std::chrono::steady_clock::time_point{};
}

bool public_host_runtime_t::relocation_target_terminal (const relocation_target_attempt_t &attempt)
{
    return attempt.terminal && attempt.terminal->task ().await_ready ();
}

bool public_host_runtime_t::relocation_target_holds_publication (
  const relocation_target_attempt_t &attempt)
{
    return attempt.authority_committed_at != std::chrono::steady_clock::time_point{}
           && !relocation_target_terminal (attempt);
}

task_t<bool> public_host_runtime_t::has_finishing_relocation_target ()
{
    co_return co_await _relocation_session_terminal_lane.run_task ([this] {
        return std::any_of (
          _relocation_target_attempts.begin (), _relocation_target_attempts.end (),
          [] (const auto &entry) { return relocation_target_holds_publication (entry.second); });
    });
}

bool public_host_runtime_t::has_pending_relocation_requests (const actor_ref_t &actor)
{
    using stateful::object_kind_t;
    return _relocation_session_terminal_lane.run_sync ([&] {
        return std::any_of (
          _relocation_target_attempts.begin (), _relocation_target_attempts.end (),
          [&] (const auto &entry) {
              const auto &attempt = entry.second;
              return relocation_target_holds_publication (attempt)
                     && std::any_of (attempt.targets.begin (), attempt.targets.end (),
                                     [&] (const auto &target) {
                                         return target.kind == object_kind_t::actor
                                                && target.key == actor.actor_id ().value ()
                                                && target.object_generation
                                                     == actor.object_generation ()
                                                && target.node_id == actor.node_rid ().value ();
                                     })
                     && std::any_of (attempt.captured_requests.begin (),
                                     attempt.captured_requests.end (), [&] (const auto &request) {
                                         return request.kind == object_kind_t::actor
                                                && request.key == actor.actor_id ().value ()
                                                && !request.delivery->task ().await_ready ();
                                     });
          });
    });
}

task_t<bool> public_host_runtime_t::await_previous_relocation_target (
  protocol::relocation_id_t relocation,
  std::uint64_t attempt_generation,
  const stateful::object_ref_t &object,
  std::chrono::steady_clock::time_point deadline)
{
    const auto terminal = co_await _relocation_session_terminal_lane.run_task ([&] {
        const bool exact = relocation.high != 0 || relocation.low != 0;
        const auto found =
          exact ? _relocation_target_attempts.find (
                    {relocation.high, relocation.low, attempt_generation})
                : std::find_if (_relocation_target_attempts.begin (),
                                _relocation_target_attempts.end (), [&] (const auto &entry) {
                                    const auto &attempt = entry.second;
                                    return relocation_target_holds_publication (attempt)
                                           && std::find (attempt.targets.begin (),
                                                         attempt.targets.end (), object)
                                                != attempt.targets.end ();
                                });
        if (found == _relocation_target_attempts.end ()
            || std::find (found->second.targets.begin (), found->second.targets.end (), object)
                 == found->second.targets.end ())
            return std::shared_ptr<task_completion_source_t<void>>{};
        return found->second.terminal;
    });
    // The caller rereads a published slot even if publication was cleared before lookup.
    if (!terminal)
        co_return false;
    auto target_terminal = terminal->task ();
    if (target_terminal.await_ready ())
        co_return true;

    using foundation::operation_terminal_t;
    auto completion = std::make_shared<task_completion_source_t<void>> ();
    auto waiter = completion->task ();
    const auto transport = _transport;
    const auto operation = co_await transport->register_local_operation_async (
      deadline, [completion] (operation_terminal_t result, result_t<zlink::message_t>) {
          if (result == operation_terminal_t::completed)
              completion->complete (result_t<void>::success ());
          else
              completion->complete (
                result_t<void>::failure (result == operation_terminal_t::timed_out
                                           ? framework_error_kind_t::deadline_exceeded
                                           : framework_error_kind_t::shutting_down,
                                         "Waiting for the previous relocation target ended"));
      });
    if (!operation)
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "Relocation wait cannot register on a stopped host");
    detail::observe_task_completion (target_terminal, [weak = std::weak_ptr (transport),
                                                       id = *operation] (const result_t<void> &) {
        if (const auto owner = weak.lock ())
            (void) owner->complete_local_operation (id);
    });
    co_await waiter;
    co_return true;
}

std::optional<public_host_runtime_t::relocation_target_attempt_t>
public_host_runtime_t::take_relocation_target_discard (relocation_target_attempt_t &attempt)
{
    if (attempt.targets.empty ())
        return std::nullopt;
    auto terminal = attempt.terminal;
    auto discarded = std::move (attempt);
    // Keep the same request identity and completion while cleanup runs.
    attempt = relocation_target_attempt_t{};
    attempt.prepare = discarded.prepare;
    attempt.pending = discarded.pending;
    attempt.restore_identity = discarded.restore_identity;
    attempt.terminal = std::move (terminal);
    return discarded;
}

task_t<void> public_host_runtime_t::discard_unverified_relocation_targets (
  std::vector<relocation_attempt_key_t> observed)
{
    if (observed.empty ()
        && !_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire))
        co_return;
    std::vector<relocation_target_attempt_t> discarded;
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto sealed = _options.mesh.shutdown_admission_seal->load (std::memory_order_acquire);
        for (auto &[key, attempt] : _relocation_target_attempts) {
            if (attempt.phase != relocation_target_phase_t::waiting_cutover)
                continue;
            if (sealed || std::find (observed.begin (), observed.end (), key) != observed.end ())
                if (auto staging = take_relocation_target_discard (attempt))
                    discarded.push_back (std::move (*staging));
        }
    });
    co_await discard_relocation_target_attempts (std::move (discarded));
}

task_t<void> public_host_runtime_t::seal_relocation_targets ()
{
    std::vector<std::shared_ptr<std::stop_source>> cancellations;
    std::vector<pending_relocation_assembly_t> assemblies;
    std::vector<task_t<void>> terminals;
    co_await _relocation_session_terminal_lane.run_task (
      [this, &cancellations, &assemblies, &terminals] {
          _options.mesh.shutdown_admission_seal->store (true, std::memory_order_release);
          lower_relocation_next_due_on_lane (relocation_due_now);
          for (const auto &[key, attempt] : _relocation_target_attempts) {
              if (attempt.terminal)
                  terminals.push_back (attempt.terminal->task ());
              if (attempt.pending)
                  cancellations.push_back (attempt.pending->cancellation);
          }
          for (auto &[key, assembly] : _relocation_assemblies)
              assemblies.push_back (std::move (assembly));
          _relocation_assemblies.clear ();
      });
    for (const auto &cancellation : cancellations)
        cancellation->request_stop ();
    for (const auto &assembly : assemblies)
        reply_relocation_assembly_failure (
          assembly, static_cast<protocol::framework_error_code> (
                      messaging::request_failure_mapper_t{}.target_failure_code (
                        framework_error_kind_t::shutting_down)));
    auto activations = co_await _lifecycle_configuration_lane.run_task (
      [this] { return std::exchange (_instance_spot_activation_admissions, {}); });
    for (const auto &[spot_id, operation] : activations)
        operation->admission.complete (result_t<instance_activation_admission_t>::failure (
          framework_error_kind_t::shutting_down, "Instance Spot activation target stopped"));
    co_await discard_unverified_relocation_targets ();
    for (auto &terminal : terminals)
        co_await terminal;
    for (const auto &[spot_id, operation] : activations)
        co_await operation->terminal.task ();
}

task_t<void> public_host_runtime_t::discard_relocation_target_attempts (
  std::vector<relocation_target_attempt_t> attempts, std::optional<framework_exception_t> failure)
{
    for (auto &attempt : attempts) {
        co_await _spot_actor_index_lane.run_task ([&] {
            for (const auto &target : attempt.targets) {
                if (target.kind != stateful::object_kind_t::actor)
                    continue;
                const auto current = _actors.find (target.key);
                if (current != _actors.end () && current->second.second == target)
                    _actors.erase (current);
            }
        });
        for (const auto &object : attempt.wire_objects) {
            try {
                (void) _relocation_wire->unregister_target (
                  attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);
            }
            catch (...) {
                _options.mesh.runtime_failures->report (std::current_exception ());
            }
        }
        try {
            if (attempt.targets.size () == 1)
                (void) co_await _objects.abort_relocation_restore_async (attempt.targets.front (),
                                                                         attempt.restore_identity);
            else
                (void) co_await _objects.abort_relocation_restore_aggregate_async (
                  attempt.targets, attempt.restore_identity);
        }
        catch (...) {
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
        if (_stateful_dispatch)
            for (const auto &target : attempt.targets) {
                try {
                    (void) _stateful_dispatch->discard_pending (target);
                }
                catch (...) {
                    _options.mesh.runtime_failures->report (std::current_exception ());
                }
            }
        if (relocation_target_wire_ready (attempt) && _actor_join_recovery_rollback) {
            std::vector<std::pair<std::string, stateful::object_ref_t>> consumed;
            for (std::size_t index = 0; index < attempt.targets.size (); ++index)
                if (attempt.targets[index].kind == stateful::object_kind_t::actor)
                    consumed.emplace_back (attempt.wire_objects[index].stable_type,
                                           attempt.targets[index]);
            co_await rollback_actor_join_recoveries (consumed);
        }
        const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                           attempt.prepare.relocation.low,
                                           attempt.prepare.target_attempt_generation};
        if (attempt.pending && !attempt.pending->ready->task ().await_ready ()) {
            const auto stopped = framework_exception_t (
              framework_error_kind_t::shutting_down, "Relocation target stopped before readiness");
            const auto &cause = failure ? *failure : stopped;
            trace_mesh_host ("relocation-assembly-failed", cause.what ());
            reply_relocation_assembly_failure (
              *attempt.pending, static_cast<protocol::framework_error_code> (
                                  messaging::request_failure_mapper_t{}.target_failure_code (
                                    cause.kind (), detail::failure_code (cause))));
        }
        co_await _relocation_session_terminal_lane.run_task ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found != _relocation_target_attempts.end ()
                && found->second.terminal == attempt.terminal)
                _relocation_target_attempts.erase (found);
        });
        attempt.terminal->complete (result_t<void>::success ());
    }
}

stateful::relocation_authority_fence_t
public_host_runtime_t::relocation_target_fence (const relocation_target_attempt_t &attempt)
{
    return stateful::relocation_authority_fence_t::from_prepare (
      stateful::relocation_primary (attempt.sources), attempt.prepare);
}

public_host_runtime_t::relocation_target_settlement_t
public_host_runtime_t::observe_relocation_target (
  const stateful::relocation_authority_fence_t &fence) const noexcept
{
    if (!_relocation_authority)
        return relocation_target_settlement_t::retry;
    try {
        switch (_relocation_authority->observe_relocation (fence)) {
            case stateful::relocation_authority_t::target_committed:
                return relocation_target_settlement_t::committed;
            case stateful::relocation_authority_t::source_preserved:
                return relocation_target_settlement_t::discard;
            default:
                break;
        }
        // An earlier read naming the source alone never ends staging; the
        // target lease loss does (01 §10).
        const auto target_live = _relocation_authority->owner_lease_live (fence.target_owner);
        return target_live && !*target_live ? relocation_target_settlement_t::discard
                                            : relocation_target_settlement_t::retry;
    }
    catch (...) {
        return relocation_target_settlement_t::retry;
    }
}

task_t<void> public_host_runtime_t::poll_relocation_target_attempts ()
{
    co_await discard_unverified_relocation_targets ();
    /* 28/52: command 44 (session_relocation_route) is a one-way send,
     * submitted exactly once per route -- there is no periodic re-select
     * and resend of an incomplete route here. Attempts with a verified
     * cutover submit their target CAS; the others only read the authority
     * that ends their staging (01 §10). */
    std::vector<relocation_attempt_key_t> pending;
    std::vector<std::pair<relocation_attempt_key_t, stateful::relocation_authority_fence_t>>
      unverified;
    std::size_t cutover_warnings = 0;
    const auto now = std::chrono::steady_clock::now ();
    co_await _relocation_session_terminal_lane.run_task ([&] {
        for (auto &[key, attempt] : _relocation_target_attempts) {
            if (relocation_target_terminal (attempt) || !relocation_target_wire_ready (attempt)
                || attempt.next_finalize_at > now)
                continue;
            if (attempt.phase >= relocation_target_phase_t::dispatch_open)
                continue;
            if (attempt.phase == relocation_target_phase_t::cutover_received) {
                pending.push_back (key);
                continue;
            }
            /* RelocationCutoverWaitTimeout is a Warning threshold only
               * (28 §4.4): it never starts the CAS or dispatch. */
            if (!attempt.cutover_warned) {
                attempt.cutover_warned = true;
                ++cutover_warnings;
                if (_relocation_metrics && !attempt.sources.empty ())
                    _relocation_metrics->timeout (attempt.sources.front ().kind);
            }
            set_relocation_finalize_at_on_lane (attempt,
                                                now + dispatch_limits::management_retry_interval);
            unverified.emplace_back (key, relocation_target_fence (attempt));
        }
    });
    for (std::size_t index = 0; index != cutover_warnings; ++index) {
        trace_mesh_host ("relocation-cutover-timeout", "warning=cutover_timeout");
        if (_relocation_target_metrics.cutover_timeout)
            _relocation_target_metrics.cutover_timeout ();
    }
    std::vector<relocation_attempt_key_t> observed;
    for (const auto &[key, fence] : unverified)
        if (observe_relocation_target (fence) == relocation_target_settlement_t::discard)
            observed.push_back (key);
    co_await discard_unverified_relocation_targets (observed);
    for (const auto &key : pending)
        (void) co_await try_finalize_relocation_target (key);
}

void public_host_runtime_t::flush_pending_session_relocation_seals ()
{
    std::vector<session_relocation_key_t> pending;
    std::vector<session_relocation_key_t> expired;
    const auto now = std::chrono::steady_clock::now ();
    const auto consumed = _relocation_session_terminal_lane.run_sync ([&] {
        std::map<session_relocation_key_t, protocol::session_relocation_seal_t> records;
        for (const auto &[key, record] : _session_seal_terminals)
            if (record.consumed
                && record.sealed.seal_result == protocol::session_relocation_seal_result_t::sealed)
                records.emplace (key, record.seal);
        return records;
    });
    // Read each registry fact outside the terminal turn. Only the captured
    // records may be pruned with it; a successor record needs its own observation.
    std::map<std::string, std::optional<stateful::stream_binding_t>> current_bindings;
    for (const auto &[key, seal] : consumed) {
        const auto [current, inserted] = current_bindings.try_emplace (seal.actor.actor_id);
        if (inserted)
            current->second = _sessions.current_binding (seal.actor.actor_id);
    }
    _relocation_session_terminal_lane.run_sync ([&] {
        for (auto it = _session_seal_terminals.begin (); it != _session_seal_terminals.end ();) {
            const auto &[key, record] = *it;
            if (record.sealed.seal_result
                == protocol::session_relocation_seal_result_t::binding_absent) {
                if (record.expires_at <= now) {
                    it = _session_seal_terminals.erase (it);
                    continue;
                }
            } else if (record.consumed) {
                const auto captured = consumed.find (key);
                if (captured != consumed.end () && captured->second == record.seal) {
                    const auto &current = current_bindings.at (record.seal.actor.actor_id);
                    if (!current || current->binding_generation != record.seal.binding_generation
                        || current->connection.connection_id
                             != stateful::stream_session_registry_t::connection_key (
                               zlink::routing_id_t::from (record.seal.session_routing_id))) {
                        it = _session_seal_terminals.erase (it);
                        continue;
                    }
                }
            } else if (record.expires_at <= now)
                expired.push_back (key);
            else if (!record.ready)
                pending.push_back (key);
            ++it;
        }
    });
    for (const auto &key : expired) {
        stateful::stream_barrier_t barrier;
        std::vector<session_seal_local_completion_t> local_completions;
        const auto expired_current = _relocation_session_terminal_lane.run_sync ([&] {
            const auto found = _session_seal_terminals.find (key);
            if (found == _session_seal_terminals.end () || found->second.consumed
                || found->second.expires_at > now)
                return false;
            barrier = found->second.barrier;
            local_completions = std::move (found->second.local_completions);
            found->second.consumed = true;
            return true;
        });
        if (!expired_current)
            continue;
        (void) _sessions.close_remote_route_seal (barrier);
        for (auto &complete : local_completions) {
            complete (foundation::operation_terminal_t::timed_out, std::nullopt);
        }
    }
    for (const auto &key : pending) {
        stateful::stream_barrier_t barrier;
        const auto pending_current = _relocation_session_terminal_lane.run_sync ([&] {
            const auto found = _session_seal_terminals.find (key);
            if (found == _session_seal_terminals.end () || found->second.consumed
                || found->second.ready)
                return false;
            barrier = found->second.barrier;
            return true;
        });
        if (!pending_current)
            continue;
        if (!_sessions.remote_route_seal_ready (barrier))
            continue;
        protocol::session_relocation_sealed_t sealed;
        std::vector<std::uint8_t> target;
        std::vector<session_seal_local_completion_t> local_completions;
        const auto completed = _relocation_session_terminal_lane.run_sync ([&] {
            const auto found = _session_seal_terminals.find (key);
            if (found == _session_seal_terminals.end () || found->second.consumed)
                return false;
            found->second.ready = true;
            sealed = found->second.sealed;
            target = found->second.response_routing_id;
            local_completions = std::move (found->second.local_completions);
            return true;
        });
        if (!completed)
            continue;
        if (!target.empty ())
            (void) _transport->send_session_relocation_sealed (target, sealed);
        for (auto &complete : local_completions) {
            complete (foundation::operation_terminal_t::completed, sealed);
        }
    }
}

task_t<void> public_host_runtime_t::submit_relocation_session_routes (relocation_attempt_key_t key)
{
    /* 28 §4.7/52: command 44 is a one-way submit -- there is no application
     * reply, so there is nothing to retry on. Each route is dispatched at
     * most once: `send_attempted` is stamped under lock before the send is
     * issued, so a re-entry for this key (this function may be invoked
     * again for an already-finalized attempt on a duplicate cutover or
     * relocationData delivery) can never re-dispatch a route that a prior
     * call already attempted, whether that prior send succeeded or
     * failed. A late duplicate 44 could otherwise cross with a newer
     * relocation and corrupt routing. */
    struct due_route_t
    {
        std::size_t index = 0;
        protocol::session_relocation_route_t route;
    };
    std::vector<due_route_t> due;
    const auto routes_current = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ()
            || found->second.authority_committed_at == std::chrono::steady_clock::time_point{})
            return false;
        for (std::size_t index = 0; index != found->second.session_routes.size (); ++index) {
            auto &state = found->second.session_routes[index];
            if (state.completed || state.send_attempted)
                continue;
            state.send_attempted = true;
            due.push_back ({index, state.route});
        }
        return true;
    });
    if (!routes_current)
        co_return;

    for (auto &pending : due) {
        bool submitted = false;
        try {
            submitted = co_await route_session_remote (
              zlink::routing_id_t::from (pending.route.session_owner_node_routing_id),
              pending.route);
        }
        catch (...) {
            submitted = false;
        }
        /* Whether the one-way send succeeded or failed, this route will
         * never be attempted again (Finding 8), so the source-local
         * journal-terminal bookkeeping this route's seal prepared is done
         * either way and must be released now -- there is no future retry
         * left to release it on. */
        std::optional<stateful::durable_session_journal_root_t> completed_journal;
        std::shared_ptr<stateful::relocation_store_port_t> session_relocations;
        const auto route_current = co_await _relocation_session_terminal_lane.run_task ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ()
                || pending.index >= found->second.session_routes.size ())
                return false;
            auto &state = found->second.session_routes[pending.index];
            if (state.completed || state.route != pending.route)
                return false;
            if (submitted) {
                state.completed = true;
            } else {
                /* Record the failure in the (already-bounded, retention-
                 * limited) per-attempt state rather than swallow it --
                 * there is no gated trace/diagnostics sink reachable from
                 * public_host_runtime_t to route this through instead.
                 * This is a one-way send: it is not retried. */
                state.send_failed = true;
            }
            const auto journal =
              _session_journal_terminals.find (session_relocation_key (state.route));
            if (journal != _session_journal_terminals.end ()) {
                completed_journal = journal->second.second.journal_root;
                session_relocations = _session_relocations;
                _session_journal_terminals.erase (journal);
            }
            return true;
        });
        if (!route_current)
            continue;
        if (completed_journal && session_relocations) {
            try {
                stateful::durable_session_journal_store_t journal_store (
                  std::move (session_relocations));
                journal_store.cleanup (*completed_journal);
            }
            catch (...) {
                _options.mesh.runtime_failures->report (std::current_exception ());
            }
        }
    }
    co_return;
}

void public_host_runtime_t::start_relocation_session_route_submission (relocation_attempt_key_t key)
{
    auto pending =
      std::make_shared<task_t<void>> (submit_relocation_session_routes (std::move (key)));
    detail::observe_task_completion (*pending, [pending] (const result_t<void> &) {});
}

bool public_host_runtime_t::submit_relocation_target_authority (
  relocation_target_attempt_t &attempt) noexcept
{
    if (!_relocation_authority || attempt.sources.empty ()
        || attempt.sources.size () != attempt.targets.size ())
        return false;

    const location_owner_token_t target_owner{
      attempt.prepare.target.target_owner_id,
      static_cast<std::int64_t> (attempt.prepare.target.target_owner_lease_generation)};
    if (target_owner.owner_id.empty () || target_owner.lease_generation <= 0)
        return false;

    try {
        const auto adopt_store_fences = [&] {
            std::vector<stateful::authority_relocation_reference_t> current;
            current.reserve (attempt.sources.size ());
            for (std::size_t index = 0; index != attempt.sources.size (); ++index) {
                const auto read = _relocation_authority->read (attempt.sources[index].kind,
                                                               attempt.sources[index].key);
                if (!read || read->source != attempt.sources[index])
                    return false;
                const auto &staged = attempt.targets[index];
                const auto &committed = read->target;
                if (committed.kind != staged.kind || committed.key != staged.key
                    || committed.object_generation != staged.object_generation
                    || committed.mesh_name != staged.mesh_name
                    || committed.node_id != staged.node_id
                    || committed.authority_owner_generation
                         <= read->source.authority_owner_generation
                    || read->target_owner.owner_id != target_owner.owner_id
                    || read->target_owner.lease_generation != target_owner.lease_generation
                    || read->relocation_reference != attempt.restore_identity.reference
                    || read->checksum_crc32c != attempt.restore_identity.checksum_crc32c
                    || read->inventory_digest != attempt.restore_identity.inventory_digest) {
                    return false;
                }
                current.push_back (*read);
            }
            for (std::size_t index = 0; index != current.size (); ++index) {
                const auto &committed = current[index].target;
                if (committed != attempt.targets[index]
                    && _objects.reconcile_relocation_restore_authority (
                         attempt.targets[index], committed, attempt.restore_identity)
                         != stateful::stateful_error_t::none) {
                    return false;
                }
            }
            attempt.ready.clear ();
            for (std::size_t index = 0; index != current.size (); ++index) {
                attempt.ready.push_back (current[index].ready);
                attempt.sources[index] = current[index].source;
                attempt.targets[index] = current[index].target;
            }
            return true;
        };
        if (adopt_store_fences ())
            return true;

        if (attempt.sources.size () == 1
            && attempt.sources.front ().kind != stateful::object_kind_t::user_spot) {
            const object_creation_target_t target_placement{
              attempt.targets.front ().mesh_name,
              node_rid_t::from_string (attempt.targets.front ().node_id),
              attempt.prepare.target.target_node_generation, target_owner};
            std::vector<std::byte> target_application_payload;
            if (attempt.targets.front ().kind == stateful::object_kind_t::actor
                && _actor_relocation_authority_resolver) {
                const auto spot = _actor_relocation_authority_resolver (attempt.targets.front ());
                if (spot) {
                    auto payload = *spot;
                    payload.owner_id = target_owner.owner_id;
                    payload.owner_lease_generation =
                      static_cast<std::uint64_t> (target_owner.lease_generation);
                    payload.node_generation = attempt.prepare.target.target_node_generation;
                    target_application_payload = runtime::encode_actor_authority_payload (payload);
                }
            }
            const auto published = _relocation_authority->publish (
              attempt.sources.front (), attempt.targets.front (), target_owner, target_placement,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest, std::move (target_application_payload),
              attempt.prepare.coordinator.expected_authority_store_version,
              attempt.prepare.relocation,
              {attempt.prepare.coordinator.owner_id,
               static_cast<std::int64_t> (attempt.prepare.coordinator.lease_generation)},
              &attempt.prepare);
            if (published.status != stateful::authority_publish_status_t::published
                || !published.current || published.current->source != attempt.sources.front ())
                return adopt_store_fences ();
            const auto &committed = published.current->target;
            const auto &staged = attempt.targets.front ();
            if (committed.kind != staged.kind || committed.key != staged.key
                || committed.object_generation != staged.object_generation
                || committed.mesh_name != staged.mesh_name || committed.node_id != staged.node_id
                || committed.authority_owner_generation
                     <= published.current->source.authority_owner_generation
                || published.current->target_owner.owner_id != target_owner.owner_id
                || published.current->target_owner.lease_generation != target_owner.lease_generation
                || published.current->relocation_reference != attempt.restore_identity.reference
                || published.current->checksum_crc32c != attempt.restore_identity.checksum_crc32c
                || published.current->inventory_digest != attempt.restore_identity.inventory_digest)
                return false;
            if (committed != staged
                && _objects.reconcile_relocation_restore_authority (staged, committed,
                                                                    attempt.restore_identity)
                     != stateful::stateful_error_t::none)
                return false;
            attempt.ready = {published.current->ready};
            attempt.sources.front () = published.current->source;
            attempt.targets.front () = committed;
            return true;
        }

        if (!_aggregate_relocation_authority)
            return false;
        if (!attempt.authority_fence) {
            const auto prepared = _aggregate_relocation_authority->prepare (
              attempt.sources, attempt.targets.front ().node_id, target_owner,
              attempt.restore_identity.reference, attempt.restore_identity.checksum_crc32c,
              attempt.restore_identity.inventory_digest, relocation_target_fence (attempt));
            if (prepared.status != stateful::aggregate_publish_status_t::prepared)
                return false;
            attempt.authority_fence = prepared.fence;
            const relocation_attempt_key_t key{attempt.prepare.relocation.high,
                                               attempt.prepare.relocation.low,
                                               attempt.prepare.target_attempt_generation};
            _relocation_session_terminal_lane.run_sync ([&] {
                const auto found = _relocation_target_attempts.find (key);
                if (found != _relocation_target_attempts.end ()
                    && found->second.prepare == attempt.prepare)
                    found->second.authority_fence = attempt.authority_fence;
            });
        }
        /* The committed aggregate record identifies this move; adopt the
         * Store-assigned owner generation of every staged participant. */
        const auto committed = _aggregate_relocation_authority->commit (*attempt.authority_fence);
        if (committed.status != stateful::aggregate_publish_status_t::committed)
            return false;
        attempt.ready.clear ();
        std::vector<stateful::object_ref_t> adopted;
        adopted.reserve (attempt.targets.size ());
        for (std::size_t index = 0; index != attempt.targets.size (); ++index) {
            const auto &staged = attempt.targets[index];
            const auto placement = std::find_if (
              committed.current.begin (), committed.current.end (), [&] (const auto &candidate) {
                  return candidate.target.kind == staged.kind && candidate.target.key == staged.key;
              });
            if (placement == committed.current.end ()
                || placement->target.object_generation != staged.object_generation
                || placement->target.mesh_name != staged.mesh_name
                || placement->target.node_id != staged.node_id
                || placement->target.authority_owner_generation
                     <= attempt.sources[index].authority_owner_generation
                || placement->target_owner.owner_id != target_owner.owner_id
                || placement->target_owner.lease_generation != target_owner.lease_generation)
                return false;
            if (placement->target != staged
                && _objects.reconcile_relocation_restore_authority (staged, placement->target,
                                                                    attempt.restore_identity)
                     != stateful::stateful_error_t::none)
                return false;
            adopted.push_back (placement->target);
            attempt.ready.push_back (placement->ready);
        }
        attempt.targets = std::move (adopted);
        return true;
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return false;
    }
}

public_host_runtime_t::relocation_target_settlement_t
public_host_runtime_t::commit_relocation_target_authority (
  relocation_target_attempt_t &attempt) noexcept
{
    /* The original NewOwner CAS (the SpotWide whole-unit batch) with the same
     * RelocationId and expected StoreVersion; a CAS that does not commit is
     * settled by the Store reading, never by a count or a timer (01 §10). */
    if (submit_relocation_target_authority (attempt))
        return relocation_target_settlement_t::committed;
    return observe_relocation_target (relocation_target_fence (attempt))
               == relocation_target_settlement_t::discard
             ? relocation_target_settlement_t::discard
             : relocation_target_settlement_t::retry;
}

bool public_host_runtime_t::adopt_committed_session_route_authorities (
  relocation_target_attempt_t &attempt) const noexcept
{
    for (auto &state : attempt.session_routes) {
        auto &route = state.route;
        if (route.route.action != protocol::session_relocation_route_action_t::commit
            || route.route.target_node_routing_id != attempt.prepare.target.target_node_routing_id
            || route.route.target_node_generation != attempt.prepare.target.target_node_generation)
            return false;

        const auto source =
          std::find_if (attempt.sources.begin (), attempt.sources.end (),
                        [&route] (const stateful::object_ref_t &candidate) {
                            return candidate.kind == stateful::object_kind_t::actor
                                   && candidate.key == route.actor.actor_id
                                   && candidate.object_generation == route.actor.object_generation;
                        });
        if (source == attempt.sources.end ())
            return false;
        const auto index =
          static_cast<std::size_t> (std::distance (attempt.sources.begin (), source));
        const auto &target = attempt.targets[index];
        if (target.kind != stateful::object_kind_t::actor || target.key != source->key
            || target.object_generation != source->object_generation
            || target.authority_owner_generation <= source->authority_owner_generation)
            return false;

        /* Command 44 is post-CAS. Its two authority fences come from the
         * exact Store relocation row adopted above; the pre-commit route
         * envelope is only transport staging and must never predict the
         * owner generation that the Session owner commits. */
        route.route.previous_authority_owner_generation = source->authority_owner_generation;
        route.route.target_authority_owner_generation = target.authority_owner_generation;
    }
    return true;
}

task_t<bool> public_host_runtime_t::try_finalize_relocation_target (relocation_attempt_key_t key)
{
    relocation_target_attempt_t attempt;
    const auto attempt_current = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ())
            return false;
        if (relocation_target_terminal (found->second)) {
            attempt = found->second;
        } else {
            const auto now = std::chrono::steady_clock::now ();
            if (found->second.phase != relocation_target_phase_t::cutover_received
                || !relocation_target_wire_ready (found->second)
                || now < found->second.next_finalize_at)
                return false;
            attempt = found->second;
            set_relocation_finalize_at_on_lane (found->second,
                                                now + dispatch_limits::management_retry_interval);
        }
        return true;
    });
    if (!attempt_current)
        co_return false;
    if (relocation_target_terminal (attempt))
        co_return true;

    const auto discard_attempt = [&] () -> task_t<void> {
        std::vector<relocation_target_attempt_t> discarded;
        co_await _relocation_session_terminal_lane.run_task ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found == _relocation_target_attempts.end ()
                || relocation_target_terminal (found->second))
                return;
            if (auto staging = take_relocation_target_discard (found->second))
                discarded.push_back (std::move (*staging));
        });
        co_await discard_relocation_target_attempts (std::move (discarded));
    };
    const auto settlement = commit_relocation_target_authority (attempt);
    if (settlement == relocation_target_settlement_t::discard) {
        co_await discard_attempt ();
        co_return false;
    }
    if (settlement != relocation_target_settlement_t::committed)
        co_return false;
    if (!adopt_committed_session_route_authorities (attempt))
        co_return false;

    /* S2 (owner CAS confirmed): stamp once, on the first tick that
     * observes the authority commit, so a retried finalize does not push
     * the target_resume window forward. */
    const auto stamped = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ())
            return false;
        if (found->second.prepare != attempt.prepare
            || found->second.session_routes.size () != attempt.session_routes.size ())
            return false;
        found->second.sources = attempt.sources;
        found->second.targets = attempt.targets;
        found->second.authority_fence = attempt.authority_fence;
        for (std::size_t index = 0; index != attempt.session_routes.size (); ++index) {
            if (found->second.session_routes[index].send_attempted)
                return false;
            found->second.session_routes[index].route = attempt.session_routes[index].route;
        }
        if (found->second.authority_committed_at == std::chrono::steady_clock::time_point{})
            found->second.authority_committed_at = std::chrono::steady_clock::now ();
        attempt.authority_committed_at = found->second.authority_committed_at;
        return true;
    });
    if (!stamped)
        co_return false;

    // Raw identities were installed during target preparation. Reflect the
    // Store-assigned authority before lifecycle opens; User-Spot Join also
    // adopts that authority in its prepared application Actor.
    const auto join_prepare = _actor_join_relocation_prepare_validator
                                ? _actor_join_relocation_prepare_validator (attempt.prepare)
                                : std::optional<bool>{};
    const auto canonical_application_join = join_prepare && *join_prepare;
    for (std::size_t index = 0; index != attempt.targets.size (); ++index) {
        const auto &target = attempt.targets[index];
        if (target.kind != stateful::object_kind_t::actor)
            continue;
        const auto advanced = co_await advance_local_actor_authority (
          target, index < attempt.ready.size () ? attempt.ready[index] : std::nullopt);
        if (advanced != stateful::stateful_error_t::none
            || (canonical_application_join
                && (!_actor_join_committed_authority_adopter
                    || !_actor_join_committed_authority_adopter (
                      target, attempt.prepare.target.target_node_generation,
                      attempt.prepare.target.target_owner_lease_generation))))
            co_return false;
    }
    for (const auto &route_state : attempt.session_routes) {
        const auto &route = route_state.route;
        if (!_bound_session_operations.prepare_relocation_target_route
            || !_bound_session_operations.prepare_relocation_target_route (
              route, attempt.prepare.target.target_owner_lease_generation))
            co_return false;
    }

    stateful::stateful_error_t committed = stateful::stateful_error_t::none;
    std::exception_ptr commit_failure;
    std::shared_ptr<task_completion_source_t<void>> normalized_before_join;
    std::function<void ()> before_actor_join_completion;
    if (canonical_application_join) {
        before_actor_join_completion = [&] {
            auto normalization =
              complete_relocation_target_when_delivered (key, &normalized_before_join).result ();
            commit_failure = normalization.exception ();
            normalization.value ();
        };
    }
    try {
        // Membership §4.2 steps 7-8: lifecycle and queue opening precede command 44.
        committed = attempt.targets.size () == 1
                      ? _objects.commit_relocation_restore (attempt.targets.front (),
                                                            attempt.restore_identity,
                                                            before_actor_join_completion)
                      : _objects.commit_relocation_restore_aggregate (attempt.targets,
                                                                      attempt.restore_identity);
    }
    catch (...) {
        commit_failure = std::current_exception ();
    }
    if (commit_failure) {
        co_await discard_attempt ();
        std::rethrow_exception (commit_failure);
    }
    if (committed != stateful::stateful_error_t::none
        && committed != stateful::stateful_error_t::already_exists) {
        co_await discard_attempt ();
        co_return false;
    }

    for (const auto &object : attempt.wire_objects)
        (void) _relocation_wire->unregister_target (
          attempt.prepare.relocation, attempt.prepare.target_attempt_generation, object);

    /* Metrics 25: target_resume is the target-local S2 (owner CAS) to
     * dispatch-open duration, independent of captured request delivery. */
    if (_relocation_metrics && !attempt.sources.empty ()
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{})
        _relocation_metrics->resume (
          attempt.sources.front ().kind,
          std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                         - attempt.authority_committed_at)
            .count ());
    if (_relocation_target_metrics.target_resume_seconds
        && attempt.authority_committed_at != std::chrono::steady_clock::time_point{}) {
        const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now ()
                                                            - attempt.authority_committed_at);
        _relocation_target_metrics.target_resume_seconds (elapsed.count ());
    }
    /* Relocation flow §4.6: command 44 follows target dispatch opening.
     * A completion on another execution resource may run before this stack
     * returns (Submit and completion §11), so send completion cannot order
     * the Session owner's route commit after the target becomes Ready. */
    start_relocation_session_route_submission (key);
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found != _relocation_target_attempts.end () && !normalized_before_join)
            found->second.phase = relocation_target_phase_t::dispatch_open;
    });
    if (normalized_before_join)
        normalized_before_join->complete (result_t<void>::success ());
    else
        co_await complete_relocation_target_when_delivered (key);
    for (const auto &target : attempt.targets) {
        auto notified = std::make_shared<task_t<bool>> (notify_stateful_owner (target));
        detail::observe_task_completion (
          *notified, [self = shared_from_this (), notified] (const result_t<bool> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { (void) result.value (); });
          });
    }
    co_return true;
}

task_t<void> public_host_runtime_t::complete_relocation_target_when_delivered (
  relocation_attempt_key_t key,
  std::shared_ptr<task_completion_source_t<void>> *before_actor_join_completion)
{
    using publication_t = std::pair<stateful::object_ref_t, protocol::relocation_prepare_t>;
    std::optional<publication_t> publication;
    const auto terminal = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end ())
            return std::shared_ptr<task_completion_source_t<void>>{};
        auto &attempt = found->second;
        // Membership §4.2 step 7: only a Join without captured requests normalizes
        // before its callback. Captured replies can be delivered after dispatch opens.
        const bool can_release =
          before_actor_join_completion
            ? attempt.phase == relocation_target_phase_t::cutover_received
                && attempt.captured_requests.empty ()
            : attempt.phase == relocation_target_phase_t::dispatch_open
                && std::ranges::all_of (attempt.captured_requests, [] (const auto &request) {
                       return request.delivery->task ().await_ready ();
                   });
        if (!can_release)
            return std::shared_ptr<task_completion_source_t<void>>{};
        if (_relocation_authority && attempt.targets.size () == 1
            && attempt.targets.front ().kind == stateful::object_kind_t::actor)
            publication.emplace (attempt.targets.front (), attempt.prepare);
        attempt.phase = relocation_target_phase_t::releasing_publication;
        attempt.captured_requests.clear ();
        return attempt.terminal;
    });
    if (!terminal)
        co_return;

    try {
        if (publication
            && !_relocation_authority->finish_relocation (publication->first, publication->second))
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Relocation target lost its authority publication fence");
        if (before_actor_join_completion)
            *before_actor_join_completion = terminal;
        else
            terminal->complete (result_t<void>::success ());
    }
    catch (...) {
        if (!before_actor_join_completion)
            terminal->complete (
              result_t<void>::failure (framework_error_kind_t::unavailable,
                                       "Relocation target could not release publication"));
        throw;
    }
}

void public_host_runtime_t::record_relocation_reply_delivered (
  relocation_attempt_key_t key, const std::shared_ptr<task_completion_source_t<void>> &delivery)
{
    delivery->complete (result_t<void>::success ());
    auto settled = complete_relocation_target_when_delivered (key);
    detail::observe_task_terminal (
      settled, [self = shared_from_this ()] (const result_t<void> &result) {
          self->_options.mesh.runtime_failures->capture ([&] { result.value (); });
      });
}

void public_host_runtime_t::reply_relocation_assembly_failure (
  const pending_relocation_assembly_t &pending, protocol::framework_error_code code)
{
    pending.ready->complete (detail::result_access_t::failure<void> (detail::with_failure_code (
      framework_exception_t (
        messaging::request_failure_mapper_t{}.failure_code_kind (static_cast<std::uint32_t> (code)),
        "Relocation target preparation failed"),
      static_cast<std::uint32_t> (code))));
    (void) _transport->reply_relocation_failed (
      pending.request, protocol::relocation_failed_t{
                         pending.prepare.relocation, pending.prepare.target_attempt_generation,
                         pending.prepare.coordinator, pending.prepare.target,
                         pending.prepare.object, protocol::relocation_role_t::target,
                         messaging::request_failure_mapper_t{}.target_failure_code (
                           messaging::request_failure_mapper_t{}.failure_code_kind (
                             static_cast<std::uint32_t> (code)),
                           static_cast<std::uint32_t> (code))});
}

task_t<void> public_host_runtime_t::rollback_actor_join_recoveries (
  const std::vector<std::pair<std::string, stateful::object_ref_t>> &consumed)
{
    if (!_actor_join_recovery_rollback)
        co_return;
    for (const auto &[stable_type, target] : consumed) {
        try {
            co_await _actor_join_recovery_rollback (stable_type, target);
        }
        catch (...) {
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
    }
}

void public_host_runtime_t::discard_relocation_assembly_staging (
  const pending_relocation_assembly_t &pending,
  const relocation_assembly_staging_t &staging) noexcept
{
    unregister_relocation_wire_targets (
      pending.prepare.relocation, pending.prepare.target_attempt_generation, staging.wire_objects);
}

void public_host_runtime_t::unregister_relocation_wire_targets (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const std::vector<protocol::relocation_object_t> &wire_objects) noexcept
{
    for (const auto &wire_object : wire_objects) {
        try {
            (void) _relocation_wire->unregister_target (relocation, target_attempt_generation,
                                                        wire_object);
        }
        catch (...) {
            _options.mesh.runtime_failures->report (std::current_exception ());
        }
    }
}

task_t<bool> public_host_runtime_t::restore_relocation_assembly (
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<const relocation_assembly_staging_t> staging)
{
    stateful::stateful_error_t restored = stateful::stateful_error_t::conflict;
    std::optional<framework_exception_t> failure;
    try {
        // Location §9.3: relocation preserves the source recorded at first acceptance.
        const auto previous_actor_identities = co_await _spot_actor_index_lane.run_task ([&] {
            std::vector<stateful::object_ref_t> previous;
            for (const auto &target : staging->targets) {
                if (target.kind != stateful::object_kind_t::actor)
                    continue;
                const auto current = _actors.find (target.key);
                if (current != _actors.end () && current->second.second != target)
                    previous.push_back (current->second.second);
            }
            return previous;
        });
        std::optional<stateful::object_ref_t> actor_target_spot;
        if (staging->targets.size () == 1
            && staging->targets.front ().kind == stateful::object_kind_t::actor
            && _actor_relocation_authority_resolver) {
            const auto spot = _actor_relocation_authority_resolver (staging->targets.front ());
            if (spot) {
                actor_target_spot = stateful::object_ref_t{
                  stateful::object_kind_t::user_spot,  spot->current_spot_id,
                  spot->current_spot_generation,       0,
                  staging->targets.front ().mesh_name, staging->targets.front ().node_id};
            }
        }
        auto restored_state = staging->frozen;
        std::vector<std::tuple<stateful::object_ref_t, protocol::relocation_data_t, std::uint64_t>>
          saved_application;
        for (std::size_t index = 0; index < restored_state.size (); ++index) {
            if (staging->targets[index].kind != stateful::object_kind_t::actor)
                continue;
            for (auto &turn : restored_state[index].pending_application)
                saved_application.emplace_back (
                  staging->targets[index],
                  protocol::relocation_data_t{
                    pending->prepare.relocation, pending->prepare.target_attempt_generation,
                    pending->prepare.coordinator, protocol::relocation_role_t::source,
                    staging->wire_objects[index], std::move (turn.frozen_record.value ())},
                  index + 1);
            restored_state[index].pending_application.clear ();
        }
        restored = co_await (
          staging->targets.size () == 1
            ? _objects.restore_relocation (
                restored_state.front (), staging->targets.front (), staging->restore_identity,
                pending->cancellation->get_token (), std::move (actor_target_spot))
            : _objects.restore_relocation_aggregate (restored_state, staging->targets,
                                                     staging->restore_identity,
                                                     pending->cancellation->get_token ()));
        // The Kernel restores Actor state; canonical raw intake installs each
        // saved payload together with its original terminal, before Ready.
        if (restored == stateful::stateful_error_t::none)
            for (const auto &[target, data, participant_id] : saved_application)
                if (!co_await stage_relocation_record (target, data, participant_id))
                    throw framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Restored Actor application intake failed");
        if (restored == stateful::stateful_error_t::none
            || restored == stateful::stateful_error_t::already_exists) {
            // Restore owns the retirement of a previous moving source record.
            // Reflect that accepted retirement in the same identity registry;
            // a different identity installed during restore remains untouched.
            co_await _spot_actor_index_lane.run_task ([&] {
                for (const auto &previous : previous_actor_identities) {
                    const auto current = _actors.find (previous.key);
                    if (current != _actors.end () && current->second.second == previous)
                        _actors.erase (current);
                }
            });
            co_return true;
        }
    }
    catch (const framework_exception_t &error) {
        failure = error;
    }
    catch (const std::exception &error) {
        failure.emplace (framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        failure.emplace (framework_error_kind_t::internal_failure, "Relocation restore failed");
    }
    if (restored == stateful::stateful_error_t::none) {
        relocation_target_attempt_t owned;
        owned.terminal = std::make_shared<task_completion_source_t<void>> ();
        owned.prepare = pending->prepare;
        owned.restore_identity = staging->restore_identity;
        owned.targets = staging->targets;
        owned.wire_objects = staging->wire_objects;
        std::vector<relocation_target_attempt_t> discarded;
        discarded.push_back (std::move (owned));
        co_await discard_relocation_target_attempts (std::move (discarded));
    } else
        discard_relocation_assembly_staging (*pending, *staging);
    const messaging::request_failure_mapper_t mapper;
    if (!failure) {
        const auto [terminal, code] = stateful_failure_pair (restored);
        failure = mapper.reply_header_exception (terminal, code, "Relocation restore");
    }
    reply_relocation_assembly_failure (
      *pending, static_cast<protocol::framework_error_code> (
                  mapper.target_failure_code (failure->kind (), detail::failure_code (*failure))));
    co_return false;
}

task_t<bool>
public_host_runtime_t::submit_relocation_ready_reply (protocol::relocation_prepare_t prepare,
                                                      const mesh::service_mailbox_record_t &request)
{
    auto submit = co_await _transport->prepare_relocation_ready_reply (
      request, protocol::relocation_ready_t{prepare.relocation, prepare.target_attempt_generation,
                                            prepare.coordinator, prepare.target, prepare.object,
                                            protocol::relocation_role_t::target});
    const relocation_attempt_key_t key{prepare.relocation.high, prepare.relocation.low,
                                       prepare.target_attempt_generation};
    const auto current_attempt = [&] () -> relocation_target_attempt_t & {
        const auto found = _relocation_target_attempts.find (key);
        if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
            || found == _relocation_target_attempts.end () || found->second.prepare != prepare
            || !found->second.pending || found->second.targets.empty ())
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Relocation target stopped before readiness reply");
        return found->second;
    };
    const auto source = co_await _relocation_session_terminal_lane.run_task (
      [&] () -> std::optional<stateful::object_ref_t> {
          const auto &attempt = current_attempt ();
          if (relocation_target_wire_ready (attempt))
              return std::nullopt;
          if (attempt.sources.empty () || !_relocation_authority)
              throw framework_exception_t (framework_error_kind_t::unavailable,
                                           "Relocation target preparation has no source authority");
          return stateful::relocation_primary (attempt.sources);
      });
    if (source) {
        const auto prepared =
          co_await _relocation_authority->record_target_prepared (*source, prepare);
        if (!prepared)
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Relocation target preparation lost its authority fence");
    }
    co_return co_await _relocation_session_terminal_lane.run_task ([&] {
        auto &attempt = current_attempt ();
        const auto sent = submit && submit ();
        if (sent && !relocation_target_wire_ready (attempt))
            set_relocation_finalize_at_on_lane (attempt, std::chrono::steady_clock::now ()
                                                           + _relocation_cutover_wait);
        return sent;
    });
}

void public_host_runtime_t::observe_relocation_ready (
  std::shared_ptr<task_completion_source_t<void>> ready,
  mesh::service_mailbox_record_t request,
  protocol::relocation_prepare_t prepare)
{
    auto ready_task = ready->task ();
    detail::observe_task_completion (
      ready_task, [self = shared_from_this (),
                   request = std::make_shared<mesh::service_mailbox_record_t> (std::move (request)),
                   prepare = std::move (prepare)] (const result_t<void> &result) {
          const auto failed = [self, request, prepare] (const framework_exception_t &error) {
              (void) self->_transport->reply_relocation_failed (
                *request,
                protocol::relocation_failed_t{
                  prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
                  prepare.target, prepare.object, protocol::relocation_role_t::target,
                  messaging::request_failure_mapper_t{}.target_failure_code (
                    error.kind (), detail::failure_code (error))});
          };
          if (!result) {
              failed (result.error ()
                        ? *result.error ()
                        : framework_exception_t (framework_error_kind_t::internal_failure,
                                                 "Relocation readiness failed without a cause"));
              return;
          }
          auto submitted = std::make_shared<task_t<bool>> (
            self->submit_relocation_ready_reply (prepare, *request));
          detail::observe_task_completion (
            *submitted, [submitted, failed] (const result_t<bool> &sent) {
                if (!sent || !sent.value ())
                    failed (sent.error () ? *sent.error ()
                                          : framework_exception_t (
                                              framework_error_kind_t::unavailable,
                                              "Relocation readiness reply could not be submitted"));
            });
      });
}

void public_host_runtime_t::request_peer_reconcile (
  const std::vector<std::uint8_t> &target,
  std::uint64_t generation,
  const std::function<void (const std::vector<std::uint8_t> &)> &reconcile)
{
    const auto local = _transport->topology ().local_descriptor ();
    if (target == local.node_routing_id && generation == local.lifecycle_generation)
        return;
    if (!peer_route_ready (_transport->topology ().peer (target), generation) && reconcile)
        reconcile (target);
}

task_t<void> public_host_runtime_t::await_relocation_session_routes (
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  const std::vector<protocol::session_relocation_route_t> &routes)
{
    const auto local = _transport->topology ().local_descriptor ();
    const auto reconcile = _transport->peer_reconcile ();
    using route_identity_t = std::pair<std::vector<std::uint8_t>, std::uint64_t>;
    std::map<route_identity_t, mesh::service_topology_registry_t::peer_observer_t> observers;
    for (const auto &route : routes) {
        if (route.session_owner_node_routing_id == local.node_routing_id
            && route.session_owner_node_generation == local.lifecycle_generation)
            continue;
        const route_identity_t identity{route.session_owner_node_routing_id,
                                        route.session_owner_node_generation};
        if (observers.contains (identity))
            continue;
        auto observer = _transport->topology ().observe_peer (identity.first);
        request_peer_reconcile (identity.first, identity.second, reconcile);
        observers.emplace (identity, std::move (observer));
    }
    auto control = std::make_shared<task_completion_source_t<void>> ();
    auto controlled = control->task ();
    std::stop_callback stopped (
      pending->cancellation->get_token (), [weak = std::weak_ptr{control}] {
          if (const auto signal = weak.lock ())
              signal->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                         "Relocation target stopped"));
      });
    struct deadline_registration_t
    {
        std::shared_ptr<detail::deadline_state_t> state =
          std::make_shared<detail::deadline_state_t> ();
        ~deadline_registration_t ()
        {
            state->cancelled.store (true, std::memory_order_release);
            detail::deadline_scheduler_t::instance ().wake ();
        }
    } deadline;
    const auto at = messaging::request_deadline_from_unix_ms (pending->prepare.deadline_unix_ms);
    detail::deadline_scheduler_t::instance ().schedule (
      at, deadline.state, [weak = std::weak_ptr{control}] {
          if (const auto signal = weak.lock ())
              signal->complete (result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                                         "Relocation source deadline expired"));
      });
    for (;;) {
        auto wake = std::make_shared<task_completion_source_t<void>> ();
        auto notified = wake->task ();
        const auto notify = [weak = std::weak_ptr{wake}] (const result_t<void> &result) {
            if (const auto signal = weak.lock ())
                signal->complete (result);
        };
        struct subscriptions_t
        {
            std::stop_source owner;
            ~subscriptions_t () { owner.request_stop (); }
        } subscriptions;
        bool all_ready = true;
        {
            detail::ambient_context_scope_t scope (nullptr, nullptr,
                                                   subscriptions.owner.get_token (), false, true);
            detail::observe_task_completion (controlled, notify);
            for (const auto &[identity, observer] : observers) {
                auto observation = observer ();
                if (peer_route_ready (observation.peer, identity.second))
                    continue;
                all_ready = false;
                detail::observe_task_completion (observation.changed, notify);
            }
        }
        if (controlled.await_ready ())
            co_await controlled;
        if (std::chrono::steady_clock::now () >= at)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         "Relocation source deadline expired");
        if (all_ready)
            co_return;
        co_await notified;
    }
}

task_t<void> public_host_runtime_t::finish_relocation_assembly (
  relocation_attempt_key_t key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<relocation_assembly_staging_t> staging)
{
    std::vector<relocation_target_attempt_t> discarded_attempts;
    const auto can_stage = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto previous = _relocation_target_attempts.find (
          {pending->prepare.relocation.high, pending->prepare.relocation.low,
           pending->prepare.target_attempt_generation - 1});
        if (previous == _relocation_target_attempts.end ())
            return true;
        auto &attempt = previous->second;
        if (attempt.prepare.coordinator != pending->prepare.coordinator
            || attempt.prepare.object != pending->prepare.object
            || attempt.prepare.target != pending->prepare.target
            || attempt.phase != relocation_target_phase_t::waiting_cutover)
            return false;
        if (auto discarded = take_relocation_target_discard (attempt))
            discarded_attempts.push_back (std::move (*discarded));
        return !discarded_attempts.empty ();
    });
    if (!can_stage) {
        co_await reply_relocation_assembly_failure (key, pending,
                                                    protocol::framework_error_code::requestFailed);
        co_return;
    }
    co_await discard_relocation_target_attempts (std::move (discarded_attempts));
    std::size_t wire_count = 0;
    for (; wire_count != staging->targets.size (); ++wire_count) {
        if (register_relocation_target_queue (pending->prepare, staging->targets[wire_count],
                                              staging->wire_objects[wire_count], wire_count + 1))
            continue;
        staging->wire_objects.resize (wire_count);
        unregister_relocation_wire_targets (pending->prepare.relocation,
                                            pending->prepare.target_attempt_generation,
                                            staging->wire_objects);
        co_await reply_relocation_assembly_failure (key, pending,
                                                    protocol::framework_error_code::requestFailed);
        co_return;
    }
    std::vector<std::pair<std::string, stateful::object_ref_t>> consumed;
    std::optional<protocol::framework_error_code> consumption_failure;
    try {
        if (_actor_join_recovery_consumer) {
            for (std::size_t index = 0; index != staging->frozen.size (); ++index) {
                auto &frozen = staging->frozen[index];
                if (frozen.owner.kind != stateful::object_kind_t::actor)
                    continue;
                const auto stable_type = frozen.stable_type;
                if (!co_await _actor_join_recovery_consumer (frozen, staging->targets[index],
                                                             pending->prepare)) {
                    consumption_failure = protocol::framework_error_code::requestProtocolError;
                    break;
                }
                consumed.emplace_back (stable_type, staging->targets[index]);
            }
        }
    }
    catch (const framework_exception_t &error) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        consumption_failure = static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (error.kind (),
                                                                     detail::failure_code (error)));
    }
    catch (const std::exception &) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        consumption_failure = static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (
            framework_error_kind_t::internal_failure));
    }
    if (consumption_failure) {
        co_await rollback_actor_join_recoveries (consumed);
        unregister_relocation_wire_targets (pending->prepare.relocation,
                                            pending->prepare.target_attempt_generation,
                                            staging->wire_objects);
        co_await reply_relocation_assembly_failure (key, pending, *consumption_failure);
        co_return;
    }
    const auto registered = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        return !_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
               && found != _relocation_target_attempts.end () && found->second.pending == pending;
    });
    if (!registered) {
        co_await rollback_actor_join_recoveries (consumed);
        discard_relocation_assembly_staging (*pending, *staging);
        co_await reply_relocation_assembly_failure (
          key, pending,
          static_cast<protocol::framework_error_code> (
            messaging::request_failure_mapper_t{}.target_failure_code (
              framework_error_kind_t::shutting_down)));
        co_return;
    }
    bool restored = false;
    std::optional<framework_exception_t> failure;
    try {
        restored = co_await restore_relocation_assembly (pending, staging);
        if (restored) {
            co_await await_relocation_session_routes (pending, staging->session_routes);
            if (co_await activate_relocation_assembly (key, pending, staging))
                co_return;
            failure.emplace (framework_error_kind_t::unavailable,
                             "Relocation readiness reply could not be submitted");
        }
    }
    catch (const framework_exception_t &error) {
        failure = error;
    }
    catch (const std::exception &error) {
        failure.emplace (framework_error_kind_t::internal_failure, error.what ());
    }
    co_await rollback_actor_join_recoveries (consumed);
    if (!restored) {
        auto code = protocol::framework_error_code::requestFailed;
        if (failure) {
            trace_mesh_host ("relocation-assembly-failed", failure->what ());
            code = static_cast<protocol::framework_error_code> (
              messaging::request_failure_mapper_t{}.target_failure_code (
                failure->kind (), detail::failure_code (*failure)));
            if (!pending->ready->task ().await_ready ())
                discard_relocation_assembly_staging (*pending, *staging);
        }
        co_await reply_relocation_assembly_failure (key, pending, code);
        co_return;
    }
    auto discarded = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end () || found->second.pending != pending)
            return std::optional<relocation_target_attempt_t>{};
        if (found->second.targets.empty () && !found->second.restore_identity.reference.empty ())
            return std::optional<relocation_target_attempt_t>{};
        if (restored && found->second.targets.empty ()) {
            found->second.restore_identity = staging->restore_identity;
            found->second.sources = std::move (staging->sources);
            found->second.targets = std::move (staging->targets);
            found->second.wire_objects = std::move (staging->wire_objects);
        }
        if (!found->second.targets.empty ())
            return take_relocation_target_discard (found->second);
        return std::optional<relocation_target_attempt_t>{};
    });
    if (discarded) {
        std::vector<relocation_target_attempt_t> cleanup;
        cleanup.push_back (std::move (*discarded));
        co_await discard_relocation_target_attempts (std::move (cleanup), std::move (failure));
    }
}

task_t<bool> public_host_runtime_t::activate_relocation_assembly (
  const relocation_attempt_key_t &key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  std::shared_ptr<relocation_assembly_staging_t> staging)
{
    std::optional<task_t<bool>> register_indexes;
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (_options.mesh.shutdown_admission_seal->load (std::memory_order_acquire)
            || found == _relocation_target_attempts.end () || found->second.pending != pending
            || relocation_target_terminal (found->second))
            throw framework_exception_t (framework_error_kind_t::shutting_down,
                                         "Relocation target stopped before activation");
        auto &attempt = found->second;
        attempt.restore_identity = staging->restore_identity;
        attempt.sources = std::move (staging->sources);
        attempt.targets = std::move (staging->targets);
        attempt.wire_objects = std::move (staging->wire_objects);
        for (auto &route : staging->session_routes) {
            attempt.session_routes.emplace_back ();
            attempt.session_routes.back ().route = std::move (route);
        }
        // The attempt owns cleanup before any raw identity is registered.
        // Submit on this turn so a later discard queues its index removal
        // after registration. The index turn reads only its owned inputs.
        register_indexes.emplace (_spot_actor_index_lane.run_task (
          [this, targets = attempt.targets, wire_objects = attempt.wire_objects] {
              // Relocation §4.3/§4.5: prepare raw identities before Ready CAS.
              // The recovering gate still prevents application execution.
              // Validate the whole unit before inserting any Actor.
              for (const auto &target : targets) {
                  if (target.kind != stateful::object_kind_t::actor)
                      continue;
                  const auto current = _actors.find (target.key);
                  if (current != _actors.end () && current->second.second != target)
                      return false;
              }
              for (std::size_t index = 0; index != targets.size (); ++index) {
                  const auto &target = targets[index];
                  if (target.kind == stateful::object_kind_t::actor)
                      _actors.try_emplace (target.key, wire_objects[index].stable_type, target);
              }
              return true;
          }));
    });
    if (!co_await *register_indexes)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Relocation target Actor identity is already registered");
    if (!co_await submit_relocation_ready_reply (pending->prepare, pending->request))
        co_return false;
    pending->ready->complete (result_t<void>::success ());
    co_return true;
}

task_t<void> public_host_runtime_t::reply_relocation_assembly_failure (
  relocation_attempt_key_t key,
  std::shared_ptr<const pending_relocation_assembly_t> pending,
  protocol::framework_error_code code)
{
    const auto terminal = co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found == _relocation_target_attempts.end () || found->second.pending != pending)
            return std::shared_ptr<task_completion_source_t<void>>{};
        return found->second.terminal;
    });
    if (!pending->ready->task ().await_ready ())
        reply_relocation_assembly_failure (*pending, code);
    co_await _relocation_session_terminal_lane.run_task ([&] {
        const auto found = _relocation_target_attempts.find (key);
        if (found != _relocation_target_attempts.end () && found->second.pending == pending)
            _relocation_target_attempts.erase (found);
    });
    if (terminal)
        terminal->complete (result_t<void>::success ());
}

void public_host_runtime_t::complete_relocation_assembly (
  const relocation_attempt_key_t &key, std::shared_ptr<pending_relocation_assembly_t> held_pending)
{
    auto &pending = *held_pending;
    // Verified identity and payload-integrity failures retain relocationDataLost.
    // Restore failures preserve their Framework kind and cause code instead.
    const auto reply_failure = [&] (protocol::framework_error_code code =
                                      protocol::framework_error_code::relocationDataLost) {
        auto failed = std::make_shared<task_t<void>> (
          reply_relocation_assembly_failure (key, held_pending, code));
        detail::observe_task_completion (
          *failed, [self = shared_from_this (), failed] (const result_t<void> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { result.value (); });
          });
    };
    try {
        auto payload = pending.assembly.take_payload ();
        /* The direct-transfer payload is exactly the schema's
     * relocation-envelope-v1 logical stream (28 §4.2): no provider
     * envelope, no embedded digest, no session-route section. Identity
     * and integrity were already verified by the assembly against the
     * command-40 manifest. */
        const auto envelope = stateful::maintenance_runtime_t::decode_envelope (payload);
        if (!envelope) {
            trace_mesh_host ("relocation-assembly-failed", "stage=decode");
            reply_failure ();
            return;
        }
        if (envelope->object.kind != pending.prepare.object.kind
            || envelope->object.object_id != pending.prepare.object.object_id
            || envelope->object.object_generation != pending.prepare.object.object_generation
            || envelope->application_version != pending.prepare.application_version
            || (envelope->object.kind != protocol::relocation_object_kind_t::instance_spot
                && envelope->object.expected_authority_owner_generation
                     != pending.prepare.object.expected_authority_owner_generation)) {
            trace_mesh_host ("relocation-assembly-failed",
                             "stage=principal-identity-or-application-version");
            reply_failure ();
            return;
        }

        const auto local = status ();
        stateful::object_kind_t principal_kind;
        switch (envelope->object.kind) {
            case protocol::relocation_object_kind_t::actor:
                principal_kind = stateful::object_kind_t::actor;
                break;
            case protocol::relocation_object_kind_t::user_spot:
                principal_kind = stateful::object_kind_t::user_spot;
                break;
            case protocol::relocation_object_kind_t::instance_spot:
                principal_kind = stateful::object_kind_t::instance_spot;
                break;
            default:
                reply_failure ();
                return;
        }
        stateful::relocation_participant_identity_t principal_identity;
        principal_identity.owner.kind = principal_kind;
        principal_identity.owner.key = envelope->object.object_id;
        principal_identity.owner.object_generation = envelope->object.object_generation;
        principal_identity.owner.authority_owner_generation =
          pending.prepare.object.expected_authority_owner_generation != 0
            ? pending.prepare.object.expected_authority_owner_generation
            : envelope->object.expected_authority_owner_generation;
        principal_identity.owner.mesh_name = _options.mesh.descriptor.mesh_name;
        principal_identity.owner.node_id =
          std::string (pending.prepare.source_node_routing_id.begin (),
                       pending.prepare.source_node_routing_id.end ());
        principal_identity.stable_type = pending.prepare.object.stable_type;

        auto inventory =
          _relocation_authority
            ? _relocation_authority->read_target_inventory (principal_identity, pending.prepare)
            : std::nullopt;
        if (!inventory) {
            trace_mesh_host ("relocation-assembly-failed", "stage=sealed-inventory");
            reply_failure ();
            return;
        }
        auto materialized =
          stateful::maintenance_runtime_t::materialize_envelope (*envelope, std::move (*inventory));
        if (!materialized) {
            trace_mesh_host ("relocation-assembly-failed", "stage=materialize");
            reply_failure ();
            return;
        }
        relocation_assembly_staging_t staging;
        staging.frozen = std::move (*materialized);
        auto &frozen = staging.frozen;

        auto &sources = staging.sources;
        auto &targets = staging.targets;
        auto &wire_objects = staging.wire_objects;
        bool principal_found = false;
        for (const auto &saved : frozen) {
            protocol::relocation_object_kind_t kind;
            switch (saved.owner.kind) {
                case stateful::object_kind_t::actor:
                    kind = protocol::relocation_object_kind_t::actor;
                    break;
                case stateful::object_kind_t::user_spot:
                    kind = protocol::relocation_object_kind_t::user_spot;
                    break;
                case stateful::object_kind_t::instance_spot:
                    kind = protocol::relocation_object_kind_t::instance_spot;
                    break;
                default:
                    reply_failure ();
                    return;
            }
            if (saved.owner.authority_owner_generation
                == std::numeric_limits<std::uint64_t>::max ()) {
                reply_failure ();
                return;
            }
            protocol::relocation_object_t wire_object{kind, saved.stable_type, saved.owner.key,
                                                      saved.owner.object_generation,
                                                      saved.owner.authority_owner_generation};
            principal_found = principal_found || wire_object == pending.prepare.object;
            auto target = saved.owner;
            target.node_id = local.routing_id ().to_string ();
            // Hidden staging has no target-issued generation until Store CAS.
            sources.push_back (saved.owner);
            targets.push_back (std::move (target));
            wire_objects.push_back (std::move (wire_object));
        }
        if (!principal_found) {
            trace_mesh_host ("relocation-assembly-failed", "stage=principal-not-found");
            reply_failure ();
            return;
        }

        /* No wrapper digest travels with the stream any more: the derived
     * inventory digest is recomputed from the reconstructed participants,
     * the same value every source computes before publishing an authority
     * row (an unpublished row's all-zero digest is the pending sentinel). */
        std::vector<stateful::object_ref_t> digest_owners;
        digest_owners.reserve (frozen.size ());
        for (const auto &saved : frozen)
            digest_owners.push_back (saved.owner);
        const auto inventory_digest =
          stateful::maintenance_runtime_t::compute_inventory_digest (digest_owners);

        for (const auto &binding : envelope->session_bindings) {
            if (binding.participant_id == 0 || binding.participant_id > frozen.size ()) {
                reply_failure ();
                return;
            }
            const auto &saved = frozen[binding.participant_id - 1];
            if (saved.owner.kind != stateful::object_kind_t::actor) {
                reply_failure ();
                return;
            }
            protocol::session_relocation_route_t route;
            route.relocation = pending.prepare.relocation;
            route.coordinator = pending.prepare.coordinator;
            route.sender_role = protocol::relocation_role_t::target;
            route.actor = {saved.owner.key, saved.owner.object_generation};
            route.session_owner_node_routing_id = binding.session_owner_node_rid;
            route.session_owner_node_generation = binding.session_owner_node_generation;
            route.session_owner_id = binding.session_owner_id;
            route.session_owner_lease_generation = binding.session_owner_lease_generation;
            route.session_routing_id = binding.session_rid;
            route.binding_generation = binding.binding_generation;
            route.route.action = protocol::session_relocation_route_action_t::commit;
            route.route.previous_authority_owner_generation =
              saved.owner.authority_owner_generation;
            route.route.target_node_routing_id = local.routing_id ().to_bytes ();
            route.route.target_node_generation = local.lifecycle_generation ();
            route.route.target_owner_lease_generation =
              pending.prepare.target.target_owner_lease_generation;
            staging.session_routes.push_back (std::move (route));
        }

        staging.restore_identity = {"direct:" + std::to_string (pending.prepare.relocation.high)
                                      + ":" + std::to_string (pending.prepare.relocation.low) + ":"
                                      + std::to_string (pending.prepare.target_attempt_generation),
                                    pending.prepare.payload_checksum_crc32c, inventory_digest};
        auto held_staging = std::make_shared<relocation_assembly_staging_t> (std::move (staging));
        auto preparing = std::make_shared<task_t<void>> (
          finish_relocation_assembly (key, held_pending, held_staging));
        detail::observe_task_completion (
          *preparing, [self = shared_from_this (), preparing] (const result_t<void> &result) {
              self->_options.mesh.runtime_failures->capture ([&] { result.value (); });
          });
    }
    catch (const framework_exception_t &error) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        reply_failure (static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (
            error.kind (), detail::failure_code (error))));
    }
    catch (const std::exception &) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        reply_failure (static_cast<protocol::framework_error_code> (
          messaging::request_failure_mapper_t{}.target_failure_code (
            framework_error_kind_t::internal_failure)));
    }
}

bool public_host_runtime_t::register_relocation_target_queue (
  const protocol::relocation_prepare_t &prepare,
  const stateful::object_ref_t &target,
  const protocol::relocation_object_t &wire_object,
  std::uint64_t participant_id)
{
    try {
        const relocation_attempt_key_t attempt_key{prepare.relocation.high, prepare.relocation.low,
                                                   prepare.target_attempt_generation};
        return _relocation_wire->register_target (
          {prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
           prepare.source_node_routing_id, prepare.source_node_generation, wire_object,
           [this, target, attempt_key, participant_id] (const protocol::relocation_data_t &data) {
               /* 28 §4.4: a pre-boundary relay record waits in the attempt's
                * boundary batch as its own accepted record; the verified
                * cutover alone stages the batch. A record after that
                * cutover can only be the source's retransmission of the
                * batch the target already took, so it is not staged again. */
               return _relocation_session_terminal_lane.run_sync ([&] {
                   const auto found = _relocation_target_attempts.find (attempt_key);
                   if (found == _relocation_target_attempts.end ()
                       || !relocation_target_wire_ready (found->second))
                       return false;
                   if (found->second.phase == relocation_target_phase_t::waiting_cutover)
                       found->second.boundary_batch.emplace_back (target, data, participant_id);
                   return true;
               });
           },
           [] (const protocol::relocation_data_t &) {}});
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        return false;
    }
}

task_t<bool>
public_host_runtime_t::stage_relocation_record (const stateful::object_ref_t &target,
                                                const protocol::relocation_data_t &data,
                                                std::uint64_t participant_id)
{
    if (!_stateful_dispatch)
        co_return false;
    const relocation_attempt_key_t key{data.relocation.high, data.relocation.low,
                                       data.target_attempt_generation};
    auto delivery =
      data.record.reply_route_id ? std::make_shared<task_completion_source_t<void>> () : nullptr;
    const auto delivered = [weak = weak_from_this (), key, delivery] {
        if (const auto owner = weak.lock ())
            owner->record_relocation_reply_delivered (key, delivery);
        return true;
    };
    const auto staged = co_await _stateful_dispatch->stage_relocated (
      target, {0, protocol::encode_frozen_record (data.record)},
      [this, relocation = data.relocation,
       target_attempt_generation = data.target_attempt_generation, coordinator = data.coordinator,
       participant_id, delivered,
       frozen_record = data.record] (const std::optional<protocol::application_payload_t> &reply) {
          if (!frozen_record.reply_route_id)
              return true;
          auto relay = stateful::maintenance_runtime_t::terminal_identity (
            relocation, target_attempt_generation, coordinator, participant_id, frozen_record);
          relay.terminal_result =
            reply ? 0u
                  : static_cast<std::uint32_t> (protocol::request_terminal_result::internalError);
          relay.failure_code = reply ? protocol::framework_error_code::none
                                     : protocol::framework_error_code::requestFailed;
          return _relocation_wire->register_terminal_target (
            {relay, frozen_record.source, reply,
             [delivered] (protocol::reply_relay_ack_status_t) { return delivered (); }, delivered});
      });
    if (staged != stateful::stateful_error_t::none)
        co_return false;
    if (delivery)
        co_await _relocation_session_terminal_lane.run_task ([&] {
            const auto found = _relocation_target_attempts.find (key);
            if (found != _relocation_target_attempts.end ())
                found->second.captured_requests.push_back ({target.kind, target.key, delivery});
        });
    co_return true;
}

bool public_host_runtime_t::relocation_management_due (
  std::chrono::steady_clock::time_point now) const noexcept
{
    return now.time_since_epoch ().count ()
           >= _relocation_next_due.load (std::memory_order_acquire);
}

void public_host_runtime_t::publish_relocation_next_due_on_lane () const
{
    auto next = relocation_never_due;
    const auto include = [&next] (std::chrono::steady_clock::time_point deadline) {
        next = std::min (next, deadline.time_since_epoch ().count ());
    };
    for (const auto &[key, assembly] : _relocation_assemblies)
        include (assembly.expires_at);
    for (const auto &[key, attempt] : _relocation_target_attempts) {
        if (!relocation_target_terminal (attempt) && relocation_target_wire_ready (attempt)
            && attempt.phase < relocation_target_phase_t::dispatch_open)
            include (attempt.next_finalize_at);
    }
    for (const auto &[key, seal] : _session_seal_terminals) {
        if (seal.consumed)
            continue;
        if (!seal.ready) {
            next = relocation_due_now;
            break;
        }
        include (seal.expires_at);
    }
    _relocation_next_due.store (next, std::memory_order_release);
}

void public_host_runtime_t::configure_relocation_gate (
  relocation_gate_t gate,
  native_spot_target_t target,
  mesh::service_mailbox_t::application_dispatch_t resume)
{
    _lifecycle_configuration_lane.run_sync ([&] {
        if (_started || _closing)
            throw std::logic_error ("relocation gate must be configured before start");
        _relocation_gate = std::move (gate);
        _native_spot_target = std::move (target);
        _relocation_resume = std::move (resume);
    });
}

task_t<std::optional<std::vector<std::vector<stateful::turn_record_t>>>>
public_host_runtime_t::cut_relocation_source (
  std::vector<stateful::object_ref_t> participants,
  std::shared_ptr<stateful::relocation_seal_binding_t> binding)
{
    const auto [locate, resume] = co_await _lifecycle_configuration_lane.run_task (
      [this] { return std::pair{_relocation_gate, _relocation_resume}; });
    // Aggregate units contain one SpotWide Spot and its Actors. Actor FIFOs use
    // that Spot's shared gate; their own suffixes are captured by the Actor owner.
    const auto principal =
      std::find_if (participants.begin (), participants.end (), [] (const auto &owner) {
          return owner.kind != stateful::object_kind_t::actor;
      });
    const auto gate_index = static_cast<std::size_t> (principal - participants.begin ());
    std::shared_ptr<runtime::serial_execution_queue_t> gate;
    if (locate && principal != participants.end ())
        gate = co_await locate (*principal);
    if (gate) {
        struct finish_relocation_t
        {
            static task_t<void> run (public_host_runtime_t *self,
                                     std::weak_ptr<runtime::serial_execution_queue_t> weak_gate,
                                     std::weak_ptr<stateful::relocation_seal_binding_t> weak,
                                     stateful::object_ref_t owner,
                                     std::decay_t<decltype (resume)> resume,
                                     bool restored)
            {
                const auto binding = weak.lock ();
                const auto gate = weak_gate.lock ();
                if (!binding || !gate)
                    co_return;
                if (restored && resume) {
                    (void) co_await self->dispatch_stateful_owner (owner, resume, gate, binding);
                } else if (!restored) {
                    (void) co_await gate->release_relocation_seal (binding);
                }
            }
        };
        binding->finish = [this, weak_gate = std::weak_ptr (gate), weak = std::weak_ptr (binding),
                           owner = *principal, resume] (bool restored) -> task_t<void> {
            return finish_relocation_t::run (this, weak_gate, weak, owner, resume, restored);
        };
    }
    std::vector<std::string> owners;
    owners.reserve (participants.size ());
    for (const auto &participant : participants)
        owners.push_back (mesh::service_mailbox_t::application_owner (
          participant.kind == stateful::object_kind_t::actor ? owner_kind_t::actor
                                                             : owner_kind_t::spot,
          participant.key));
    // The cut takes only originals the stateful ingress places in a source FIFO.
    const auto admits = [] (const mesh::service_mailbox_record_t &original) {
        if (original.application) {
            const auto &record = original.application->record;
            return !record.application_record && is_stateful_application (record.kind);
        }
        if (original.parts ().size () != 2)
            return false;
        try {
            const auto kind = original.application_kind ();
            return kind && is_stateful_application (*kind);
        }
        catch (...) {
            return false;
        }
    };
    std::optional<std::vector<std::vector<mesh::service_mailbox_record_t>>> taken;
    const auto bind = [&binding] { binding->bound.store (true, std::memory_order_release); };
    // Producer order: the mailbox, then the owner's own producer boundary inside step.
    const auto cut = [&] (const std::function<void ()> &step) {
        taken = _transport->mailbox ().cut_owners (owners, admits, step);
        return taken.has_value ();
    };
    std::vector<std::shared_ptr<runtime::serial_application_record_t>> suffix;
    if (gate)
        suffix = co_await gate->take_application_suffix (bind, cut, binding);
    else
        (void) cut (bind);
    if (!taken)
        co_return std::nullopt;
    // Originals accepted before the cut enter their source FIFOs in mailbox order, ahead of
    // the ingress held after it; each keeps its reply route and mailbox credit.
    for (std::size_t index = 0; index != participants.size (); ++index)
        for (auto &original : (*taken)[index])
            co_await ingest_cut_original (participants[index], std::move (original));
    // The owner's exported suffix keeps its accepted owner, like the Actor suffix.
    std::vector<std::vector<stateful::turn_record_t>> exported (participants.size ());
    for (auto &record : suffix) {
        stateful::turn_record_t turn;
        if (record->invocation ())
            turn.application_payload_bytes = record->invocation ()->first.size ();
        else if (record->payload ()) {
            turn = std::move (*record->payload ());
            record->payload ().reset ();
        }
        turn.accepted_owner = std::move (record);
        exported[gate_index].push_back (std::move (turn));
    }
    co_return exported;
}

task_t<void> public_host_runtime_t::ingest_cut_original (stateful::object_ref_t owner,
                                                         mesh::service_mailbox_record_t original)
{
    if (!original.application) {
        auto retained = co_await _objects.retained_relocation_source (owner);
        (void) co_await _stateful_dispatch->ingest_claimed (std::move (owner), std::move (original),
                                                            std::move (retained));
        co_return;
    }
    std::optional<mesh::service_mailbox_record_t> taken;
    std::optional<messaging::request_wire_failure_t> failure;
    taken.emplace (std::move (original));
    try {
        auto query = local_application_query (*taken->application);
        auto application = taken->application.retain ();
        auto source = taken->source_original;
        (void) co_await accept_local_application (std::move (*taken), std::move (query),
                                                  std::move (application), std::move (source),
                                                  taken);
        co_return;
    }
    catch (const protocol::service_wire_error_t &) {
        failure = messaging::request_wire_failure_t{
          static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
          static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError)};
    }
    catch (const framework_exception_t &error) {
        if (error.kind () == framework_error_kind_t::internal_failure)
            _options.mesh.runtime_failures->report (std::current_exception ());
        failure = messaging::request_failure_mapper_t{}.target_failure_reply (error);
    }
    catch (...) {
        _options.mesh.runtime_failures->report (std::current_exception ());
        failure = messaging::request_failure_mapper_t{}.target_failure_reply (
          framework_error_kind_t::internal_failure);
    }
    // A rejected original is answered once from the record this ingress still owns.
    if (!taken)
        co_return;
    const auto credit = std::exchange (taken->transferred, {});
    if (failure && !taken->source_routing_id.empty () && taken->correlation)
        (void) _transport->reply_failure (*taken, failure->terminal_result, failure->failure_code);
    if (credit)
        (void) credit ();
}

} // namespace zlink::framework::runtime::host
