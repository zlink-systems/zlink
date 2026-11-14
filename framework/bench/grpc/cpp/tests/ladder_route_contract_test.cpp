/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
// Reuse the actual target handlers; every operation below uses public Framework APIs.
#define main bench_target_entry
#include "../framework/bench_framework_cpp_server.cpp"
#undef main
#include <condition_variable>
#include <set>

namespace route_contract
{
using namespace std::chrono_literals;
using typed_t = ladder::typed_payload_t;

struct deferred_payload_t
{
    static constexpr const char *packet_name = "DeadlineProbe";
    std::vector<std::byte> body;
};
}

template <> struct zlink::json::dto_traits<route_contract::deferred_payload_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("body", &route_contract::deferred_payload_t::body)};
};

namespace route_contract
{
void require (bool value, const char *reason)
{
    if (!value)
        throw std::runtime_error (reason);
}
struct observations_t
{
    std::mutex mutex;
    std::condition_variable changed;
    int constructed = 0, destroyed = 0, received = 0, errors = 0;
    int probe_constructed = 0, probe_destroyed = 0;
    bool deferred_entered = false;
    std::set<std::thread::id> handlers;
};
struct scoped_metrics_t : server_metrics_t
{
    explicit scoped_metrics_t (observations_t &state) : state (state)
    {
        std::lock_guard lock (state.mutex);
        ++state.constructed;
        state.handlers.insert (std::this_thread::get_id ());
    }
    ~scoped_metrics_t ()
    {
        const auto stats = nlohmann::json::parse (snapshot_json ());
        std::lock_guard lock (state.mutex);
        ++state.destroyed;
        state.received += stats.at ("allPhaseMessages").get<int> ();
        state.errors += stats.at ("errors").get<int> ();
        state.changed.notify_all ();
    }
    observations_t &state;
};
struct scope_probe_t
{
    explicit scope_probe_t (observations_t &state) : state (state)
    {
        std::lock_guard lock (state.mutex);
        ++state.probe_constructed;
    }
    ~scope_probe_t ()
    {
        std::lock_guard lock (state.mutex);
        ++state.probe_destroyed;
        state.changed.notify_all ();
    }
    observations_t &state;
};
struct deferred_reply_t
{
    observations_t &state;
    fw::task_completion_source_t<deferred_payload_t> reply;
};
struct deferred_handler_t
{
    deferred_handler_t (deferred_reply_t &deferred, scope_probe_t &) : deferred (deferred) {}
    fw::task_t<deferred_payload_t> handle (const deferred_payload_t &request)
    {
        if (request.body.empty ())
            throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                              "deliberate contract handler failure");
        {
            std::lock_guard lock (deferred.state.mutex);
            deferred.state.deferred_entered = true;
            deferred.state.changed.notify_all ();
        }
        co_return co_await deferred.reply.task ();
    }
    deferred_reply_t &deferred;
};
class capture_client_t final : public fw::hosted_service_t
{
  public:
    explicit capture_client_t (fw::route_client_t *&client) : client (client) {}
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        client = &services.get_required<fw::route_client_t> ();
        co_return;
    }
    void stop () noexcept override {}
    fw::route_client_t *&client;
};

template <typename Predicate>
void await (observations_t &state, Predicate predicate, const char *reason)
{
    std::unique_lock lock (state.mutex);
    require (state.changed.wait_for (lock, 3s, predicate), reason);
}

void trace (fw::app_t &app, const char *side)
{
    if (const auto *prefix = std::getenv ("ZLINK_CPP_LADDER_TEST_LOG_PREFIX")) {
        app.logging ().use_file (std::string (prefix) + "." + side + ".flow");
        app.add_zlink_framework ().configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
    }
}

struct completion_t
{
    int count = 0;
    fw::framework_error_kind_t error = fw::framework_error_kind_t::internal_failure;
};
template <typename Request>
fw::task_t<void> observe (Request &request, completion_t &completion)
{
    try { (void) co_await request; }
    catch (const fw::framework_exception_t &error) { completion.error = error.kind (); }
    ++completion.count;
    co_return;
}

void run ()
{
    require (fw::detail::ablation::level () == 7, "route contract requires bundled L5_7");
    observations_t state;
    auto deferred = std::make_shared<deferred_reply_t> (state);
    stats_http_server_t target (std::vector<int>{}, bench_http_callback_t{});
    stats_http_server_t source (std::vector<int>{}, bench_http_callback_t{});
    trace (target.app (), "target");
    trace (source.app (), "source");
    auto &target_options = target.app ().add_zlink_framework ();
    target_options.services ().add_factory<server_metrics_t> (
      [&] (fw::service_provider_t &) -> std::shared_ptr<server_metrics_t> {
          return std::make_shared<scoped_metrics_t> (state);
      }, fw::service_lifetime_t::scoped);
    target_options.services ().add_factory<scope_probe_t> (
      [&] (fw::service_provider_t &) { return std::make_shared<scope_probe_t> (state); },
      fw::service_lifetime_t::scoped);
    target_options.services ().add_factory<deferred_reply_t> (
      [deferred] (fw::service_provider_t &) { return deferred; }, fw::service_lifetime_t::singleton);
    const auto rid = zlink::routing_id_t::from ("ladder-route-contract-target");
    std::string endpoint;
    {
        zlink::context_t reservation_context;
        zlink::router_socket_t reservation (reservation_context);
        reservation.bind ("tcp://127.0.0.1:0");
        endpoint = reservation.options ().last_endpoint ();
    }
    auto mesh = target_options.add_route_mesh ("bench");
    mesh.set_object_role (fw::object_role_t::none).listen (endpoint).set_routing_id (rid);
    mesh.add_route_request_handler<echo_handler_t<typed_t>, typed_t, typed_t> ("BenchPayload")
      .add_route_send_handler<command_handler_t<typed_t>, typed_t> ("BenchPayload")
      .add_route_request_handler<deferred_handler_t, deferred_payload_t, deferred_payload_t> ("DeadlineProbe");
    auto &source_options = source.app ().add_zlink_framework ();
    auto source_mesh = source_options.add_route_mesh ("bench");
    source_mesh.set_object_role (fw::object_role_t::none).listen ("tcp://127.0.0.1:0")
      .set_routing_id (zlink::routing_id_t::from ("ladder-route-contract-source"));
    source_mesh.peer_connections ().connect (rid, endpoint);
    fw::route_client_t *client = nullptr;
    source.app ().add_hosted_service (std::make_unique<capture_client_t> (client));
    require (target.start () && source.start () && client, "public Framework hosts failed to start");
    for (size_t size : {1024, 4096}) {
        auto expected = ladder::make_typed_payload (size, 123, phase_active, 456);
        auto request = client->request_to_node ("bench", rid, expected).timeout (2s).async<typed_t> ();
        require (static_cast<bool> (request.admitted ().result ()), "public request admission failed");
        const auto &reply = request.result ();
        require (reply && reply.value ().body () == expected.body (), "public typed request/reply changed its body");
        auto sent = client->send_to_node ("bench", rid,
          ladder::make_typed_payload (size, 123, phase_active, 456)).async ();
        require (static_cast<bool> (sent.result ()), "public typed send admission failed");
    }
    await (state, [&] { return state.destroyed == 4; }, "per-dispatch DI scopes did not close");
    {
        std::lock_guard lock (state.mutex);
        require (state.constructed == 4 && state.received == 4 && state.errors == 0,
                 "handlers did not receive a new DI scope for each record");
        require (!state.handlers.contains (std::this_thread::get_id ()), "handler ran on caller thread");
    }
    std::puts ("L5_7 public route: 1K/4K send/request reply valid; four activation scopes created/disposed");
    auto rejected = client->request_to_node ("bench", rid, deferred_payload_t{}).timeout (2s)
                      .async<deferred_payload_t> ();
    const auto &failure = rejected.result ();
    require (!failure && failure.error_kind () == fw::framework_error_kind_t::protocol_error,
             "public completion did not propagate handler failure");
    await (state, [&] { return state.probe_destroyed == 1; }, "failed handler scope did not close");
    auto timed = client->request_to_node ("bench", rid, deferred_payload_t{{std::byte{0x42}}})
                   .timeout (500ms).async<deferred_payload_t> ();
    completion_t completion;
    auto observer = observe (timed, completion);
    // Always release the application promise, including when an assertion fails.
    struct release_t {
        std::shared_ptr<deferred_reply_t> deferred;
        ~release_t () { deferred->reply.complete (fw::result_t<deferred_payload_t>::success ({})); }
    } release{deferred};
    await (state, [&] { return state.deferred_entered; }, "deferred handler was not activated");
    const auto terminal = observer.result_for (3s);
    require (terminal && *terminal, "public deadline completion did not finish");
    require (completion.count == 1 && completion.error == fw::framework_error_kind_t::deadline_exceeded,
             "deadline did not produce exactly one timeout completion");
    {
        std::lock_guard lock (state.mutex);
        require (state.probe_constructed == 2 && state.probe_destroyed == 1,
                 "in-flight handler lost its DI scope before physical completion");
    }
    deferred->reply.complete (fw::result_t<deferred_payload_t>::success (deferred_payload_t{{std::byte{0x42}}}));
    await (state, [&] { return state.probe_destroyed == 2; }, "late reply did not close the handler scope");
    require (!timed.result () && timed.result ().error_kind () == fw::framework_error_kind_t::deadline_exceeded
               && completion.count == 1, "late reply changed the operation terminal");
    auto expected = ladder::make_typed_payload (1024, 123, phase_active, 456);
    auto after = client->request_to_node ("bench", rid, expected).timeout (2s).async<typed_t> ();
    require (after.result () && after.result ().value ().body () == expected.body (),
             "request after expired operation failed");
    await (state, [&] { return state.destroyed == 5; }, "final handler scope did not close");
    std::puts ("L5_7 completion: handler error and deadline propagated; late reply kept terminal; next request valid");
    source.stop ();
    target.stop ();
    require (state.constructed == state.destroyed && state.probe_constructed == state.probe_destroyed,
             "public hosts leaked dispatch scopes");
}
}
int main ()
{
    try { route_contract::run (); }
    catch (const std::exception &error) {
        std::fprintf (stderr, "L5_7 route contract: %s\n", error.what ());
        return 1;
    }
}
