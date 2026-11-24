/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>
#include "runtime/actors/actor_ref_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "../support/host_readiness.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fw = zlink::framework;
namespace client = zlink::stream_connector;
using namespace std::chrono_literals;
struct Probe
{
    static constexpr const char *packet_name = "Probe";
    std::string value;
};
struct Pong
{
    static constexpr const char *packet_name = "Pong";
    std::string value;
};
struct Push
{
    static constexpr const char *packet_name = "Push";
    std::string value;
};
template <> struct zlink::json::dto_traits<Probe>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &Probe::value)};
};
template <> struct zlink::json::dto_traits<Pong>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &Pong::value)};
};
template <> struct zlink::json::dto_traits<Push>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &Push::value)};
};

namespace
{
std::atomic<int> callbacks{0};
std::atomic<int> bound_callbacks{0};
std::mutex bound_gate;
std::condition_variable bound_ready;
std::atomic<int> replacements{0};
std::atomic<int> actor_ingress{0};
bool reject_duplicate = false;
enum class bound_failure_t
{
    none,
    standard,
    nonstandard
};
bound_failure_t bound_failure = bound_failure_t::none;
std::mutex sessions_gate;
std::condition_variable sessions_ready;
std::deque<fw::stream_t> sessions;

template <class T> T await (fw::task_t<T> task)
{
    auto result = task.result_for (10s);
    if (!result)
        throw std::runtime_error ("operation deadline");
    if (!*result)
        throw *result->error ();
    if constexpr (!std::is_void_v<T>)
        return result->value ();
}
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
class Player final : public fw::actor_t
{
    fw::actor_context_t _context;

  public:
    explicit Player (fw::actor_context_t context) : _context (std::move (context)) {}
    fw::actor_context_t &context () noexcept override { return _context; }
    const fw::actor_context_t &context () const noexcept override { return _context; }
    fw::task_t<void> on_session_bound (const fw::actor_session_binding_identity_t &incoming,
                                       std::stop_token) override
    {
        require (!incoming.session_owner_node_rid.empty ()
                   && incoming.session_owner_node_generation != 0
                   && incoming.binding_generation != 0,
                 "committed callback identity is incomplete");
        {
            const std::lock_guard lock (bound_gate);
            ++bound_callbacks;
        }
        bound_ready.notify_all ();
        if (bound_failure == bound_failure_t::standard)
            throw std::runtime_error ("session-bound-public-test-failure");
        if (bound_failure == bound_failure_t::nonstandard)
            throw 42;
        co_return;
    }
    fw::task_t<std::optional<std::string>>
    on_duplicate_bind (const fw::actor_session_binding_identity_t &,
                       const fw::actor_session_binding_identity_t &,
                       std::stop_token) override
    {
        ++callbacks;
        co_return reject_duplicate ? std::optional<std::string> ("already signed in")
                                   : std::nullopt;
    }
};
class Factory final : public fw::actor_factory_t<Player>
{
  public:
    fw::task_t<std::shared_ptr<Player>> create (fw::actor_context_t context,
                                                std::stop_token) override
    {
        co_return std::make_shared<Player> (std::move (context));
    }
};
class Entry final : public fw::entry_spot_t<Player>
{
    fw::entry_spot_context_t _context;

  public:
    explicit Entry (fw::entry_spot_context_t context) : _context (std::move (context)) {}
    fw::entry_spot_context_t &context () noexcept override { return _context; }
    const fw::entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override { _context.handlers ().add_actor_request<&Entry::probe> (); }
    fw::task_t<Pong> probe (Player &, fw::message_context_t &, const Probe &request)
    {
        ++actor_ingress;
        co_return Pong{request.value};
    }
    fw::task_t<void> on_actor_joined (Player &) override { co_return; }
    fw::task_t<void> on_leave_actor (Player &) override { co_return; }
    fw::task_t<void> on_disconnect_actor (Player &) override { co_return; }
};
class Session final : public fw::packet_stream_session_t
{
  public:
    fw::task_t<void> on_connected (fw::stream_t &stream) override
    {
        {
            const std::lock_guard lock (sessions_gate);
            sessions.push_back (stream);
        }
        sessions_ready.notify_one ();
        co_return;
    }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_actor_binding_replaced (fw::stream_t &, std::string) override
    {
        ++replacements;
        co_return;
    }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const fw::message_t &message) override
    {
        if (dispatch.actor) {
            auto response = co_await dispatch.actor->relay_request (message).async ();
            co_await stream.reply_packet (response).async ();
        }
        co_return;
    }
};
class Observer final : public fw::hosted_service_t
{
  public:
    std::promise<void> started;
    fw::actor_manager_t *actors = nullptr;
    fw::service_provider_t *services = nullptr;
    fw::framework_runtime_t *runtime = nullptr;
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        actors = &services.get_required<fw::actor_manager_t> ();
        runtime = &services.get_required<fw::framework_runtime_t> ();
        this->services = &services;
        started.set_value ();
        co_return;
    }
    void stop () noexcept override {}
};
struct Host
{
    fw::app_t app = fw::app_t::create ();
    Observer *observer;
    std::thread thread;
    std::shared_ptr<fw::runtime::in_memory_location_store_t> locations;
    Host (std::shared_ptr<fw::runtime::in_memory_location_store_t> store = {},
          std::string peer = {})
    {
        auto &options = app.add_zlink_framework ();
        options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
        locations =
          store ? std::move (store) : std::make_shared<fw::runtime::in_memory_location_store_t> ();
        options.add_location_store (locations);
        auto mesh = options.add_route_mesh ("game")
                      .set_automatic_routing_id_prefix ("same-node-binding")
                      .listen ("tcp://127.0.0.1:0");
        if (peer.empty ()) {
            mesh.objects ()
              .server ()
              .add_entry_spot<Entry> ()
              .add_actor_factory<Player, Factory> ("player")
              .disable_relocation ();
        } else {
            mesh.objects ().client ();
            mesh.peer_connections ().connect (peer);
        }
        options.add_stream_node ("clients")
          .bind ("tcp://127.0.0.1:0")
          .enable_actor_dispatch ()
          .register_session<Session> ();
        auto service = std::make_unique<Observer> ();
        observer = service.get ();
        auto started = observer->started.get_future ();
        app.add_hosted_service (std::move (service));
        thread = std::thread ([this] {
            char name[] = "same-node-duplicate-bind";
            char *args[] = {name, nullptr};
            app.run (1, args);
        });
        if (started.wait_for (10s) != std::future_status::ready) {
            app.stop ();
            thread.join ();
            throw std::runtime_error ("host startup deadline");
        }
        fw::test::wait_for_serving (*observer->services);
    }
    ~Host ()
    {
        app.stop ();
        if (thread.joinable ())
            thread.join ();
    }
};
struct Client
{
    client::connector_t connector;
    fw::stream_t stream;
    std::promise<void> closed;
    std::promise<std::shared_ptr<client::actor_t>> bound;
    std::optional<client::subscription_t> bound_observation;
    std::optional<client::subscription_t> observation;
    explicit Client (Host &host) :
        connector ([&] {
            client::connector_options_t options;
            options.endpoint =
              host.observer->runtime->listener_status (fw::listener_kind_t::stream, "clients")
                .endpoint;
            options.dispatch_mode = client::dispatch_mode_t::immediate;
            options.reconnect.enabled = false;
            return client::connector_factory_t::create (options);
        }())
    {
        bound_observation.emplace (
          connector.on_actor_bound ([this] (const auto &actor) { bound.set_value (actor); }));
        observation.emplace (connector.on_disconnected ([this] (auto) { closed.set_value (); }));
        require (static_cast<bool> (connector.connect ()), "client connect failed");
        std::unique_lock lock (sessions_gate);
        require (sessions_ready.wait_for (lock, 10s, [] { return !sessions.empty (); }),
                 "session not connected");
        stream = sessions.front ();
        sessions.pop_front ();
    }
    ~Client () { (void) connector.close (); }
};
}
int main (int argc, char **argv)
{
    try {
        fw::completion_owner_t completion;
        const std::string_view mode = argc > 1 ? std::string_view (argv[1]) : std::string_view ();
        if (mode == "missing-authority" || mode == "generation-mismatch"
            || mode == "stale-location") {
            Host host;
            await (host.observer->actors->create (fw::actor_id_t ("bind-target"), "player")
                     .in_mesh ("game")
                     .async ());
            const auto actor =
              await (host.observer->actors->find (fw::actor_id_t ("bind-target"))).value ();
            const bool different_generation = mode == "generation-mismatch";
            const bool missing_authority = mode == "missing-authority";
            const auto submitted = fw::detail::actor_ref_access_t::make (
              mode == "stale-location" ? fw::node_rid_t::from_string ("stale-node")
                                       : actor.node_rid (),
              std::string ("player"), missing_authority ? "missing-actor" : "bind-target",
              different_generation ? actor.object_generation () + 1 : actor.object_generation (),
              std::string (actor.mesh_name ()));
            Client session (host);
            const auto result = session.stream.actors ().bind (submitted).async ().result_for (10s);
            require (result.has_value (), "bind deadline");
            require (!*result, "bind with a mismatched ActorRef succeeded");
            const auto expected = missing_authority ? fw::framework_error_kind_t::not_found
                                  : different_generation
                                    ? fw::framework_error_kind_t::invalid_operation
                                    : fw::framework_error_kind_t::unavailable;
            if (result->error_kind () != expected) {
                throw std::runtime_error (
                  "wrong bind error kind (expected " + std::to_string (static_cast<int> (expected))
                  + ", got " + std::to_string (static_cast<int> (result->error_kind ())) + ")");
            }
            return 0;
        }
        const bool remote = argc > 1 && std::string_view (argv[1]) == "remote";
        const std::string argument = argc > 1 ? argv[1] : "accept";
        bound_failure = argument == "bound-standard-error"      ? bound_failure_t::standard
                        : argument == "bound-nonstandard-error" ? bound_failure_t::nonstandard
                                                                : bound_failure_t::none;
        const std::string name =
          remote || bound_failure != bound_failure_t::none ? "accept" : argument;
        std::ifstream input (ZLINK_BINDING_CONFORMANCE_PATH);
        require (static_cast<bool> (input), "shared binding fixture was not found");
        const auto fixture = nlohmann::json::parse (input);
        const auto &cases = fixture.at ("sameNodeDuplicateBind");
        const auto scenario = std::find_if (cases.begin (), cases.end (), [&] (const auto &item) {
            return item.at ("name") == name;
        });
        require (scenario != cases.end (), "unknown binding scenario");
        reject_duplicate = !scenario->at ("accepted").get<bool> ();
        Host host;
        await (host.observer->actors->create (fw::actor_id_t ("duplicate-player"), "player")
                 .in_mesh ("game")
                 .async ());
        auto actor =
          await (host.observer->actors->find (fw::actor_id_t ("duplicate-player"))).value ();
        Client current (host);
        await (current.stream.actors ().bind (actor).async ());
        {
            std::unique_lock lock (bound_gate);
            require (bound_ready.wait_for (lock, 10s, [] { return bound_callbacks == 1; }),
                     "initial bind callback missing");
        }
        auto old_bound = current.bound.get_future ();
        require (old_bound.wait_for (10s) == std::future_status::ready,
                 "initial bound notification missing");
        const auto old_actor = old_bound.get ();
        const auto before = old_actor->request (Probe{"before"}).submit<Pong> ();
        require (before && before.value ().value == "before", "initial Actor ingress failed");
        std::unique_ptr<Host> remote_host;
        if (remote)
            remote_host = std::make_unique<Host> (
              host.locations,
              host.observer->runtime->listener_status (fw::listener_kind_t::route_mesh, "game")
                .endpoint);
        Client incoming (remote_host ? *remote_host : host);
        require (callbacks == 0, "initial bind invoked duplicate callback");
        const auto result = incoming.stream.actors ().bind (actor).async ().result_for (10s);
        require (result.has_value (), "duplicate bind deadline");
        std::cout << "callbacks=" << callbacks << " accepted=" << static_cast<bool> (*result)
                  << " replacements=" << replacements << std::endl;
        require (callbacks == scenario->at ("callbackCount").get<int> (),
                 "same-node bind bypassed Actor duplicate callback");
        if (!reject_duplicate) {
            std::unique_lock lock (bound_gate);
            require (bound_ready.wait_for (lock, 10s, [] { return bound_callbacks == 2; }),
                     "replacement bind callback missing");
        }
        require (bound_callbacks == (reject_duplicate ? 1 : 2),
                 "rejected bind invoked committed callback");
        if (reject_duplicate) {
            require (!*result, "rejected duplicate bind succeeded");
            require (result->error_kind () == fw::framework_error_kind_t::rejected,
                     "wrong rejection kind");
            require (current.stream.actors ().find ("duplicate-player").has_value (),
                     "current binding lost after rejection");
            require (!incoming.stream.actors ().find ("duplicate-player"),
                     "rejected binding installed");
            require (replacements == scenario->at ("replacementCount").get<int> (),
                     "rejection notified replacement");
        } else {
            require (static_cast<bool> (*result), "accepted duplicate bind failed");
            auto new_bound = incoming.bound.get_future ();
            require (new_bound.wait_for (10s) == std::future_status::ready,
                     "replacement bound notification missing");
            const auto successor = new_bound.get ();
            const auto response = successor->request (Probe{"current"}).submit<Pong> ();
            require (response && response.value ().value == "current",
                     "successor Actor ingress failed");
            std::promise<void> pushed;
            auto subscription =
              incoming.connector.on<Push> ([&] (const auto &) { pushed.set_value (); });
            auto binding = incoming.stream.actors ().find ("duplicate-player");
            await (binding->bound_session ().send (Push{"current"}).async ());
            require (pushed.get_future ().wait_for (10s) == std::future_status::ready,
                     "successor push failed");
            require (current.closed.get_future ().wait_for (10s) == std::future_status::ready,
                     "retired physical Session not closed");
            require (replacements == scenario->at ("replacementCount").get<int> (),
                     "retired Session missing replacement notification");
            const auto retired = old_actor->request (Probe{"retired"}).submit<Pong> ();
            require (!retired, "retired Actor ingress accepted");
            require (actor_ingress == 2, "retired Actor ingress reached the handler");
            require (incoming.stream.actors ().find ("duplicate-player").has_value (),
                     "incoming binding missing");
        }
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what () << std::endl;
        return 1;
    }
}
