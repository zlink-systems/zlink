/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

namespace login_admission
{
namespace fw = zlink::framework;
namespace connector = zlink::stream_connector;

inline std::string setting (const char *name)
{
    const auto *value = std::getenv (name);
    if (!value)
        throw std::runtime_error (std::string ("Missing login admission setting ") + name);
    return value;
}

inline std::filesystem::path marker (const char *name)
{
    return std::filesystem::path (setting ("ZLINK_LOGIN_DIR")) / name;
}

inline std::chrono::milliseconds timeout ()
{
    return std::chrono::milliseconds (std::stoi (setting ("ZLINK_LOGIN_TIMEOUT_MS")));
}

inline void wait_for (const char *name)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout ();
    const auto path = marker (name);
    while (!std::filesystem::exists (path)) {
        if (std::chrono::steady_clock::now () >= deadline)
            throw std::runtime_error (std::string ("Timed out waiting for ") + name);
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

class barrier_t
{
  public:
    barrier_t () :
        _watcher ([this, release = marker ("release")] (std::stop_token stop) {
            while (!stop.stop_requested () && !std::filesystem::exists (release))
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            _release.complete (fw::result_t<void>::success ());
        })
    {
    }

    fw::task_t<void> wait () const { return _release.task (); }

  private:
    fw::task_completion_source_t<void> _release;
    std::jthread _watcher;
};

class session_t final : public fw::packet_stream_session_t
{
  public:
    explicit session_t (barrier_t &barrier) : _barrier (barrier) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &,
                                const fw::message_t &payload) override
    {
        const auto request = payload.decode<std::string> ();
        std::ofstream (marker ("server.events"), std::ios::app)
          << request << '|' << stream.session_id () << '\n';
        if (request == setting ("ZLINK_LOGIN_HELD")) {
            std::ofstream (marker ("held")).close ();
            co_await _barrier.wait ();
        }
        co_await stream.reply_packet (fw::message_t::from (request)).async ();
    }

  private:
    barrier_t &_barrier;
};

inline int run_connector (const std::string &endpoint)
{
    connector::connector_options_t options;
    options.endpoint = endpoint;
    options.request_timeout = timeout ();
    options.dispatch_mode = connector::dispatch_mode_t::immediate;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto held = connector::connector_factory_t::create (options);
    auto contender = connector::connector_factory_t::create (options);
    if (!held.connect () || !contender.connect ())
        throw std::runtime_error ("Login admission connect failed");

    auto waiting = contender.on_admission_waiting ([] (const auto &event) {
        std::ofstream (marker ("waiting"), std::ios::app)
          << event.request_packet_name << '|' << event.retry_after.count () << '\n';
    });
    const auto packet = setting ("ZLINK_LOGIN_PACKET");
    const auto request = [&] (auto &client, const char *name) {
        const auto value = setting (name);
        auto reply =
          client.request (std::string (value)).packet_name (packet).template submit<std::string> ();
        if (!reply)
            throw std::runtime_error ("Login request failed: " + reply.error ()->message);
        if (reply.value () != value)
            throw std::runtime_error ("Unexpected login reply: " + reply.value ());
    };

    request (contender, "ZLINK_LOGIN_CONTROL");
    auto pending = std::async (std::launch::async, [&] { request (held, "ZLINK_LOGIN_HELD"); });
    wait_for ("held");
    request (contender, "ZLINK_LOGIN_CONTENDER");
    pending.get ();
    return 0;
}
} // namespace login_admission
