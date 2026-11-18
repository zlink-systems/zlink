/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/actors/actor_client.hpp"

#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/owned_task.hpp"
#include "runtime/actors/actor_manager_access.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/execution/state_lane.hpp"
#include "runtime/handlers/handler_registry_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"

#include <zlink/framework/contracts/locations/stores.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <tuple>
#include <vector>

namespace zlink::framework
{

namespace detail
{
namespace
{

inline thread_local std::optional<bool> current_actor_request_release_turn;

class actor_request_turn_intent_scope_t
{
  public:
    explicit actor_request_turn_intent_scope_t (bool release_turn) :
        _previous (current_actor_request_release_turn)
    {
        current_actor_request_release_turn = release_turn;
    }

    ~actor_request_turn_intent_scope_t () { current_actor_request_release_turn = _previous; }

    actor_request_turn_intent_scope_t (const actor_request_turn_intent_scope_t &) = delete;
    actor_request_turn_intent_scope_t &
    operator= (const actor_request_turn_intent_scope_t &) = delete;

  private:
    std::optional<bool> _previous;
};

bool actor_request_releases_current_turn ()
{
    assert (current_actor_request_release_turn.has_value ());
    return *current_actor_request_release_turn;
}
} // namespace

class actor_manager_state_t
{
  public:
    actor_manager_access_t::create_fn_t create_actor;
    actor_manager_access_t::find_fn_t find_actor;
    actor_manager_access_t::find_spot_fn_t find_spot;
    actor_manager_access_t::destroy_fn_t destroy_actor;
    std::atomic<std::uint64_t> operation_sequence{1};
};

class actor_create_call_state_t
{
  public:
    std::shared_ptr<actor_manager_state_t> manager;
    bool exclusive = false;
    std::optional<actor_id_t> actor_id;
    std::string stable_type;
    std::optional<std::string> mesh_name;
    std::optional<message_t> request;
    std::chrono::milliseconds timeout{std::chrono::seconds (30)};
    bool mesh_set = false;
    bool request_set = false;
    bool timeout_set = false;
    bool submitted = false;
    runtime::state_lane_t lane;
};
} // namespace detail

namespace
{
void require_call_option (bool &flag, const char *name)
{
    if (flag)
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     std::string ("Actor create option was set more than once: ")
                                       + name);
    flag = true;
}
} // namespace

actor_create_call_t::actor_create_call_t (
  std::shared_ptr<detail::actor_create_call_state_t> state) :
    _state (std::move (state))
{
}

actor_create_call_t::~actor_create_call_t () = default;
actor_create_call_t::actor_create_call_t (actor_create_call_t &&) noexcept = default;
actor_create_call_t &actor_create_call_t::operator= (actor_create_call_t &&) noexcept = default;

actor_create_call_t &actor_create_call_t::in_mesh (std::string mesh_name)
{
    _state->lane
      .run ([&] {
          require_call_option (_state->mesh_set, "in_mesh");
          _state->mesh_name = std::move (mesh_name);
      })
      .get ();
    return *this;
}

actor_create_call_t &actor_create_call_t::creation_request (message_t request)
{
    _state->lane
      .run ([&] {
          require_call_option (_state->request_set, "creation_request");
          _state->request = std::move (request);
      })
      .get ();
    return *this;
}

actor_create_call_t &actor_create_call_t::timeout (std::chrono::milliseconds timeout)
{
    _state->lane
      .run ([&] {
          require_call_option (_state->timeout_set, "timeout");
          if (timeout <= std::chrono::milliseconds::zero ())
              throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                           "Actor create timeout must be positive");
          _state->timeout = timeout;
      })
      .get ();
    return *this;
}

task_t<actor_create_result_t> actor_create_call_t::async ()
{
    const auto state = _state;
    auto [create, exclusive, actor_id, stable_type, mesh_name, request, timeout, operation] =
      co_await state->lane.run_task ([state] {
          if (state->submitted)
              throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                           "Actor create call was already submitted");
          state->submitted = true;
          if (!state->manager || !state->manager->create_actor)
              throw framework_exception_t (framework_error_kind_t::not_configured,
                                           "Actor manager is not bound to an object runtime");
          const auto sequence = state->manager->operation_sequence.fetch_add (1);
          const creation_operation_id_t operation{
            static_cast<std::uint64_t> (reinterpret_cast<std::uintptr_t> (state->manager.get ())),
            sequence};
          return std::tuple{state->manager->create_actor,
                            state->exclusive,
                            *state->actor_id,
                            state->stable_type,
                            state->mesh_name,
                            state->request,
                            state->timeout,
                            operation};
      });
    co_return co_await create (exclusive, std::move (actor_id), std::move (stable_type),
                               std::move (mesh_name), std::move (request), timeout, operation);
}

task_t<actor_create_result_t> actor_create_call_t::yield ()
{
    return detail::yield_call<actor_create_result_t> ([this] { return async (); });
}

actor_manager_t::actor_manager_t () : _state (std::make_shared<detail::actor_manager_state_t> ())
{
}

actor_manager_t::actor_manager_t (std::shared_ptr<detail::actor_manager_state_t> state) :
    _state (std::move (state))
{
}

actor_manager_t::~actor_manager_t () = default;
actor_manager_t::actor_manager_t (actor_manager_t &&) noexcept = default;
actor_manager_t &actor_manager_t::operator= (actor_manager_t &&) noexcept = default;

actor_create_call_t actor_manager_t::create (actor_id_t actor_id, std::string stable_type)
{
    auto state = std::make_shared<detail::actor_create_call_state_t> ();
    state->manager = _state;
    state->exclusive = true;
    state->actor_id = std::move (actor_id);
    state->stable_type = std::move (stable_type);
    return actor_create_call_t (std::move (state));
}

actor_create_call_t actor_manager_t::get_or_create (actor_id_t actor_id, std::string stable_type)
{
    auto state = std::make_shared<detail::actor_create_call_state_t> ();
    state->manager = _state;
    state->actor_id = std::move (actor_id);
    state->stable_type = std::move (stable_type);
    return actor_create_call_t (std::move (state));
}

task_t<std::optional<actor_ref_t>> actor_manager_t::find (actor_id_t actor_id) const
{
    if (!_state || !_state->find_actor)
        return task_t<std::optional<actor_ref_t>> (result_t<std::optional<actor_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Actor manager is not bound to an object runtime"));
    return _state->find_actor (std::move (actor_id));
}

task_t<std::optional<spot_ref_t>> actor_manager_t::find_spot (actor_id_t actor_id) const
{
    if (!_state || !_state->find_spot)
        return task_t<std::optional<spot_ref_t>> (result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Actor manager is not bound to an object runtime"));
    return _state->find_spot (std::move (actor_id));
}

task_t<bool> actor_manager_t::destroy (actor_ref_t actor)
{
    if (!_state || !_state->destroy_actor)
        return task_t<bool> (
          result_t<bool>::failure (framework_error_kind_t::not_configured,
                                   "Actor manager is not bound to an object runtime"));
    return _state->destroy_actor (std::move (actor));
}

actor_manager_t detail::actor_manager_access_t::create (create_fn_t create_actor,
                                                        find_fn_t find_actor,
                                                        find_spot_fn_t find_spot,
                                                        destroy_fn_t destroy_actor)
{
    auto state = std::make_shared<actor_manager_state_t> ();
    state->create_actor = std::move (create_actor);
    state->find_actor = std::move (find_actor);
    state->find_spot = std::move (find_spot);
    state->destroy_actor = std::move (destroy_actor);
    return actor_manager_t (std::move (state));
}

actor_send_call_t::actor_send_call_t (actor_client_t &client,
                                      actor_id_t actor_id,
                                      std::string packet_name,
                                      message_t message) :
    _client (&client),
    _actor_id (std::move (actor_id)),
    _packet_name (std::move (packet_name)),
    _message (std::move (message))
{
}

actor_send_call_t &actor_send_call_t::metadata (std::string key, std::string value)
{
    _metadata[std::move (key)] = std::move (value);
    return *this;
}

task_t<void> actor_send_call_t::async ()
{
    if (_submission->claimed ()) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "actor send call has already been submitted"));
    }
    try {
        if (_client)
            _client->admit_call ();
    }
    catch (...) {
        return task_t<void> (detail::current_exception_result<void> ());
    }
    if (_client == nullptr) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "actor send call is not bound to an actor client"));
    }
    if (!_submission->try_claim ()) {
        return task_t<void> (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "actor send call has already been submitted"));
    }
    return _client->send_erased (std::move (_actor_id), std::move (_packet_name),
                                 std::move (_message), std::move (_metadata));
}

actor_request_call_t::actor_request_call_t (actor_client_t &client,
                                            actor_id_t actor_id,
                                            std::string packet_name,
                                            message_t request) :
    _client (&client),
    _actor_id (std::move (actor_id)),
    _packet_name (std::move (packet_name)),
    _request (std::move (request))
{
}

actor_request_call_t &actor_request_call_t::timeout (std::chrono::milliseconds timeout)
{
    _timeout = timeout;
    return *this;
}

actor_request_call_t &actor_request_call_t::metadata (std::string key, std::string value)
{
    _metadata.insert_or_assign (std::move (key), std::move (value));
    return *this;
}

request_task_t<message_t> actor_request_call_t::async_message ()
{
    return start (false);
}

task_t<message_t> actor_request_call_t::yield_message ()
{
    return detail::yield_call<message_t> ([this] { return start (true); });
}

request_task_t<message_t> actor_request_call_t::start (bool release_turn)
{
    try {
        _client->admit_call ();
    }
    catch (...) {
        return request_task_t<message_t> (detail::current_exception_result<message_t> ());
    }
    if (!runtime::current_actor_execution.actor_key.empty ()) {
        const auto separator = runtime::current_actor_execution.actor_key.rfind (':');
        const auto current_actor_id =
          separator == std::string::npos
            ? runtime::current_actor_execution.actor_key
            : runtime::current_actor_execution.actor_key.substr (separator + 1);
        if (current_actor_id == _actor_id.value ()) {
            return request_task_t<message_t> (result_t<message_t>::failure (
              framework_error_kind_t::invalid_operation,
              "awaited request to the current Actor cannot complete while its FIFO claim is held"));
        }
    }
    // A Yield releases the turn where the handler suspends (task.hpp); until
    // then the reply decoding resumes in the held turn like an async call.
    auto held = detail::held_serial_turn_await_scheduler ();
    detail::actor_request_turn_intent_scope_t request_intent (release_turn);
    auto task = _client->request_erased (std::move (_actor_id), std::move (_packet_name),
                                         std::move (_request), _timeout, _metadata);
    if (!held) {
        return task;
    }
    return detail::reschedule_task (std::move (task), std::move (held));
}

serializer_registry_t &actor_request_call_t::serializers () const
{
    return _client->actor_client_serializers ();
}

} // namespace zlink::framework

namespace zlink::framework::runtime
{

bool actor_request_requires_current_spot_gate (std::string_view target_spot_id, bool release_turn)
{
    assert (!target_spot_id.empty ());
    return !release_turn && detail::current_serial_turn_allows_yield ()
           && !current_actor_execution.spot_id.empty ()
           && target_spot_id == current_actor_execution.spot_id;
}


class actor_client_impl_t final : public actor_client_t,
                                  public std::enable_shared_from_this<actor_client_impl_t>
{
    friend struct actor_client_test_access_t;

  public:
    actor_client_impl_t (live_location_reader_t &store,
                         serializer_registry_t &serializers,
                         std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes,
                         std::shared_ptr<actor_location_observer_t> actor_locations,
                         location_options_t options,
                         std::function<void ()> admit_call) :
        _store (&store),
        _serializers (detail::serializer_registry_access_t::retain (serializers)),
        _mesh_nodes (std::move (mesh_nodes)),
        _actor_locations (std::move (actor_locations)),
        _location_options (std::move (options)),
        _admit_call (std::move (admit_call))
    {
        _message_follow_subscriptions.reserve (_mesh_nodes.size ());
        try {
            for (const auto &mesh_node : _mesh_nodes) {
                if (!mesh_node)
                    continue;
                const auto subscription_id =
                  mesh_node->subscribe_message_follow_invalidation ([this] (const auto &notice) {
                      invalidate_cached_route_on_message_follow (notice);
                  });
                _message_follow_subscriptions.emplace_back (mesh_node, subscription_id);
            }
        }
        catch (...) {
            release_message_follow_subscriptions ();
            throw;
        }
    }

    ~actor_client_impl_t () override { release_message_follow_subscriptions (); }

    task_t<void> send_to_ref (actor_ref_t actor, std::string packet_name, message_t message)
    {
        auto resolved = co_await resolve_explicit_actor (actor);
        if (!resolved) {
            throw *resolved.error ();
        }
        auto relayed = co_await relay_explicit_actor_packet (
          resolved.value (), runtime::messaging::message_kind_t::command, std::move (packet_name),
          std::move (message), _default_timeout);
        (void) relayed;
        co_return;
    }

    task_t<message_t> request_to_ref (actor_ref_t actor,
                                      std::string packet_name,
                                      message_t message,
                                      std::chrono::milliseconds timeout)
    {
        auto resolved = co_await resolve_explicit_actor (actor);
        if (!resolved) {
            co_return detail::propagate_failure<message_t> (resolved,
                                                            "explicit Actor route is unavailable");
        }
        auto relayed = co_await relay_explicit_actor_packet (
          resolved.value (), runtime::messaging::message_kind_t::request, std::move (packet_name),
          std::move (message), timeout);
        if (!relayed) {
            co_return result_t<message_t>::failure (framework_error_kind_t::internal_failure,
                                                    "explicit Actor route reply body is missing");
        }
        co_return result_t<message_t>::success (message_t::from_raw (*relayed, &_serializers));
    }

  protected:
    void admit_call () override
    {
        if (_admit_call)
            _admit_call ();
    }
    task_t<void> send_erased (actor_id_t actor_id,
                              std::string packet_name,
                              message_t message,
                              const actor_send_call_t::metadata_map_t &metadata) override
    {
        /* Frame-owned metadata copy before the first suspension: the reference
         * belongs to a caller-owned call object that can unwind while the
         * route lookup is suspended. */
        actor_send_call_t::metadata_map_t metadata_frame = metadata;
        if (!first_mesh_node ())
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "actor send requires a running MeshNode");
        auto actor = co_await await_result (resolve_actor (std::string (actor_id.value ())));
        if (!actor) {
            const auto failed =
              detail::propagate_failure<void> (actor, "actor route was not found");
            const auto *error = failed.error ();
            throw framework_exception_t (failed.error_kind (), error != nullptr
                                                                 ? error->what ()
                                                                 : "actor route was not found");
        }
        const auto submitted = co_await submit_send (
          actor.value (), std::move (packet_name), std::move (message), std::move (metadata_frame));
        if (!submitted) {
            const auto *error = submitted.error ();
            throw framework_exception_t (submitted.error_kind (),
                                         error != nullptr ? error->what () : "actor send failed");
        }
        co_return;
    }

    request_task_t<message_t>
    request_erased (actor_id_t actor_id,
                    std::string packet_name,
                    message_t request,
                    std::optional<std::chrono::milliseconds> timeout,
                    const actor_request_call_t::metadata_map_t &metadata) override
    {
        const auto lifetime = shared_from_this ();
        const auto release_turn = detail::actor_request_releases_current_turn ();
        const actor_request_call_t::metadata_map_t metadata_frame = metadata;
        const auto source = first_mesh_node ();
        if (!source)
            co_return result_t<message_t>::failure (framework_error_kind_t::unavailable,
                                                    "Actor request requires a running MeshNode");
        const auto admitted = co_await detail::request_admission_source_t{};
        detail::host::pending_operation_t operation;
        co_await source->register_actor_request (operation, timeout.value_or (_default_timeout),
                                                 admitted);
        try {
            const auto actor_id_value = std::string (actor_id.value ());
            auto actor = co_await await_result (resolve_actor (actor_id_value));
            const auto deadline = operation.registry->deadline (operation.id);
            if (!deadline || *deadline <= std::chrono::steady_clock::now ()) {
                const auto ended = co_await await_result (operation.completion->task ());
                co_return detail::propagate_failure<message_t> (
                  ended, "Actor request ended during lookup");
            }
            if (!actor) {
                auto failure =
                  detail::propagate_failure<message_t> (actor, "actor route was not found");
                operation.registry->fail (
                  operation.id, runtime::foundation::operation_terminal_t::transport_failed,
                  detail::result_access_t::failure<zlink::message_t> (failure.exception ()));
                co_return failure;
            }
            if (!release_turn && detail::current_serial_turn_allows_yield ()
                && !runtime::current_actor_execution.spot_id.empty ()
                && actor_request_requires_current_spot_gate (actor.value ().spot_id,
                                                             release_turn)) {
                auto failure = result_t<message_t>::failure (
                  framework_error_kind_t::invalid_operation,
                  "awaited request requires the current Spot execution gate");
                operation.registry->fail (
                  operation.id, runtime::foundation::operation_terminal_t::transport_failed,
                  detail::result_access_t::failure<zlink::message_t> (failure.exception ()));
                co_return failure;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              *deadline - std::chrono::steady_clock::now ());
            const auto request_id =
              _request_id_prefix + std::to_string (_request_id_seq.fetch_add (1));
            auto submitted =
              submit_request (actor.value (), std::move (packet_name), std::move (request),
                              remaining, request_id, metadata_frame, operation);
            co_return co_await detail::request_submission (std::move (submitted));
        }
        catch (...) {
            operation.registry->fail (
              operation.id, runtime::foundation::operation_terminal_t::transport_failed,
              detail::result_access_t::failure<zlink::message_t> (std::current_exception ()));
            throw;
        }
    }

    serializer_registry_t &actor_client_serializers () override { return _serializers; }

  private:
    void release_message_follow_subscriptions () noexcept
    {
        for (const auto &[mesh_node, subscription_id] : _message_follow_subscriptions) {
            mesh_node->unsubscribe_message_follow_invalidation (subscription_id);
        }
        _message_follow_subscriptions.clear ();
    }

    struct resolved_actor_t
    {
        actor_ref_t framework_ref;
        actor_ref_t native_ref;
        std::uint64_t node_generation = 0;
        spot_id_t spot_id;
        std::string mesh_name;
        std::uint64_t authority_owner_generation = 0;
        std::uint64_t owner_lease_generation = 0;
        location_owner_token_t owner;
    };

    task_t<resolved_actor_t> resolve_actor (std::string actor_id)
    {
        const auto runtime = first_mesh_node ();
        if (!runtime) {
            co_return result_t<resolved_actor_t>::failure (
              framework_error_kind_t::unavailable, "actor lookup requires a running MeshNode");
        }
        if (_location_options.route_cache_max_age > std::chrono::milliseconds::zero ()) {
            std::lock_guard lock (_route_cache_gate);
            const auto cached = _route_cache.find (actor_id);
            if (cached != _route_cache.end ()) {
                if (std::chrono::steady_clock::now () < cached->second.expires_at) {
                    co_return result_t<resolved_actor_t>::success (cached->second.actor);
                }
                _route_cache.erase (cached);
            }
        }
        auto read = co_await await_result (_store->read_authority (actor_authority_key (actor_id)));
        if (!read) {
            co_return detail::propagate_failure<resolved_actor_t> (read,
                                                                   "actor authority lookup failed");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        const auto projection = snapshot
                                  ? zlink::framework::runtime::decode_actor_authority_payload (
                                      snapshot->payload, snapshot->object_generation)
                                  : std::nullopt;
        if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor || !projection
            || projection->actor.actor_id ().value () != actor_id) {
            co_return result_t<resolved_actor_t>::failure (framework_error_kind_t::not_found,
                                                           "actor route was not found");
        }
        if (projection->spot_id.empty ()) {
            co_return result_t<resolved_actor_t>::failure (framework_error_kind_t::not_found,
                                                           "actor SPOT route was not found");
        }
        auto resolved =
          resolved_actor_t{projection->actor,
                           projection->actor,
                           snapshot->allocation.target.node_lifecycle_generation,
                           projection->spot_id,
                           snapshot->allocation.target.mesh_name,
                           snapshot->authority_owner_generation,
                           static_cast<std::uint64_t> (snapshot->owner.lease_generation),
                           snapshot->owner};
        const auto lease_lifetime = co_await _store->owner_admission_lifetime (snapshot->owner);
        if (_location_options.route_cache_max_age > std::chrono::milliseconds::zero ()
            && lease_lifetime) {
            const auto lifetime =
              std::min (std::chrono::duration_cast<std::chrono::steady_clock::duration> (
                          _location_options.route_cache_max_age),
                        *lease_lifetime);
            if (lifetime > std::chrono::steady_clock::duration::zero ()) {
                std::lock_guard lock (_route_cache_gate);
                _route_cache.insert_or_assign (
                  actor_id, cached_actor_t{resolved, std::chrono::steady_clock::now () + lifetime});
            }
        }
        co_return result_t<resolved_actor_t>::success (std::move (resolved));
    }

    task_t<result_t<resolved_actor_t>> resolve_explicit_actor (actor_ref_t actor)
    {
        if (!first_mesh_node ()) {
            co_return result_t<resolved_actor_t>::failure (
              framework_error_kind_t::unavailable,
              "explicit Actor route requires a running MeshNode");
        }
        auto read = co_await await_result (
          _store->read_authority (actor_authority_key (actor.actor_id ().value ())));
        if (!read) {
            co_return detail::propagate_failure<resolved_actor_t> (read,
                                                                   "Actor authority lookup failed");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        const auto projection = snapshot
                                  ? zlink::framework::runtime::decode_actor_authority_payload (
                                      snapshot->payload, snapshot->object_generation)
                                  : std::nullopt;
        if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor || !projection
            || projection->actor.actor_id () != actor.actor_id ()
            || projection->actor.node_rid ().value () != actor.node_rid ().value ()) {
            co_return result_t<resolved_actor_t>::failure (
              framework_error_kind_t::unavailable,
              "explicit Actor route no longer identifies the current incarnation");
        }
        const auto mesh_name = actor.mesh_name ().empty () ? snapshot->allocation.target.mesh_name
                                                           : std::string (actor.mesh_name ());
        co_return result_t<resolved_actor_t>::success (resolved_actor_t{
          projection->actor, projection->actor,
          snapshot->allocation.target.node_lifecycle_generation, projection->spot_id, mesh_name,
          snapshot->authority_owner_generation,
          static_cast<std::uint64_t> (snapshot->owner.lease_generation), snapshot->owner});
    }

    task_t<result_t<void>> submit_send (resolved_actor_t actor,
                                        std::string packet_name,
                                        message_t message,
                                        actor_send_call_t::metadata_map_t metadata)
    {
        /* A one-way remote send has no owner terminal to repair a route-cache
         * hit after an exact Actor destroy.  Re-read only the existence bit:
         * an active or moving authority keeps the normal route/backlog path,
         * while an actually missing authority is exposed as NotFound before
         * transport admission. */
        if (co_await cached_route_target_deleted (
              std::string (actor.framework_ref.actor_id ().value ()))) {
            invalidate_cached_route_on_stale (actor, framework_error_kind_t::not_found);
            co_return result_t<void>::failure (framework_error_kind_t::not_found,
                                               "actor route was not found");
        }
        auto runtime = mesh_node (actor.mesh_name);
        if (!runtime) {
            co_return result_t<void>::failure (framework_error_kind_t::unavailable,
                                               "actor send requires a running MeshNode");
        }
        auto relayed = co_await relay_actor_command (
          runtime, actor, std::move (packet_name), std::move (message), _default_timeout, metadata);
        if (!relayed) {
            invalidate_cached_route_on_stale (actor, relayed.error_kind ());
            co_return detail::propagate_failure<void> (relayed, "actor send failed");
        }
        co_return result_t<void>::success ();
    }

    task_t<bool> cached_route_target_deleted (std::string actor_id)
    {
        {
            std::lock_guard lock (_route_cache_gate);
            if (!_route_cache.contains (actor_id))
                co_return false;
        }
        try {
            const auto read =
              co_await await_result (_store->read_authority (actor_authority_key (actor_id)));
            if (!read)
                co_return false;
            co_return std::get_if<authority_snapshot_t> (&read.value ()) == nullptr;
        }
        catch (...) {
            co_return false;
        }
    }

    task_t<std::optional<zlink::message_t>>
    relay_explicit_actor_packet (const resolved_actor_t &actor,
                                 runtime::messaging::message_kind_t kind,
                                 std::string packet_name,
                                 message_t message,
                                 std::chrono::milliseconds timeout)
    {
        auto route = mesh_node (actor.mesh_name);
        if (!route) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "explicit Actor route requires a running MeshNode");
        }
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (kind, detail::actor_message_channel_name,
                                             std::move (packet_name), timeout);
        header.content_type = _serializers.content_type (message._type);
        header.correlation_id = _request_id_prefix + std::to_string (_request_id_seq.fetch_add (1));
        co_return co_await route->relay_application_actor (
          actor.native_ref, header, detail::message_to_raw (message, _serializers), timeout,
          zlink::routing_id_t::from (std::uint32_t{0}), runtime::protocol::actor_route_fence_t{}, 0,
          runtime::protocol::wire_operation_id_t{}, 0, true);
    }

    detail::request_receiver_t<result_t<runtime::messaging::message_parts_t>>
    prepare_request_receiver (resolved_actor_t actor,
                              detail::request_receiver_t<result_t<message_t>> receiver)
    {
        return receiver.project<result_t<runtime::messaging::message_parts_t>> (
          [self = shared_from_this (), actor = std::move (actor)] (const auto &terminal) {
              using reply_t = result_t<message_t>;
              auto relayed =
                terminal ? terminal.value ()
                         : detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                             terminal.exception ());
              auto decoded = relayed ? detail::decode_actor_reply (relayed.value ())
                                     : detail::propagate_failure<std::optional<zlink::message_t>> (
                                         relayed, "actor request failed");
              if (!decoded) {
                  self->invalidate_cached_route_on_stale (actor, decoded.error_kind ());
                  return result_t<reply_t>::success (
                    detail::propagate_failure<message_t> (decoded, "actor request failed"));
              }
              if (!decoded.value ())
                  return result_t<reply_t>::success (
                    reply_t::failure (framework_error_kind_t::internal_failure,
                                      "actor request reply body is missing"));
              return result_t<reply_t>::success (
                reply_t::success (message_t::from_raw (*decoded.value (), &self->_serializers)));
          });
    }

    static task_t<void> submit_prepared_request (
      std::shared_ptr<detail::mesh_node_runtime_t> runtime,
      resolved_actor_t actor,
      runtime::messaging::message_parts_t parts,
      std::chrono::milliseconds timeout,
      detail::host::pending_operation_t operation,
      detail::request_receiver_t<result_t<runtime::messaging::message_parts_t>> receiver)
    {
        try {
            auto copied = parts.items ();
            const auto submit = co_await runtime->request_to_actor (
              actor.native_ref, copied, operation, timeout, {}, actor.authority_owner_generation,
              actor.owner_lease_generation, std::nullopt, actor.node_generation);
            if (submit != zlink::submit_result_t::ok)
                throw runtime::messaging::map_submit_result_exception (submit, "Actor submission");
            receiver.admitted->complete (result_t<void>::success ());
        }
        catch (...) {
            const auto failure = std::current_exception ();
            operation.registry->fail (operation.id,
                                      runtime::foundation::operation_terminal_t::transport_failed,
                                      detail::result_access_t::failure<zlink::message_t> (failure));
            // Without a physical producer, the registry's terminal is the result.
            // A rejected continuation may follow a timeout whose callback is queued.
            if (operation.receiver) {
                auto completion = operation.completion->task ();
                detail::observe_task_terminal (completion, std::move (operation.receiver));
            }
        }
    }

    request_task_t<result_t<message_t>>
    submit_request (resolved_actor_t actor,
                    std::string packet_name,
                    message_t request,
                    std::chrono::milliseconds timeout,
                    std::string request_id,
                    actor_request_call_t::metadata_map_t metadata,
                    detail::host::pending_operation_t operation)
    {
        return detail::request_task_access_t::receive_public<result_t<message_t>> (
          [this, actor = std::move (actor), packet_name = std::move (packet_name),
           request = std::move (request), timeout, request_id = std::move (request_id),
           metadata = std::move (metadata), operation] (auto receiver) mutable {
              auto runtime = mesh_node (actor.mesh_name);
              if (!runtime)
                  throw framework_exception_t (framework_error_kind_t::unavailable,
                                               "actor request requires a running MeshNode");
              runtime::messaging::client_call_codec_t codec;
              auto header =
                codec.create_envelope (runtime::messaging::message_kind_t::request,
                                       detail::actor_message_channel_name, packet_name, timeout);
              header.content_type = _serializers.content_type (request._type);
              header.correlation_id = std::move (request_id);
              header.metadata = std::move (metadata);
              auto parts = detail::encode_actor_packet (
                header, detail::message_to_raw (request, _serializers));
              auto physical = prepare_request_receiver (actor, receiver);
              runtime->prepare_actor_request_reply (operation, physical);
              auto work = std::make_shared<std::function<task_t<void> ()>> (
                [runtime, actor = std::move (actor), parts = std::move (parts), timeout, operation,
                 physical] () mutable {
                    return submit_prepared_request (runtime, std::move (actor), std::move (parts),
                                                    timeout, operation, physical);
                });
              runtime::start_owned_task<void> (std::move (work),
                                               [admitted = receiver.admitted] (const auto &result) {
                                                   if (!result)
                                                       admitted->complete (result);
                                               });
          });
    }

    task_t<result_t<void>>
    relay_actor_command (std::shared_ptr<detail::mesh_node_runtime_t> runtime,
                         resolved_actor_t actor,
                         std::string packet_name,
                         message_t message,
                         std::chrono::milliseconds timeout,
                         actor_send_call_t::metadata_map_t metadata = {})
    {
        runtime::messaging::client_call_codec_t codec;
        auto header =
          codec.create_envelope (runtime::messaging::message_kind_t::command,
                                 detail::actor_message_channel_name, packet_name, timeout);
        header.content_type = _serializers.content_type (message._type);
        header.metadata = metadata;
        auto parts =
          detail::encode_actor_packet (header, detail::message_to_raw (message, _serializers));
        try {
            auto copied = parts.items ();
            const auto submit = co_await runtime->send_to_actor (actor.native_ref, copied, {},
                                                                 actor.authority_owner_generation,
                                                                 actor.owner_lease_generation);
            if (submit != zlink::submit_result_t::ok) {
                if (submit == zlink::submit_result_t::terminated) {
                    co_return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                                              "actor send runtime is stopped");
                }
                co_return result_t<void>::failure (
                  runtime::messaging::map_submit_result_exception (submit, "Actor submission")
                    .kind (),
                  "actor send was not accepted (result "
                    + std::to_string (static_cast<int> (submit)) + ")");
            }
            co_return result_t<void>::success ();
        }
        catch (const framework_exception_t &) {
            co_return detail::result_access_t::failure<void> (std::current_exception ());
        }
        catch (const std::exception &error) {
            co_return map_native_exception<void> (error, "actor send failed");
        }
    }

    void invalidate_cached_route_on_stale (const resolved_actor_t &actor,
                                           framework_error_kind_t kind)
    {
        if (kind != framework_error_kind_t::unavailable
            && kind != framework_error_kind_t::not_found) {
            return;
        }
        std::lock_guard lock (_route_cache_gate);
        const auto found =
          _route_cache.find (std::string (actor.framework_ref.actor_id ().value ()));
        if (found == _route_cache.end ())
            return;
        const auto &cached = found->second.actor;
        if (cached.framework_ref.object_generation () == actor.framework_ref.object_generation ()
            && cached.native_ref.node_rid ().value () == actor.native_ref.node_rid ().value ()
            && cached.node_generation == actor.node_generation
            && cached.authority_owner_generation == actor.authority_owner_generation
            && cached.owner_lease_generation == actor.owner_lease_generation)
            _route_cache.erase (found);
    }

    void invalidate_cached_route_on_message_follow (
      const runtime::protocol::message_follow_notice_t &notice)
    {
        const auto *source = std::get_if<runtime::protocol::actor_route_fence_t> (&notice.source);
        const auto *target = std::get_if<runtime::protocol::actor_route_fence_t> (&notice.target);
        if (!source || !target || source->actor_id != target->actor_id
            || source->object_generation != target->object_generation)
            return;
        std::lock_guard lock (_route_cache_gate);
        const auto found = _route_cache.find (source->actor_id);
        if (found == _route_cache.end ())
            return;
        const auto &cached = found->second.actor;
        if (cached.framework_ref.object_generation () != source->object_generation
            || cached.authority_owner_generation != source->authority_owner_generation
            || cached.owner_lease_generation != source->owner_lease_generation
            || cached.native_ref.node_rid ().empty ()
            || cached.native_ref.node_rid ().value ()
                 != zlink::routing_id_t::from (source->target_node_routing_id).to_string ())
            return;
        _route_cache.erase (found);
    }

    std::shared_ptr<detail::mesh_node_runtime_t> first_mesh_node () const
    {
        for (const auto &mesh_node : _mesh_nodes)
            if (mesh_node)
                return mesh_node;
        return {};
    }

    std::shared_ptr<detail::mesh_node_runtime_t> mesh_node (const std::string &mesh_name) const
    {
        if (!mesh_name.empty ()) {
            for (const auto &mesh_node : _mesh_nodes) {
                if (mesh_node && mesh_node->mesh_name () == mesh_name)
                    return mesh_node;
            }
            return {};
        }
        return first_mesh_node ();
    }

    template <typename TResult>
    static result_t<TResult> map_native_exception (const std::exception &error,
                                                   const char *fallback)
    {
        if (detail::is_cancellation_exception (error))
            return detail::result_access_t::failure<TResult> (std::current_exception ());
        const std::string message = error.what () && *error.what () ? error.what () : fallback;
        const auto *system = dynamic_cast<const std::system_error *> (&error);
        if (system
            && (system->code () == std::errc::not_connected
                || system->code () == std::errc::network_unreachable
                || system->code () == std::errc::host_unreachable
                || system->code () == std::errc::connection_refused
                || system->code () == std::errc::connection_reset
                || system->code () == std::errc::connection_aborted
                || system->code () == std::errc::no_such_file_or_directory)) {
            return result_t<TResult>::failure (framework_error_kind_t::unavailable, message);
        }
        return result_t<TResult>::failure (framework_error_kind_t::internal_failure, message);
    }

    live_location_reader_t *_store;
    serializer_registry_t _serializers;
    std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> _mesh_nodes;
    std::shared_ptr<actor_location_observer_t> _actor_locations;
    location_options_t _location_options;
    std::vector<std::pair<std::shared_ptr<detail::mesh_node_runtime_t>,
                          detail::mesh_node_runtime_t::message_follow_subscription_id_t>>
      _message_follow_subscriptions;
    struct cached_actor_t
    {
        resolved_actor_t actor;
        std::chrono::steady_clock::time_point expires_at;
    };
    std::mutex _route_cache_gate;
    std::map<std::string, cached_actor_t> _route_cache;
    std::chrono::milliseconds _default_timeout{std::chrono::seconds (30)};
    std::function<void ()> _admit_call;
    const std::string _request_id_prefix =
      std::to_string (reinterpret_cast<std::uintptr_t> (this)) + "-";
    std::atomic<std::uint64_t> _request_id_seq{1};
};

namespace
{
auto test_actor_route (const protocol::actor_route_fence_t &fence)
{
    return detail::actor_ref_access_t::make (
      node_rid_t::from_string (
        zlink::routing_id_t::from (fence.target_node_routing_id).to_string ()),
      "late-result", fence.actor_id, fence.object_generation);
}
}

void actor_client_test_access_t::cache (actor_client_t &base,
                                        const protocol::actor_route_fence_t &fence)
{
    auto &client = dynamic_cast<actor_client_impl_t &> (base);
    auto actor = test_actor_route (fence);
    std::lock_guard lock (client._route_cache_gate);
    client._route_cache.insert_or_assign (
      fence.actor_id, actor_client_impl_t::cached_actor_t{{actor,
                                                           actor,
                                                           fence.target_node_generation,
                                                           "spot",
                                                           "mesh",
                                                           fence.authority_owner_generation,
                                                           fence.owner_lease_generation,
                                                           {}},
                                                          std::chrono::steady_clock::now ()
                                                            + std::chrono::hours (1)});
}

bool actor_client_test_access_t::cached (actor_client_t &base, const std::string &actor_id)
{
    auto &client = dynamic_cast<actor_client_impl_t &> (base);
    std::lock_guard lock (client._route_cache_gate);
    return client._route_cache.contains (actor_id);
}

detail::request_receiver_t<result_t<messaging::message_parts_t>>
actor_client_test_access_t::prepare (actor_client_t &base,
                                     const protocol::actor_route_fence_t &fence,
                                     detail::request_receiver_t<result_t<message_t>> receiver)
{
    auto &client = dynamic_cast<actor_client_impl_t &> (base);
    auto actor = test_actor_route (fence);
    return client.prepare_request_receiver ({actor,
                                             actor,
                                             fence.target_node_generation,
                                             "spot",
                                             "mesh",
                                             fence.authority_owner_generation,
                                             fence.owner_lease_generation,
                                             {}},
                                            std::move (receiver));
}

std::shared_ptr<actor_client_t>
make_actor_client (live_location_reader_t &store,
                   serializer_registry_t &serializers,
                   std::vector<std::shared_ptr<detail::mesh_node_runtime_t>> mesh_nodes,
                   std::shared_ptr<actor_location_observer_t> actor_locations,
                   location_options_t options,

                   std::function<void ()> admit_call)
{
    return std::make_shared<actor_client_impl_t> (store, serializers, std::move (mesh_nodes),
                                                  std::move (actor_locations), std::move (options),
                                                  std::move (admit_call));
}

task_t<void> send_to_actor_ref (actor_client_t &client,
                                actor_ref_t actor,
                                std::string packet_name,
                                message_t message)
{
    auto *implementation = dynamic_cast<actor_client_impl_t *> (&client);
    if (!implementation) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                   "explicit Actor route requires the framework Actor client"));
    }
    return implementation->send_to_ref (std::move (actor), std::move (packet_name),
                                        std::move (message));
}

task_t<message_t> request_to_actor_ref (actor_client_t &client,
                                        actor_ref_t actor,
                                        std::string packet_name,
                                        message_t message,
                                        std::chrono::milliseconds timeout)
{
    auto *implementation = dynamic_cast<actor_client_impl_t *> (&client);
    if (!implementation) {
        return task_t<message_t> (result_t<message_t>::failure (
          framework_error_kind_t::invalid_operation,
          "explicit Actor route requires the framework Actor client"));
    }
    return implementation->request_to_ref (std::move (actor), std::move (packet_name),
                                           std::move (message), timeout);
}

} // namespace zlink::framework::runtime
