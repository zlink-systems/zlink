/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "login_admission.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>

namespace login_admission_queue
{
namespace fw = zlink::framework;
namespace connector = zlink::stream_connector;
using namespace std::chrono_literals;
using login_admission::setting;

inline constexpr auto timeout = 30s;
inline constexpr auto poll_interval = 10ms;

inline std::filesystem::path marker (const std::string &name)
{
    return std::filesystem::path (setting ("ZLINK_LOGIN_QUEUE_DIR")) / name;
}

inline void append (const std::filesystem::path &path, const std::string &value)
{
    std::ofstream file;
    file.exceptions (std::ios::failbit | std::ios::badbit);
    file.open (path, std::ios::app);
    file << value;
    file.close ();
}

struct server_markers_t
{
    const std::string role = setting ("ZLINK_LOGIN_QUEUE_ROLE");
    const std::string packet = setting ("ZLINK_LOGIN_QUEUE_PACKET");
    fw::task_completion_source_t<void> released;
};

class session_t final : public fw::packet_stream_session_t
{
  public:
    explicit session_t (server_markers_t &markers) : _markers (markers) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &context,
                                const fw::message_t &payload) override
    {
        if (context.packet_name != _markers.packet)
            throw std::runtime_error ("Unexpected login queue packet: " + context.packet_name);
        const auto value = payload.decode<std::string> ();
        const auto event = "|" + _markers.role + "|" + value + "\n";
        const auto events = marker ("handlers.events");
        append (events, "enter" + event);
        try {
            if (value == "held") {
                append (marker ("held"), "");
                co_await _markers.released.task ();
            }
            co_await stream.reply_packet (fw::message_t::from (value)).async ();
        }
        catch (...) {
            append (events, "leave" + event);
            throw;
        }
        append (events, "leave" + event);
    }

  private:
    server_markers_t &_markers;
};

inline int run_server (fw::app_t &app, server_markers_t &markers, int argc, char **argv)
{
    const auto ready = marker (markers.role + ".ready");
    const auto stop = marker (markers.role + ".stop");
    const auto held = marker ("held");
    const auto release = marker ("release");
    auto host = std::async (std::launch::async, [&] { return app.run (argc, argv); });
    try {
        bool ready_written = false;
        std::optional<std::chrono::steady_clock::time_point> release_deadline;
        bool release_observed = false;
        while (host.wait_for (poll_interval) != std::future_status::ready) {
            if (!ready_written && app.is_ready ()) {
                append (ready, "ready\n");
                ready_written = true;
            }
            if (std::filesystem::exists (stop))
                break;
            if (release_observed)
                continue;
            if (std::filesystem::exists (release)) {
                markers.released.complete (fw::result_t<void>::success ());
                release_observed = true;
            } else if (!release_deadline && std::filesystem::exists (held)) {
                release_deadline = std::chrono::steady_clock::now () + timeout;
            } else if (release_deadline && std::chrono::steady_clock::now () >= *release_deadline) {
                throw std::runtime_error ("Timed out waiting for release");
            }
        }
    }
    catch (...) {
        markers.released.complete (fw::result_t<void>::failure (
          fw::framework_error_kind_t::shutting_down, "Login queue marker observation failed"));
        app.request_stop ();
        throw;
    }
    markers.released.complete (fw::result_t<void>::failure (
      fw::framework_error_kind_t::shutting_down, "Login queue host stopped"));
    if (host.wait_for (0ms) != std::future_status::ready)
        app.request_stop ();
    return host.get ();
}

inline int run_connector (const std::string &first_endpoint, const std::string &peer_endpoint)
{
    connector::connector_options_t options;
    options.endpoint = first_endpoint;
    options.request_timeout = timeout;
    options.dispatch_mode = connector::dispatch_mode_t::immediate;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto held = connector::connector_factory_t::create (options);
    options.endpoint = peer_endpoint;
    auto second = connector::connector_factory_t::create (options);
    auto third = connector::connector_factory_t::create (options);
    if (!held.connect () || !second.connect () || !third.connect ())
        throw std::runtime_error ("Login queue connect failed");

    const auto observe_position = [] (auto &client, const std::string &name) {
        return client.on_admission_position (
          [path = marker (name + ".position")] (auto position, auto estimated_wait) {
              if (position < 1 || estimated_wait.count () < 0)
                  throw std::runtime_error ("Invalid admission position: " + path.string ());
              append (path, std::to_string (position) + "|"
                              + std::to_string (estimated_wait.count ()) + "\n");
          });
    };
    auto second_position = observe_position (second, "second");
    auto third_position = observe_position (third, "third");
    const auto packet = setting ("ZLINK_LOGIN_QUEUE_PACKET");
    const auto request = [&] (auto &client, std::string value) {
        auto outcome = std::make_shared<std::promise<void>> ();
        auto pending = outcome->get_future ();
        client.request (std::string (value))
          .packet_name (packet)
          .template submit<std::string> ([outcome, value] (auto reply) {
              if (!reply || reply.value () != value) {
                  outcome->set_exception (std::make_exception_ptr (std::runtime_error (
                    "Login queue request " + value + ": "
                    + (reply ? "unexpected reply " + reply.value () : reply.error ()->message))));
                  return;
              }
              outcome->set_value ();
          });
        return pending;
    };

    auto first = request (held, "held");
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    const auto held_marker = marker ("held");
    while (!std::filesystem::exists (held_marker)) {
        if (first.wait_for (poll_interval) == std::future_status::ready) {
            first.get ();
            throw std::runtime_error ("Request completed before held marker");
        }
        if (std::chrono::steady_clock::now () >= deadline)
            throw std::runtime_error ("Timed out waiting for held marker");
    }
    auto pending_second = request (second, "second");
    auto pending_third = request (third, "third");
    first.get ();
    pending_second.get ();
    pending_third.get ();
    for (auto *client : {&held, &second, &third}) {
        if (const auto closed = client->close (); !closed)
            throw std::runtime_error ("Login queue close failed: " + closed.error ()->message);
    }
    std::cout << "admitted held\nadmitted second\nadmitted third\n";
    return 0;
}
} // namespace login_admission_queue
