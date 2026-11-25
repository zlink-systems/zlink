/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

struct ws_shutdown_probe_t
{
    zlink::framework::task_completion_source_t<void> release;
    std::promise<zlink::framework::stream_t> entered;
    std::vector<std::string> callbacks;
    std::optional<zlink::framework::framework_error_kind_t> await_error;
    bool destroyed = false;
};

class ws_shutdown_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    explicit ws_shutdown_session_t (std::shared_ptr<ws_shutdown_probe_t> probe) :
        _probe (std::move (probe))
    {
    }

    ~ws_shutdown_session_t () override { _probe->destroyed = true; }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        _probe->entered.set_value (stream);
        try {
            co_await _probe->release.task ();
            _probe->callbacks.emplace_back ("connected");
        }
        catch (const zlink::framework::framework_exception_t &error) {
            _probe->await_error = error.kind ();
        }
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        _probe->callbacks.emplace_back ("disconnected");
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        _probe->callbacks.emplace_back ("error");
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        _probe->callbacks.emplace_back ("packet");
        co_return;
    }

  private:
    std::shared_ptr<ws_shutdown_probe_t> _probe;
};

int ws_session_shutdown (bool force)
{
    using namespace zlink::framework;
    using namespace std::chrono_literals;
    constexpr auto deadline = 2s;
    auto probe = std::make_shared<ws_shutdown_probe_t> ();
    auto entered = probe->entered.get_future ();
    auto app = std::make_unique<app_t> (app_t::create ());
    app->advanced ().services ().add_factory<ws_shutdown_session_t> (
      [probe] (service_provider_t &) { return std::make_unique<ws_shutdown_session_t> (probe); },
      service_lifetime_t::scoped);
    {
        auto &options = app->add_zlink_framework ();
        options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
        options.add_stream_node ("ws-shutdown")
          .bind ("ws://127.0.0.1:0")
          .register_session<ws_shutdown_session_t> ();
    }
    auto probe_received = std::make_shared<std::promise<void>> ();
    auto received = probe_received->get_future ();
    app->logging ()
      .use_file (force ? "ws-shutdown-force.flow" : "ws-shutdown-drain.flow")
      .set_min_level (log_level_t::debug)
      .use_callback_sink ([probe_received] (const log_record_t &record) {
          const auto has = [&record] (std::string_view key, std::string_view value) {
              return std::any_of (record.fields.begin (), record.fields.end (),
                                  [key, value] (const auto &field) {
                                      return field.key == key && field.value == value;
                                  });
          };
          if (has ("event_id", "zlink.message_flow") && has ("phase", "received")
              && has ("packet", "Probe"))
              probe_received->set_value ();
      });

    auto running = std::async (std::launch::async, [&] { return app->run (0, nullptr); });
    const auto ready_deadline = std::chrono::steady_clock::now () + 5s;
    while (!app->is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::yield ();
    auto fail_setup = [&] {
        probe->release.complete (result_t<void>::success ());
        app->stop ();
        std::cerr << "WS shutdown fixture failed to connect or send\n";
        return 1;
    };
    if (!app->is_ready ())
        return fail_setup ();
    zlink::stream_connector::connector_options_t options;
    {
        auto services = app->advanced ().services ().build_provider ();
        options.endpoint = services.get_required<framework_runtime_t> ()
                             .listener_status (listener_kind_t::stream, "ws-shutdown")
                             .endpoint;
    }
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    options.connect_timeout = 2s;
    auto client = zlink::stream_connector::connector_factory_t::create (options);
    auto transport_disconnected = std::make_shared<std::promise<void>> ();
    auto transport_closed = transport_disconnected->get_future ();
    auto subscription = client.on_disconnected (
      [transport_disconnected] (auto) { transport_disconnected->set_value (); });
    if (!client.connect () || entered.wait_for (5s) != std::future_status::ready)
        return fail_setup ();
    auto stream = entered.get ();
    client.send (zlink::stream_connector::packet_t{.name = "Probe", .payload = {'{', '}'}})
      .submit ();
    if (received.wait_for (5s) != std::future_status::ready)
        return fail_setup ();

    auto shutdown = app->shutdown (deadline);
    if (!force)
        probe->release.complete (result_t<void>::success ());
    auto result = shutdown.result_for (deadline + 500ms);
    if (!result) {
        std::cerr << "WS shutdown still pending after watchdog (deadline=2000 ms)\n";
        probe->release.complete (result_t<void>::success ());
        return 2;
    }
    const auto expected_outcome =
      force ? termination_outcome_t::force_stopped : termination_outcome_t::stopped;
    const auto expected_reason =
      force ? termination_reason_t::deadline_exceeded : termination_reason_t::none;
    const auto exit = running.get ();
    app.reset ();
    const bool source_completed = !force || probe->release.complete (result_t<void>::success ());
    const auto connected =
      std::count (probe->callbacks.begin (), probe->callbacks.end (), "connected");
    const auto disconnected =
      std::count (probe->callbacks.begin (), probe->callbacks.end (), "disconnected");
    const bool callbacks_ok =
      force
        ? probe->callbacks.empty () && probe->await_error == framework_error_kind_t::shutting_down
        : !probe->await_error && connected == 1 && disconnected == 1
            && probe->callbacks.front () == "connected"
            && probe->callbacks.back () == "disconnected";
    const auto late_send =
      force ? stream.write_packet (message_t::from (std::string ("late"))).async ().result_for (2s)
            : std::nullopt;
    const bool send_ok = !force
                         || (late_send && !*late_send
                             && late_send->error_kind () == framework_error_kind_t::shutting_down);
    const bool transport_ok = transport_closed.wait_for (2s) == std::future_status::ready;
    if (!*result || result->value ().outcome != expected_outcome
        || result->value ().reason != expected_reason || exit != 0 || !callbacks_ok || !send_ok
        || !transport_ok || !probe->destroyed || !source_completed) {
        std::cerr << "WS shutdown contract failed: force=" << force << " result=" << bool (*result)
                  << " exit=" << exit << " callbacks=" << probe->callbacks.size ()
                  << " await_cancelled=" << probe->await_error.has_value ()
                  << " late_send=" << send_ok << " transport_closed=" << transport_ok
                  << " session_destroyed=" << probe->destroyed
                  << " source_completed=" << source_completed << '\n';
        return 3;
    }
    std::cout << (force ? "ForceStopped/DeadlineExceeded" : "Stopped/None")
              << " with callback and transport assertions satisfied\n";
    return 0;
}

} // namespace

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string_view (argv[1]) == "--ws-shutdown-force")
        return ws_session_shutdown (true);
    if (argc == 2 && std::string_view (argv[1]) == "--ws-shutdown-drain")
        return ws_session_shutdown (false);
    return 1;
}
