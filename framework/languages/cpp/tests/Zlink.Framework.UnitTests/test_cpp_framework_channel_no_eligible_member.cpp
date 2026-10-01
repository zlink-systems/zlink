/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"

#include <gtest/gtest.h>

#include <string>
#include <chrono>
#include <future>
#include <thread>

/*
 * 06-framework-api "no eligible select-one member": applying eligibility and
 * drain can leave a select-one channel with no member to pick while the send
 * path and its connection are still there. That ends as unavailable, not
 * not_found, and a request and a one-way send agree. A node-direct target that
 * is genuinely absent still ends as not_found.
 */
namespace
{

using zlink::framework::framework_error_kind_t;
using zlink::framework::runtime::messaging::map_channel_submit_result_exception;
using zlink::framework::runtime::messaging::map_submit_result_exception;

using namespace std::chrono_literals;
namespace fw = zlink::framework;

struct selection_handler_t
{
    using request_type = std::string;
    using reply_type = std::string;
    std::string handle (const std::string &value) { return value; }
};

class running_app_t
{
  public:
    explicit running_app_t (fw::app_t &app) :
        _app (app), _run (std::async (std::launch::async, [&app] {
            char name[] = "channel-selection-contract";
            char *arguments[] = {name, nullptr};
            return app.run (1, arguments);
        }))
    {
    }
    ~running_app_t ()
    {
        _app.stop ();
        _run.wait ();
    }
    bool wait_ready ()
    {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!_app.is_ready () && std::chrono::steady_clock::now () < deadline) {
            if (_run.wait_for (0ms) == std::future_status::ready)
                return false;
            std::this_thread::yield ();
        }
        return _app.is_ready ();
    }

  private:
    fw::app_t &_app;
    std::future<int> _run;
};

TEST (CppFrameworkChannelNoEligibleMember, PublicRouteMeshMissingSnapshotEndsNotFound)
{
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        auto mesh = options.add_route_mesh ("selection-missing");
        mesh.set_routing_id (zlink::routing_id_t::from ("selection-missing-client"));
        mesh.set_bind_host ("127.0.0.1").set_advertise_host ("127.0.0.1").listen ();
        mesh.channel ("selection-missing").client ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    running_app_t running (app);
    ASSERT_TRUE (running.wait_ready ());
    auto &channels = provider.get_required<fw::route_client_t> ();
    const auto send =
      channels.send_to_channel ("selection-missing", std::string ("request")).async ().result ();
    const auto request = channels.request_to_channel ("selection-missing", std::string ("request"))
                           .timeout (1s)
                           .async<std::string> ()
                           .result ();
    EXPECT_EQ (framework_error_kind_t::not_found, send.error_kind ())
      << (send.error () ? send.error ()->what () : "no error");
    EXPECT_EQ (framework_error_kind_t::not_found, request.error_kind ())
      << (request.error () ? request.error ()->what () : "no error");
}

TEST (CppFrameworkChannelNoEligibleMember, PublicRouteMeshWeightZeroEndsUnavailable)
{
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto server = fw::app_t::create ();
    server.add_zlink_framework ([store] (fw::zlink_framework_options_t &options) {
        options.add_location_store (store);
        auto mesh = options.add_route_mesh ("selection-zero");
        mesh.set_routing_id (zlink::routing_id_t::from ("selection-zero-server"));
        mesh.set_bind_host ("127.0.0.1").set_advertise_host ("127.0.0.1").listen ();
        mesh.channel ("selection-zero")
          .server ()
          .set_weight (0)
          .add_request_handler<selection_handler_t, std::string, std::string> ();
    });
    auto server_provider = server.advanced ().services ().build_provider ();
    running_app_t running_server (server);
    ASSERT_TRUE (running_server.wait_ready ());
    const auto endpoint = server_provider.get_required<fw::framework_runtime_t> ()
                            .listener_status (fw::listener_kind_t::route_mesh, "selection-zero")
                            .endpoint;
    ASSERT_FALSE (endpoint.empty ());
    auto client = fw::app_t::create ();
    client.add_zlink_framework ([store, &endpoint] (fw::zlink_framework_options_t &options) {
        options.add_location_store (store);
        auto mesh = options.add_route_mesh ("selection-zero");
        mesh.set_routing_id (zlink::routing_id_t::from ("selection-zero-client"));
        mesh.set_bind_host ("127.0.0.1").set_advertise_host ("127.0.0.1").listen ();
        mesh.peer_connections ().connect (endpoint);
        mesh.channel ("selection-zero").client ();
    });
    auto provider = client.advanced ().services ().build_provider ();
    running_app_t running_client (client);
    ASSERT_TRUE (running_client.wait_ready ());
    auto &routes = provider.get_required<fw::route_mesh_runtime_t> ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (routes.snapshot ("selection-zero").ready_peer_count == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    ASSERT_GT (routes.snapshot ("selection-zero").ready_peer_count, 0u);
    auto &channels = provider.get_required<fw::route_client_t> ();
    const auto send =
      channels.send_to_channel ("selection-zero", std::string ("request")).async ().result ();
    const auto request = channels.request_to_channel ("selection-zero", std::string ("request"))
                           .timeout (1s)
                           .async<std::string> ()
                           .result ();
    EXPECT_EQ (framework_error_kind_t::unavailable, send.error_kind ())
      << (send.error () ? send.error ()->what () : "no error");
    EXPECT_EQ (framework_error_kind_t::unavailable, request.error_kind ())
      << (request.error () ? request.error ()->what () : "no error");
}

TEST (CppFrameworkChannelNoEligibleMember,
      PublicClientServerMissingTargetExpiresAtConfiguredTimeout)
{
    auto app = fw::app_t::create ();
    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        options.add_client_server_channel ("selection-client-missing")
          .client ()
          .set_send_timeout (100ms);
    });
    auto provider = app.advanced ().services ().build_provider ();
    running_app_t running (app);
    ASSERT_TRUE (running.wait_ready ());
    auto &channels = provider.get_required<fw::channel_client_t> ();
    const auto send =
      channels.send ("selection-client-missing", std::string ("request")).async ().result ();
    const auto request = channels.request ("selection-client-missing", std::string ("request"))
                           .timeout (50ms)
                           .async<std::string> ()
                           .result ();
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, send.error_kind ())
      << (send.error () ? send.error ()->what () : "no error");
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, request.error_kind ())
      << (request.error () ? request.error ()->what () : "no error");
}

TEST (CppFrameworkChannelNoEligibleMember, PublicClientServerWeightZeroEndsUnavailable)
{
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto server = fw::app_t::create ();
    server.add_zlink_framework ([store] (fw::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.add_client_server_channel ("selection-client-zero")
          .server ()
          .listen ()
          .set_weight (0)
          .add_request_handler<selection_handler_t, std::string, std::string> ();
    });
    auto server_provider = server.advanced ().services ().build_provider ();
    running_app_t running_server (server);
    ASSERT_TRUE (running_server.wait_ready ());
    const auto endpoint =
      server_provider.get_required<fw::framework_runtime_t> ()
        .listener_status (fw::listener_kind_t::client_server, "selection-client-zero")
        .endpoint;
    ASSERT_FALSE (endpoint.empty ());
    auto client = fw::app_t::create ();
    client.add_zlink_framework ([store, &endpoint] (fw::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.add_client_server_channel ("selection-client-zero")
          .client ()
          .set_send_timeout (1s)
          .connect (endpoint);
    });
    auto provider = client.advanced ().services ().build_provider ();
    running_app_t running_client (client);
    ASSERT_TRUE (running_client.wait_ready ());
    auto &runtime = provider.get_required<fw::client_server_runtime_t> ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    auto status = runtime.snapshot ("selection-client-zero");
    while (status.targets.empty () && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
        status = runtime.snapshot ("selection-client-zero");
    }
    ASSERT_FALSE (status.targets.empty ());
    EXPECT_FALSE (status.is_ready);
    ASSERT_EQ (0, status.targets.front ().weight);
    auto &channels = provider.get_required<fw::channel_client_t> ();
    const auto started = std::chrono::steady_clock::now ();
    const auto send =
      channels.send ("selection-client-zero", std::string ("request")).async ().result ();
    const auto request = channels.request ("selection-client-zero", std::string ("request"))
                           .timeout (1s)
                           .async<std::string> ()
                           .result ();
    EXPECT_EQ (framework_error_kind_t::unavailable, send.error_kind ())
      << (send.error () ? send.error ()->what () : "no error");
    EXPECT_EQ (framework_error_kind_t::unavailable, request.error_kind ())
      << (request.error () ? request.error ()->what () : "no error");
    EXPECT_LT (std::chrono::steady_clock::now () - started, 500ms);
}

TEST (CppFrameworkChannelNoEligibleMember, ChannelNotFoundEndsUnavailable)
{
    const auto mapped = map_channel_submit_result_exception (zlink::submit_result_t::not_found,
                                                             "channel had no eligible member");
    EXPECT_EQ (framework_error_kind_t::unavailable, mapped.kind ());
}

TEST (CppFrameworkChannelNoEligibleMember, NodeDirectNotFoundStaysNotFound)
{
    const auto mapped = map_submit_result_exception (zlink::submit_result_t::not_found,
                                                     "node direct target is absent");
    EXPECT_EQ (framework_error_kind_t::not_found, mapped.kind ());
}

TEST (CppFrameworkChannelNoEligibleMember, ChannelKeepsEveryOtherSubmitResult)
{
    EXPECT_EQ (
      framework_error_kind_t::unavailable,
      map_channel_submit_result_exception (zlink::submit_result_t::not_connected, "not connected")
        .kind ());
    EXPECT_EQ (
      framework_error_kind_t::shutting_down,
      map_channel_submit_result_exception (zlink::submit_result_t::terminated, "terminated")
        .kind ());
    EXPECT_EQ (
      framework_error_kind_t::deadline_exceeded,
      map_channel_submit_result_exception (zlink::submit_result_t::backpressured, "backpressured")
        .kind ());
    EXPECT_EQ (
      framework_error_kind_t::rejected,
      map_channel_submit_result_exception (zlink::submit_result_t::not_admitted, "not admitted")
        .kind ());
}

} // namespace
