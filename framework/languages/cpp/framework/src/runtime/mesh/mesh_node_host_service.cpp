/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/configuration/service_scope.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/instance_spot_application.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/utils/available_processor_count.hpp"
#include "runtime/utils/weak_runtime_owner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::execute_actor_create_operation_target;
using mesh_node_host_service_detail::mesh_new_claim_intent;
using mesh_node_host_service_detail::reject_application_request;
using mesh_node_host_service_detail::terminal_callback_guard_t;
using mesh_node_host_service_detail::trace_mesh_application;

namespace
{
bool requires_location_owner_state (const mesh::service_mailbox_record_t &record)
{
    if (record.application) {
        return !detail::is_node_or_channel_record_kind (record.application->record.kind);
    }
    if (record.parts ().empty ())
        return true;
    const auto header = record.service_header ();
    switch (header.kind) {
        case protocol::command::nodeSend:
        case protocol::command::nodeRequest:
        case protocol::command::channelSend:
        case protocol::command::channelRequest:
            return false;
        default:
            return true;
    }
}

} // namespace

struct mesh_node_host_service_t::actor_destroy_callback_gate_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::size_t active = 0;

    bool try_enter ()
    {
        std::lock_guard lock (mutex);
        if (stopping)
            return false;
        ++active;
        return true;
    }

    void leave ()
    {
        std::lock_guard lock (mutex);
        --active;
        if (active == 0)
            changed.notify_all ();
    }

    void stop_and_wait () noexcept
    {
        std::unique_lock lock (mutex);
        stopping = true;
        changed.wait (lock, [this] { return active == 0; });
    }
};

application_dispatch_terminal_owner_t::application_dispatch_terminal_owner_t (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::function<void ()> complete_stateful_dispatch,
  host::mailbox_callback_t release_mailbox_reservation,
  handler_entry_t application_entry) :
    _application_entry (std::move (application_entry)),
    _node (std::move (node)),
    _complete_stateful_dispatch (std::move (complete_stateful_dispatch)),
    _release_mailbox_reservation (std::move (release_mailbox_reservation))
{
}

application_dispatch_terminal_owner_t::application_dispatch_terminal_owner_t (
  application_dispatch_terminal_owner_t &&other) noexcept :
    _application_entry (std::move (other._application_entry)),
    _settled (other._settled.exchange (true, std::memory_order_acq_rel)),
    _node (std::move (other._node)),
    _physical_work (std::move (other._physical_work)),
    _complete_stateful_dispatch (std::move (other._complete_stateful_dispatch)),
    _release_mailbox_reservation (std::move (other._release_mailbox_reservation))
{
    other._complete_stateful_dispatch = {};
    other._release_mailbox_reservation = {};
}

application_dispatch_terminal_owner_t::~application_dispatch_terminal_owner_t () noexcept
{
    settle ();
}

void application_dispatch_terminal_owner_t::settle () noexcept
{
    if (_settled.exchange (true, std::memory_order_acq_rel))
        return;

    _application_entry = {};
    invoke (_complete_stateful_dispatch);
    invoke (_release_mailbox_reservation);
    auto node = std::move (_node);
    _complete_stateful_dispatch = {};
    _release_mailbox_reservation = {};
    if (node)
        node->application_work_finished ();
    node.reset ();
    _physical_work.reset ();
}

host::mailbox_callback_t application_dispatch_terminal_owner_t::take_mailbox_reservation () noexcept
{
    return std::exchange (_release_mailbox_reservation, {});
}

void application_dispatch_terminal_owner_t::retain_physical_work (
  offload_executor_t::retained_work_t work) noexcept
{
    _physical_work = std::move (work);
}

template <typename Callback>
void application_dispatch_terminal_owner_t::invoke (const Callback &callback) noexcept
{
    if (!callback)
        return;
    try {
        callback ();
    }
    catch (...) {
    }
}

namespace
{
void trace_mesh_host_stop (const char *stage)
{
    const char *value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    if (value != nullptr && std::string_view (value) != "" && std::string_view (value) != "0")
        std::cerr << "zlink-cpp-host-stop stage=mesh-host-" << stage << std::endl;
}

handler_registry_t &empty_handler_filters ()
{
    static handler_registry_t filters;
    return filters;
}
} // namespace

mesh_node_host_service_t::mesh_node_host_service_t (
  std::shared_ptr<runtime_failure_collector_t> failures,
  std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
  serializer_registry_t &serializers,
  dispatch_options_t dispatch_options,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    mesh_node_host_service_t (std::move (failures),
                              std::move (registrations),
                              serializers,
                              empty_handler_filters (),
                              std::move (dispatch_options),
                              std::move (listener_statuses),
                              std::move (application_jobs))
{
}

mesh_node_host_service_t::mesh_node_host_service_t (
  std::shared_ptr<runtime_failure_collector_t> failures,
  std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
  serializer_registry_t &serializers,
  handler_registry_t &filters,
  dispatch_options_t dispatch_options,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    hosted_service_lifecycle_t (std::move (failures)),
    _registrations (std::move (registrations)),
    _serializers (&serializers),
    _filters (&filters),
    _dispatch_options (std::make_shared<const dispatch_options_t> (std::move (dispatch_options))),
    _application_dispatch (std::make_shared<offload_executor_t> (
      _registrations.size () + 1,
      _registrations.size ()
        + std::max<std::size_t> (2, ::zlink::framework::runtime::available_processor_count ()),
      std::chrono::milliseconds (100),
      "zlink-mesh-app")),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (application_job_queue_configuration_t{
            application_job_queue_profile_t::balanced, std::nullopt, 1,
            static_cast<std::uint32_t> (std::numeric_limits<std::int32_t>::max ())})),
    _listener_statuses (std::move (listener_statuses))
{
    _nodes.reserve (_registrations.size ());
    for (const auto &registration : _registrations) {
        auto node = std::make_shared<detail::mesh_node_runtime_t> (registration);
        node->bind_serializers (serializers);
        _nodes.push_back (std::move (node));
    }
}

mesh_node_host_service_t::~mesh_node_host_service_t ()
{
    stop ();
}

using receive_activity_t = std::pair<bool, std::optional<std::chrono::steady_clock::time_point>>;

task_t<receive_activity_t>
next_receive_activity_async (std::shared_ptr<detail::mesh_node_runtime_t> node,
                             std::shared_ptr<detail::spot_node_builder_state_t> spot_state,
                             bool include_next_activity)
{
    detail::spot_node_runtime_t maintenance (std::move (spot_state));
    const auto management_next =
      co_await maintenance.advance_management_async (include_next_activity);
    if (!include_next_activity)
        co_return receive_activity_t{false, std::nullopt};
    auto [local_pending, next] = co_await node->native_node ().next_dispatch_activity_async ();
    if (management_next && (!next || *management_next < *next))
        next = management_next;
    co_return receive_activity_t{local_pending, next};
}

auto mesh_node_host_service_t::make_application_owner_work (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::shared_ptr<detail::mesh_node_builder_state_t> registration,
  std::string owner)
{
    return [node = std::move (node), registration = std::move (registration),
            owner = std::move (owner), services = _services, serializers = _serializers,
            filters = _filters, failures = _runtime_failures, options = _dispatch_options,
            executor = _application_dispatch.get ()] (bool reject_only) {
        detail::ambient_context_scope_t infrastructure (nullptr, nullptr, {}, false, true);
        auto pending = drain_application_owner (node, registration, owner, reject_only, services,
                                                serializers, filters, options, executor);
        detail::observe_task_terminal (pending, [failures] (const result_t<void> &result) {
            failures->capture ([&] { result.value (); });
        });
    };
}

struct mesh_node_host_service_t::node_receive_role_t final : offload_executor_t::role_t
{
    node_receive_role_t (
      mesh_node_host_service_t &host,
      std::shared_ptr<detail::mesh_node_runtime_t> node,
      std::shared_ptr<detail::mesh_node_builder_state_t> registration,
      std::shared_ptr<std::optional<application_job_queue_t::permit_t>> receive_permit) :
        host (host),
        node (std::move (node)),
        registration (std::move (registration)),
        receive_permit (std::move (receive_permit)),
        supply (host._application_jobs, [node = this->node] { node->signal_dispatch_activity (); })
    {
    }

    bool run () override
    {
        if (host._stop.load (std::memory_order_acquire)) {
            supply.close ();
            return false;
        }
        rotate ();
        return true;
    }

    bool publish () override
    {
        auto ready = std::move (ready_batch);
        ready_batch.reset ();
        const bool published = ready && !ready->first.empty ();
        mesh::service_mailbox_t::notify_application_ready (ready);
        return published;
    }

    void rotate ()
    {
        auto &application_permit = *receive_permit;
        auto &mailbox = node->native_node ().transport ().mailbox ();
        const auto observe_management = [&] {
            pending_management->armed = std::make_shared<std::atomic_bool> (false);
            detail::observe_task_completion (
              pending_management->activity,
              [node = node, armed = pending_management->armed] (const auto &) {
                  if (armed->exchange (false, std::memory_order_acq_rel))
                      node->signal_dispatch_activity ();
              });
        };
        const auto start_management = [&] (bool include_next_activity) {
            pending_management.emplace (pending_management_t{
              next_receive_activity_async (node, registration->spot_state, include_next_activity),
              {},
              !include_next_activity});
            if (include_next_activity)
                observe_management ();
        };
        supply.ensure_waiter (dispatch_limits::receive_batch_messages);
        auto budget = supply.take_budget ();
        application_permit = budget
                               ? std::optional<application_job_queue_t::permit_t> (budget->take ())
                               : std::nullopt;
        auto application_permit_budget =
          budget ? std::move (*budget) : application_job_queue_t::permit_batch_t{};
        const auto next_application_receive = [&] {
            application_permit.reset ();
            if (host._stop.load (std::memory_order_acquire))
                return false;
            if (!application_permit_budget)
                return false;
            application_permit.emplace (application_permit_budget.take ());
            return static_cast<bool> (application_permit);
        };
        const bool accept_application_receive = static_cast<bool> (application_permit);
        mailbox.begin_application_receive_turn ();
        terminal_callback_guard_t receive_turn (
          [&mailbox] { mailbox.end_application_receive_turn (); });

        const auto count =
          std::move (
            node->dispatch_hosted_ready (
              [&] (const host::ready_record_t &owner, const host::receive_record_t &record,
                   std::vector<zlink::message_t> parts, application_job_queue_t::permit_t permit) {
                  auto accepted_record = record;
                  accepted_record.received_at = std::chrono::system_clock::now ();
                  trace_mesh_application ("callback", record, parts.size ());
                  detail::spot_node_runtime_t spot_runtime (registration->spot_state);
                  const bool transfer_dispatch =
                    owner.owner_kind == host::owner_kind_t::actor
                    && (record.kind == host::record_kind_t::actor_send
                        || record.kind == host::record_kind_t::actor_request)
                    && owner.actor && spot_runtime.actor_transfer_in_progress (*owner.actor);
                  const bool framework_object_dispatch =
                    (owner.owner_kind == host::owner_kind_t::actor
                     && (record.kind == host::record_kind_t::actor_send
                         || record.kind == host::record_kind_t::actor_request))
                    || (owner.owner_kind == host::owner_kind_t::spot
                        && (record.kind == host::record_kind_t::spot_send
                            || record.kind == host::record_kind_t::spot_request
                            || record.kind == host::record_kind_t::spot_multicast
                            || record.kind == host::record_kind_t::spot_control));
                  const auto retain_mailbox_reservation = record.retain_mailbox_reservation;
                  const auto complete_stateful_dispatch = record.complete_stateful_dispatch;
                  const auto release_mailbox_reservation = record.release_mailbox_reservation;
                  const auto release_mailbox = [&] {
                      if (release_mailbox_reservation) {
                          release_mailbox_reservation ();
                      }
                  };
                  terminal_callback_guard_t release_guard (release_mailbox_reservation);
                  terminal_callback_guard_t stateful_guard (complete_stateful_dispatch);
                  auto run_direct = [this] (auto &&work) {
                      {
                          std::lock_guard lock (host._dispatch_gate_mutex);
                          ++host._active_direct_dispatch;
                      }
                      try {
                          work ();
                      }
                      catch (...) {
                          {
                              std::lock_guard lock (host._dispatch_gate_mutex);
                              --host._active_direct_dispatch;
                          }
                          host._dispatch_gate_changed.notify_all ();
                          throw;
                      }
                      {
                          std::lock_guard lock (host._dispatch_gate_mutex);
                          --host._active_direct_dispatch;
                      }
                      host._dispatch_gate_changed.notify_all ();
                  };
                  if (framework_object_dispatch && transfer_dispatch) {
                      bool handled = false;
                      try {
                          run_direct ([&] {
                              const auto header = decode_mesh_record_header (
                                record, parts,
                                detail::message_flow_tracer_t (host._dispatch_options)
                                  .flow_tracing (),
                                host._dispatch_options);
                              if (!header) {
                                  return;
                              }
                              handled = spot_runtime.dispatch_mesh_record (
                                owner, accepted_record, parts, header.value (), *host._services,
                                *host._serializers, {}, nullptr,
                                handler_entry_t (record.before_application_handler,
                                                 std::move (permit)));
                          });
                      }
                      catch (...) {
                          release_mailbox ();
                          throw;
                      }
                      if (handled || transfer_dispatch) {
                          release_mailbox ();
                          return;
                      }
                  }
                  if (owner.domain == host::ready_domain_t::application) {
                      mesh::service_mailbox_record_t mailbox_record;
                      mailbox_record.owner = mesh::service_mailbox_t::application_owner (owner);
                      mailbox_record.domain = mesh::service_mailbox_domain_t::application;
                      mailbox_record.before_application_handler = record.before_application_handler;
                      mailbox_record.application_permit.emplace (std::move (permit));
                      mailbox_record.application.emplace (host::local_application_dispatch_t{
                        owner, accepted_record, std::move (parts)});
                      const auto accepted =
                        node->native_node ().transport ().mailbox ().try_enqueue (
                          std::move (mailbox_record));
                      if (!accepted) {
                          reject_application_request (record,
                                                      std::move (mailbox_record.application->parts),
                                                      framework_error_kind_t::shutting_down,
                                                      "MeshNode application mailbox is closed");
                          return;
                      }
                      if (retain_mailbox_reservation)
                          retain_mailbox_reservation ();
                      stateful_guard.dismiss ();
                      release_guard.dismiss ();
                      return;
                  }
                  run_direct ([&] {
                      auto decoded_header = decode_mesh_record_header (
                        record, parts,
                        detail::message_flow_tracer_t (host._dispatch_options).flow_tracing (),
                        host._dispatch_options);
                      if (!decoded_header)
                          return;
                      std::function<void ()> direct_terminal;
                      if (record.kind == host::record_kind_t::node_send) {
                          // Native receives already own their work; this transfers only
                          // the existing terminal callbacks, without an application job.
                          auto terminal = std::make_shared<application_dispatch_terminal_owner_t> (
                            nullptr, complete_stateful_dispatch, release_mailbox_reservation,
                            handler_entry_t{});
                          stateful_guard.dismiss ();
                          release_guard.dismiss ();
                          if (retain_mailbox_reservation)
                              retain_mailbox_reservation ();
                          direct_terminal = [terminal] { terminal->settle (); };
                      }
                      const auto handled = spot_runtime.dispatch_mesh_record (
                        owner, accepted_record, parts, decoded_header.value (), *host._services,
                        *host._serializers, direct_terminal);
                      if (handled || !decoded_header.value ()) {
                          return;
                      }
                      detail::mesh_record_dispatcher_t dispatcher (
                        *host._services, *host._serializers, registration->handlers, *host._filters,
                        host._dispatch_options, {}, registration);
                      auto pending = dispatcher.dispatch (accepted_record, std::move (parts),
                                                          std::move (*decoded_header.value ()), {},
                                                          host._application_dispatch.get ());
                  });
              },
              accept_application_receive, next_application_receive, readiness))
            .result ()
            .value ();
        ready_batch = mailbox.take_application_receive_turn ();
        receive_turn.dismiss ();
        application_permit.reset ();
        application_permit_budget.release_without_handler ();
        if (count != 0 && pending_management) {
            if (pending_management->armed)
                pending_management->armed->store (false, std::memory_order_release);
            pending_management->stale = true;
            if (pending_management->activity.await_ready ()) {
                (void) detail::task_result_access_t::read (pending_management->activity).value ();
                pending_management.reset ();
            }
        }
        if (!pending_management)
            start_management (count == 0);
        bool local_pending = count != 0;
        std::optional<std::chrono::steady_clock::time_point> next_activity;
        if (count == 0) {
            if (pending_management->stale && pending_management->activity.await_ready ()) {
                (void) detail::task_result_access_t::read (pending_management->activity).value ();
                pending_management.reset ();
                start_management (true);
            }
            if (!pending_management->armed)
                observe_management ();
            if (!pending_management->activity.await_ready ()) {
                pending_management->armed->store (true, std::memory_order_release);
            }
            if (pending_management->activity.await_ready ()) {
                pending_management->armed->store (false, std::memory_order_release);
                const auto activity =
                  detail::task_result_access_t::read (pending_management->activity).value ();
                pending_management.reset ();
                local_pending = activity.first && accept_application_receive;
                next_activity = activity.second;
            }
        }
        readiness.reset ();
        // A pump may consume the wake for supply delivered after this turn's
        // take. Recheck its owner state before waiting for the next rotation.
        if (!local_pending && !ready_batch && !host._stop.load (std::memory_order_acquire)
            && !supply.has_supply ()) {
            readiness = node->native_node ().wait_for_hosted_dispatch_activity (
              std::chrono::milliseconds (-1), accept_application_receive, next_activity);
        }
    }

    struct pending_management_t
    {
        task_t<receive_activity_t> activity;
        std::shared_ptr<std::atomic_bool> armed;
        bool stale = false;
    };
    mesh_node_host_service_t &host;
    const std::shared_ptr<detail::mesh_node_runtime_t> node;
    const std::shared_ptr<detail::mesh_node_builder_state_t> registration;
    const std::shared_ptr<std::optional<application_job_queue_t::permit_t>> receive_permit;
    std::optional<zlink::poll_event_flag_t> readiness;
    mesh::service_mailbox_t::application_ready_batch_t ready_batch;
    application_supply_slot_t supply;
    std::optional<pending_management_t> pending_management;
};

task_t<void> mesh_node_host_service_t::start (service_provider_t &services)
{
    std::exception_ptr failure;
    try {
        _completion_dispatcher = detail::borrow_completion_dispatcher ();
        _services = &services;
        for (const auto &registration : _registrations)
            registration->handlers.bind_codecs (*_serializers);
        const auto actor_gateway = services.get<detail::actor_gateway_runtime_t> ();
        if (actor_gateway) {
            actor_gateway->get ().bind_serializers (*_serializers);
        }
        _stop.store (false, std::memory_order_release);
        _descriptor_stop = std::stop_source{};
        _actor_destroy_gate = std::make_shared<actor_destroy_callback_gate_t> ();
        const auto locations = services.get<location_runtime_t> ();
        _location_runtime = locations ? &locations->get () : nullptr;
        auto store = _location_runtime ? std::shared_ptr<location_repository_t> (
                                           &services.get_required<location_repository_t> (),
                                           [] (location_repository_t *) noexcept {})
                                       : nullptr;
        _location_store = store;
        for (const auto &registration : _registrations) {
            if (!registration || !registration->spot_state)
                continue;
            const auto gate = _actor_destroy_gate;
            detail::spot_node_runtime_t spot_runtime (registration->spot_state);
            struct destroy_actor_authority_t
            {
                static task_t<void> run (mesh_node_host_service_t *self,
                                         std::decay_t<decltype (gate)> gate,
                                         actor_ref_t actor,
                                         bool closing,
                                         std::string expected_store_version)
                {
                    if (!gate->try_enter ())
                        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                                     "Actor destroy interrupted by Mesh shutdown");
                    struct leave_t
                    {
                        std::shared_ptr<actor_destroy_callback_gate_t> held;
                        ~leave_t () { held->leave (); }
                    } leave{gate};
                    co_await self->update_actor_destroy_authority (
                      std::move (actor), closing, std::move (expected_store_version));
                }
            };
            spot_runtime.on_destroy_actor (
              [this, gate] (actor_ref_t actor, std::string expected_store_version) {
                  return destroy_actor_authority_t::run (this, gate, std::move (actor), true,
                                                         std::move (expected_store_version));
              },
              [this, gate] (actor_ref_t actor) {
                  return destroy_actor_authority_t::run (this, gate, std::move (actor), false, {});
              });
            if (actor_gateway) {
                spot_runtime.on_actor_ref_updated (
                  [gateway = &actor_gateway->get ()] (const actor_ref_t &actor) {
                      return gateway->update_actor_ref (actor);
                  });
            }
        }
        _accept_application_dispatch.store (!_location_runtime
                                              || _location_runtime->owner_lease_usable (),
                                            std::memory_order_release);
        std::shared_ptr<stateful::relocation_store_port_t> instance_relocations;
        const auto has_instance_factories = std::any_of (
          _registrations.begin (), _registrations.end (), [] (const auto &registration) {
              return registration && registration->spot_state
                     && !registration->spot_state->snapshot.instance_spot_names.empty ();
          });
        if (has_instance_factories) {
            auto &relocations = services.get_required<stateful::relocation_store_port_t> ();
            instance_relocations = std::shared_ptr<stateful::relocation_store_port_t> (
              &relocations, [] (auto *) noexcept {});
        }
        for (std::size_t index = 0; index < _nodes.size (); ++index) {
            const auto registration = _registrations[index];
            const auto source = _nodes[index];
            registration->spot_state->create_user_spot =
              [this, weak_source = std::weak_ptr (source)] (
                bool exclusive, std::optional<spot_id_t> spot_id, std::string stable_type,
                std::optional<std::string> mesh_name, std::optional<message_t> request,
                std::chrono::milliseconds timeout) {
                  const auto source = detail::lock_runtime_owner (weak_source);
                  return create_user_spot (source, exclusive, std::move (spot_id),
                                           std::move (stable_type), std::move (mesh_name),
                                           std::move (request), timeout);
              };
            registration->spot_state->find_user_spot = [this] (spot_id_t spot_id) {
                return find_user_spot (std::move (spot_id));
            };
            registration->spot_state->close_user_spot =
              [this, weak_source = std::weak_ptr (source)] (
                spot_ref_t spot, std::chrono::steady_clock::time_point deadline,
                std::uint64_t wire_deadline) {
                  return close_user_spot (detail::lock_runtime_owner (weak_source),
                                          std::move (spot), deadline, wire_deadline);
              };
            registration->spot_state->begin_user_spot_close =
              [weak_source = std::weak_ptr (source)] (const spot_id_t &spot_id,
                                                      std::uint64_t object_generation,
                                                      std::uint64_t authority_owner_generation) {
                  const auto source = detail::lock_runtime_owner (weak_source);
                  return source->native_node ().begin_local_user_spot_close (
                    std::string (spot_id), object_generation, authority_owner_generation);
              };
            _nodes[index]->configure_stateful_dispatch (
              [this] {
                  return _location_runtime ? _location_runtime->current_owner_observation ()
                                           : std::shared_ptr<const location_owner_token_t>{};
              },
              _location_runtime ? &services.get_required<live_location_reader_t> () : nullptr);
            struct materialize_user_spot_t
            {
                static task_t<host::user_spot_materialize_result_t>
                run (std::decay_t<decltype (source)> source,
                     std::decay_t<decltype (registration)> registration,
                     serializer_registry_t *serializers,
                     stateful::object_ref_t object,
                     std::string stable_type,
                     std::vector<std::byte> payload)
                {
                    std::vector<std::uint8_t> rid_bytes;
                    rid_bytes.reserve (object.key.size ());
                    for (const auto value : object.key)
                        rid_bytes.push_back (
                          static_cast<std::uint8_t> (static_cast<unsigned char> (value)));
                    std::vector<std::uint8_t> request_bytes;
                    request_bytes.reserve (payload.size ());
                    for (const auto value : payload)
                        request_bytes.push_back (std::to_integer<std::uint8_t> (value));
                    auto spot_id = spot_id_t (zlink::routing_id_t::from (rid_bytes).to_string ());
                    const auto native_id = std::string (spot_id);
                    auto native_spot = std::make_shared<host::spot_handle_t> (
                      source->native_node ().shared_from_this (), object);
                    co_await registration->spot_state->lane.run_task ([&] {
                        registration->spot_state->native_spots_by_id.insert_or_assign (native_id,
                                                                                       native_spot);
                    });
                    detail::spot_node_runtime_t spots (registration->spot_state);
                    std::optional<detail::local_spot_create_result_t> created;
                    std::function<task_t<void> (bool)> preparation_terminal;
                    std::exception_ptr failure;
                    try {
                        auto creation = co_await spots.get_or_create_spot_async (
                          stable_type, std::move (spot_id), zlink::message_t::from (request_bytes),
                          object.object_generation, object.mesh_name,
                          object.authority_owner_generation, &preparation_terminal);
                        created.emplace (std::move (*creation));
                    }
                    catch (...) {
                        failure = std::current_exception ();
                    }
                    if (failure) {
                        co_await registration->spot_state->lane.run_task ([&] {
                            const auto found =
                              registration->spot_state->native_spots_by_id.find (native_id);
                            if (found != registration->spot_state->native_spots_by_id.end ()
                                && found->second == native_spot)
                                registration->spot_state->native_spots_by_id.erase (found);
                        });
                        std::rethrow_exception (failure);
                    }
                    std::optional<protocol::application_payload_t> application_reply;
                    if (created->reply) {
                        application_reply = protocol::application_payload_t{
                          stable_type, "application/octet-stream",
                          detail::message_to_raw (*created->reply, *serializers).to_bytes ()};
                    }
                    co_return host::user_spot_materialize_result_t{
                      created->state == spot_create_state_t::created
                        || created->state == spot_create_state_t::existing,
                      std::move (application_reply), std::move (preparation_terminal)};
                }
            };
            _nodes[index]->configure_user_spot_operations (
              store,
              [this, weak_registration = std::weak_ptr (registration),
               weak_source = std::weak_ptr (source)] (const stateful::object_ref_t &object,
                                                      const std::string &stable_type,
                                                      const std::vector<std::byte> &payload)
                -> task_t<host::user_spot_materialize_result_t> {
                  return materialize_user_spot_t::run (
                    detail::lock_runtime_owner (weak_source),
                    detail::lock_runtime_owner (weak_registration), _serializers, object,
                    stable_type, payload);
              },
              [weak_registration = std::weak_ptr (registration)] (
                const std::string &spot_id, std::uint64_t object_generation,
                std::chrono::system_clock::time_point deadline, host::spot_close_begin_t begin,
                host::spot_close_done_t done) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  // The materializer registers the activation under this local id.
                  std::vector<std::uint8_t> rid_bytes;
                  rid_bytes.reserve (spot_id.size ());
                  for (const auto value : spot_id)
                      rid_bytes.push_back (
                        static_cast<std::uint8_t> (static_cast<unsigned char> (value)));
                  detail::spot_node_runtime_t (registration->spot_state)
                    .close_user_spot_owner (zlink::routing_id_t::from (rid_bytes).to_string (),
                                            object_generation, deadline, std::move (begin),
                                            std::move (done));
              });
            if (!registration->spot_state->snapshot.instance_spot_names.empty ()) {
                if (!instance_relocations)
                    throw framework_exception_t (
                      framework_error_kind_t::not_configured,
                      "Instance Spot factories require a Relocation Store");
                registration->spot_state->admit_instance_spot_idle_eviction =
                  [weak_source = std::weak_ptr (source)] (
                    const spot_id_t &spot_id, std::string_view stable_type,
                    std::uint64_t object_generation, std::uint64_t authority_owner_generation,
                    std::function<void ()> close_local) {
                      const auto source = detail::lock_runtime_owner (weak_source);
                      return source->native_node ().evict_instance_spot (
                        std::string (stable_type), std::string (spot_id), object_generation,
                        authority_owner_generation, std::move (close_local));
                  };
                registration->spot_state->begin_instance_spot_close =
                  [weak_source = std::weak_ptr (source)] (
                    const spot_id_t &spot_id, std::string_view stable_type,
                    std::uint64_t object_generation, std::uint64_t authority_owner_generation) {
                      const auto source = detail::lock_runtime_owner (weak_source);
                      return source->native_node ().begin_instance_spot_close (
                        std::string (stable_type), std::string (spot_id), object_generation,
                        authority_owner_generation);
                  };
                struct prepare_instance_spot_t
                {
                    static task_t<bool>
                    run (std::decay_t<decltype (registration)> registration,
                         protocol::instance_spot_activation_header_t request,
                         authority_snapshot_t snapshot,
                         std::function<task_t<void> (bool)> &preparation_terminal)
                    {
                        if (snapshot.allocation.object_kind
                              != placement_object_kind_t::instance_spot
                            || snapshot.allocation.stable_type != request.target.stable_type
                            || snapshot.allocation.target.mesh_name != request.target.mesh_name
                            || snapshot.allocation.target.node_rid.value ()
                                 != zlink::routing_id_t::from (
                                      request.target.target_node_routing_id)
                                      .to_string ()
                            || snapshot.allocation.target.node_lifecycle_generation
                                 != request.target.target_node_generation)
                            co_return false;
                        const auto created =
                          co_await detail::spot_node_runtime_t (registration->spot_state)
                            .get_or_create_spot_async (
                              request.target.stable_type, spot_id_t (request.target.spot_id),
                              zlink::message_t{}, snapshot.object_generation,
                              request.target.mesh_name, snapshot.authority_owner_generation,
                              &preparation_terminal);
                        co_return created->state == spot_create_state_t::created
                          || created->state == spot_create_state_t::existing;
                    }
                };
                _nodes[index]->configure_instance_spot_operations (
                  store, instance_relocations, [this] { return current_location_owner (); },
                  host::instance_spot_activation_materializer_t{
                    [weak_registration = std::weak_ptr (registration)] (
                      const protocol::instance_spot_activation_header_t &request,
                      const authority_snapshot_t &snapshot,
                      std::function<task_t<void> (bool)> &preparation_terminal) -> task_t<bool> {
                        return prepare_instance_spot_t::run (
                          detail::lock_runtime_owner (weak_registration), request, snapshot,
                          preparation_terminal);
                    },
                    [this, weak_registration = std::weak_ptr (registration), &services] (
                      std::shared_ptr<const protocol::instance_activation_recovery_t>
                        original_command,
                      std::function<task_t<zlink::message_t> (std::function<void ()> &)> missing,
                      std::function<void ()> accept_activation) {
                        const auto registration = detail::lock_runtime_owner (weak_registration);
                        const auto &request = original_command->activation;
                        const auto &metadata = original_command->metadata;
                        const auto &application = original_command->application_payload;
                        auto completion = std::make_shared<
                          task_completion_source_t<host::instance_spot_activation_result_t>> ();
                        auto output = completion->task ();
                        auto done = [completion] (host::instance_spot_activation_result_t result) {
                            completion->complete (
                              result_t<host::instance_spot_activation_result_t>::success (
                                std::move (result)));
                        };
                        auto decoded_application = messaging::decode_instance_spot_application (
                          application,
                          request.request ? messaging::message_kind_t::request
                                          : messaging::message_kind_t::command,
                          detail::message_flow_tracer_t (_dispatch_options).flow_tracing ());
                        std::map<std::string, std::string> decoded;
                        if (!decoded_application
                            || (metadata
                                && !detail::mesh_metadata_codec_t::decode (*metadata, decoded))) {
                            done ({static_cast<std::uint32_t> (
                                     protocol::request_terminal_result::protocolError),
                                   static_cast<std::uint32_t> (
                                     protocol::framework_error_code::requestProtocolError),
                                   std::nullopt});
                            return output;
                        }
                        auto terminal = std::make_shared<std::function<void ()>> ();
                        std::shared_ptr<const detail::instance_spot_retained_message_t> original;
                        if (request.target.authority_owner_generation == 0
                            || request.target.instance_intent) {
                            original =
                              std::make_shared<const detail::instance_spot_retained_message_t> (
                                original_command, [missing = std::move (missing), terminal] {
                                    return missing (*terminal);
                                });
                        }
                        auto &decoded_header = decoded_application.value ().header;
                        auto flow_id =
                          decoded_header.flow_id
                            ? std::make_optional (std::string (*decoded_header.flow_id))
                            : std::nullopt;
                        std::optional<flow_value_t> terminal_flow;
                        auto running = std::make_shared<task_t<zlink::message_t>> (
                          detail::spot_node_runtime_t (registration->spot_state)
                            .dispatch_instance_activation (
                              spot_id_t (request.target.spot_id),
                              std::string (decoded_header.message_name),
                              std::string (decoded_header.content_type),
                              std::move (decoded_application.value ().body), std::move (decoded),
                              request.request,
                              std::to_string (request.operation.high) + ":"
                                + std::to_string (request.operation.low),
                              services, *_serializers, std::move (accept_activation),
                              std::move (flow_id), decoded_header.flow_origin, terminal.get (),
                              original, decoded_header.flow_id ? nullptr : &terminal_flow));
                        detail::observe_task_completion (
                          *running, [running, is_request = request.request,
                                     decoded_header = std::move (decoded_header), terminal,
                                     terminal_flow = std::move (terminal_flow),
                                     done] (const result_t<zlink::message_t> &reply) mutable {
                              if (!reply) {
                                  const auto failure =
                                    messaging::request_failure_mapper_t{}.target_failure_reply (
                                      reply.exception ());
                                  const auto wire =
                                    failure.value_or (messaging::request_wire_failure_t{
                                      105, static_cast<std::uint32_t> (
                                             protocol::framework_error_code::requestFailed)});
                                  const auto *error = reply.error ();
                                  auto handler_error =
                                    error
                                        && detail::error_origin (*error)
                                             == detail::error_origin_t::application
                                      ? reply.exception ()
                                      : std::exception_ptr{};
                                  done ({wire.terminal_result, wire.failure_code, std::nullopt,
                                         std::move (*terminal), std::move (handler_error),
                                         std::move (terminal_flow)});
                                  return;
                              }
                              std::optional<protocol::application_payload_t> response;
                              if (is_request) {
                                  auto reply_header =
                                    detail::channel_reply_writer_t{}.create_reply_header (
                                      messaging::message_kind_t::response, {}, decoded_header);
                                  reply_header.flow_id = decoded_header.flow_id;
                                  reply_header.flow_origin = decoded_header.flow_origin;
                                  response = messaging::encode_instance_spot_application (
                                    reply_header, reply.value ().copy ());
                              }
                              done ({0, 0, std::move (response), std::move (*terminal)});
                          });
                        return output;
                    },
                    [weak_registration = std::weak_ptr (registration)] (
                      protocol::instance_spot_activation_header_t request) {
                        const auto registration = detail::lock_runtime_owner (weak_registration);
                        const auto select = registration->spot_state->select_instance_spot_target;
                        if (select)
                            return select (std::move (request));
                        return task_t<result_t<protocol::instance_spot_activation_header_t>> (
                          result_t<result_t<protocol::instance_spot_activation_header_t>>::success (
                            result_t<protocol::instance_spot_activation_header_t>::success (
                              std::move (request))));
                    },
                    [weak_registration =
                       std::weak_ptr (registration)] (const std::string &stable_type)
                      -> std::optional<detail::factory_relocation_kind_t> {
                        const auto registration = detail::lock_runtime_owner (weak_registration);
                        const auto &types = registration->spot_state->snapshot.instance_spot_names;
                        if (std::find (types.begin (), types.end (), stable_type) == types.end ())
                            return std::nullopt;
                        const auto found =
                          registration->spot_state->spot_factory_relocations.find (stable_type);
                        return found == registration->spot_state->spot_factory_relocations.end ()
                                 ? std::nullopt
                                 : std::make_optional (found->second.kind);
                    },
                    [weak_registration = std::weak_ptr (registration)] (
                      const protocol::instance_spot_activation_header_t &request,
                      const authority_snapshot_t &snapshot) {
                        const auto registration = detail::lock_runtime_owner (weak_registration);
                        return detail::spot_node_runtime_t (registration->spot_state)
                          .matches_instance_activation (
                            request.target.spot_id, request.target.stable_type,
                            request.target.mesh_name, snapshot.object_generation,
                            snapshot.authority_owner_generation);
                    },
                    registration->spot_state->snapshot.instance_spot_names});
            }
        }
        for (std::size_t index = 0; index < _nodes.size (); ++index) {
            const auto &node = _nodes[index];
            const auto registration = _registrations[index];
            if (_location_runtime) {
                const auto spot_resolver =
                  zlink::framework::detail::service_scope_access_t::get_required_shared<
                    spot_address_resolver_t> (services);
                auto &actor_resolver = services.get_required<actor_address_resolver_t> ();
                const auto location_options_at_startup = _location_runtime->options ();
                const auto mesh_name = node->mesh_name ();
                node->configure_spot_route_fence_resolver (
                  [spot_resolver,
                   mesh_name] (zlink::routing_id_t target_node_rid, std::string target_spot_id,
                               std::uint64_t) -> task_t<std::optional<host::route_fence_t>> {
                      const auto resolver = spot_resolver;
                      const auto resolved = co_await await_result (
                        resolver->resolve_spot_address (mesh_name, std::move (target_spot_id)));
                      if (!resolved || !resolved.value ())
                          co_return std::nullopt;
                      const auto &address = *resolved.value ();
                      if (address.node_rid != target_node_rid
                          || address.authority_owner_generation == 0
                          || address.owner.lease_generation <= 0)
                          co_return std::nullopt;
                      co_return host::route_fence_t{
                        address.authority_owner_generation,
                        static_cast<std::uint64_t> (address.owner.lease_generation),
                        address.node_generation};
                  },
                  location_options_at_startup.owner_lease_fencing_margin,
                  location_options_at_startup.session_relocation_seal_timeout);
                node->configure_actor_route_resolver (
                  [&actor_resolver] (actor_ref_t actor) -> task_t<std::optional<spot_address_t>> {
                      const auto resolved = co_await actor_resolver.resolve_actor_address (
                        std::string (actor.actor_id ().value ()));
                      if (!resolved)
                          co_return std::nullopt;
                      const auto &address = *resolved;
                      if (address.authority_owner_generation == 0
                          || address.owner.lease_generation <= 0)
                          co_return std::nullopt;
                      co_return address;
                  },
                  [&actor_resolver] (const protocol::actor_route_fence_t &source) {
                      const auto expected = spot_address_t{
                        {},
                        zlink::routing_id_t::from (source.target_node_routing_id),
                        {},
                        0,
                        {},
                        source.object_generation,
                        source.authority_owner_generation,
                        location_owner_token_t{
                          {}, static_cast<std::int64_t> (source.owner_lease_generation)},
                        source.target_node_generation};
                      (void) actor_resolver.invalidate_actor_address_if_matches (source.actor_id,
                                                                                 expected);
                  });
                registration->spot_state->actor_route_admission =
                  [&actor_resolver] (const protocol::actor_route_fence_t &route) {
                      try {
                          const auto resolved = infrastructure_result (
                            [&] { return actor_resolver.resolve_actor_address (route.actor_id); });
                          if (!resolved || !resolved.value ())
                              return false;
                          const auto &address = *resolved.value ();
                          return address.node_rid
                                   == zlink::routing_id_t::from (route.target_node_routing_id)
                                 && address.node_generation == route.target_node_generation
                                 && address.authority_owner_generation
                                      == route.authority_owner_generation
                                 && address.owner.lease_generation > 0
                                 && static_cast<std::uint64_t> (address.owner.lease_generation)
                                      == route.owner_lease_generation;
                      }
                      catch (...) {
                          return false;
                      }
                  };
            }
            node->configure_actor_create_operations (
              [weak_node = std::weak_ptr (node), store,
               weak_registration = std::weak_ptr (registration)] (
                const protocol::actor_create_header_t &request,
                host::actor_create_operation_target_completion_t completion) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  const auto node = detail::lock_runtime_owner (weak_node);
                  execute_actor_create_operation_target (node, store, registration, request,
                                                         completion);
              });
            co_await node->start (
              [registration] (const stateful::object_ref_t &participant)
                -> task_t<std::shared_ptr<runtime::serial_execution_queue_t>> {
                  if (participant.kind == stateful::object_kind_t::actor)
                      return task_t<std::shared_ptr<runtime::serial_execution_queue_t>> (
                        result_t<std::shared_ptr<runtime::serial_execution_queue_t>>::success (
                          nullptr));
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .spot_execution_queue (participant.key);
              },
              [registration] (std::string key, std::optional<protocol::spot_route_fence_t> route) {
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .prepare_spot_dispatch (std::move (key), std::move (route));
              },
              [weak = std::weak_ptr (node), registration, services = _services,
               serializers = _serializers, filters = _filters, options = _dispatch_options,
               executor = _application_dispatch.get ()] (
                const host::ready_record_t &owner, const host::receive_record_t &record,
                std::vector<zlink::message_t> parts, application_job_queue_t::permit_t permit) {
                  const auto node = weak.lock ();
                  if (!node)
                      return;
                  node->application_work_enqueued ();
                  node->application_work_started ();
                  dispatch_application (node, registration, owner, record, std::move (parts), false,
                                        services, serializers, filters, options, {}, executor,
                                        std::move (permit));
              });
            if (_listener_statuses) {
                const auto status = node->status ();
                _listener_statuses->update (listener_kind_t::route_mesh, node->mesh_name (),
                                            status.local_endpoint ());
            }
            node->native_node ().objects ().configure_relocation_state (
              [weak_registration = std::weak_ptr (registration)] (
                const stateful::object_ref_t &spot, const std::string &stable_type,
                std::stop_token cancellation) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .capture_relocation_state (spot, stable_type, cancellation);
              },
              [weak_registration = std::weak_ptr (registration)] (
                const stateful::frozen_object_state_t &frozen, const stateful::object_ref_t &target,
                std::stop_token cancellation) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .restore_spot_relocation_state (frozen, target, cancellation);
              });
            node->native_node ().objects ().configure_relocation_materialization (
              [weak_registration = std::weak_ptr (registration)] (
                const stateful::frozen_object_state_t &frozen, const stateful::object_ref_t &target,
                const std::optional<stateful::object_ref_t> &target_spot,
                std::stop_token cancellation) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .materialize_relocation_state (frozen, target, target_spot, cancellation);
              },
              [weak_registration = std::weak_ptr (registration)] (
                const std::vector<stateful::object_ref_t> &targets,
                const std::function<void ()> &before_actor_join_completion) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .commit_relocation_materialization (targets, before_actor_join_completion);
              },
              [weak_registration = std::weak_ptr (registration)] (
                const std::vector<stateful::object_ref_t> &targets) {
                  const auto registration = detail::lock_runtime_owner (weak_registration);
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .abort_relocation_materialization_async (targets);
              });
        }
        _published_mesh_nodes.clear ();
        _published_mesh_descriptors.clear ();
        _changed_mesh_descriptors.clear ();
        for (std::size_t index = 0; _location_runtime && index < _nodes.size (); ++index) {
            const auto &node = _nodes[index];
            const auto &registration = _registrations[index];
            const auto status = node->status ();
            mesh_node_descriptor_t descriptor;
            const auto owner = current_location_owner ();
            descriptor.mesh_name = node->mesh_name ();
            descriptor.rid = status.routing_id ();
            descriptor.lifecycle_generation = status.lifecycle_generation ();
            descriptor.descriptor_revision = 1;
            descriptor.endpoint = status.local_endpoint ();
            descriptor.channel_weights = node->channel_weights ();
            descriptor.object_role = registration->object_role;
            descriptor.placement_weight = node->placement_weight ();
            descriptor.capacity.actors.limit = node->actor_limit ();
            descriptor.capacity.spots.limit = node->spot_limit ();
            descriptor.activation_concurrency.limit = node->activation_admission ().limit ();
            descriptor.state = framework_runtime_state_t::serving;
            descriptor.security_identity = "default";
            descriptor.owner_id = owner ? owner->owner_id : std::string{};
            descriptor.lease_generation = owner ? owner->lease_generation : 0;
            if (registration->object_role == object_role_t::server) {
                descriptor.entry_spot_id = node->native_node ().entry_spot ().spot_id ();
                for (const auto &stable_type : registration->spot_state->snapshot.actor_types) {
                    const auto configured =
                      registration->spot_state->actor_factories.find (stable_type);
                    const auto relocation =
                      configured != registration->spot_state->actor_factories.end ()
                        ? configured->second.relocation.kind
                        : detail::factory_relocation_kind_t::disabled;
                    const auto has_state_adapter =
                      relocation == detail::factory_relocation_kind_t::preserve_state;
                    const auto entry_accepted =
                      co_await detail::spot_node_runtime_t (registration->spot_state)
                        .entry_accepts_actor (stable_type);
                    descriptor.object_capabilities.push_back (object_capability_t{
                      .object_kind = placement_object_kind_t::actor,
                      .stable_type = stable_type,
                      .policy = has_state_adapter ? maintenance_policy_kind_t::snapshot
                                : relocation == detail::factory_relocation_kind_t::recreate
                                  ? maintenance_policy_kind_t::recreate
                                  : maintenance_policy_kind_t::disabled,
                      .has_snapshot_adapter = has_state_adapter,
                      .entry_accepted = entry_accepted});
                }
                for (const auto &stable_type : registration->spot_state->snapshot.spot_names) {
                    if (registration->spot_state->snapshot.entry_spot_name == stable_type)
                        continue;
                    if (std::find (registration->spot_state->snapshot.instance_spot_names.begin (),
                                   registration->spot_state->snapshot.instance_spot_names.end (),
                                   stable_type)
                        != registration->spot_state->snapshot.instance_spot_names.end ())
                        continue;
                    const auto configured =
                      registration->spot_state->spot_factory_relocations.find (stable_type);
                    const auto relocation =
                      configured != registration->spot_state->spot_factory_relocations.end ()
                        ? configured->second.kind
                        : detail::factory_relocation_kind_t::disabled;
                    descriptor.object_capabilities.push_back (object_capability_t{
                      .object_kind = placement_object_kind_t::user_spot,
                      .stable_type = stable_type,
                      .policy = relocation == detail::factory_relocation_kind_t::preserve_state
                                  ? maintenance_policy_kind_t::snapshot
                                : relocation == detail::factory_relocation_kind_t::recreate
                                  ? maintenance_policy_kind_t::recreate
                                  : maintenance_policy_kind_t::disabled,
                      .has_snapshot_adapter =
                        relocation == detail::factory_relocation_kind_t::preserve_state});
                    descriptor.capacity.spot_types.push_back (spot_type_capacity_t{
                      .object_kind = placement_object_kind_t::user_spot,
                      .stable_type = stable_type,
                      .usage = {.limit = registration->spot_state->spot_stable_type_limits.at (
                                  stable_type)}});
                }
                for (const auto &stable_type :
                     registration->spot_state->snapshot.instance_spot_names) {
                    const auto configured =
                      registration->spot_state->spot_factory_relocations.find (stable_type);
                    const auto relocation =
                      configured != registration->spot_state->spot_factory_relocations.end ()
                        ? configured->second.kind
                        : detail::factory_relocation_kind_t::disabled;
                    descriptor.object_capabilities.push_back (object_capability_t{
                      .object_kind = placement_object_kind_t::instance_spot,
                      .stable_type = stable_type,
                      .policy = relocation == detail::factory_relocation_kind_t::preserve_state
                                  ? maintenance_policy_kind_t::snapshot
                                : relocation == detail::factory_relocation_kind_t::recreate
                                  ? maintenance_policy_kind_t::recreate
                                  : maintenance_policy_kind_t::disabled,
                      .has_snapshot_adapter =
                        relocation == detail::factory_relocation_kind_t::preserve_state});
                    descriptor.capacity.spot_types.push_back (spot_type_capacity_t{
                      .object_kind = placement_object_kind_t::instance_spot,
                      .stable_type = stable_type,
                      .usage = {.limit = registration->spot_state->spot_stable_type_limits.at (
                                  stable_type)}});
                }
                std::sort (descriptor.object_capabilities.begin (),
                           descriptor.object_capabilities.end (),
                           [] (const object_capability_t &left, const object_capability_t &right) {
                               return std::tie (left.object_kind, left.stable_type)
                                      < std::tie (right.object_kind, right.stable_type);
                           });
                std::sort (
                  descriptor.capacity.spot_types.begin (), descriptor.capacity.spot_types.end (),
                  [] (const spot_type_capacity_t &left, const spot_type_capacity_t &right) {
                      return std::tie (left.object_kind, left.stable_type)
                             < std::tie (right.object_kind, right.stable_type);
                  });
            }
            if (owner) {
                const auto written = co_await _location_runtime->descriptor_write_confirmed ([&] {
                    return _location_store->update_mesh_node (
                      descriptor, mesh_new_claim_intent (_registrations, descriptor));
                });
                if (written)
                    _published_mesh_nodes.push_back ({descriptor.mesh_name, descriptor.rid});
            }
            _published_mesh_descriptors.push_back (std::move (descriptor));
        }
        for (std::size_t index = 0; index < _published_mesh_descriptors.size (); ++index) {
            const auto &node = _nodes[index];
            node->bind_descriptor_publisher (
              [this, index] (const std::map<std::string, int> &channel_weights,
                             int placement_weight, std::uint64_t descriptor_revision) {
                  {
                      std::lock_guard lock (_descriptor_publish_mutex);
                      if (!_location_store || !_location_runtime
                          || index >= _published_mesh_descriptors.size ())
                          throw framework_exception_t (
                            framework_error_kind_t::protocol_error,
                            "MeshNode Location descriptor publisher is not active");
                      auto &descriptor = _published_mesh_descriptors[index];
                      // Every change takes a revision above the cached one, so a publication in
                      // flight sees the change at completion and leaves it recorded.
                      if (descriptor.descriptor_revision
                          == std::numeric_limits<std::uint64_t>::max ())
                          throw framework_exception_t (framework_error_kind_t::internal_failure,
                                                       "MeshNode descriptor revision is exhausted");
                      descriptor.channel_weights = channel_weights;
                      descriptor.placement_weight = placement_weight;
                      descriptor.descriptor_revision =
                        std::max (descriptor.descriptor_revision + 1, descriptor_revision);
                      _changed_mesh_descriptors.insert (descriptor.mesh_name);
                  }
                  request_descriptor_publication ();
              });
        }
        if (_location_runtime)
            co_await _location_runtime->set_descriptor_renewal (
              [this] { request_descriptor_publication (); });
        for (std::size_t index = 0; index < _nodes.size (); ++index) {
            const auto node = _nodes[index];
            const auto registration = _registrations[index];
            auto receive_permit =
              std::make_shared<std::optional<application_job_queue_t::permit_t>> ();
            mesh::service_mailbox_t::application_ready_t submit_owner =
              [this, executor = _application_dispatch, node,
               registration] (const std::string &owner) {
                  auto start = make_application_owner_work (node, registration, owner);
                  if (!executor->try_submit_internal ([start] { start (false); }))
                      start (true);
              };
            auto role =
              std::make_shared<node_receive_role_t> (*this, node, registration, receive_permit);
            auto &mailbox = node->native_node ().transport ().mailbox ();
            auto actor_dispatch_options =
              std::make_shared<const dispatch_options_t> (_dispatch_options);
            mailbox.bind_application_dispatch (
              [this, node, receive_permit] (mesh::service_mailbox_record_t &record) {
                  std::lock_guard lock (_dispatch_gate_mutex);
                  if (!_accept_application_dispatch.load (std::memory_order_relaxed)
                      || (requires_location_owner_state (record)
                          && (!_location_runtime || !_location_runtime->owner_lease_usable ()))) {
                      if (record.before_application_handler
                          || (record.application_permit && *record.application_permit)) {
                          record.before_application_handler = {};
                          record.application_permit.reset ();
                      } else
                          receive_permit->reset ();
                      node->application_work_enqueued ();
                      return;
                  }
                  const auto *accepted = record.application
                                           ? record.application->record.application_record.get ()
                                           : nullptr;
                  if (!record.before_application_handler
                      && (!record.application_permit || !*record.application_permit)
                      && !(accepted && accepted->saved_work ())) {
                      if (!*receive_permit)
                          throw std::logic_error (
                            "Application owner enqueue requires a receive permit");
                      record.application_permit.emplace (std::move (**receive_permit));
                      receive_permit->reset ();
                      record.application_permit->mark_queued ();
                  }
                  node->application_work_enqueued ();
              },
              std::move (submit_owner),
              [node, registration, services = _services, serializers = _serializers,
               filters = _filters, options = std::move (actor_dispatch_options)] (
                const std::string &actor_id, mesh::service_mailbox_t::stateful_take_t take) {
                  return detail::spot_node_runtime_t (registration->spot_state)
                    .notify_actor_application_ready (
                      actor_id,
                      [node, registration, services, serializers, filters, options,
                       take = std::move (take)] () mutable -> std::optional<serial_prefix_work_t> {
                          std::optional<serial_prefix_work_t> item;
                          take ([&] (const host::ready_record_t &owner,
                                     const host::receive_record_t &record,
                                     std::vector<zlink::message_t> parts,
                                     application_job_queue_t::permit_t permit) {
                              node->application_work_enqueued ();
                              item.emplace ();
                              item->name = "actor-native";
                              item->options.application_record = record.application_record;
                              if (record.before_application_handler || permit)
                                  record.application_record->admission.retain_initial (
                                    handler_entry_t (record.before_application_handler,
                                                     std::move (permit)));
                              record.application_record->retire_invocation =
                                [node] (result_t<void>) {
                                    node->application_work_started ();
                                    node->application_work_finished ();
                                };
                              auto accepted_record = record;
                              accepted_record.before_application_handler = {};
                              item->work = [node, registration, owner,
                                            record = std::move (accepted_record),
                                            parts = std::move (parts), services, serializers,
                                            filters, options] (auto complete) mutable {
                                  record.application_record->retire_invocation = {};
                                  node->application_work_started ();
                                  dispatch_application (
                                    node, registration, owner, record, std::move (parts), false,
                                    services, serializers, filters, *options, std::move (complete));
                              };
                              item->cancel = [node, record] {
                                  node->application_work_started ();
                                  application_dispatch_terminal_owner_t terminal (
                                    node, record.complete_stateful_dispatch, {});
                                  auto parts = stateful::with_accepted_application (
                                    *record.application_record->payload (),
                                    [] (auto, const auto &application) {
                                        return protocol::decode_application_parts (application);
                                    });
                                  reject_application_request (
                                    record, std::move (parts),
                                    framework_error_kind_t::shutting_down,
                                    "Actor private application queue is stopping");
                              };
                          });
                          return item;
                      });
              });
            if (!_application_dispatch->start_role (std::move (role)))
                throw std::logic_error ("MeshNode receive executor is stopped");
        }
    }
    catch (...) {
        failure = std::current_exception ();
    }
    if (failure) {
        // Rollback drains workers as well as closing completion owners.
        co_await external_lifecycle_turn_t{};
        stop ();
        std::rethrow_exception (failure);
    }
    co_return;
}

void mesh_node_host_service_t::request_stop () noexcept
{
    _descriptor_stop.request_stop ();
    seal_application_dispatch ();
    if (_application_jobs)
        _application_jobs->stop ();
}

void mesh_node_host_service_t::seal_application_dispatch () noexcept
{
    {
        std::lock_guard lock (_dispatch_gate_mutex);
        _accept_application_dispatch.store (false, std::memory_order_release);
    }
    _dispatch_gate_changed.notify_all ();
}

bool mesh_node_host_service_t::wait_for_accepted_callbacks_until (
  std::chrono::steady_clock::time_point deadline) noexcept
{
    std::unique_lock lock (_dispatch_gate_mutex);
    auto settled = [this] {
        if (_active_direct_dispatch != 0)
            return false;
        return std::all_of (_nodes.begin (), _nodes.end (), [] (const auto &node) {
            return node->pending_application_callbacks () == 0
                   && node->active_application_callbacks () == 0
                   && node->pending_transport_operations () == 0
                   && node->active_completion_waiters () == 0;
        });
    };
    while (!settled () && std::chrono::steady_clock::now () < deadline) {
        _dispatch_gate_changed.wait_until (
          lock,
          std::min (deadline, std::chrono::steady_clock::now () + std::chrono::milliseconds (1)));
    }
    return settled ();
}

void mesh_node_host_service_t::stop () noexcept
{
    if (_actor_destroy_gate)
        _runtime_failures->capture ([&] {
            _actor_destroy_gate->stop_and_wait ();
            _actor_destroy_gate.reset ();
        });
    request_stop ();
    if (_application_dispatch)
        _runtime_failures->capture ([&] { _application_dispatch->drain_application (); });
    _stop.store (true, std::memory_order_release);
    for (const auto &node : _nodes)
        node->signal_dispatch_activity ();
    if (_application_dispatch)
        _runtime_failures->capture ([&] { _application_dispatch->drain (); });
    _completion_dispatcher.reset ();
    for (auto &node : _nodes)
        _runtime_failures->capture ([&] { node->bind_descriptor_publisher ({}); });
    if (_location_runtime)
        _runtime_failures->capture ([&] {
            infrastructure_result ([&] {
                return _location_runtime->set_descriptor_renewal ({});
            }).value ();
        });
    std::shared_ptr<task_completion_source_t<bool>> publication;
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        publication = _descriptor_publication;
    }
    if (publication)
        _runtime_failures->capture ([&] { publication->task ().result (); });
    if (_location_store)
        for (const auto &descriptor : _published_mesh_descriptors) {
            if (descriptor.owner_id.empty ())
                continue;
            _runtime_failures->capture ([&] {
                auto deadline = std::chrono::steady_clock::time_point::max ();
                const auto &lifecycle = _registrations.at (0)->spot_state->host_lifecycle;
                if (lifecycle) {
                    const auto shutdown = lifecycle ();
                    if (shutdown.shutdown_deadline) {
                        deadline =
                          std::chrono::steady_clock::now ()
                          + (*shutdown.shutdown_deadline - std::chrono::system_clock::now ());
                    }
                }
                if (std::chrono::steady_clock::now () >= deadline)
                    return;
                infrastructure_result ([&] {
                    return _location_store->remove_mesh_node (
                      {descriptor.mesh_name, descriptor.rid},
                      {descriptor.owner_id, descriptor.lease_generation}, deadline);
                }).value ();
            });
        }
    {
        std::lock_guard lock (_descriptor_publish_mutex);
        _published_mesh_nodes.clear ();
        _published_mesh_descriptors.clear ();
        _changed_mesh_descriptors.clear ();
        _location_runtime = nullptr;
    }
    for (auto &node : _nodes) {
        _runtime_failures->capture ([&] { node->stop (); });
        if (_listener_statuses)
            _runtime_failures->capture ([&] {
                _listener_statuses->remove (listener_kind_t::route_mesh, node->mesh_name ());
            });
    }
}

void mesh_node_host_service_t::visit_relocation_nodes (
  const std::function<void (const std::shared_ptr<detail::mesh_node_runtime_t> &)> &visitor) const
{
    if (!visitor)
        return;
    for (const auto &node : _nodes)
        visitor (node);
}

std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_node_host_service_t::nodes () const
{
    return _nodes;
}

task_t<void> mesh_node_host_service_t::drain_application_owner (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::shared_ptr<detail::mesh_node_builder_state_t> registration,
  std::string owner,
  bool reject_only,
  service_provider_t *services,
  serializer_registry_t *serializers,
  const handler_registry_t *filters,
  detail::retained_dispatch_options_t dispatch_options,
  offload_executor_t *executor)
{
    offload_executor_t::retained_work_t physical_work;
    // Move parameter dependencies into locals so they are destroyed before credit.
    auto owned_node = std::move (node);
    auto owned_registration = std::move (registration);
    auto &mailbox = owned_node->native_node ().transport ().mailbox ();
    if (!mailbox.begin_application_drain (owner))
        co_return;
    const auto deadline = std::chrono::steady_clock::now () + dispatch_limits::owner_time_budget;
    try {
        const mesh::service_mailbox_t::application_dispatch_t dispatch =
          [&] (const host::ready_record_t &ready, const host::receive_record_t &record,
               std::vector<zlink::message_t> parts, application_job_queue_t::permit_t permit) {
              dispatch_application (owned_node, owned_registration, ready, record,
                                    std::move (parts), reject_only, services, serializers, filters,
                                    dispatch_options, {}, executor, std::move (permit));
          };
        const std::function<void ()> started = [&owned_node] {
            owned_node->application_work_started ();
        };
        const std::function<void ()> rejected = [&owned_node] {
            owned_node->application_work_finished ();
        };
        while (true) {
            auto pending = owned_node->native_node ().dispatch_application_owner (
              owner, dispatch, started, rejected);
            bool claimed;
            if (const auto ready = std::get_if<bool> (&pending))
                claimed = *ready;
            else {
                auto &task = std::get<task_t<bool>> (pending);
                if (!task.await_ready () && !physical_work && executor)
                    physical_work = executor->retain_internal_work ();
                claimed = co_await task;
            }
            if (!claimed)
                break;
            if (!reject_only && std::chrono::steady_clock::now () >= deadline)
                break;
        }
    }
    catch (...) {
        mailbox.end_application_drain (owner);
        throw;
    }
    mailbox.end_application_drain (owner);
}

task_t<zlink::submit_result_t>
mesh_node_host_service_t::submit_local_node_send (std::shared_ptr<detail::mesh_node_runtime_t> node,
                                                  std::vector<zlink::message_t> parts)
{
    const auto found = std::find (_nodes.begin (), _nodes.end (), node);
    if (found == _nodes.end () || _services == nullptr || _serializers == nullptr)
        co_return zlink::submit_result_t::not_found;
    const auto index = static_cast<std::size_t> (std::distance (_nodes.begin (), found));
    const auto registration = _registrations[index];
    const auto source_rid = co_await node->routing_id_async ();
    if (!source_rid)
        co_return zlink::submit_result_t::not_found;
    node->note_local_node_submit_attempt ();

    {
        std::lock_guard lock (_dispatch_gate_mutex);
        if (!_accept_application_dispatch.load (std::memory_order_relaxed)
            || (_location_runtime && !_location_runtime->owner_lease_usable ()))
            co_return zlink::submit_result_t::terminated;
    }
    using permit_t = application_job_queue_t::permit_t;
    auto supplied = std::make_shared<task_completion_source_t<std::shared_ptr<permit_t>>> ();
    auto waiter = _application_jobs->wait_for_supply (
      [supplied] (std::optional<permit_t> permit) {
          supplied->complete (result_t<std::shared_ptr<permit_t>>::success (
            permit ? std::make_shared<permit_t> (std::move (*permit)) : nullptr));
      },
      application_job_queue_t::origin_t::local);
    auto application_job = co_await supplied->task ();
    if (!application_job)
        co_return zlink::submit_result_t::terminated;
    {
        std::lock_guard lock (_dispatch_gate_mutex);
        if (!_accept_application_dispatch.load (std::memory_order_relaxed)
            || (_location_runtime && !_location_runtime->owner_lease_usable ()))
            co_return zlink::submit_result_t::terminated;
        application_job->mark_queued ();
    }

    host::ready_record_t owner;
    host::receive_record_t record;
    record.kind = host::record_kind_t::node_send;
    record.domain = host::ready_domain_t::application;
    record.source_node_rid = *source_rid;
    mesh::service_mailbox_record_t mailbox_record;
    mailbox_record.owner = mesh::service_mailbox_t::application_owner (owner);
    mailbox_record.domain = mesh::service_mailbox_domain_t::application;
    mailbox_record.application_permit.emplace (std::move (*application_job));
    mailbox_record.application.emplace (
      host::local_application_dispatch_t{std::move (owner), std::move (record), std::move (parts)});
    if (!node->native_node ().transport ().mailbox ().try_enqueue (std::move (mailbox_record)))
        co_return zlink::submit_result_t::terminated;
    co_return zlink::submit_result_t::ok;
}

} // namespace zlink::framework::runtime
