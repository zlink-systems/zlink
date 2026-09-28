/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Runs framework/runtime/conformance/spot-close-v1.json against an in-process
// app with a Location Store (Spot address messaging §7, §9; gate §7).

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ZLINK_SPOT_CLOSE_CONFORMANCE_PATH
#error "Spot Close conformance fixture path is required"
#endif

namespace
{

using namespace std::chrono_literals;
namespace zf = zlink::framework;

constexpr const char *mesh_name = "spot-close-mesh";
constexpr const char *node_name = "spot-close-node";

// ---------------------------------------------------------------------------
// Observation shared by the application types of one scenario.

struct observation_t
{
    std::mutex mutex;
    std::vector<std::string> order;
    std::vector<std::string> diagnostics;
    std::vector<std::string> notes;
    int on_closing_calls = 0;
    int handler_calls = 0;
    int joins_admitted = 0;
    bool on_closing_throws = false;
    // OnClosing waits until the scenario releases it, so the act observes a
    // Spot whose Close has sealed it and committed Closing.
    bool on_closing_blocks = false;
    std::promise<void> closing_entered;
    std::promise<void> closing_released;
    std::string handler_mode;
    std::string watched_spot;

    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        order.push_back (std::move (event));
    }
};

observation_t *observed = nullptr;

observation_t &current ()
{
    return *observed;
}

// ---------------------------------------------------------------------------
// Messages.

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

void to_json (nlohmann::json &json, const close_probe_request_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
void from_json (const nlohmann::json &json, close_probe_request_t &value)
{
    value.value = json.at ("value").get<int> ();
}
void to_json (nlohmann::json &json, const close_probe_reply_t &value)
{
    json = nlohmann::json{{"value", value.value}};
}
void from_json (const nlohmann::json &json, close_probe_reply_t &value)
{
    value.value = json.at ("value").get<int> ();
}
void to_json (nlohmann::json &json, const join_room_t &value)
{
    json = nlohmann::json{{"room", value.room}};
}
void from_json (const nlohmann::json &json, join_room_t &value)
{
    value.room = json.at ("room").get<std::string> ();
}

// ---------------------------------------------------------------------------
// Application types.

class member_actor_t final : public zf::actor_t
{
  public:
    explicit member_actor_t (zf::actor_context_t context) : _context (std::move (context)) {}
    zf::actor_context_t &context () noexcept override { return _context; }
    const zf::actor_context_t &context () const noexcept override { return _context; }
    zf::task_t<void> on_join_completed (const zf::actor_join_completion_t &completion) override
    {
        std::string note = "actorJoin:";
        if (std::holds_alternative<zf::actor_join_accepted_t> (completion))
            note += "accepted";
        else if (std::holds_alternative<zf::actor_join_rejected_t> (completion))
            note += "rejected";
        else
            note += "failed:"
                    + std::to_string (
                      static_cast<int> (std::get<zf::actor_join_failed_t> (completion).error_kind));
        std::lock_guard lock (current ().mutex);
        current ().notes.push_back (std::move (note));
        co_return;
    }

  private:
    zf::actor_context_t _context;
};

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

  private:
    zf::entry_spot_context_t _context;
};

class close_room_spot_t final : public zf::spot_t<member_actor_t>
{
  public:
    explicit close_room_spot_t (zf::spot_context_t context) : _context (std::move (context)) {}
    zf::spot_context_t &context () noexcept override { return _context; }
    const zf::spot_context_t &context () const noexcept override { return _context; }
    void configure () override
    {
        _context.handlers ().add_handler<&close_room_spot_t::probe> (
          close_probe_request_t::packet_name);
        // An Actor handler registers this Spot's Join admission for member_actor_t.
        _context.handlers ().add_actor_send<&close_room_spot_t::member_ping> (
          join_room_t::packet_name);
    }

    close_probe_reply_t probe (const close_probe_request_t &request)
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().handler_calls;
        }
        return close_probe_reply_t{request.value + 1};
    }

    void member_ping (member_actor_t &, zf::message_context_t &, const join_room_t &) {}

    zf::task_t<zf::spot_actor_join_result_t> on_actor_join (std::string_view,
                                                            const zf::message_t &) override
    {
        {
            std::lock_guard lock (current ().mutex);
            ++current ().joins_admitted;
        }
        co_return zf::spot_actor_join_result_t::accept ();
    }
    zf::task_t<void> on_actor_joined (member_actor_t &) override
    {
        current ().record ("joinCompleted");
        co_return;
    }
    zf::task_t<void> on_leave_actor (member_actor_t &) override { co_return; }

    zf::task_t<void> on_closing (const zf::spot_closing_context_t &, std::stop_token) override
    {
        bool fail = false;
        bool blocks = false;
        {
            std::lock_guard lock (current ().mutex);
            ++current ().on_closing_calls;
            current ().order.push_back ("onClosing");
            fail = current ().on_closing_throws;
            blocks = current ().on_closing_blocks;
        }
        if (blocks) {
            current ().closing_entered.set_value ();
            current ().closing_released.get_future ().wait ();
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
    }
    zf::instance_spot_context_t &context () noexcept override { return _context; }
    const zf::instance_spot_context_t &context () const noexcept override { return _context; }
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

    std::atomic_bool fail_closing_commit_once{false};
    std::atomic_bool fail_authority_release_once{false};

    zf::task_t<zf::store_read_result_t> read (zf::store_key_t key) override
    {
        return _inner->read (std::move (key));
    }

    zf::task_t<zf::store_write_result_t> write (zf::store_write_request_t request) override
    {
        bool watched_put = false;
        bool watched_delete = false;
        for (const auto &mutation : request.mutations) {
            if (const auto *put = std::get_if<zf::store_put_t> (&mutation))
                watched_put = watched_put || is_watched (put->key);
            if (const auto *erase = std::get_if<zf::store_delete_t> (&mutation))
                watched_delete = watched_delete || is_watched (erase->key);
        }
        if ((watched_put && fail_closing_commit_once.exchange (false))
            || (watched_delete && fail_authority_release_once.exchange (false))) {
            return zf::task_t<zf::store_write_result_t> (
              zf::result_t<zf::store_write_result_t>::success (zf::store_write_conflict_t{}));
        }
        auto result = _inner->write (std::move (request)).result ();
        if (watched_delete && result
            && std::holds_alternative<zf::store_write_applied_t> (result.value ()))
            current ().record ("authorityReleased");
        return zf::task_t<zf::store_write_result_t> (std::move (result));
    }

    zf::task_t<zf::store_scan_result_t> scan (zf::store_scan_request_t request) override
    {
        return _inner->scan (std::move (request));
    }

  private:
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
};

// ---------------------------------------------------------------------------
// Scenario runner.

std::string error_name (zf::framework_error_kind_t kind)
{
    switch (kind) {
        case zf::framework_error_kind_t::not_found:
            return "NotFound";
        case zf::framework_error_kind_t::rejected:
            return "Rejected";
        case zf::framework_error_kind_t::unavailable:
            return "Unavailable";
        case zf::framework_error_kind_t::shutting_down:
            return "ShuttingDown";
        case zf::framework_error_kind_t::invalid_operation:
            return "InvalidOperation";
        default:
            return "Error" + std::to_string (static_cast<int> (kind));
    }
}

template <typename T> nlohmann::json close_result (const zf::result_t<T> &result)
{
    if (result)
        return result.value ();
    return error_name (result.error_kind ());
}

bool wait_until (const std::function<bool ()> &condition,
                 std::chrono::milliseconds limit = std::chrono::milliseconds (3000))
{
    const auto deadline = std::chrono::steady_clock::now () + limit;
    while (!condition ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::sleep_for (1ms);
    }
    return true;
}

class scenario_client_t final : public zf::hosted_service_t
{
  public:
    scenario_client_t (zf::app_t &app, const nlohmann::json &scenario) :
        _app (&app), _scenario (scenario)
    {
    }

    zf::task_t<void> start (zf::service_provider_t &services) override
    {
        try {
            run (services);
        }
        catch (const std::exception &error) {
            failure = error.what ();
        }
        {
            // Observed before the app stops: shutdown cleanup is not part of the act.
            std::lock_guard lock (current ().mutex);
            actual["onClosingCalls"] = current ().on_closing_calls;
            actual["handlerCalls"] = current ().handler_calls;
            actual["order"] = current ().order;
            actual["diagnostics"] = current ().diagnostics;
            actual["notes"] = current ().notes;
            actual["joinsAdmitted"] = current ().joins_admitted;
        }
        if (!stopping)
            _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    nlohmann::json actual = nlohmann::json::object ();
    std::string failure;
    bool stopping = false;

  private:
    std::string authority_state (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &locations = services.get_required<zf::location_repository_t> ();
        const auto read =
          locations.read_authority (zf::runtime::spot_authority_key (spot_id)).result ().value ();
        const auto *snapshot = std::get_if<zf::authority_snapshot_t> (&read);
        if (!snapshot)
            return "Missing";
        if (const auto user = zf::runtime::decode_direct_user_spot_authority_payload (
              std::span<const std::byte> (snapshot->payload))) {
            switch (user->state) {
                case zf::runtime::user_spot_authority_state_t::creating:
                    return "Creating";
                case zf::runtime::user_spot_authority_state_t::ready:
                    return "Ready";
                case zf::runtime::user_spot_authority_state_t::closing:
                    return "Closing";
            }
        }
        const std::string_view instance_closing = "zlink:instance-spot:closing:v1\n";
        if (snapshot->payload.size () >= instance_closing.size ()
            && std::equal (instance_closing.begin (), instance_closing.end (),
                           snapshot->payload.begin (), [] (char left, std::byte right) {
                               return static_cast<unsigned char> (left)
                                      == std::to_integer<unsigned char> (right);
                           }))
            return "Closing";
        return "Ready";
    }

    zf::spot_ref_t create_room (zf::service_provider_t &services, const std::string &spot_id)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        const auto created =
          manager.get_or_create (zf::spot_id_t (spot_id), "room").timeout (3s).async ().result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "User Spot create failed");
        const auto found = manager.find (zf::spot_id_t (spot_id)).result ();
        if (!found || !found.value ())
            throw std::runtime_error ("User Spot find failed");
        return *found.value ();
    }

    zf::result_t<bool> manager_close (zf::service_provider_t &services, const zf::spot_ref_t &ref)
    {
        auto &manager = services.get_required<zf::spot_manager_t> ();
        return manager.close (ref).result ();
    }

    zf::result_t<close_probe_reply_t>
    direct_request (const std::string &spot_id, zf::service_provider_t &services, int value)
    {
        auto route = _app->advanced ().zlink ().route_client (
          services.get_required<zf::serializer_registry_t> ());
        return route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{value})
          .timeout (3s)
          .async<close_probe_reply_t> ()
          .result ();
    }

    void join_member (zf::service_provider_t &services,
                      const std::string &room,
                      const std::string &actor_id)
    {
        auto &actors = services.get_required<zf::actor_manager_t> ();
        const auto created = actors.get_or_create (zf::actor_id_t (actor_id), "member")
                               .timeout (3s)
                               .async ()
                               .result ();
        if (!created)
            throw std::runtime_error (created.error () ? created.error ()->what ()
                                                       : "member Actor create failed");
        auto &client = services.get_required<zf::actor_client_t> ();
        const auto sent =
          client.send (zf::actor_id_t (actor_id), join_room_t{room}).async ().result ();
        if (!sent)
            throw std::runtime_error (sent.error () ? sent.error ()->what ()
                                                    : "member Join request failed");
    }

    void run (zf::service_provider_t &services)
    {
        const auto name = _scenario.at ("name").get<std::string> ();
        const auto &given = _scenario.at ("given");
        const auto act = _scenario.at ("act").get<std::string> ();
        const auto spot_id = "close-" + name;
        {
            std::lock_guard lock (current ().mutex);
            current ().watched_spot = spot_id;
            current ().on_closing_throws = given.value ("onClosing", "") == "throws";
            current ().on_closing_blocks = given.value ("onClosing", "") == "blocks";
            current ().handler_mode = given.value ("handler", "");
        }
        auto &store = *_store;

        if (given.contains ("handler")) {
            // Instance Spot: the handler turn calls context Close itself.
            auto route = _app->advanced ().zlink ().route_client (
              services.get_required<zf::serializer_registry_t> ());
            const auto activated =
              route.request_to_spot (zf::spot_id_t (spot_id), close_probe_request_t{0})
                .instance_spot ("session")
                .timeout (3s)
                .async<close_probe_reply_t> ()
                .result ();
            if (!activated)
                throw std::runtime_error ("Instance Spot activation failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
            }
            if (act != "directRequestWithoutInstanceIntent")
                throw std::runtime_error ("unsupported act for a handler scenario: " + act);
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? "handlerReply" : "handlerFailure";
            (void) wait_until ([&] { return authority_state (services, spot_id) == "Missing"; });
            actual["authority"] = authority_state (services, spot_id);
            return;
        }

        const auto authority = given.at ("authority").get<std::string> ();
        std::optional<zf::spot_ref_t> ref;
        if (authority != "Missing")
            ref = create_room (services, spot_id);
        if (authority == "Closing") {
            // A Close whose authority release fails leaves Closing committed.
            store.fail_authority_release_once.store (true);
            (void) manager_close (services, *ref);
            if (authority_state (services, spot_id) != "Closing")
                throw std::runtime_error ("could not establish a Closing authority");
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        if (given.value ("members", 0) > 0) {
            join_member (services, spot_id, "member-" + name);
            if (!wait_until ([&] {
                    std::lock_guard lock (current ().mutex);
                    return std::count (current ().order.begin (), current ().order.end (),
                                       "joinCompleted")
                           > 0;
                }))
                throw std::runtime_error ("member Join did not complete");
        }
        if (given.value ("closingCommit", "") == "ownerFenceMismatch")
            store.fail_closing_commit_once.store (true);
        if (given.value ("failOnce", "") == "authorityReleased")
            store.fail_authority_release_once.store (true);

        if (act == "directRequestDuringClose") {
            // The first request installs the Ready route in the source cache,
            // so the second one reaches the owner while its Close runs.
            if (!direct_request (spot_id, services, 1))
                throw std::runtime_error ("the route warm-up request failed");
            {
                std::lock_guard lock (current ().mutex);
                current ().handler_calls = 0;
            }
            auto entered = current ().closing_entered.get_future ();
            std::thread closing ([&] { (void) manager_close (services, *ref); });
            if (entered.wait_for (3s) != std::future_status::ready) {
                current ().closing_released.set_value ();
                closing.join ();
                throw std::runtime_error ("OnClosing did not start");
            }
            const auto authority_during = authority_state (services, spot_id);
            const auto reply = direct_request (spot_id, services, 1);
            current ().closing_released.set_value ();
            closing.join ();
            actual["authorityDuringClose"] = authority_during;
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["authority"] = authority_state (services, spot_id);
            return;
        }
        if (act == "directRequestWithoutInstanceIntent") {
            if (given.at ("runtime") == "Draining") {
                stopping = true;
                (void) _app->shutdown (5s);
                (void) wait_until ([&] {
                    return _app->runtime_state () == zf::framework_runtime_state_t::draining;
                });
            }
            const auto reply = direct_request (spot_id, services, 1);
            actual["result"] = reply ? nlohmann::json ("handlerReply")
                                     : nlohmann::json (error_name (reply.error_kind ()));
            if (!reply && reply.error ())
                actual["resultDetail"] = reply.error ()->what ();
            actual["creationIntent"] = false;
            return;
        }
        if (given.value ("closeGeneration", "") == "previous") {
            // A predecessor generation exists once the same Spot ID is closed
            // and created again; the setup Close is not part of the observation.
            if (manager_close (services, *ref).value () != true)
                throw std::runtime_error ("the setup Close did not close the first generation");
            ref = create_room (services, spot_id);
            std::lock_guard lock (current ().mutex);
            current ().on_closing_calls = 0;
            current ().order.clear ();
        }
        const auto close_ref = [&] {
            if (ref && given.value ("closeGeneration", "") == "previous") {
                if (ref->object_generation () <= 1)
                    throw std::runtime_error (
                      "the scenario needs a SpotRef whose generation has a predecessor");
                return zf::spot_ref_t (ref->spot_id (), ref->object_generation () - 1,
                                       std::string (ref->mesh_name ()), ref->node_rid ());
            }
            if (ref)
                return *ref;
            return zf::spot_ref_t (zf::spot_id_t (spot_id), 1, mesh_name,
                                   zf::node_rid_t::from_string (node_name));
        }();
        if (act == "managerClose") {
            actual["result"] = close_result (manager_close (services, close_ref));
        } else if (act == "managerCloseThenManagerCloseAgain") {
            const auto first = manager_close (services, close_ref);
            actual["results"] = nlohmann::json::array (
              {first ? nlohmann::json (first.value ()) : nlohmann::json ("failure")});
            actual["authorityAfterFailure"] = authority_state (services, spot_id);
            actual["results"].push_back (close_result (manager_close (services, close_ref)));
        } else if (act == "joinAcceptedThenManagerClose") {
            const auto admitted_before = [&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted;
            }();
            std::thread join ([&] { join_member (services, spot_id, "member-" + name); });
            const bool admitted = wait_until ([&] {
                std::lock_guard lock (current ().mutex);
                return current ().joins_admitted > admitted_before;
            });
            const auto closed = manager_close (services, close_ref);
            current ().record ("closeCompleted");
            join.join ();
            if (!admitted)
                throw std::runtime_error ("the Join was not accepted before Close");
            actual["result"] = close_result (closed);
        } else {
            throw std::runtime_error ("unsupported act: " + act);
        }
        actual["authority"] = authority_state (services, spot_id);
        if (ref && authority_state (services, spot_id) == "Ready") {
            const auto admitted = direct_request (spot_id, services, 5);
            actual["admission"] = admitted ? "open" : "sealed";
        }
    }

  public:
    std::shared_ptr<close_fault_store_t> _store;

  private:
    zf::app_t *_app;
    nlohmann::json _scenario;
};

const nlohmann::json &spot_close_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_SPOT_CLOSE_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error ("Spot Close conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    }();
    return fixture;
}

const std::set<std::string> &known_scenarios ()
{
    static const std::set<std::string> names{
      "close-absent-incarnation-is-false",
      "close-other-generation-is-invalid-operation",
      "close-with-membership-is-false-and-keeps-authority",
      "close-after-accepted-join-observes-its-membership",
      "failure-before-closing-commit-keeps-authority",
      "on-closing-failure-is-diagnostic-and-cleanup-continues"};
    return names;
}

nlohmann::json run_scenario (const nlohmann::json &scenario, std::string &failure)
{
    observation_t observation;
    observed = &observation;

    const auto observe_diagnostics = [&observation] (const zf::message_flow_event_t &event) {
        if (event.result == zf::message_flow_result_t::failed) {
            std::string note =
              "flow:" + event.packet_name.value_or ("") + ":" + event.detail_result.value_or ("");
            if (event.exception) {
                try {
                    std::rethrow_exception (event.exception);
                }
                catch (const std::exception &error) {
                    note += std::string (":") + error.what ();
                }
                catch (...) {
                }
            }
            std::lock_guard lock (observation.mutex);
            observation.notes.push_back (std::move (note));
        }
        if (event.packet_name == std::optional<std::string> ("spot_close")
            && event.detail_result == std::optional<std::string> ("on_closing_failed")) {
            std::lock_guard lock (observation.mutex);
            observation.diagnostics.push_back ("onClosingFailed");
        }
    };

    auto inner = std::make_shared<zf::runtime::in_memory_location_store_t> ();
    auto store = std::make_shared<close_fault_store_t> (inner);
    auto relocations = std::make_shared<zf::runtime::in_memory_relocation_store_t> ();
    auto app = zf::app_t::create ();
    app.add_zlink_framework ([&] (zf::zlink_framework_options_t &options) {
        options.configure_dispatch ().message_flow (zf::message_flow_log_mode_t::detailed);
        zf::detail::dispatch_options_access_t::set_observer_for_tests (
          options.configure_dispatch (), observe_diagnostics);
        options.add_location_store (store);
        options.add_relocation_store (relocations);
        options.add_route_mesh (mesh_name)
          .set_object_role (zf::object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (std::string (node_name)))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<close_entry_spot_t> ([] (zf::entry_spot_context_t context) {
              return std::make_shared<close_entry_spot_t> (std::move (context));
          })
          .add_spot_factory<close_room_spot_t> (
            "room",
            [] (zf::spot_context_t context) {
                return std::make_shared<close_room_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_instance_spot_factory<close_session_spot_t> (
            "session",
            [] (zf::instance_spot_context_t context) {
                return std::make_shared<close_session_spot_t> (std::move (context));
            },
            [] (auto &factory) { factory.disable_relocation (); })
          .add_actor_factory<member_actor_t, member_actor_factory_t> (
            "member", std::make_shared<member_actor_factory_t> (),
            [] (auto &factory) { factory.disable_relocation (); });
    });
    auto client = std::make_unique<scenario_client_t> (app, scenario);
    client->_store = store;
    auto *runner = client.get ();
    app.add_hosted_service (std::move (client));
    (void) app.run (0, nullptr);

    failure = runner->failure;
    auto actual = runner->actual;
    observed = nullptr;
    return actual;
}

TEST (ZLinkFrameworkSpotCloseConformance, RunsEveryFixtureScenario)
{
    const auto &fixture = spot_close_fixture ();
    ASSERT_EQ ("zlink.framework.spot-close", fixture.at ("fixture").get<std::string> ());
    ASSERT_EQ (1, fixture.at ("version").get<int> ());
    const auto &invariants = fixture.at ("invariants");
    EXPECT_TRUE (invariants.at ("contextCloseReturnsValue").get<bool> ());
    static_assert (
      std::is_same_v<decltype (std::declval<zf::instance_spot_context_t &> ().close ()),
                     zf::task_t<bool>>);

    for (const auto &scenario : fixture.at ("scenarios")) {
        const auto name = scenario.at ("name").get<std::string> ();
        SCOPED_TRACE (name);
        ASSERT_TRUE (known_scenarios ().contains (name)) << "unknown scenario " << name;
        std::string failure;
        const auto actual = run_scenario (scenario, failure);
        std::cout << "spot-close scenario " << name << ": " << actual.dump () << std::endl;
        if (!failure.empty ()) {
            ADD_FAILURE () << failure << ": " << actual.dump ();
            continue;
        }
        const auto &expect = scenario.at ("expect");
        for (const auto &[key, value] : expect.items ()) {
            if (key == "order") {
                // The expected events appear in this relative order.
                std::vector<std::string> seen;
                for (const auto &event : actual.at ("order"))
                    if (std::find (value.begin (), value.end (), event) != value.end ())
                        seen.push_back (event.get<std::string> ());
                EXPECT_EQ (value.get<std::vector<std::string>> (), seen) << actual.dump ();
                continue;
            }
            if (key == "results") {
                if (!actual.contains ("results")) {
                    ADD_FAILURE () << "results not observed: " << actual.dump ();
                    continue;
                }
                EXPECT_EQ (value, actual.at ("results")) << actual.dump ();
                continue;
            }
            if (!actual.contains (key)) {
                ADD_FAILURE () << key << " not observed: " << actual.dump ();
                continue;
            }
            EXPECT_EQ (value, actual.at (key)) << key << ": " << actual.dump ();
        }
    }
}

} // namespace
