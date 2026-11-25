/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/host_readiness.hpp"
#include "../support/location_reclaim_clock.hpp"

#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/location_auto_connect_host_service.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/client_server/client_server_location_runtime.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/owner_lease_time_store.hpp"

#include <gtest/gtest.h>
#include <zlink/framework.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
using zlink::framework::actor_location_t;
using zlink::framework::location_kind_t;
using zlink::framework::location_object_entry_t;
using zlink::framework::location_object_filter_t;
using zlink::framework::location_options_t;
using zlink::framework::location_owner_token_t;
using zlink::framework::location_page_request_t;
using zlink::framework::location_page_t;
using zlink::framework::location_role_t;
using zlink::framework::location_runtime_query_t;
using zlink::framework::location_runtime_status_t;
using zlink::framework::location_service_summary_filter_t;
using zlink::framework::location_service_summary_t;
using zlink::framework::location_topology_entry_t;
using zlink::framework::location_topology_filter_t;
using zlink::framework::location_topology_state_t;
using zlink::framework::location_write_intent_t;
using zlink::framework::location_write_status_t;
using zlink::framework::route_kind_t;
using zlink::framework::route_location_key_t;
using zlink::framework::spot_location_t;
using zlink::framework::runtime::in_memory_location_repository_t;
using zlink::framework::runtime::in_memory_location_store_t;
using zlink::framework::runtime::location_auto_connect_host_service_t;
using zlink::framework::runtime::location_runtime_t;
using zlink::framework::runtime::store_location_resolvers_t;
using zlink::framework::runtime::store_location_runtime_query_t;

class no_op_stream_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        return success ();
    }

  private:
    static zlink::framework::task_t<void> success ()
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

class echo_stream_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        return success ();
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &stream,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &payload) override
    {
        stream.reply_packet (payload).async ();
        return success ();
    }

  private:
    static zlink::framework::task_t<void> success ()
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }
};

class stop_after_delay_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit stop_after_delay_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

  private:
    zlink::framework::app_t *_app;
};

class stream_roundtrip_client_t final : public zlink::framework::hosted_service_t
{
  public:
    stream_roundtrip_client_t (zlink::framework::app_t &app,
                               zlink::framework::detail::stream_runtime_t runtime) :
        _app (&app), _runtime (std::move (runtime))
    {
    }
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        for (int attempt = 0; attempt < 80; ++attempt) {
            try {
                const auto endpoint =
                  services.get_required<zlink::framework::framework_runtime_t> ()
                    .listener_status (zlink::framework::listener_kind_t::stream, "stream-node")
                    .endpoint;
                run_once (endpoint);
                observed = true;
                break;
            }
            catch (const std::exception &) {
                last_error = std::current_exception ();
                std::this_thread::sleep_for (std::chrono::milliseconds (25));
            }
        }
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool observed = false;
    std::exception_ptr last_error;

    std::string last_error_message () const
    {
        if (!last_error) {
            return {};
        }
        try {
            std::rethrow_exception (last_error);
        }
        catch (const std::exception &error) {
            return error.what ();
        }
        catch (...) {
            return "unknown stream roundtrip failure";
        }
    }

  private:
    struct tcp_endpoint_t
    {
        std::string host;
        std::string port;
    };

    static tcp_endpoint_t parse_endpoint (const std::string &endpoint)
    {
        constexpr std::string_view prefix = "tcp://";
        if (endpoint.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("test stream endpoint must be tcp");
        }
        const auto host_start = prefix.size ();
        const auto separator = endpoint.rfind (':');
        if (separator == std::string::npos || separator <= host_start
            || separator + 1 >= endpoint.size ()) {
            throw std::runtime_error ("test stream endpoint is malformed");
        }
        return {endpoint.substr (host_start, separator - host_start),
                endpoint.substr (separator + 1)};
    }

    std::vector<std::uint8_t> make_frame (const zlink::framework::detail::stream_header_t &header,
                                          const zlink::message_t &payload)
    {
        auto encoded_header = _runtime.encode_header (header);
        if (!encoded_header) {
            throw std::runtime_error ("failed to encode test stream header");
        }
        const auto payload_bytes = payload.to_bytes ();
        std::vector<std::uint8_t> frame;
        frame.reserve (6 + encoded_header.value ().size () + payload_bytes.size ());
        const auto header_size = encoded_header.value ().size ();
        frame.push_back (static_cast<std::uint8_t> ((header_size >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (header_size & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 24) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 16) & 0xff));
        frame.push_back (static_cast<std::uint8_t> ((payload_bytes.size () >> 8) & 0xff));
        frame.push_back (static_cast<std::uint8_t> (payload_bytes.size () & 0xff));
        frame.insert (frame.end (), encoded_header.value ().begin (),
                      encoded_header.value ().end ());
        frame.insert (frame.end (), payload_bytes.begin (), payload_bytes.end ());
        return frame;
    }

    zlink::message_t read_reply (boost::asio::ip::tcp::socket &socket)
    {
        std::array<std::uint8_t, 6> prefix{};
        boost::asio::read (socket, boost::asio::buffer (prefix));
        const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
        const auto payload_size = (static_cast<std::size_t> (prefix[2]) << 24)
                                  | (static_cast<std::size_t> (prefix[3]) << 16)
                                  | (static_cast<std::size_t> (prefix[4]) << 8)
                                  | static_cast<std::size_t> (prefix[5]);
        std::vector<std::uint8_t> header_bytes (header_size);
        std::vector<std::uint8_t> payload_bytes (payload_size);
        boost::asio::read (socket, boost::asio::buffer (header_bytes));
        boost::asio::read (socket, boost::asio::buffer (payload_bytes));
        auto header = _runtime.decode_header (header_bytes);
        if (!header
            || header.value ().kind () != zlink::framework::detail::stream_message_kind_t::response
            || header.value ().request_seq () != 91
            || header.value ().codec () != zlink::framework::stream_codec_t::raw) {
            if (!header) {
                throw std::runtime_error ("failed to decode stream reply header");
            }
            throw std::runtime_error (
              "unexpected stream reply header kind="
              + std::to_string (static_cast<int> (header.value ().kind ())) + " seq="
              + (header.value ().request_seq () ? std::to_string (*header.value ().request_seq ())
                                                : std::string ("none"))
              + " packet=" + std::string (header.value ().packet_name ()));
        }
        return zlink::message_t::from (payload_bytes);
    }

    void run_once (const std::string &bound_endpoint)
    {
        const auto endpoint = parse_endpoint (bound_endpoint);
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver (io);
        boost::asio::ip::tcp::socket socket (io);
        boost::asio::connect (socket, resolver.resolve (endpoint.host, endpoint.port));
        zlink::framework::detail::stream_header_t header (
          zlink::framework::detail::stream_message_kind_t::request,
          zlink::framework::stream_codec_t::raw,
          zlink::framework::detail::stream_header_flags_t::has_request_seq, 91, "stream.roundtrip");
        header.with_correlation_id ("stream-roundtrip");
        auto frame = make_frame (header, zlink::message_t::from (std::string ("stream-payload")));
        boost::asio::write (socket, boost::asio::buffer (frame));
        if (read_reply (socket).to_string () != "stream-payload") {
            throw std::runtime_error ("unexpected stream reply payload");
        }
        boost::system::error_code ignored;
        socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket.close (ignored);
    }

    zlink::framework::app_t *_app;
    zlink::framework::detail::stream_runtime_t _runtime;
};
} // namespace

namespace
{
TEST (ZLinkFrameworkStoreLocationResolvers, AppStreamHostStartsAndStopsTcpListener)
{
    auto app = zlink::framework::app_t::create ();

    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_stream_node ("stream-node")
          .bind ("tcp://127.0.0.1:0")
          .register_session<echo_stream_session_t> ();
    });
    auto client = std::make_unique<stream_roundtrip_client_t> (
      app, zlink::framework::detail::stream_runtime_t::from (app.advanced ().zlink ()));
    auto *client_ptr = client.get ();
    app.add_hosted_service (std::move (client));

    EXPECT_EQ (0, app.run (0, nullptr));
    ASSERT_NE (nullptr, client_ptr);
    EXPECT_TRUE (client_ptr->observed) << client_ptr->last_error_message ();
}

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
TEST (ZLinkFrameworkStoreLocationResolvers, ForcedStreamCloseCannotOutliveReader)
{
    namespace fw = zlink::framework;
    fw::detail::configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { fw::detail::shutdown_stream_dispatch_executor (); }
    } executor_guard;
    fw::service_collection_t services;
    fw::handler_registry_t handlers;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    services.add_singleton<fw::detail::actor_gateway_runtime_t> ();
    services.add_factory<fw::session_actor_manager_t> (
      [] (fw::service_provider_t &scope) {
          return std::make_unique<fw::session_actor_manager_t> (
            scope.get_required<fw::detail::actor_gateway_runtime_t> ().manager ());
      },
      fw::service_lifetime_t::scoped);
    services.add_singleton<echo_stream_session_t> ();
    fw::zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_stream_node ("shutdown-race")
      .bind ("tcp://127.0.0.1:0")
      .register_session ("shutdown-session");
    options.apply ();
    auto provider = services.build_provider ();
    auto runtime = fw::detail::stream_runtime_t::from (builder);
    auto statuses = std::make_shared<fw::runtime::listener_status_registry_t> ();
    fw::runtime::stream_host_service_t host (
      std::make_shared<fw::runtime::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{"shutdown-session",
        [] (fw::service_provider_t &scope) -> fw::packet_stream_session_t & {
            return scope.get_required<echo_stream_session_t> ();
        }}},
      std::chrono::seconds (30), nullptr, {}, statuses);
    host.start (provider);

    std::promise<std::pair<std::weak_ptr<void>, std::weak_ptr<void>>> registered;
    auto observed = registered.get_future ();
    std::promise<bool> paused;
    auto reached_pause = paused.get_future ();
    std::promise<void> resume;
    auto resumed = resume.get_future ().share ();
    auto force_close = fw::runtime::stream_host_service_test_access_t::observe_shutdown (
      host,
      {[&] (auto listener, auto connection) { registered.set_value ({listener, connection}); },
       [&] (bool in_lane) {
           paused.set_value (in_lane);
           resumed.wait ();
       }});
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket client (io);
    const auto endpoint = statuses->find (fw::listener_kind_t::stream, "shutdown-race")->endpoint;
    const auto port =
      static_cast<unsigned short> (std::stoi (endpoint.substr (endpoint.rfind (':') + 1)));
    client.connect ({boost::asio::ip::make_address ("127.0.0.1"), port});
    ASSERT_EQ (std::future_status::ready, observed.wait_for (std::chrono::seconds (5)));
    auto [listener, connection] = observed.get ();
    auto forced = std::async (std::launch::async, std::move (force_close));
    ASSERT_EQ (std::future_status::ready, reached_pause.wait_for (std::chrono::seconds (5)));
    const bool in_lane = reached_pause.get ();
    client.close ();
    // Outside the registry lane the reader can finish before the forced post.
    // Inside it, unregister must wait for the submission to finish.
    if (!in_lane)
        host.stop ();
    resume.set_value ();
    forced.get ();
    host.stop ();
    EXPECT_TRUE (connection.expired ());
    EXPECT_TRUE (listener.expired ());
    provider.close ();
}
#endif
} // namespace
