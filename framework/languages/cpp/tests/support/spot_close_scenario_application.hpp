/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "spot_close_actor_fixture.hpp"

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/messaging/request_deadline.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include "runtime_failure_fixture.hpp"
#include "host_readiness.hpp"
#include "telemetry_observation.hpp"
#include "manual_internal_scheduler.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::test::spot_close
{
using namespace std::chrono_literals;
namespace zf = zlink::framework;

constexpr const char *mesh_name = "spot-close-mesh";
constexpr const char *node_name = "spot-close-node";
constexpr const char *queued_request_flow_id = "019fc5b9-9df3-786b-bb69-d55358f6d48b";

// ---------------------------------------------------------------------------
// Observation shared by the application types of one scenario.

struct observation_t
{
    std::function<zf::task_t<void> (zf::spot_context_t &, std::string_view)> lifecycle_close_probe;
    std::mutex mutex;
    std::vector<std::string> order;
    std::vector<std::string> diagnostics;
    std::vector<std::string> notes;
    int on_closing_calls = 0;
    int handler_calls = 0;
    int factory_calls = 0;
    int joins_admitted = 0;
    bool on_closing_throws = false;
    // OnClosing waits until the scenario releases it, so the act observes a
    // Spot whose Close has committed Closing.
    bool on_closing_blocks = false;
    std::optional<zf::spot_closing_context_t> closing_context;
    std::stop_token cleanup_token;
    zf::framework_runtime_t *shutdown_runtime = nullptr;
    std::optional<std::chrono::system_clock::time_point> host_closing_deadline;
    bool cleanup_stopped_at_invocation = false;
    std::atomic_bool cleanup_seen_in_callback{false};
    std::atomic_bool cleanup_context_preserved{false};
    bool observe_entry_closing = false;
    bool entry_closing_blocks = false;
    std::optional<zf::spot_closing_context_t> entry_closing_context;
    std::stop_token entry_cleanup_token;
    bool entry_stopped_at_invocation = false;
    std::size_t entry_native_nodes_at_invocation = 0;
    std::atomic_bool entry_saw_cancellation{false};
    zf::task_completion_source_t<void> entry_closing_released;
    std::promise<void> closing_entered;
    std::promise<zf::actor_join_completion_t> closing_join_completed;
    zf::task_completion_source_t<void> closing_released;
    std::optional<std::promise<void>> queued_request_admitted;
    bool room_timer = false;
    std::optional<zf::timer_t> timer_resource;
    int timer_calls = 0;
    int timer_calls_on_closing = 0;
    // Both source handlers register their Actor barriers before either ends.
    std::atomic_bool cross_join_active{false};
    std::promise<void> cross_a_reserved;
    std::promise<void> cross_b_reserved;
    std::promise<void> cross_a_initial;
    std::promise<void> cross_b_initial;
    std::condition_variable cross_progress;
    std::promise<void> cross_sources_released;
    std::shared_future<void> cross_sources_release = cross_sources_released.get_future ().share ();
    std::string handler_mode;
    std::string watched_spot;
    std::shared_ptr<zf::detail::spot_context_state_t> room_state;
    std::shared_ptr<zf::detail::spot_context_state_t> session_state;
    std::shared_ptr<zf::detail::timer_state_t> preparation_timer;
    bool prepare_timer = false;
    bool initialized = false;
    std::size_t prepared_authority_puts = 0;
    bool preparation_timer_closes = false;
    std::optional<zf::task_t<bool>> preparation_timer_close;

    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        order.push_back (std::move (event));
        cross_progress.notify_all ();
    }
};

extern observation_t *observed;

struct close_probe_request_t
{
    static constexpr const char *packet_name = "spot-close-probe-request";
    int value{};
};

struct close_probe_reply_t
{
    static constexpr const char *packet_name = "spot-close-probe-reply";
    int value{};
};

struct join_room_t
{
    static constexpr const char *packet_name = "spot-close-join-room";
    std::string room;
};
} // namespace zlink::framework::test::spot_close

template <>
struct zlink::json::dto_traits<zlink::framework::test::spot_close::close_probe_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::spot_close::close_probe_request_t::value)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::spot_close::close_probe_reply_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::spot_close::close_probe_reply_t::value)};
};

template <> struct zlink::json::dto_traits<zlink::framework::test::spot_close::join_room_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("room", &zlink::framework::test::spot_close::join_room_t::room)};
};

namespace zlink::framework::test::spot_close
{
observation_t &current ();

void install_preparation_timer (const std::shared_ptr<zf::detail::spot_context_state_t> &state);

class member_actor_factory_t final : public zf::actor_factory_t<member_actor_t>
{
  public:
    zf::task_t<std::shared_ptr<member_actor_t>> create (zf::actor_context_t context,
                                                        std::stop_token) override
    {
        co_return std::make_shared<member_actor_t> (std::move (context));
    }
};

class close_entry_spot_t final : public zf::entry_spot_t<member_actor_t>
{
  public:
    explicit close_entry_spot_t (zf::entry_spot_context_t context) : _context (std::move (context))
    {
    }
    zf::entry_spot_context_t &context () noexcept override { return _context; }
    const zf::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_actor_send<&close_entry_spot_t::join_room> (
          join_room_t::packet_name);
    }

    void join_room (member_actor_t &actor, zf::message_context_t &, const join_room_t &request)
    {
        actor.context ().join_spot (zf::spot_id_t (request.room)).defer ();
    }

    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }
    zf::task_t<void> on_closing (const zf::spot_closing_context_t &context,
                                 std::stop_token cleanup) override
    {
        auto &observation = current ();
        if (!observation.observe_entry_closing)
            co_return;
        {
            std::lock_guard lock (observation.mutex);
            observation.entry_closing_context = context;
            observation.entry_cleanup_token = cleanup;
            observation.entry_stopped_at_invocation = cleanup.stop_requested ();
            const auto state = zf::detail::spot_context_access_t::state (_context);
            observation.entry_native_nodes_at_invocation =
              zf::detail::spot_node_runtime_t (state->node).native_node () ? 1 : 0;
        }
        std::stop_callback cancelled (cleanup, [&observation] {
            observation.entry_saw_cancellation.store (true);
            observation.entry_closing_released.complete (zf::result_t<void>::success ());
        });
        if (observation.entry_closing_blocks)
            co_await observation.entry_closing_released.task ();
    }


  private:
    zf::entry_spot_context_t _context;
};

class close_room_spot_t;

struct close_room_timer_handler_t
{
    zf::task_t<void> handle (close_room_spot_t &, const zf::timer_tick_t &)
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().timer_calls;
            current ().order.push_back ("timer");
        }
        co_return;
    }
};

class close_other_room_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit close_other_room_spot_t (zf::spot_context_t context) : _context (std::move (context))
    {
    }
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &) override { co_return; }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

  private:
    zf::spot_context_t _context;
};

class close_room_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit close_room_spot_t (zf::spot_context_t context) : _context (std::move (context))
    {
        current ().room_state = zf::detail::spot_context_access_t::state (_context);
    }
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_initialize () override
    {
        install_preparation_timer (zf::detail::spot_context_access_t::state (_context));
        co_return;
    }
    void configure () override
    {
        _context.handlers ().add_handler<&close_room_spot_t::probe> (
          close_probe_request_t::packet_name);
        // An Actor handler registers this Spot's Join admission for member_actor_t.
        _context.handlers ().add_actor_send<&close_room_spot_t::member_ping> (
          join_room_t::packet_name);
        bool timer = false;
        {
            std::lock_guard lock (current ().mutex);
            timer = current ().room_timer;
        }
        if (timer) {
            auto resource = _context.add_timer<close_room_timer_handler_t> ("close-timer", 5ms);
            std::lock_guard lock (current ().mutex);
            current ().timer_resource = std::move (resource);
        }
    }

    close_probe_reply_t probe (const close_probe_request_t &request)
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().handler_calls;
        }
        return close_probe_reply_t{request.value + 1};
    }

    // A member asked to join another room moves there (cross-join scenario).
    void member_ping (member_actor_t &actor, zf::message_context_t &, const join_room_t &request)
    {
        if (request.room != _context.spot_id ()) {
            actor.context ().join_spot (zf::spot_id_t (request.room)).defer ();
            if (current ().cross_join_active.load ()) {
                auto &reserved = actor.context ().actor_id ().value () == "cross-a"
                                   ? current ().cross_a_reserved
                                   : current ().cross_b_reserved;
                reserved.set_value ();
                current ().cross_sources_release.wait ();
            }
        }
    }

    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().joins_admitted;
        }
        if (current ().lifecycle_close_probe)
            co_await current ().lifecycle_close_probe (_context, "join");
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &actor) override
    {
        if (current ().lifecycle_close_probe)
            co_await current ().lifecycle_close_probe (_context, "joined");
        current ().record ("joinCompleted");
        current ().record ("joinStarted:" + _context.spot_id ());
        if (current ().cross_join_active.load ())
            current ().record ("joinedDone:" + std::string (actor.context ().actor_id ().value ()));
        co_return;
    }
    zf::task_t<void> on_leave_actor (member_actor_t &) override
    {
        if (current ().lifecycle_close_probe)
            co_await current ().lifecycle_close_probe (_context, "leave");
        current ().record ("leaveCompleted:" + _context.spot_id ());
        co_return;
    }

    zf::task_t<void> on_closing (const zf::spot_closing_context_t &context,
                                 std::stop_token cleanup) override
    {
        bool fail = false;
        bool blocks = false;
        {
            std::lock_guard lock (current ().mutex);
            if (current ().shutdown_runtime)
                current ().host_closing_deadline =
                  current ().shutdown_runtime->status ().operation_deadline;
            current ().closing_context = context;
            current ().cleanup_token = cleanup;
            current ().cleanup_stopped_at_invocation = cleanup.stop_requested ();
            ++current ().on_closing_calls;
            current ().order.push_back ("onClosing");
            current ().timer_calls_on_closing = current ().timer_calls;
            fail = current ().on_closing_throws;
            blocks = current ().on_closing_blocks;
        }
        std::stop_callback cleanup_observer (cleanup, [observation = &current (), &context] {
            observation->cleanup_seen_in_callback.store (true);
            observation->cleanup_context_preserved.store (
              observation->host_closing_deadline
              && context.deadline == *observation->host_closing_deadline);
        });
        if (blocks) {
            current ().closing_entered.set_value ();
            co_await current ().closing_released.task ();
        }
        if (fail)
            throw std::runtime_error ("fixture OnClosing failure");
        co_return;
    }

  private:
    zf::spot_context_t _context;
};

class close_session_spot_t final : public zf::instance_spot_t
{
  public:
    explicit close_session_spot_t (zf::instance_spot_context_t context) :
        _context (std::move (context))
    {
        current ().session_state = zf::detail::spot_context_access_t::state (_context);
    }
    zf::instance_spot_context_t &context () noexcept override { return _context; }
    const zf::instance_spot_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_initialize () override
    {
        install_preparation_timer (zf::detail::spot_context_access_t::state (_context));
        co_return;
    }
    void configure () override
    {
        _context.handlers ().add_handler<&close_session_spot_t::probe> ();
    }

    close_probe_reply_t probe (const close_probe_request_t &request)
    {
        std::string mode;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().handler_calls;
            mode = request.value == 0 ? std::string{} : current ().handler_mode;
        }
        if (mode == "closeTwiceThenComplete") {
            _context.close ();
            _context.close ();
            current ().record ("handlerReturned");
        } else if (mode == "closeThenThrow") {
            _context.close ();
            current ().record ("handlerThrew");
            throw std::runtime_error ("fixture handler failure after Close");
        }
        return close_probe_reply_t{request.value + 1};
    }

    zf::task_t<void> on_closing (const zf::spot_closing_context_t &, std::stop_token) override
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().on_closing_calls;
            current ().order.push_back ("onClosing");
        }
        co_return;
    }

  private:
    zf::instance_spot_context_t _context;
};

// ---------------------------------------------------------------------------
// Location Store with one-shot write faults on a watched Spot authority row.

class close_fault_store_t final : public zf::location_store_t
{
  public:
    explicit close_fault_store_t (std::shared_ptr<zf::runtime::in_memory_location_store_t> inner) :
        _inner (std::move (inner))
    {
    }

    std::atomic_bool capture_ready{false};
    std::atomic_bool replay_ready{false};
    std::map<std::string, zf::store_read_result_t> ready_reads;
    std::atomic_bool fail_closing_commit_once{false};
    std::atomic_bool fail_authority_release_once{false};
    // The next Closing commit stays pending until release_held_commit.
    std::atomic_bool hold_closing_commit_once{false};
    std::atomic_bool hold_authority_release_once{false};
    // Zero disables this fixture; Ready/cursor/pointer are prepared puts 1/2/3.
    std::size_t hold_prepared_authority_put = 0;
    std::atomic_bool owner_lease_released{false};
    std::promise<void> commit_held;
    std::atomic_bool hold_read_once{false};
    std::atomic_bool hold_resolved_read_once{false};
    std::promise<void> read_held;

    void release_held_read ()
    {
        std::optional<zf::store_key_t> key;
        std::optional<zf::task_completion_source_t<zf::store_read_result_t>> completion;
        std::optional<zf::result_t<zf::store_read_result_t>> snapshot;
        {
            std::lock_guard lock (_held_mutex);
            hold_read_once.store (false);
            key = std::move (_held_read_key);
            completion = std::move (_held_read_completion);
            _held_read_key.reset ();
            _held_read_completion.reset ();
            snapshot = std::move (_held_read_snapshot);
            _held_read_snapshot.reset ();
        }
        if (completion)
            completion->complete (snapshot ? std::move (*snapshot)
                                           : _inner->read (std::move (*key)).result ());
    }

    // Completes the held Closing commit: applied, or an authority version conflict.
    void release_held_commit (bool conflict)
    {
        std::optional<zf::store_write_request_t> request;
        std::optional<zf::task_completion_source_t<zf::store_write_result_t>> completion;
        {
            std::lock_guard lock (_held_mutex);
            request = std::move (_held_request);
            completion = std::move (_held_completion);
            _held_request.reset ();
            _held_completion.reset ();
        }
        if (!completion)
            return;
        if (conflict) {
            completion->complete (conflict_after_authority_change (std::move (*request)).result ());
            return;
        }
        completion->complete (_inner->write (std::move (*request)).result ());
    }

    zf::task_t<zf::store_read_result_t> read (zf::store_key_t key) override
    {
        if (is_watched (key) && hold_resolved_read_once.exchange (false)) {
            auto snapshot = _inner->read (key).result ();
            std::lock_guard lock (_held_mutex);
            zf::task_completion_source_t<zf::store_read_result_t> completion;
            auto pending = completion.task ();
            _held_read_snapshot = std::move (snapshot);
            _held_read_completion = std::move (completion);
            read_held.set_value ();
            return pending;
        }
        if (is_watched (key)) {
            std::lock_guard lock (_held_mutex);
            if (hold_read_once.exchange (false)) {
                zf::task_completion_source_t<zf::store_read_result_t> completion;
                auto pending = completion.task ();
                _held_read_key = std::move (key);
                _held_read_completion = std::move (completion);
                read_held.set_value ();
                return pending;
            }
        }
        {
            std::lock_guard lock (_held_mutex);
            if (replay_ready) {
                const auto found = ready_reads.find (key.value);
                if (found != ready_reads.end ())
                    return zf::task_t<zf::store_read_result_t>{
                      zf::result_t<zf::store_read_result_t>::success (found->second)};
            }
        }
        auto result = _inner->read (key);
        if (capture_ready && is_watched (key)) {
            auto value = result.result ();
            if (!value)
                throw std::runtime_error ("Ready snapshot read failed");
            std::lock_guard lock (_held_mutex);
            ready_reads.insert_or_assign (key.value, value.value ());
            return zf::task_t<zf::store_read_result_t>{std::move (value)};
        }
        return result;
    }

    zf::task_t<zf::store_write_result_t> write (zf::store_write_request_t request) override
    {
        bool watched_put = false;
        bool watched_delete = false;
        bool owner_delete = false;
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<zf::store_put_t> (&mutation))
                watched_put = watched_put || is_watched (put->key);
            if (const auto *erase = std::get_if<zf::store_delete_t> (&mutation)) {
                watched_delete = watched_delete || is_watched (erase->key);
                owner_delete =
                  owner_delete || erase->key.value.starts_with (std::string ("owner-lease\0", 12));
            }
        }
        bool hold_prepared = false;
        if (watched_put) {
            std::lock_guard lock (current ().mutex);
            if (current ().initialized) {
                ++current ().prepared_authority_puts;
                hold_prepared = current ().prepared_authority_puts == hold_prepared_authority_put;
            }
        }
        if (hold_prepared || (watched_put && hold_closing_commit_once.exchange (false))
            || (watched_delete && hold_authority_release_once.exchange (false))) {
            zf::task_completion_source_t<zf::store_write_result_t> completion;
            auto held = completion.task ();
            {
                std::lock_guard lock (_held_mutex);
                _held_request = std::move (request);
                _held_completion = std::move (completion);
            }
            commit_held.set_value ();
            co_return co_await held;
        }
        if (watched_put && fail_closing_commit_once.exchange (false))
            co_return co_await conflict_after_authority_change (std::move (request));
        if (watched_delete && fail_authority_release_once.exchange (false)) {
            co_return zf::store_write_result_t{zf::store_write_conflict_t{}};
        }
        auto result = co_await _inner->write (std::move (request));
        if (owner_delete && std::holds_alternative<zf::store_write_applied_t> (result))
            owner_lease_released.store (true);
        if (watched_delete && std::holds_alternative<zf::store_write_applied_t> (result))
            current ().record ("authorityReleased");
        co_return result;
    }

    zf::task_t<zf::store_scan_result_t> scan (zf::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

  private:
    zf::task_t<zf::store_write_result_t>
    conflict_after_authority_change (zf::store_write_request_t request)
    {
        for (const auto &mutation : request.mutations) {
            const auto &key = std::visit (
              [] (const auto &value) -> const zf::store_key_t & { return value.key; }, mutation);
            if (!is_watched (key))
                continue;
            const auto current = std::get<zf::store_found_t> (co_await _inner->read (key));
            zf::store_write_request_t concurrent{
              {}, {zf::store_put_t{key, current.value.bytes, std::nullopt}}};
            const auto rewritten = co_await _inner->write (std::move (concurrent));
            const auto *applied = std::get_if<zf::store_write_applied_t> (&rewritten);
            if (!applied || applied->put_versions.size () != 1)
                throw std::runtime_error ("fixture authority rewrite failed");
            EXPECT_NE (current.value.version.value, applied->put_versions.front ().version.value);
            const auto result = co_await _inner->write (std::move (request));
            EXPECT_TRUE (std::holds_alternative<zf::store_write_conflict_t> (result));
            co_return result;
        }
        throw std::runtime_error ("fixture authority mutation missing");
    }

    bool is_watched (const zf::store_key_t &key) const
    {
        std::string suffix ("\0spot\0", 6);
        {
            std::lock_guard lock (current ().mutex);
            if (current ().watched_spot.empty ())
                return false;
            suffix += current ().watched_spot;
        }
        return key.value.size () >= suffix.size ()
               && key.value.compare (key.value.size () - suffix.size (), suffix.size (), suffix)
                    == 0;
    }

    std::shared_ptr<zf::runtime::in_memory_location_store_t> _inner;
    std::mutex _held_mutex;
    std::optional<zf::store_key_t> _held_read_key;
    std::optional<zf::result_t<zf::store_read_result_t>> _held_read_snapshot;
    std::optional<zf::task_completion_source_t<zf::store_read_result_t>> _held_read_completion;
    std::optional<zf::store_write_request_t> _held_request;
    std::optional<zf::task_completion_source_t<zf::store_write_result_t>> _held_completion;
};
} // namespace zlink::framework::test::spot_close
