/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"

using namespace zlink::framework;

namespace
{
class counting_store_t final : public location_store_t
{
  public:
    std::atomic_size_t calls{0};
    task_t<store_read_result_t> read (store_key_t key) override
    {
        ++calls;
        return store.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        ++calls;
        return store.write (std::move (request));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        ++calls;
        return store.scan (std::move (request));
    }

  private:
    runtime::in_memory_location_store_t store;
};

template <typename T> void expect_prestart (T pending)
{
    ASSERT_TRUE (pending.await_ready ());
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::invalid_operation);
}
}

TEST (PrestartCall, ConfiguredMessagingRejectsAtTerminal)
{
    auto app = app_t::create ();
    app.add_zlink_framework ([] (auto &) {});
    auto provider = app.advanced ().services ().build_provider ();
    auto &route = provider.get_required<route_client_t> ();
    auto &channel = provider.get_required<channel_client_t> ();
    auto &publisher = provider.get_required<publisher_t> ();
    const auto rid = zlink::routing_id_t::from ("target");
    const auto spot = spot_id_t ("target");
    expect_prestart (route.send_to_node ("mesh", rid, std::string ("x")).async ());
    expect_prestart (route.request_to_node ("mesh", rid, std::string ("x")).async<std::string> ());
    expect_prestart (route.send_to_channel ("channel", std::string ("x")).async ());
    expect_prestart (route.request_to_channel ("channel", std::string ("x")).async<std::string> ());
    expect_prestart (route.send_to_spot (spot, std::string ("x")).async ());
    expect_prestart (route.request_to_spot (spot, std::string ("x")).async<std::string> ());
    expect_prestart (route.send_to_spot (spot, std::string ("x")).instance_spot ().async ());
    expect_prestart (
      route.request_to_spot (spot, std::string ("x")).instance_spot ().async<std::string> ());
    expect_prestart (channel.send ("channel", std::string ("x")).async ());
    expect_prestart (channel.request ("channel", std::string ("x")).async<std::string> ());
    expect_prestart (publisher.publish ("channel", "topic", std::string ("x")).async ());
}

TEST (PrestartCall, ActorAndLogicalPublishDoNotReadStore)
{
    auto app = app_t::create ();
    auto store = std::make_shared<counting_store_t> ();
    app.add_zlink_framework ([&] (auto &options) {
        options.add_location_store (store);
        options.add_route_mesh ("mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("source"))
          .objects ()
          .server ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &actor = provider.get_required<actor_client_t> ();
    auto &publisher = provider.get_required<spot_publisher_client_t> ();
    store->calls = 0;
    expect_prestart (actor.send (actor_id_t ("actor"), std::string ("x")).async ());
    expect_prestart (actor.request (actor_id_t ("actor"), std::string ("x")).async<std::string> ());
    expect_prestart (publisher.publish ("channel", "topic", std::string ("x")).async ());
    EXPECT_EQ (store->calls.load (), 0u);
}

namespace
{
class counting_resolver_t final : public runtime::spot_address_resolver_t
{
  public:
    std::size_t calls = 0;
    task_t<std::optional<runtime::spot_address_t>> resolve_spot_address (std::string,
                                                                         std::string id) override
    {
        ++calls;
        runtime::spot_address_t address;
        address.mesh_name = "mesh";
        address.node_rid = zlink::routing_id_t::from ("target");
        address.spot_id = std::move (id);
        address.spot_generation = 1;
        co_return address;
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
};

class startup_probe_t final : public hosted_service_t
{
  public:
    explicit startup_probe_t (std::function<void (service_provider_t &)> probe) :
        probe (std::move (probe))
    {
    }
    task_t<void> start (service_provider_t &provider) override
    {
        probe (provider);
        co_return;
    }
    void stop () noexcept override {}

  private:
    std::function<void (service_provider_t &)> probe;
};

runtime::messaging::message_parts_t reply_parts (serializer_registry_t &serializers)
{
    runtime::messaging::client_call_codec_t codec;
    return codec.encode_payload_parts (
      codec.create_envelope (runtime::messaging::message_kind_t::response, "mesh", "reply"),
      serializers.get<std::string> ().serialize_with_content_type ("ok"));
}
}

TEST (PrestartCall, AdmissionPrecedesStoreAndTransportAndAllowsHostedStartup)
{
    auto app = app_t::create ();
    auto &builder = app.advanced ().zlink ();
    auto configured_channel = builder.channel ("channel");
    configured_channel.enable_client ().connect ("tcp://127.0.0.1:1");
    configured_channel.enable_publisher (false).bind ("tcp://127.0.0.1:0");
    app.add_zlink_framework ([] (auto &) {});
    auto provider = app.advanced ().services ().build_provider ();
    auto &serializers = provider.get_required<serializer_registry_t> ();
    auto &route = provider.get_required<route_client_t> ();
    auto &channel = provider.get_required<channel_client_t> ();
    auto &publisher = provider.get_required<publisher_t> ();
    auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
    auto resolver = std::make_shared<counting_resolver_t> ();
    runtime.bind_spot_address_resolver (resolver);
    std::size_t transports = 0;
    using parts_t = runtime::messaging::message_parts_t;
    runtime.bind_mesh_node_transport (
      "mesh",
      [&] (const auto &, auto) {
          ++transports;
          return task_t<result_t<void>> (
            result_t<result_t<void>>::success (result_t<void>::success ()));
      },
      [&] (const auto &, auto, auto, auto receiver) {
          ++transports;
          receiver.admitted->complete (result_t<void>::success ());
          receiver.complete (result_t<result_t<parts_t>>::success (
            result_t<parts_t>::success (reply_parts (serializers))));
      });
    runtime.bind_spot_mesh_transport (
      "mesh",
      [&] (const auto &, const auto &, auto, auto) {
          ++transports;
          return task_t<result_t<void>> (
            result_t<result_t<void>>::success (result_t<void>::success ()));
      },
      [&] (const auto &, const auto &, auto, auto, auto) {
          ++transports;
          return request_task_t<result_t<parts_t>> (result_t<result_t<parts_t>>::success (
            result_t<parts_t>::success (reply_parts (serializers))));
      });
    const auto raw_reply = [&] {
        return detail::encoded_payload_to_raw (serializers.get<std::string> ().serialize ("ok"));
    };
    runtime.bind_client_server_transport (
      "channel",
      [&] (auto, auto, auto, auto) {
          ++transports;
          return task_t<void> (result_t<void>::success ());
      },
      [&] (auto, auto, auto, auto, auto) {
          ++transports;
          return request_task_t<zlink::message_t> (
            result_t<zlink::message_t>::success (raw_reply ()));
      });
    runtime.bind_fanout_transport ("channel", [&] (auto, auto, auto, auto, auto) {
        ++transports;
        return task_t<void> (result_t<void>::success ());
    });
    runtime.bind_instance_spot_activator (
      [&] (const auto &, const auto &, const auto &, const auto &, auto, auto, const auto &, auto) {
          ++transports;
          return task_t<result_t<void>> (
            result_t<result_t<void>>::success (result_t<void>::success ()));
      },
      [&] (const auto &, const auto &, const auto &, auto, auto, auto, auto, auto) {
          ++transports;
          return request_task_t<zlink::message_t> (
            result_t<zlink::message_t>::success (raw_reply ()));
      });
    const auto rid = zlink::routing_id_t::from ("target");
    auto before = route.request_to_node ("mesh", rid, std::string ("x")).async<std::string> ();
    expect_prestart (std::move (before));
    expect_prestart (route.send_to_node ("mesh", rid, std::string ("x")).async ());
    expect_prestart (route.send_to_channel ("channel", std::string ("x")).async ());
    expect_prestart (route.request_to_channel ("channel", std::string ("x")).async<std::string> ());
    expect_prestart (route.send_to_spot ("spot", std::string ("x")).async ());
    expect_prestart (route.request_to_spot ("spot", std::string ("x")).async<std::string> ());
    expect_prestart (route.send_to_spot ("spot", std::string ("x")).instance_spot ().async ());
    expect_prestart (
      route.request_to_spot ("spot", std::string ("x")).instance_spot ().async<std::string> ());
    expect_prestart (channel.send ("channel", std::string ("x")).async ());
    expect_prestart (channel.request ("channel", std::string ("x")).async<std::string> ());
    expect_prestart (publisher.publish ("channel", "topic", std::string ("x")).async ());
    EXPECT_EQ (resolver->calls, 0u);
    EXPECT_EQ (transports, 0u);
    auto node_send = route.send_to_node ("mesh", rid, std::string ("x"));
    auto route_send = route.send_to_channel ("channel", std::string ("x"));
    auto spot_send = route.send_to_spot ("spot", std::string ("x"));
    auto instance_send = route.send_to_spot ("spot", std::string ("x"));
    instance_send.instance_spot ();
    auto channel_send = channel.send ("channel", std::string ("x"));
    auto fanout_publish = publisher.publish ("channel", "topic", std::string ("x"));
    expect_prestart (node_send.async ());
    expect_prestart (route_send.async ());
    expect_prestart (spot_send.async ());
    expect_prestart (instance_send.async ());
    expect_prestart (channel_send.async ());
    expect_prestart (fanout_publish.async ());
    auto created_before_start = route.request_to_node ("mesh", rid, std::string ("x"));
    auto rejected = route.request_to_node ("mesh", rid, std::string ("x")).async<std::string> ();
    bool visited = false;
    app.add_hosted_service (std::make_unique<startup_probe_t> ([&] (auto &) {
        visited = true;
        std::size_t admissions = 0;
        runtime.bind_runtime_admission ([&] {
            ++admissions;
            return std::pair{false, true};
        });
        EXPECT_EQ (rejected.result ().error_kind (), framework_error_kind_t::invalid_operation);
        EXPECT_EQ (created_before_start.async<std::string> ().result ().value (), "ok");
        EXPECT_TRUE (route.send_to_node ("mesh", rid, std::string ("x")).async ().result ());
        EXPECT_TRUE (route.send_to_channel ("channel", std::string ("x")).async ().result ());
        EXPECT_EQ (route.request_to_channel ("channel", std::string ("x"))
                     .async<std::string> ()
                     .result ()
                     .value (),
                   "ok");
        EXPECT_TRUE (route.send_to_spot ("spot", std::string ("x")).async ().result ());
        EXPECT_EQ (route.request_to_spot ("spot", std::string ("x"))
                     .async<std::string> ()
                     .result ()
                     .value (),
                   "ok");
        EXPECT_TRUE (
          route.send_to_spot ("spot", std::string ("x")).instance_spot ().async ().result ());
        EXPECT_EQ (route.request_to_spot ("spot", std::string ("x"))
                     .instance_spot ()
                     .async<std::string> ()
                     .result ()
                     .value (),
                   "ok");
        EXPECT_TRUE (channel.send ("channel", std::string ("x")).async ().result ());
        EXPECT_EQ (
          channel.request ("channel", std::string ("x")).async<std::string> ().result ().value (),
          "ok");
        EXPECT_TRUE (publisher.publish ("channel", "topic", std::string ("x")).async ().result ());
        EXPECT_EQ (resolver->calls, 4u);
        EXPECT_EQ (transports, 11u);
        EXPECT_EQ (admissions, 11u);
        EXPECT_TRUE (node_send.async ().result ());
        EXPECT_TRUE (route_send.async ().result ());
        EXPECT_TRUE (spot_send.async ().result ());
        EXPECT_TRUE (instance_send.async ().result ());
        EXPECT_TRUE (channel_send.async ().result ());
        EXPECT_TRUE (fanout_publish.async ().result ());
        EXPECT_EQ (admissions, 17u);
        runtime.bind_runtime_admission ({});
        app.stop ();
    }));
    EXPECT_EQ (app.run (0, nullptr), 0);
    EXPECT_TRUE (visited);
}

TEST (PrestartCall, ActorAndLogicalPublishUseNormalResultsDuringHostedStartup)
{
    auto app = app_t::create ();
    auto store = std::make_shared<counting_store_t> ();
    app.add_zlink_framework ([&] (auto &options) {
        options.add_location_store (store);
        options.add_route_mesh ("mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("source"))
          .objects ()
          .server ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &logical = provider.get_required<spot_publisher_client_t> ();
    auto logical_call = logical.publish ("channel", "topic", std::string ("x"));
    expect_prestart (logical_call.async ());
    bool visited = false;
    app.add_hosted_service (std::make_unique<startup_probe_t> ([&] (auto &provider) {
        visited = true;
        auto &actor = provider.template get_required<actor_client_t> ();
        auto &publisher = provider.template get_required<spot_publisher_client_t> ();
        EXPECT_TRUE (logical_call.async ().result ());
        // A missing Actor follows ordinary lookup after runtime start.
        EXPECT_EQ (
          actor.send (actor_id_t ("actor"), std::string ("x")).async ().result ().error_kind (),
          framework_error_kind_t::not_found);
        EXPECT_EQ (actor.request (actor_id_t ("actor"), std::string ("x"))
                     .template async<std::string> ()
                     .result ()
                     .error_kind (),
                   framework_error_kind_t::not_found);
        EXPECT_TRUE (publisher.publish ("channel", "topic", std::string ("x")).async ().result ());
        app.stop ();
    }));
    EXPECT_EQ (app.run (0, nullptr), 0);
    EXPECT_TRUE (visited);
}

namespace
{
template <typename T> void expect_shutdown (const char *operation, T pending)
{
    SCOPED_TRACE (operation);
    const auto result = pending.result ();
    ASSERT_FALSE (result);
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::shutting_down)
      << result.error ()->what ();
}

template <typename T> void expect_reusable_shutdown (const char *operation, T call)
{
    expect_shutdown (operation, call.async ());
    expect_shutdown (operation, call.async ());
}

class shutdown_probe_t final : public hosted_service_t
{
  public:
    shutdown_probe_t (app_t &app, bool &stopped) : app (app), stopped (stopped) {}
    task_t<void> start (service_provider_t &) override
    {
        app.stop ();
        co_return;
    }
    void stop () noexcept override { stopped = true; }

  private:
    app_t &app;
    bool &stopped;
};
}

TEST (PrestartCall, AllMessagingRejectsAfterHostedServicesStop)
{
    auto app = app_t::create ();
    auto store = std::make_shared<counting_store_t> ();
    app.add_zlink_framework ([&] (auto &options) {
        options.add_location_store (store);
        options.add_route_mesh ("mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("source"))
          .objects ()
          .server ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &route = provider.get_required<route_client_t> ();
    auto &channel = provider.get_required<channel_client_t> ();
    auto &fanout = provider.get_required<publisher_t> ();
    auto &actor = provider.get_required<actor_client_t> ();
    auto &logical = provider.get_required<spot_publisher_client_t> ();
    bool stopped = false;
    app.add_hosted_service (std::make_unique<shutdown_probe_t> (app, stopped));
    ASSERT_EQ (app.run (0, nullptr), 0);
    ASSERT_TRUE (stopped);
    store->calls = 0;
    const auto rid = zlink::routing_id_t::from ("target");
    const auto spot = spot_id_t ("target");
    expect_shutdown ("Node send", route.send_to_node ("mesh", rid, std::string ("x")).async ());
    expect_shutdown ("Node request",
                     route.request_to_node ("mesh", rid, std::string ("x")).async<std::string> ());
    expect_shutdown ("Route Channel send",
                     route.send_to_channel ("channel", std::string ("x")).async ());
    expect_shutdown ("Route Channel request",
                     route.request_to_channel ("channel", std::string ("x")).async<std::string> ());
    expect_shutdown ("Spot send", route.send_to_spot (spot, std::string ("x")).async ());
    expect_shutdown ("Spot request",
                     route.request_to_spot (spot, std::string ("x")).async<std::string> ());
    expect_shutdown ("Instance send",
                     route.send_to_spot (spot, std::string ("x")).instance_spot ().async ());
    expect_shutdown (
      "Instance request",
      route.request_to_spot (spot, std::string ("x")).instance_spot ().async<std::string> ());
    expect_shutdown ("Channel send", channel.send ("channel", std::string ("x")).async ());
    expect_shutdown ("Channel request",
                     channel.request ("channel", std::string ("x")).async<std::string> ());
    expect_shutdown ("Fanout publish",
                     fanout.publish ("channel", "topic", std::string ("x")).async ());
    expect_shutdown ("Actor send", actor.send (actor_id_t ("actor"), std::string ("x")).async ());
    expect_shutdown ("Actor request",
                     actor.request (actor_id_t ("actor"), std::string ("x")).async<std::string> ());
    expect_shutdown ("Logical Multicast publish",
                     logical.publish ("channel", "topic", std::string ("x")).async ());
    expect_reusable_shutdown ("Node send reuse",
                              route.send_to_node ("mesh", rid, std::string ("x")));
    expect_reusable_shutdown ("Route Channel send reuse",
                              route.send_to_channel ("channel", std::string ("x")));
    expect_reusable_shutdown ("Spot send reuse", route.send_to_spot (spot, std::string ("x")));
    expect_reusable_shutdown ("Instance send reuse",
                              route.send_to_spot (spot, std::string ("x")).instance_spot ());
    expect_reusable_shutdown ("Channel send reuse", channel.send ("channel", std::string ("x")));
    expect_reusable_shutdown ("Fanout publish reuse",
                              fanout.publish ("channel", "topic", std::string ("x")));
    expect_reusable_shutdown ("Actor send reuse",
                              actor.send (actor_id_t ("actor"), std::string ("x")));
    expect_reusable_shutdown ("Logical Multicast reuse",
                              logical.publish ("channel", "topic", std::string ("x")));
    EXPECT_EQ (store->calls.load (), 0u);
}

namespace
{
class admitted_actor_client_t final : public actor_client_t
{
  public:
    admitted_actor_client_t (detail::channel_runtime_t admission,
                             serializer_registry_t &serializers) :
        admission (std::move (admission)), serializers (serializers)
    {
    }
    std::size_t sends = 0;

  protected:
    void admit_call () override { admission.admit_outbound (); }
    task_t<void> send_erased (actor_id_t,
                              std::string,
                              message_t,
                              const actor_send_call_t::metadata_map_t &) override
    {
        ++sends;
        return task_t<void> (result_t<void>::success ());
    }
    request_task_t<message_t> request_erased (actor_id_t,
                                              std::string,
                                              message_t,
                                              std::optional<std::chrono::milliseconds>,
                                              const actor_request_call_t::metadata_map_t &) override
    {
        return request_task_t<message_t> (
          result_t<message_t>::failure (framework_error_kind_t::not_found, "No request target"));
    }
    serializer_registry_t &actor_client_serializers () override { return serializers; }

  private:
    detail::channel_runtime_t admission;
    serializer_registry_t &serializers;
};
}

TEST (PrestartCall, RejectedActorSendCanSubmitSameCallDuringHostedStartup)
{
    auto app = app_t::create ();
    app.add_zlink_framework ([] (auto &) {});
    auto provider = app.advanced ().services ().build_provider ();
    admitted_actor_client_t actor (
      detail::channel_runtime_t::from (app.advanced ().zlink ().message_bus ()),
      provider.get_required<serializer_registry_t> ());
    auto call = actor.send (actor_id_t ("actor"), std::string ("x"));
    auto copy = call;
    expect_prestart (call.async ());
    EXPECT_EQ (actor.sends, 0u);
    app.add_hosted_service (std::make_unique<startup_probe_t> ([&] (auto &) {
        EXPECT_TRUE (call.async ().result ());
        EXPECT_EQ (actor.sends, 1u);
        EXPECT_FALSE (copy.async ().result ());
        EXPECT_EQ (actor.sends, 1u);
        app.stop ();
    }));
    EXPECT_EQ (app.run (0, nullptr), 0);
}

TEST (PrestartCall, ShutdownTakesPrecedenceOverNotStarted)
{
    auto state = std::make_shared<detail::channel_runtime_state_t> ();
    detail::channel_runtime_t runtime (state);
    runtime.bind_runtime_admission ([] { return std::pair{true, false}; });
    auto submit = [&] () -> task_t<void> {
        runtime.admit_outbound ();
        co_return;
    };
    expect_shutdown ("shutdown before start", submit ());
}


TEST (PrestartCall, EachPublicTerminalChecksHostAdmissionOnce)
{
    auto app = app_t::create ();
    auto store = std::make_shared<counting_store_t> ();
    app.add_zlink_framework ([&] (auto &options) {
        options.add_location_store (store);
        options.add_route_mesh ("mesh")
          .listen ("tcp://127.0.0.1:0")
          .set_routing_id (zlink::routing_id_t::from ("source"))
          .objects ()
          .server ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &route = provider.get_required<route_client_t> ();
    auto &channel = provider.get_required<channel_client_t> ();
    auto &fanout = provider.get_required<publisher_t> ();
    auto &actor = provider.get_required<actor_client_t> ();
    auto &logical = provider.get_required<spot_publisher_client_t> ();
    auto runtime = detail::channel_runtime_t::from (app.advanced ().zlink ().message_bus ());
    std::size_t admissions = 0;
    runtime.bind_runtime_admission ([&] {
        ++admissions;
        return std::pair{false, true};
    });
    auto check = [&] (const char *name, auto submit) {
        SCOPED_TRACE (name);
        const auto before = admissions;
        // Missing destinations terminate locally after the host accepts the call.
        (void) submit ().result ();
        EXPECT_EQ (admissions, before + 1);
    };
    const auto rid = zlink::routing_id_t::from ("missing");
    const auto spot = spot_id_t ("missing");
    check ("Node send",
           [&] { return route.send_to_node ("missing", rid, std::string ("x")).async (); });
    check ("Node request", [&] {
        return route.request_to_node ("missing", rid, std::string ("x")).async<std::string> ();
    });
    check ("Route Channel send",
           [&] { return route.send_to_channel ("missing", std::string ("x")).async (); });
    check ("Route Channel request", [&] {
        return route.request_to_channel ("missing", std::string ("x")).async<std::string> ();
    });
    check ("Spot send", [&] { return route.send_to_spot (spot, std::string ("x")).async (); });
    check ("Spot request",
           [&] { return route.request_to_spot (spot, std::string ("x")).async<std::string> (); });
    check ("Instance send",
           [&] { return route.send_to_spot (spot, std::string ("x")).instance_spot ().async (); });
    check ("Instance request", [&] {
        return route.request_to_spot (spot, std::string ("x"))
          .instance_spot ()
          .async<std::string> ();
    });
    check ("Channel send", [&] { return channel.send ("missing", std::string ("x")).async (); });
    check ("Channel request",
           [&] { return channel.request ("missing", std::string ("x")).async<std::string> (); });
    check ("Fanout publish",
           [&] { return fanout.publish ("missing", "topic", std::string ("x")).async (); });
    check ("Actor send",
           [&] { return actor.send (actor_id_t ("missing"), std::string ("x")).async (); });
    check ("Actor request", [&] {
        return actor.request (actor_id_t ("missing"), std::string ("x")).async<std::string> ();
    });
    check ("Logical Multicast",
           [&] { return logical.publish ("missing", "topic", std::string ("x")).async (); });
    EXPECT_EQ (admissions, 14u);
}
