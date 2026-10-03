/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_record_fields.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef ZLINK_SPOT_CLOSE_CONFORMANCE_PATH
#error "Spot Close conformance fixture path is required"
#endif

namespace
{
namespace zf = zlink::framework;
using namespace std::chrono_literals;
constexpr const char *mesh = "reincarnate-conformance-mesh";
constexpr const char *node = "reincarnate-conformance-node";
constexpr const char *spot_id = "reincarnate-conformance-spot";
constexpr const char *stable_type = "reincarnate-conformance";
constexpr auto request_timeout = 3s;
constexpr std::size_t runtime_observer_capacity = 1;

enum class branch_t
{
    replay,
    initialization_failure,
    draining,
    relocating,
    cold_activation_close,
    asynchronous_initialization,
    asynchronous_initialization_failure
};

bool initializer_is_held (branch_t branch)
{
    return branch == branch_t::asynchronous_initialization
           || branch == branch_t::asynchronous_initialization_failure;
}

bool initializer_fails (branch_t branch)
{
    return branch == branch_t::initialization_failure
           || branch == branch_t::asynchronous_initialization_failure;
}

struct warm_request_t
{
    static constexpr const char *packet_name = "reincarnate-warm";
};
struct close_request_t
{
    static constexpr const char *packet_name = "reincarnate-close";
};
struct intent_request_t
{
    static constexpr const char *packet_name = "reincarnate-intent";
};
struct intent_send_t
{
    static constexpr const char *packet_name = "reincarnate-intent-send";
};
struct followup_intent_request_t
{
    static constexpr const char *packet_name = "reincarnate-followup-intent";
};
struct no_intent_request_t
{
    static constexpr const char *packet_name = "reincarnate-no-intent";
};
struct reply_t
{
    static constexpr const char *packet_name = "reincarnate-reply";
    std::uint64_t generation = 0;
    std::string owner_node;
    int restored_value = 0;
};

#define EMPTY_REQUEST_JSON(Type)                                                                   \
    void to_json (nlohmann::json &json, const Type &)                                              \
    {                                                                                              \
        json = nlohmann::json::object ();                                                          \
    }                                                                                              \
    void from_json (const nlohmann::json &, Type &)                                                \
    {                                                                                              \
    }
EMPTY_REQUEST_JSON (warm_request_t)
EMPTY_REQUEST_JSON (close_request_t)
EMPTY_REQUEST_JSON (intent_request_t)
EMPTY_REQUEST_JSON (intent_send_t)
EMPTY_REQUEST_JSON (followup_intent_request_t)
EMPTY_REQUEST_JSON (no_intent_request_t)
#undef EMPTY_REQUEST_JSON
void to_json (nlohmann::json &json, const reply_t &reply)
{
    json = {{"generation", reply.generation},
            {"ownerNode", reply.owner_node},
            {"restoredValue", reply.restored_value}};
}
void from_json (const nlohmann::json &json, reply_t &reply)
{
    reply.generation = json.at ("generation").get<std::uint64_t> ();
    reply.owner_node = json.at ("ownerNode").get<std::string> ();
    reply.restored_value = json.at ("restoredValue").get<int> ();
}

struct evidence_t
{
    explicit evidence_t (branch_t branch) : branch (branch) {}
    branch_t branch;
    bool relocation_sealed = false;
    bool complete_relocation = false;
    bool phase_published_before_drain_flag = false;
    std::shared_ptr<zf::detail::spot_context_state_t> activation;
    std::optional<std::promise<void>> send_arrived{std::in_place};
    std::promise<void> send_terminated;
    std::promise<void> relocation_readiness_held;
    std::vector<std::string> send_diagnostics;
    std::vector<std::string> send_error_types;
    std::vector<std::string> send_surfaces;
    std::vector<std::string> send_reasons;
    std::vector<zf::framework_error_kind_t> send_error_kinds;
    std::mutex mutex;
    std::promise<void> closing_entered;
    zf::task_completion_source_t<void> closing_release;
    std::promise<void> journal_clear_entered;
    zf::task_completion_source_t<void> journal_clear_release;
    std::promise<void> initializer_entered;
    zf::task_completion_source_t<void> initializer_release;
    std::uint64_t initializing_generation = 0;
    std::optional<std::promise<void>> intent_arrived{std::in_place};
    std::optional<std::promise<void>> followup_arrived{std::in_place};
    std::optional<std::promise<void>> draining_observed{std::in_place};
    std::string intent_flow;
    std::optional<zf::task_t<bool>> close_completion;
    std::vector<std::string> order;
    std::vector<std::uint64_t> business_generations;
    std::vector<std::string> business_packets;
    std::map<std::string, int> terminals;
    std::vector<zf::authority_snapshot_t> committed;
    std::vector<std::uint64_t> deleted_generations;
    std::optional<zf::authority_snapshot_t> last_authority;
    std::string authority_provider_key;
    int factory_calls = 0;
    int factory_calls_at_close = 0;
    int closing_calls = 0;
    int missing_placement_calls = 0;
    // Source-of-record differs from the old in-memory projection after Close.
    int durable_value = 7;
};

// The opaque Store SPI records successful authority mutations by querying the
// existing repository decoder against its inner provider; no record codec is duplicated.
class observed_store_t final : public zf::location_store_t
{
  public:
    explicit observed_store_t (std::shared_ptr<evidence_t> evidence) :
        _evidence (std::move (evidence)),
        _inner (std::make_shared<zf::runtime::in_memory_location_store_t> ())
    {
    }

    zf::task_t<zf::store_read_result_t> read (zf::store_key_t key) override
    {
        auto result = co_await _inner->read (key);
        const auto *found = std::get_if<zf::store_found_t> (&result);
        if (found) {
            const auto *begin = reinterpret_cast<const char *> (found->value.bytes.data ());
            const auto json =
              nlohmann::json::parse (begin, begin + found->value.bytes.size (), nullptr, false);
            if (!json.is_discarded ()
                && json.contains (zf::runtime::location_record_fields::objectGeneration)
                && json.contains (zf::runtime::location_record_fields::payload)) {
                const auto payload =
                  zf::runtime::decode_instance_spot_authority_payload (zf::runtime::base64_decode (
                    json.at (zf::runtime::location_record_fields::payload).get<std::string> ()));
                if (payload && payload->stable_type == stable_type && payload->spot_id == spot_id) {
                    std::lock_guard lock (_evidence->mutex);
                    _evidence->authority_provider_key = key.value;
                }
            }
        }
        co_return result;
    }

    zf::task_t<zf::store_write_result_t> write (zf::store_write_request_t request) override
    {
        {
            std::lock_guard lock (_evidence->mutex);
            if (_evidence->closing_calls > 0)
                for (const auto &condition : request.conditions)
                    if (const auto *missing =
                          std::get_if<zf::store_missing_condition_t> (&condition);
                        missing && missing->key.value == _evidence->authority_provider_key)
                        ++_evidence->missing_placement_calls;
        }
        bool clears_terminal_journal = false;
        if (_evidence->branch == branch_t::cold_activation_close) {
            std::optional<zf::runtime::instance_spot_authority_payload_t> previous;
            {
                std::lock_guard lock (_evidence->mutex);
                if (_evidence->last_authority)
                    previous = zf::runtime::decode_instance_spot_authority_payload (
                      _evidence->last_authority->payload);
            }
            if (previous && previous->activation_recovery
                && previous->activation_recovery->replay_cursor
                     == previous->activation_recovery->inbox_sequence) {
                for (const auto &mutation : request.mutations) {
                    const auto *put = std::get_if<zf::store_put_t> (&mutation);
                    if (!put)
                        continue;
                    const auto *begin = reinterpret_cast<const char *> (put->bytes.data ());
                    const auto record =
                      nlohmann::json::parse (begin, begin + put->bytes.size (), nullptr, false);
                    if (record.is_discarded ()
                        || !record.contains (zf::runtime::location_record_fields::objectGeneration)
                        || !record.contains (zf::runtime::location_record_fields::payload))
                        continue;
                    const auto payload = zf::runtime::decode_instance_spot_authority_payload (
                      zf::runtime::base64_decode (
                        record.at (zf::runtime::location_record_fields::payload)
                          .get<std::string> ()));
                    clears_terminal_journal =
                      payload && payload->spot_id == spot_id
                      && payload->state == zf::runtime::instance_spot_authority_state_t::ready
                      && !payload->activation_recovery;
                    if (clears_terminal_journal)
                        break;
                }
            }
        }
        if (clears_terminal_journal) {
            _evidence->journal_clear_entered.set_value ();
            co_await _evidence->journal_clear_release.task ();
        }
        auto result = co_await _inner->write (std::move (request));
        if (std::holds_alternative<zf::store_write_applied_t> (result)) {
            auto observed =
              std::make_shared<zf::task_completion_source_t<zf::authority_read_result_t>> ();
            auto observed_task = observed->task ();
            if (!zf::detail::submit_blocking_call ([inner = _inner, observed] {
                    try {
                        zf::runtime::provider_location_repository_t repository (*inner);
                        observed->complete (
                          repository.read_authority (zf::runtime::spot_authority_key (spot_id))
                            .result ());
                    }
                    catch (const zf::framework_exception_t &error) {
                        observed->complete (
                          zf::detail::result_access_t::failure<zf::authority_read_result_t> (
                            error));
                    }
                    catch (const std::exception &error) {
                        observed->complete (zf::result_t<zf::authority_read_result_t>::failure (
                          zf::framework_error_kind_t::internal_failure, error.what ()));
                    }
                    catch (...) {
                        observed->complete (zf::result_t<zf::authority_read_result_t>::failure (
                          zf::framework_error_kind_t::internal_failure,
                          "Store observation failed"));
                    }
                })) {
                observed->complete (zf::result_t<zf::authority_read_result_t>::failure (
                  zf::framework_error_kind_t::shutting_down,
                  "Store observation executor is stopping"));
            }
            const auto read = co_await observed_task;
            const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&read);
            std::lock_guard lock (_evidence->mutex);
            if (clears_terminal_journal)
                _evidence->order.push_back ("activationJournalCleared");
            const auto &previous = _evidence->last_authority;
            if (snapshot && (!previous || previous->store_version != snapshot->store_version)) {
                if (previous && previous->object_generation != snapshot->object_generation)
                    _evidence->order.push_back ("authorityReincarnated");
                _evidence->committed.push_back (*snapshot);
                _evidence->last_authority = *snapshot;
            } else if (!snapshot && previous) {
                const auto reincarnated =
                  std::find (_evidence->order.begin (), _evidence->order.end (),
                             "authorityReincarnated")
                  != _evidence->order.end ();
                _evidence->order.push_back (reincarnated ? "newGenerationDeleted"
                                                         : "authorityReleased");
                _evidence->deleted_generations.push_back (previous->object_generation);
                _evidence->last_authority.reset ();
            }
        }
        co_return result;
    }
    zf::task_t<zf::store_scan_result_t> scan (zf::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }
    zf::task_t<zf::authority_read_result_t> inspect_authority ()
    {
        zf::runtime::provider_location_repository_t repository (*_inner);
        co_return co_await repository.read_authority (zf::runtime::spot_authority_key (spot_id));
    }

  private:
    std::shared_ptr<evidence_t> _evidence;
    std::shared_ptr<zf::runtime::in_memory_location_store_t> _inner;
};

class reincarnating_spot_t final : public zf::instance_spot_t
{
  public:
    reincarnating_spot_t (zf::instance_spot_context_t context,
                          std::shared_ptr<evidence_t> evidence) :
        _context (std::move (context)), _evidence (std::move (evidence))
    {
        _evidence->activation = zf::detail::spot_context_access_t::state (_context);
    }
    zf::instance_spot_context_t &context () noexcept override { return _context; }
    const zf::instance_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ()
          .add_handler<&reincarnating_spot_t::warm> ()
          .add_handler<&reincarnating_spot_t::close> ()
          .add_handler<&reincarnating_spot_t::intent> ()
          .add_handler<&reincarnating_spot_t::send_intent> ()
          .add_handler<&reincarnating_spot_t::followup> ()
          .add_handler<&reincarnating_spot_t::without_intent> ();
    }
    zf::task_t<void> on_initialize () override
    {
        bool recreating;
        {
            std::lock_guard lock (_evidence->mutex);
            recreating = _evidence->closing_calls != 0;
            if (recreating && initializer_is_held (_evidence->branch)) {
                _evidence->initializing_generation = _context.object_generation ();
                _evidence->initializer_entered.set_value ();
            }
        }
        if (recreating && initializer_is_held (_evidence->branch))
            co_await _evidence->initializer_release.task ();
        if (recreating && _evidence->branch == branch_t::initialization_failure)
            throw std::runtime_error ("conformance initializer failure");
        {
            std::lock_guard lock (_evidence->mutex);
            _projection = _evidence->durable_value;
            if (recreating) {
                _evidence->order.push_back ("newIncarnationInitialized");
                _evidence->order.push_back ("storedStateRestored");
            }
        }
        co_return;
    }
    zf::task_t<void> on_closing (const zf::spot_closing_context_t &context,
                                 std::stop_token) override
    {
        if (context.reason != zf::spot_close_reason_t::explicit_close)
            co_return;
        {
            std::lock_guard lock (_evidence->mutex);
            if (_evidence->committed.empty ())
                throw std::runtime_error ("OnClosing has no initial Store incarnation evidence");
            if (_context.object_generation () != _evidence->committed.front ().object_generation)
                co_return;
            ++_evidence->closing_calls;
            _evidence->order.push_back ("onClosing");
            _evidence->factory_calls_at_close = _evidence->factory_calls;
            _evidence->closing_entered.set_value ();
        }
        co_await _evidence->closing_release.task ();
    }
    reply_t warm (const warm_request_t &) { return reply (); }
    reply_t close (const close_request_t &)
    {
        auto operation = _context.close ();
        std::lock_guard lock (_evidence->mutex);
        _evidence->close_completion.emplace (std::move (operation));
        return reply ();
    }
    reply_t intent (const intent_request_t &)
    {
        return business_reply (intent_request_t::packet_name);
    }
    void send_intent (const intent_send_t &) { (void) business_reply (intent_send_t::packet_name); }
    reply_t followup (const followup_intent_request_t &)
    {
        return business_reply (followup_intent_request_t::packet_name);
    }
    reply_t without_intent (const no_intent_request_t &)
    {
        return business_reply (no_intent_request_t::packet_name);
    }

  private:
    reply_t reply () const
    {
        return {_context.object_generation (), std::string (_context.node_rid ().value ()),
                _projection};
    }
    reply_t business_reply (std::string packet)
    {
        std::lock_guard lock (_evidence->mutex);
        _evidence->business_generations.push_back (_context.object_generation ());
        _evidence->business_packets.push_back (std::move (packet));
        _evidence->order.push_back (_evidence->branch == branch_t::cold_activation_close
                                      ? "clearHeldApplicationMarker"
                                      : "pendingIntentMessagesExecuted");
        return reply ();
    }
    zf::instance_spot_context_t _context;
    std::shared_ptr<evidence_t> _evidence;
    int _projection = 0;
};

// The terminal observer records request completion before caller continuation scheduling.
zf::task_t<reply_t> count_completion (zf::task_t<reply_t> task,
                                      std::shared_ptr<evidence_t> evidence,
                                      std::string packet)
{
    zf::detail::observe_task_terminal (
      task, [evidence, packet] (const zf::result_t<reply_t> &result) {
          std::lock_guard lock (evidence->mutex);
          ++evidence->terminals[packet];
          if (!result) {
              if ((packet == intent_request_t::packet_name
                   || packet == followup_intent_request_t::packet_name)
                  && initializer_fails (evidence->branch))
                  evidence->order.push_back ("pendingMessagesTypedFailure");
              if (packet == intent_request_t::packet_name
                  && (evidence->branch == branch_t::draining
                      || evidence->branch == branch_t::relocating))
                  evidence->order.push_back ("pendingMessagesTerminated");
          }
      });
    try {
        auto reply = co_await task;
        co_return reply;
    }
    catch (const zf::framework_exception_t &error) {
        co_return zf::result_t<reply_t>::failure (error.kind (), error.what ());
    }
}

void require_ready (std::future<void> &future, const char *what)
{
    if (future.wait_for (request_timeout) != std::future_status::ready)
        throw std::runtime_error (what);
    future.get ();
}

// Holds the public host relocation operation at its existing readiness boundary.
class relocation_hold_spot_t final : public zf::spot_t<zf::actor_t>
{
  public:
    explicit relocation_hold_spot_t (zf::spot_context_t context) : _context (std::move (context)) {}
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&relocation_hold_spot_t::hold> ();
    }
    reply_t hold (const warm_request_t &) { return {}; }
    zf::task_t<zf::spot_create_response_t> on_create (const zf::message_t &) override
    {
        co_return zf::spot_create_response_t::accept ();
    }
    zf::task_t<void> on_initialize () override { co_return; }
    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        co_return zf::spot_actor_join_result_t::reject ();
    }
    zf::task_t<void> on_actor_joined (zf::actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (zf::actor_t &) override { co_return; }

  private:
    zf::spot_context_t _context;
};
class relocation_hold_adapter_t final : public zf::spot_relocation_adapter_t<relocation_hold_spot_t>
{
  public:
    zf::task_t<std::vector<std::byte>> capture (relocation_hold_spot_t &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    zf::task_t<void>
    restore (relocation_hold_spot_t &, std::vector<std::byte>, std::stop_token) override
    {
        co_return;
    }
};

class exercise_t final : public zf::hosted_service_t
{
  public:
    exercise_t (zf::app_t &app,
                std::shared_ptr<evidence_t> evidence,
                std::shared_ptr<observed_store_t> store) :
        _app (&app), _evidence (std::move (evidence)), _store (std::move (store))
    {
    }
    zf::task_t<void> start (zf::service_provider_t &services) override
    {
        _worker = std::thread ([this, services] () mutable {
            try {
                auto serving = std::make_shared<std::promise<void>> ();
                auto observed = serving->get_future ();
                auto completed = std::make_shared<std::atomic_bool> (false);
                auto &runtime = services.get_required<zf::framework_runtime_t> ();
                auto observation = runtime.observe (
                  runtime_observer_capacity,
                  [serving, completed] (
                    const zf::observed_status_t<zf::framework_runtime_status_t> &status) {
                      if (status.status.state == zf::framework_runtime_state_t::serving
                          && !completed->exchange (true))
                          serving->set_value ();
                  });
                require_ready (observed, "source host did not reach Serving");
                observation->close ();
                run (services);
            }
            catch (const std::exception &error) {
                failure = error.what ();
                _evidence->journal_clear_release.complete (zf::result_t<void>::success ());
                _evidence->closing_release.complete (zf::result_t<void>::success ());
                _evidence->initializer_release.complete (zf::result_t<void>::failure (
                  zf::framework_error_kind_t::internal_failure, "conformance initializer aborted"));
            }
            if (_app->runtime_state () != zf::framework_runtime_state_t::draining)
                _app->stop ();
        });
        co_return;
    }
    void stop () noexcept override
    {
        if (_worker.joinable ())
            _worker.join ();
    }
    std::string failure;
    std::optional<reply_t> original_reply;
    std::optional<zf::result_t<reply_t>> intent_result;
    std::optional<zf::result_t<reply_t>> followup_result;
    std::optional<zf::result_t<reply_t>> no_intent_result;
    std::optional<zf::authority_snapshot_t> original_authority;
    std::optional<zf::authority_snapshot_t> final_authority;
    std::optional<zf::placement_capacity_t> original_capacity;
    std::optional<zf::placement_capacity_t> final_capacity;
    bool close_pending_at_clear = false;
    int closing_calls_at_clear = 0;
    bool journal_present_at_clear = false;
    std::optional<reply_t> clear_marker_reply;
    std::optional<zf::result_t<bool>> close_result;
    bool close_pending_during_initializer = false;
    std::size_t handlers_during_initializer = 0;
    std::optional<zf::authority_snapshot_t> authority_during_initializer;
    int factories_before_followup = 0;
    int factories_after_followup = 0;

  private:
    void run_cold_close (zf::service_provider_t &services)
    {
        auto route = _app->advanced ().zlink ().route_client (
          services.get_required<zf::serializer_registry_t> ());
        auto clear_entered = _evidence->journal_clear_entered.get_future ();
        auto closing = _evidence->closing_entered.get_future ();
        auto activation = std::async (std::launch::async, [&] {
            auto task = route.request_to_spot (zf::spot_id_t (spot_id), close_request_t{})
                          .instance_spot (stable_type)
                          .timeout (request_timeout)
                          .async<reply_t> ();
            return count_completion (std::move (task), _evidence, close_request_t::packet_name)
              .result ();
        });
        try {
            require_ready (clear_entered,
                           "activation terminal journal clear did not enter the Store");
            const auto held = _store->inspect_authority ().result ().value ();
            const auto &authority = std::get<zf::authority_snapshot_t> (held);
            original_authority = authority;
            const auto payload =
              zf::runtime::decode_instance_spot_authority_payload (authority.payload);
            journal_present_at_clear = payload && payload->activation_recovery
                                       && payload->activation_recovery->replay_cursor
                                            == payload->activation_recovery->inbox_sequence;
            // Close was enqueued by the cold handler before this application marker.
            // Lifecycle priority makes an old, unguarded Close run before the marker;
            // the deferred journal barrier permits only the application marker here.
            auto marker_task =
              route.request_to_spot (zf::spot_id_t (spot_id), no_intent_request_t{})
                .timeout (request_timeout)
                .async<reply_t> ();
            const auto marker = count_completion (std::move (marker_task), _evidence,
                                                  no_intent_request_t::packet_name)
                                  .result ();
            if (!marker)
                throw std::runtime_error ("application marker failed while journal clear was held");
            clear_marker_reply = marker.value ();
            {
                std::lock_guard lock (_evidence->mutex);
                closing_calls_at_clear = _evidence->closing_calls;
                close_pending_at_clear =
                  _evidence->close_completion && !_evidence->close_completion->await_ready ();
            }
            _evidence->journal_clear_release.complete (zf::result_t<void>::success ());
            const auto result = activation.get ();
            if (!result)
                throw std::runtime_error ("original cold activation terminal did not succeed");
            original_reply = result.value ();
            require_ready (closing, "cold activation Close disappeared before OnClosing");
            _evidence->closing_release.complete (zf::result_t<void>::success ());
            close_result.emplace (_evidence->close_completion->result ());
            const auto final = _store->inspect_authority ().result ().value ();
            if (const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&final))
                final_authority = *snapshot;
        }
        catch (...) {
            _evidence->journal_clear_release.complete (zf::result_t<void>::success ());
            _evidence->closing_release.complete (zf::result_t<void>::success ());
            throw;
        }
    }

    void run (zf::service_provider_t &services)
    {
        if (_evidence->branch == branch_t::cold_activation_close) {
            run_cold_close (services);
            return;
        }
        auto route = _app->advanced ().zlink ().route_client (
          services.get_required<zf::serializer_registry_t> ());
        if (_evidence->branch == branch_t::relocating) {
            if (!_evidence->complete_relocation) {
                auto &manager = services.get_required<zf::spot_manager_t> ();
                auto created = manager
                                 .get_or_create (zf::spot_id_t ("reincarnate-relocation-hold"),
                                                 "reincarnate-relocation-hold")
                                 .timeout (request_timeout)
                                 .async ()
                                 .result ();
                if (!created)
                    throw std::runtime_error (created.error ()
                                                ? created.error ()->what ()
                                                : "relocation readiness Spot was not created");
                if (created.value ().spot.node_rid ().value () != node)
                    throw std::runtime_error ("relocation readiness Spot was not owned by source");
                auto held = route
                              .request_to_spot (zf::spot_id_t ("reincarnate-relocation-hold"),
                                                warm_request_t{})
                              .timeout (request_timeout)
                              .async<reply_t> ()
                              .result ();
                if (!held)
                    throw std::runtime_error ("public relocation readiness did not defer");
            }
            _evidence->relocation_readiness_held.set_value ();
            auto &meshes = services.get_required<zf::route_mesh_runtime_t> ();
            auto ready = std::make_shared<std::promise<void>> ();
            auto observed = ready->get_future ();
            auto completed = std::make_shared<std::atomic_bool> (false);
            auto observation = meshes.observe (
              mesh, runtime_observer_capacity,
              [ready, completed] (const zf::observed_status_t<zf::mesh_node_snapshot_t> &status) {
                  if (status.status.ready_peer_count != 0 && !completed->exchange (true))
                      ready->set_value ();
              });
            require_ready (observed, "target host peer did not become Ready");
            observation->close ();
        }
        const auto warmed = route.request_to_spot (zf::spot_id_t (spot_id), warm_request_t{})
                              .instance_spot (stable_type)
                              .timeout (request_timeout)
                              .async<reply_t> ()
                              .result ();
        if (!warmed)
            throw std::runtime_error ("initial public Instance activation failed");
        original_reply = warmed.value ();
        auto &locations = services.get_required<zf::location_repository_t> ();
        const auto initial =
          locations.read_authority (zf::runtime::spot_authority_key (spot_id)).result ().value ();
        original_authority = std::get<zf::authority_snapshot_t> (initial);
        const auto original_nodes = locations.list_mesh_nodes (mesh).result ().value ();
        for (const auto &descriptor : original_nodes.items)
            if (descriptor.rid.to_string () == original_reply->owner_node)
                original_capacity = descriptor.capacity;
        if (!original_capacity)
            throw std::runtime_error ("original owner capacity is not observable");
        auto closing = _evidence->closing_entered.get_future ();
        auto initializer_entered = _evidence->initializer_entered.get_future ();
        std::future<void> intent_arrived;
        {
            std::lock_guard lock (_evidence->mutex);
            _evidence->order.clear ();
            intent_arrived = _evidence->intent_arrived->get_future ();
        }
        const auto accepted = route.request_to_spot (zf::spot_id_t (spot_id), close_request_t{})
                                .timeout (request_timeout)
                                .async<reply_t> ()
                                .result ();
        if (!accepted)
            throw std::runtime_error ("application Close trigger failed");
        require_ready (closing, "public OnClosing did not start");
        {
            std::lock_guard lock (_evidence->mutex);
            ++_evidence->durable_value;
        }
        auto intent = std::async (std::launch::async, [&] {
            auto task = route.request_to_spot (zf::spot_id_t (spot_id), intent_request_t{})
                          .instance_spot (stable_type)
                          .timeout (request_timeout)
                          .async<reply_t> ();
            return count_completion (std::move (task), _evidence, intent_request_t::packet_name)
              .result ();
        });
        auto no_intent = std::async (std::launch::async, [&] {
            auto task = route.request_to_spot (zf::spot_id_t (spot_id), no_intent_request_t{})
                          .timeout (request_timeout)
                          .async<reply_t> ();
            return count_completion (std::move (task), _evidence, no_intent_request_t::packet_name)
              .result ();
        });
        std::future<zf::result_t<reply_t>> followup;
        try {
            require_ready (intent_arrived, "intent request did not reach the Closing owner");
            const auto held = _store->inspect_authority ().result ().value ();
            const auto &fence = std::get<zf::authority_snapshot_t> (held);
            const auto payload = zf::runtime::decode_instance_closing_state (fence.payload);
            if (!payload || payload->stable_type != stable_type || payload->spot_id != spot_id
                || payload->object_generation != fence.object_generation
                || payload->authority_owner_generation != fence.authority_owner_generation
                || fence.object_generation != original_authority->object_generation
                || fence.owner.owner_id != original_authority->owner.owner_id
                || fence.owner.lease_generation != original_authority->owner.lease_generation
                || fence.allocation.target.node_rid.value ()
                     != original_authority->allocation.target.node_rid.value ())
                throw std::runtime_error (
                  "intent admission did not preserve the Closing owner fence");
            no_intent_result.emplace (no_intent.get ());
            if (_evidence->branch == branch_t::draining
                || _evidence->branch == branch_t::relocating) {
                auto arrived = _evidence->send_arrived->get_future ();
                auto sent = route.send_to_spot (zf::spot_id_t (spot_id), intent_send_t{})
                              .instance_spot (stable_type)
                              .async ()
                              .result ();
                if (!sent)
                    throw std::runtime_error ("public intent send was not accepted");
                require_ready (arrived, "intent send did not reach the Closing owner");
            }
            if (_evidence->branch == branch_t::draining) {
                auto &runtime = services.get_required<zf::framework_runtime_t> ();
                auto drained = _evidence->draining_observed->get_future ();
                auto observation = runtime.observe (
                  runtime_observer_capacity,
                  [evidence = _evidence] (
                    const zf::observed_status_t<zf::framework_runtime_status_t> &status) {
                      if (status.status.state != zf::framework_runtime_state_t::draining)
                          return;
                      std::lock_guard lock (evidence->mutex);
                      if (evidence->draining_observed) {
                          evidence->draining_observed->set_value ();
                          evidence->draining_observed.reset ();
                      }
                  });
                (void) _app->shutdown (request_timeout);
                require_ready (drained, "public runtime observer did not report Draining");
                observation->close ();
                if (_evidence->phase_published_before_drain_flag) {
                    // Reproduce the phase-first publication snapshot at the Close owner.
                    auto owner = _evidence->activation->lane_owner.lock ();
                    if (!owner)
                        owner = _evidence->activation->node;
                    owner->lane
                      .run ([&] { owner->drain_flag = std::make_shared<std::atomic_bool> (false); })
                      .get ();
                }
            }
            std::optional<zf::task_t<zf::relocation_result_t>> relocation;
            if (_evidence->branch == branch_t::relocating) {
                relocation.emplace (
                  _app->relocate ({.mode = zf::relocation_mode_t::planned_maintenance,
                                   .deadline = request_timeout}));
                if (!_evidence->complete_relocation
                    && _app->runtime_state () != zf::framework_runtime_state_t::relocating)
                    throw std::runtime_error ("public host did not enter Relocating");
                if (_evidence->complete_relocation) {
                    if (relocation->result ().value ().outcome
                          != zf::relocation_outcome_t::relocated
                        || _app->runtime_state () != zf::framework_runtime_state_t::relocated)
                        throw std::runtime_error (
                          "public host relocation did not complete before Close release");
                }
            }
            if (_evidence->branch == branch_t::draining
                || _evidence->branch == branch_t::relocating) {
                const auto state = _evidence->activation;
                const auto owner = state->lane_owner.lock ();
                if (_evidence->relocation_sealed)
                    state->detach_application_instance (false,
                                                        zf::spot_close_reason_t::relocation_out);
                const auto sealed =
                  owner->lane.run ([&] { return state->admission_sealed; }).get ();
                if (sealed != _evidence->relocation_sealed)
                    throw std::runtime_error (
                      "operational admission seal did not match fixture phase");
            }
            _evidence->closing_release.complete (zf::result_t<void>::success ());
            if (initializer_is_held (_evidence->branch)) {
                require_ready (initializer_entered, "new incarnation initializer did not start");
                const auto published = _store->inspect_authority ().result ().value ();
                const auto &new_authority = std::get<zf::authority_snapshot_t> (published);
                const auto payload =
                  zf::runtime::decode_instance_spot_authority_payload (new_authority.payload);
                if (!payload
                    || payload->state != zf::runtime::instance_spot_authority_state_t::ready)
                    throw std::runtime_error (
                      "new Ready authority was not published before initializer hold");
                std::future<void> joined;
                {
                    std::lock_guard lock (_evidence->mutex);
                    factories_before_followup = _evidence->factory_calls;
                    joined = _evidence->followup_arrived->get_future ();
                }
                followup = std::async (std::launch::async, [&] {
                    auto task =
                      route.request_to_spot (zf::spot_id_t (spot_id), followup_intent_request_t{})
                        .instance_spot (stable_type)
                        .timeout (request_timeout)
                        .async<reply_t> ();
                    return count_completion (std::move (task), _evidence,
                                             followup_intent_request_t::packet_name)
                      .result ();
                });
                require_ready (joined, "followup intent did not join the pending owner FIFO");
                {
                    std::lock_guard lock (_evidence->mutex);
                    close_pending_during_initializer =
                      _evidence->close_completion && !_evidence->close_completion->await_ready ();
                    handlers_during_initializer = _evidence->business_generations.size ();
                    factories_after_followup = _evidence->factory_calls;
                }
                const auto held = _store->inspect_authority ().result ().value ();
                authority_during_initializer = std::get<zf::authority_snapshot_t> (held);
                const auto completed =
                  initializer_fails (_evidence->branch)
                    ? zf::result_t<void>::failure (zf::framework_error_kind_t::internal_failure,
                                                   "conformance asynchronous initializer failure")
                    : zf::result_t<void>::success ();
                _evidence->initializer_release.complete (completed);
            }
        }
        catch (...) {
            _evidence->closing_release.complete (zf::result_t<void>::success ());
            _evidence->initializer_release.complete (zf::result_t<void>::failure (
              zf::framework_error_kind_t::internal_failure, "conformance initializer aborted"));
            if (intent.valid () && intent.wait_for (0ms) == std::future_status::ready)
                intent_result.emplace (intent.get ());
            throw;
        }
        intent_result.emplace (intent.get ());
        if (_evidence->branch == branch_t::relocating
            && _app->runtime_state ()
                 != (_evidence->complete_relocation ? zf::framework_runtime_state_t::relocated
                                                    : zf::framework_runtime_state_t::relocating))
            throw std::runtime_error ("host left Relocating before pending request terminal");
        if (_evidence->branch == branch_t::draining || _evidence->branch == branch_t::relocating) {
            auto terminated = _evidence->send_terminated.get_future ();
            require_ready (terminated, "pending intent send diagnostic did not complete");
        }
        if (followup.valid ())
            followup_result.emplace (followup.get ());
        const auto closed = _evidence->close_completion->result ();
        if (!initializer_fails (_evidence->branch) && (!closed || !closed.value ()))
            throw std::runtime_error ("Close did not complete successfully");
        const auto final = _store->inspect_authority ().result ().value ();
        if (const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&final))
            final_authority = *snapshot;
        if (_evidence->branch == branch_t::replay
            || _evidence->branch == branch_t::asynchronous_initialization) {
            const auto final_nodes = locations.list_mesh_nodes (mesh).result ().value ();
            for (const auto &descriptor : final_nodes.items)
                if (descriptor.rid.to_string () == original_reply->owner_node)
                    final_capacity = descriptor.capacity;
        }
    }
    zf::app_t *_app;
    std::thread _worker;
    std::shared_ptr<evidence_t> _evidence;
    std::shared_ptr<observed_store_t> _store;
};

const nlohmann::json &branch_fixture (const char *name)
{
    static const auto fixture = [] {
        std::ifstream stream (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
        if (!stream)
            throw std::runtime_error ("Close fixture could not be opened");
        return nlohmann::json::parse (stream);
    }();
    for (const auto &branch : fixture.at ("closeBranches"))
        if (branch.at ("name") == name)
            return branch;
    throw std::runtime_error ("Close branch is missing from fixture");
}

void configure_app (zf::app_t &app,
                    const std::shared_ptr<evidence_t> &evidence,
                    const std::shared_ptr<observed_store_t> &store,
                    const std::shared_ptr<zf::runtime::in_memory_relocation_store_t> &relocations,
                    std::string routing_id = node)
{
    app.logging ().use_file ("spot-reincarnate.flow").set_min_level (zf::log_level_t::debug);
    app.add_zlink_framework ([&] (zf::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (zf::message_flow_log_mode_t::detailed);
        zf::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options.configure_dispatch (),
          [evidence] (const zf::message_dispatch_error_event_t &event) {
              if (event.packet_name != intent_send_t::packet_name || !event.error_message
                  || event.message_kind != zf::dispatch_message_kind_t::send)
                  return;
              std::lock_guard lock (evidence->mutex);
              evidence->send_diagnostics.push_back (*event.error_message);
              evidence->send_error_types.push_back (event.error_type.value_or (""));
              evidence->send_surfaces.emplace_back (zf::detail::enum_name (event.surface));
              evidence->send_reasons.emplace_back (zf::detail::enum_name (event.reason));
              ASSERT_TRUE (event.exception);
              try {
                  std::rethrow_exception (event.exception);
              }
              catch (const zf::framework_exception_t &error) {
                  evidence->send_error_kinds.push_back (error.kind ());
              }
              if (evidence->send_diagnostics.size () == 1)
                  evidence->send_terminated.set_value ();
          });
        zf::detail::dispatch_options_access_t::set_observer_for_tests (
          options.configure_dispatch (), [evidence] (const zf::message_flow_event_t &event) {
              if (event.packet_name == intent_send_t::packet_name
                  && event.outcome == zf::message_flow_outcome_t::admitted
                  && event.detail_stage
                       == std::optional<std::string> ("invoke_erased.post_serial")) {
                  std::lock_guard lock (evidence->mutex);
                  if (evidence->send_arrived) {
                      evidence->send_arrived->set_value ();
                      evidence->send_arrived.reset ();
                  }
              }
              // This observer belongs to the only node that registers this packet.
              // post_serial/admitted is emitted only after its owner FIFO takes the work.
              if ((event.packet_name != std::optional<std::string> (intent_request_t::packet_name)
                   && event.packet_name
                        != std::optional<std::string> (followup_intent_request_t::packet_name))
                  || event.surface != zf::dispatch_error_surface_t::spot_actor
                  || event.outcome != zf::message_flow_outcome_t::admitted
                  || event.detail_stage != std::optional<std::string> ("invoke_erased.post_serial")
                  || (event.spot_id && *event.spot_id != spot_id) || !event.flow_id)
                  return;
              std::lock_guard lock (evidence->mutex);
              if (event.packet_name == intent_request_t::packet_name && evidence->intent_arrived) {
                  evidence->intent_flow = *event.flow_id;
                  evidence->intent_arrived->set_value ();
                  evidence->intent_arrived.reset ();
              } else if (event.packet_name == followup_intent_request_t::packet_name
                         && evidence->followup_arrived) {
                  evidence->followup_arrived->set_value ();
                  evidence->followup_arrived.reset ();
              }
          });
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        options.add_route_mesh (mesh)
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (routing_id))
          .listen ("tcp://127.0.0.1:0")
          .add_instance_spot_factory<reincarnating_spot_t> (
            stable_type,
            [evidence] (zf::instance_spot_context_t context) {
                {
                    std::lock_guard lock (evidence->mutex);
                    ++evidence->factory_calls;
                }
                return std::make_shared<reincarnating_spot_t> (std::move (context), evidence);
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_spot_factory<relocation_hold_spot_t> (
            "reincarnate-relocation-hold",
            [] (zf::spot_context_t context) {
                return std::make_shared<relocation_hold_spot_t> (std::move (context));
            },
            [] (auto &factory) {
                factory.set_execution_mode (zf::user_spot_execution_mode_t::spot_wide);
                factory.set_relocation_coordination_mode (
                  zf::spot_relocation_coordination_mode_t::application_signaled);
                factory.template preserve_state_with<relocation_hold_adapter_t> ();
            });
    });
}

void check_branch (branch_t kind,
                   const char *name,
                   bool sealed = false,
                   bool complete_relocation = false,
                   bool phase_published_before_drain_flag = false)
{
    const auto &branch = branch_fixture (name);
    const auto &given = branch.at ("given");
    const auto &expect = branch.at ("expect");
    ASSERT_TRUE (given.at ("pendingIntent").get<bool> ());
    if (initializer_fails (kind))
        ASSERT_EQ ("fails", given.at ("initialization"));
    if (kind == branch_t::draining || kind == branch_t::relocating)
        ASSERT_NE (given.at ("host").end (),
                   std::find (given.at ("host").begin (), given.at ("host").end (),
                              kind == branch_t::draining ? "Draining" : "Relocating"));
    else
        ASSERT_EQ ("Serving", given.at ("host"));
    auto evidence = std::make_shared<evidence_t> (kind);
    evidence->relocation_sealed = sealed;
    evidence->complete_relocation = complete_relocation;
    evidence->phase_published_before_drain_flag = phase_published_before_drain_flag;
    auto store = std::make_shared<observed_store_t> (evidence);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    configure_app (app, evidence, store, relocations);
    std::unique_ptr<zf::app_t> target;
    std::thread target_thread;
    if (kind == branch_t::relocating) {
        target = std::make_unique<zf::app_t> (zf::app_t::create ());
        configure_app (*target, evidence, store, relocations, "reincarnate-relocation-target");
        auto held = evidence->relocation_readiness_held.get_future ();
        target_thread = std::thread ([&, held = std::move (held)] () mutable {
            if (held.wait_for (request_timeout) == std::future_status::ready)
                (void) target->run (0, nullptr);
        });
    }
    auto runner = std::make_unique<exercise_t> (app, evidence, store);
    auto *exercise = runner.get ();
    app.add_hosted_service (std::move (runner));
    (void) app.run (0, nullptr);
    if (target) {
        target->stop ();
        target_thread.join ();
    }
    ASSERT_TRUE (exercise->failure.empty ())
      << exercise->failure
      << (exercise->intent_result && !*exercise->intent_result && exercise->intent_result->error ()
            ? std::string ("; intent terminal: ") + exercise->intent_result->error ()->what ()
            : std::string{});
    ASSERT_TRUE (exercise->original_reply);
    ASSERT_TRUE (exercise->original_authority);
    ASSERT_TRUE (exercise->intent_result);
    ASSERT_TRUE (exercise->no_intent_result);
    EXPECT_FALSE (*exercise->no_intent_result);
    EXPECT_EQ (zf::framework_error_kind_t::not_found, exercise->no_intent_result->error_kind ());
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (1, evidence->closing_calls);
    if (initializer_is_held (kind)) {
        EXPECT_TRUE (exercise->close_pending_during_initializer);
        EXPECT_EQ (0u, exercise->handlers_during_initializer);
        ASSERT_TRUE (exercise->authority_during_initializer);
        EXPECT_EQ (evidence->initializing_generation,
                   exercise->authority_during_initializer->object_generation);
        EXPECT_NE (exercise->original_authority->object_generation,
                   exercise->authority_during_initializer->object_generation);
        EXPECT_EQ (exercise->original_authority->owner.owner_id,
                   exercise->authority_during_initializer->owner.owner_id);
        EXPECT_EQ (exercise->original_authority->owner.lease_generation,
                   exercise->authority_during_initializer->owner.lease_generation);
    }
    EXPECT_EQ (expect.at ("messageTerminalCount").get<int> (),
               evidence->terminals[intent_request_t::packet_name]);
    EXPECT_EQ (1, evidence->terminals[no_intent_request_t::packet_name]);
    const auto old_generation = exercise->original_reply->generation;
    EXPECT_EQ (expect.at ("oldHandlerCalls").get<int> (),
               std::count (evidence->business_generations.begin (),
                           evidence->business_generations.end (), old_generation));
    std::vector<std::string> seen;
    for (const auto &event : evidence->order)
        if (std::find (expect.at ("order").begin (), expect.at ("order").end (), event)
            != expect.at ("order").end ())
            seen.push_back (event);
    EXPECT_EQ (expect.at ("order").get<std::vector<std::string>> (), seen);
    if (kind == branch_t::draining || kind == branch_t::relocating) {
        EXPECT_FALSE (*exercise->intent_result);
        const auto host = kind == branch_t::draining ? "Draining" : "Relocating";
        const std::map<std::string, zf::framework_error_kind_t> kinds{
          {"ShuttingDown", zf::framework_error_kind_t::shutting_down},
          {"Unavailable", zf::framework_error_kind_t::unavailable}};
        EXPECT_EQ (kinds.at (expect.at ("messageTerminalByHost").at (host).get<std::string> ()),
                   exercise->intent_result->error_kind ());
        const auto &diagnostic = expect.at ("sendDiagnosticsByHost").at (host);
        EXPECT_EQ ((std::vector<std::string>{diagnostic.at ("surface").get<std::string> ()}),
                   evidence->send_surfaces);
        EXPECT_EQ ((std::vector<std::string>{diagnostic.at ("reason").get<std::string> ()}),
                   evidence->send_reasons);
        EXPECT_EQ ((std::vector<zf::framework_error_kind_t>{
                     kinds.at (diagnostic.at ("kind").get<std::string> ())}),
                   evidence->send_error_kinds);
        const auto wire = zf::runtime::messaging::request_failure_mapper_t{}.target_failure_reply (
          kinds.at (diagnostic.at ("kind").get<std::string> ()));
        ASSERT_TRUE (wire);
        EXPECT_EQ ((std::vector<std::string>{zf::runtime::messaging::request_failure_mapper_t{}
                                               .reply_header_exception (wire->terminal_result,
                                                                        wire->failure_code,
                                                                        "Instance Spot activation")
                                               .what ()}),
                   evidence->send_diagnostics);
        EXPECT_EQ ((std::vector<std::string>{typeid (zf::framework_exception_t).name ()}),
                   evidence->send_error_types);
        EXPECT_EQ (expect.at ("missingPlacementCalls").get<int> (),
                   evidence->missing_placement_calls);
    }
    if (kind == branch_t::draining || kind == branch_t::relocating) {
        EXPECT_FALSE (exercise->final_authority);
        EXPECT_EQ ("Missing", expect.at ("authority"));
        EXPECT_EQ (expect.at ("thisHostFactoryCalls").get<int> (),
                   evidence->factory_calls - evidence->factory_calls_at_close);
        EXPECT_TRUE (evidence->business_generations.empty ());
        return;
    }
    const auto reincarnated =
      std::find_if (evidence->committed.begin (), evidence->committed.end (),
                    [old_generation] (const auto &snapshot) {
                        return snapshot.object_generation != old_generation;
                    });
    ASSERT_NE (reincarnated, evidence->committed.end ());
    EXPECT_EQ ("storeIssuedDifferent", expect.at ("objectGeneration"));
    EXPECT_NE (old_generation, reincarnated->object_generation);
    EXPECT_NE (exercise->original_authority->authority_owner_generation,
               reincarnated->authority_owner_generation);
    EXPECT_EQ (exercise->original_authority->owner.owner_id, reincarnated->owner.owner_id);
    EXPECT_EQ (exercise->original_authority->owner.lease_generation,
               reincarnated->owner.lease_generation);
    EXPECT_EQ (exercise->original_authority->allocation.target.node_rid.value (),
               reincarnated->allocation.target.node_rid.value ());
    EXPECT_EQ (exercise->original_authority->allocation.capacity_bundle.actor_slots,
               reincarnated->allocation.capacity_bundle.actor_slots);
    EXPECT_EQ (exercise->original_authority->allocation.capacity_bundle.spot_slots,
               reincarnated->allocation.capacity_bundle.spot_slots);
    EXPECT_EQ (exercise->original_authority->allocation.state, reincarnated->allocation.state);
    EXPECT_EQ (exercise->original_authority->allocation.target.mesh_name,
               reincarnated->allocation.target.mesh_name);
    EXPECT_EQ (exercise->original_authority->allocation.target.node_lifecycle_generation,
               reincarnated->allocation.target.node_lifecycle_generation);
    ASSERT_EQ (exercise->original_authority->allocation.capacity_bundle.spot_type.has_value (),
               reincarnated->allocation.capacity_bundle.spot_type.has_value ());
    if (reincarnated->allocation.capacity_bundle.spot_type) {
        EXPECT_EQ (exercise->original_authority->allocation.capacity_bundle.spot_type->slots,
                   reincarnated->allocation.capacity_bundle.spot_type->slots);
        EXPECT_EQ (exercise->original_authority->allocation.capacity_bundle.spot_type->stable_type,
                   reincarnated->allocation.capacity_bundle.spot_type->stable_type);
    }
    if (initializer_fails (kind)) {
        EXPECT_FALSE (*exercise->intent_result);
        EXPECT_NE (nullptr, exercise->intent_result->error ());
        EXPECT_EQ ("typedFailure", expect.at ("messageTerminal"));
        EXPECT_EQ ((std::vector<std::uint64_t>{reincarnated->object_generation}),
                   evidence->deleted_generations);
        EXPECT_NE (zf::framework_error_kind_t::deadline_exceeded,
                   exercise->intent_result->error_kind ());
        EXPECT_NE (zf::framework_error_kind_t::shutting_down,
                   exercise->intent_result->error_kind ());
        EXPECT_FALSE (exercise->final_authority);
        EXPECT_EQ ("Missing", expect.at ("authority"));
        EXPECT_EQ (expect.at ("newHandlerCalls").get<int> (),
                   evidence->business_generations.size ());
    } else {
        ASSERT_TRUE (*exercise->intent_result);
        EXPECT_EQ ("reply", expect.at ("messageTerminal"));
        ASSERT_TRUE (exercise->final_authority);
        const auto &reply = exercise->intent_result->value ();
        EXPECT_EQ (reincarnated->object_generation, reply.generation);
        EXPECT_EQ (exercise->original_reply->owner_node, reply.owner_node);
        const auto payload =
          zf::runtime::decode_instance_spot_authority_payload (exercise->final_authority->payload);
        ASSERT_TRUE (payload);
        EXPECT_EQ (zf::runtime::instance_spot_authority_state_t::ready, payload->state);
        EXPECT_EQ ("Ready", expect.at ("authority"));
        ASSERT_TRUE (exercise->original_capacity);
        ASSERT_TRUE (exercise->final_capacity);
        EXPECT_EQ (exercise->original_capacity->actors.active,
                   exercise->final_capacity->actors.active);
        EXPECT_EQ (exercise->original_capacity->actors.reserved,
                   exercise->final_capacity->actors.reserved);
        EXPECT_EQ (exercise->original_capacity->spots.active,
                   exercise->final_capacity->spots.active);
        EXPECT_EQ (exercise->original_capacity->spots.reserved,
                   exercise->final_capacity->spots.reserved);
        EXPECT_EQ (evidence->durable_value, reply.restored_value);
        EXPECT_NE (exercise->original_reply->restored_value, reply.restored_value);
        EXPECT_EQ ("storeIssuedDifferent", expect.at ("authorityOwnerGeneration"));
        for (const auto *field : {"owner", "lease", "capacity"})
            EXPECT_EQ ("unchanged", expect.at (field));
        EXPECT_EQ (expect.at ("newHandlerCalls").get<int> (),
                   std::count (evidence->business_generations.begin (),
                               evidence->business_generations.end (), reply.generation));
    }
}

// These two-message regressions intentionally do not reinterpret the shared
// fixture's one-message handler/terminal counts.
void check_pending_initializer_two_requests (branch_t kind)
{
    ASSERT_TRUE (initializer_is_held (kind));
    auto evidence = std::make_shared<evidence_t> (kind);
    auto store = std::make_shared<observed_store_t> (evidence);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    configure_app (app, evidence, store, relocations);
    auto runner = std::make_unique<exercise_t> (app, evidence, store);
    auto *exercise = runner.get ();
    app.add_hosted_service (std::move (runner));
    (void) app.run (0, nullptr);
    ASSERT_TRUE (exercise->failure.empty ()) << exercise->failure;
    ASSERT_TRUE (exercise->original_reply);
    ASSERT_TRUE (exercise->original_authority);
    ASSERT_TRUE (exercise->authority_during_initializer);
    ASSERT_TRUE (exercise->intent_result);
    ASSERT_TRUE (exercise->followup_result);
    ASSERT_TRUE (exercise->no_intent_result);
    EXPECT_FALSE (*exercise->no_intent_result);
    EXPECT_EQ (zf::framework_error_kind_t::not_found, exercise->no_intent_result->error_kind ());
    EXPECT_TRUE (exercise->close_pending_during_initializer);
    EXPECT_EQ (0u, exercise->handlers_during_initializer);
    EXPECT_EQ (exercise->factories_before_followup, exercise->factories_after_followup);
    const auto &new_authority = *exercise->authority_during_initializer;
    EXPECT_NE (exercise->original_authority->object_generation, new_authority.object_generation);
    EXPECT_NE (exercise->original_authority->authority_owner_generation,
               new_authority.authority_owner_generation);
    EXPECT_EQ (exercise->original_authority->owner.owner_id, new_authority.owner.owner_id);
    EXPECT_EQ (exercise->original_authority->owner.lease_generation,
               new_authority.owner.lease_generation);
    EXPECT_EQ (exercise->original_authority->allocation.target.node_rid.value (),
               new_authority.allocation.target.node_rid.value ());
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (evidence->initializing_generation, new_authority.object_generation);
    EXPECT_EQ (evidence->factory_calls_at_close + 1, exercise->factories_before_followup);
    EXPECT_EQ (exercise->factories_before_followup, evidence->factory_calls);
    EXPECT_EQ (1, evidence->closing_calls);
    EXPECT_EQ (1, evidence->terminals[intent_request_t::packet_name]);
    EXPECT_EQ (1, evidence->terminals[followup_intent_request_t::packet_name]);
    EXPECT_EQ (1, evidence->terminals[no_intent_request_t::packet_name]);
    const std::vector<std::string> expected_packets{intent_request_t::packet_name,
                                                    followup_intent_request_t::packet_name};
    if (initializer_fails (kind)) {
        for (const auto *result : {&*exercise->intent_result, &*exercise->followup_result}) {
            EXPECT_FALSE (*result);
            EXPECT_NE (nullptr, result->error ());
            EXPECT_NE (zf::framework_error_kind_t::deadline_exceeded, result->error_kind ());
            EXPECT_NE (zf::framework_error_kind_t::shutting_down, result->error_kind ());
            EXPECT_NE (zf::framework_error_kind_t::not_found, result->error_kind ());
        }
        EXPECT_TRUE (evidence->business_generations.empty ());
        EXPECT_TRUE (evidence->business_packets.empty ());
        EXPECT_FALSE (exercise->final_authority);
        EXPECT_EQ ((std::vector<std::uint64_t>{new_authority.object_generation}),
                   evidence->deleted_generations);
    } else {
        ASSERT_TRUE (*exercise->intent_result);
        ASSERT_TRUE (*exercise->followup_result);
        ASSERT_TRUE (exercise->final_authority);
        EXPECT_EQ (new_authority.object_generation, exercise->final_authority->object_generation);
        EXPECT_EQ (expected_packets, evidence->business_packets);
        EXPECT_EQ ((std::vector<std::uint64_t>{new_authority.object_generation,
                                               new_authority.object_generation}),
                   evidence->business_generations);
        for (const auto *result : {&*exercise->intent_result, &*exercise->followup_result}) {
            EXPECT_EQ (new_authority.object_generation, result->value ().generation);
            EXPECT_EQ (exercise->original_reply->owner_node, result->value ().owner_node);
            EXPECT_EQ (evidence->durable_value, result->value ().restored_value);
        }
        ASSERT_TRUE (exercise->original_capacity);
        ASSERT_TRUE (exercise->final_capacity);
        EXPECT_EQ (exercise->original_capacity->actors.active,
                   exercise->final_capacity->actors.active);
        EXPECT_EQ (exercise->original_capacity->actors.reserved,
                   exercise->final_capacity->actors.reserved);
        EXPECT_EQ (exercise->original_capacity->spots.active,
                   exercise->final_capacity->spots.active);
        EXPECT_EQ (exercise->original_capacity->spots.reserved,
                   exercise->final_capacity->spots.reserved);
    }
}

TEST (ZLinkFrameworkSpotReincarnateConformance, ClosingIntentReplaysOnSameOwner)
{
    check_branch (branch_t::replay, "reincarnate-pending-intent");
}
TEST (ZLinkFrameworkSpotReincarnateConformance, FailedInitializationDeletesNewGeneration)
{
    check_branch (branch_t::initialization_failure, "reincarnate-initialization-fails");
}
TEST (ZLinkFrameworkSpotReincarnateConformance, DrainReleasesInsteadOfReincarnatingHere)
{
    const auto &given = branch_fixture ("release-during-host-drain-or-relocation").at ("given");
    const std::map<std::string, branch_t> hosts{{"Draining", branch_t::draining},
                                                {"Relocating", branch_t::relocating}};
    for (const auto &host : given.at ("host"))
        for (const auto &seal : given.at ("relocationSeal")) {
            SCOPED_TRACE (host.get<std::string> () + "/" + seal.get<std::string> ());
            ASSERT_TRUE (seal == "before" || seal == "after");
            check_branch (hosts.at (host.get<std::string> ()),
                          "release-during-host-drain-or-relocation", seal == "after");
        }
}
TEST (ZLinkFrameworkSpotReincarnateConformance, DrainingPhaseAloneForbidsReincarnation)
{
    check_branch (branch_t::draining, "release-during-host-drain-or-relocation", false, false,
                  true);
}
TEST (ZLinkFrameworkSpotReincarnateConformance, RelocatedHostDoesNotReincarnateOldOwner)
{
    check_branch (branch_t::relocating, "release-during-host-drain-or-relocation", false, true);
}
TEST (ZLinkFrameworkSpotReincarnateConformance, PendingInitializerCompletesBeforeIntentHandler)
{
    check_pending_initializer_two_requests (branch_t::asynchronous_initialization);
}
TEST (ZLinkFrameworkSpotReincarnateConformance, PendingInitializerFailureDeletesNewGeneration)
{
    check_pending_initializer_two_requests (branch_t::asynchronous_initialization_failure);
}
TEST (ZLinkFrameworkSpotReincarnateConformance, ColdActivationCloseWaitsForJournalTerminalClear)
{
    auto evidence = std::make_shared<evidence_t> (branch_t::cold_activation_close);
    auto store = std::make_shared<observed_store_t> (evidence);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    configure_app (app, evidence, store, relocations);
    auto runner = std::make_unique<exercise_t> (app, evidence, store);
    auto *exercise = runner.get ();
    app.add_hosted_service (std::move (runner));
    (void) app.run (0, nullptr);
    ASSERT_TRUE (exercise->failure.empty ()) << exercise->failure;
    EXPECT_TRUE (exercise->journal_present_at_clear);
    EXPECT_TRUE (exercise->close_pending_at_clear);
    EXPECT_EQ (0, exercise->closing_calls_at_clear);
    ASSERT_TRUE (exercise->original_reply);
    ASSERT_TRUE (exercise->original_authority);
    ASSERT_TRUE (exercise->clear_marker_reply);
    EXPECT_EQ (exercise->original_authority->object_generation,
               exercise->clear_marker_reply->generation);
    EXPECT_EQ (exercise->original_authority->object_generation,
               exercise->original_reply->generation);
    ASSERT_TRUE (exercise->close_result);
    ASSERT_TRUE (*exercise->close_result);
    EXPECT_TRUE (exercise->close_result->value ());
    EXPECT_FALSE (exercise->final_authority);
    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (1, evidence->closing_calls);
    EXPECT_EQ (1, evidence->terminals[close_request_t::packet_name]);
    EXPECT_EQ (1, evidence->terminals[no_intent_request_t::packet_name]);
    EXPECT_EQ ((std::vector<std::string>{"clearHeldApplicationMarker", "activationJournalCleared",
                                         "onClosing", "authorityReleased"}),
               evidence->order);
}
} // namespace
