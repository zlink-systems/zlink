/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"

#include <zlink/framework.hpp>
#include <zlink.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/fanout/fanout_subscription.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace fanout = zlink::framework::runtime::fanout;
namespace mesh = zlink::framework::runtime::mesh;

struct gate_event_t
{
    static constexpr const char *packet_name = "fanout.gate.event";
    int value{};
};

}

template <> struct zlink::json::dto_traits<gate_event_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &gate_event_t::value)};
};

namespace
{

struct gate_state_t
{
    zlink::framework::task_completion_source_t<void> first_terminal;
    std::promise<void> first_started;
    std::promise<void> other_started;
    std::atomic_int first_count{0};
    std::atomic_int first_completed{0};
    std::atomic_int cancelled_waits{0};
    std::atomic_int other_count{0};
    std::atomic_int later_started{0};
    std::promise<void> later_completed;
    std::mutex mutex;
    std::vector<int> starts;
    std::vector<int> completions;
    std::atomic_int scopes_created{0};
    std::atomic_int scopes_destroyed{0};
    std::atomic_int scopes_live{0};
    std::atomic_int failures{0};
    bool fail_first = false;
};

std::shared_ptr<gate_state_t> gate_state;

struct gate_scope_t
{
    std::shared_ptr<gate_state_t> state = gate_state;
    gate_scope_t ()
    {
        ++state->scopes_created;
        ++state->scopes_live;
    }
    ~gate_scope_t ()
    {
        ++state->scopes_destroyed;
        --state->scopes_live;
    }
    int marker = 73;
};

class gate_handler_t
{
  public:
    using event_type = gate_event_t;
    explicit gate_handler_t (gate_scope_t &scope) : _scope (scope) {}

    zlink::framework::task_t<void>
    handle (const gate_event_t &event, const zlink::framework::publish_message_context_t &context)
    {
        if (event.value == 1) {
            if (gate_state->first_count.fetch_add (1, std::memory_order_acq_rel) == 0) {
                {
                    std::lock_guard lock (gate_state->mutex);
                    gate_state->starts.push_back (event.value);
                }
                gate_state->first_started.set_value ();
                try {
                    co_await gate_state->first_terminal.task ();
                }
                catch (const zlink::framework::framework_exception_t &error) {
                    if (error.kind () == zlink::framework::framework_error_kind_t::shutting_down)
                        ++gate_state->cancelled_waits;
                    throw;
                }
                EXPECT_EQ (event.value, 1);
                EXPECT_EQ (context.channel_name, "gate-a");
                EXPECT_EQ (_scope.marker, 73);
                {
                    std::lock_guard lock (gate_state->mutex);
                    gate_state->completions.push_back (event.value);
                }
                gate_state->first_completed.fetch_add (1, std::memory_order_acq_rel);
                if (gate_state->fail_first)
                    throw zlink::framework::framework_exception_t (
                      zlink::framework::framework_error_kind_t::internal_failure,
                      "fanout first terminal failed");
                co_return;
            }
        }
        if (event.value == 2) {
            {
                std::lock_guard lock (gate_state->mutex);
                gate_state->starts.push_back (event.value);
                gate_state->completions.push_back (event.value);
            }
            gate_state->later_started.fetch_add (1, std::memory_order_acq_rel);
            gate_state->later_completed.set_value ();
        }
        if (event.value == 3
            && gate_state->other_count.fetch_add (1, std::memory_order_acq_rel) == 0)
            gate_state->other_started.set_value ();
        co_return;
    }

  private:
    gate_scope_t &_scope;
};

class gate_publish_client_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit gate_publish_client_t (zlink::framework::app_t &app) : _app (&app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        auto publisher = _app->advanced ().zlink ().publisher ();
        (void) services;
        auto first = gate_state->first_started.get_future ();
        auto other = gate_state->other_started.get_future ();
        auto later = gate_state->later_completed.get_future ();
        for (int attempt = 0; attempt < 80 && !first_received; ++attempt) {
            try {
                co_await publisher.publish ("gate-a", "gate", gate_event_t{1}).async ();
                first_submitted = true;
            }
            catch (const zlink::framework::framework_exception_t &error) {
                last_error = error.what ();
            }
            first_received = first.wait_for (25ms) == std::future_status::ready;
        }
        if (first_received) {
            try {
                co_await publisher.publish ("gate-a", "gate", gate_event_t{2}).async ();
                later_submitted = true;
            }
            catch (const zlink::framework::framework_exception_t &error) {
                last_error = error.what ();
            }
            if (later_submitted) {
                later_received = later.wait_for (2s) == std::future_status::ready;
                try {
                    co_await publisher.publish ("gate-b", "gate", gate_event_t{3}).async ();
                    other_submitted = true;
                }
                catch (const zlink::framework::framework_exception_t &error) {
                    last_error = error.what ();
                }
                other_completed =
                  other_submitted && other.wait_for (2s) == std::future_status::ready;
                if (other_completed) {
                    const auto deadline = std::chrono::steady_clock::now () + 2s;
                    while (gate_state->scopes_live.load () != 1
                           && std::chrono::steady_clock::now () < deadline)
                        std::this_thread::sleep_for (1ms);
                    later_before_release =
                      gate_state->later_started.load (std::memory_order_acquire);
                }
            }
        }
        prepared.set_value ();
        co_return;
    }

    void stop () noexcept override {}

    bool first_submitted = false;
    bool first_received = false;
    bool later_submitted = false;
    bool later_received = false;
    bool other_submitted = false;
    bool other_completed = false;
    int later_before_release = -1;
    bool later_completed = false;
    bool terminal_completed_once = false;
    bool terminal_repeated = false;
    std::promise<void> prepared;
    std::string last_error;

  private:
    zlink::framework::app_t *_app;
};

void configure_gate_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<zlink::framework::runtime::in_memory_location_store_t> &store)
{
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_location_store (store);
        options.services ().add_scoped<gate_scope_t> ();
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options.configure_dispatch (), [state = gate_state] (const auto &event) {
              if (event.channel_name == "gate-a")
                  ++state->failures;
          });
        options.handlers ().group ("gate").add_publish<gate_handler_t> ();
        options.add_fanout_channel ("gate-a")
          .set_routing_id (zlink::routing_id_t::from ("gate-a-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("gate");
        options.add_fanout_channel ("gate-b")
          .set_routing_id (zlink::routing_id_t::from ("gate-b-publisher"))
          .enable_publisher ("tcp://127.0.0.1:0")
          .enable_subscriber ()
          .use_handler_group ("gate");
    });
}

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

std::vector<std::string> configured_subscription_topics (std::vector<std::string> topics)
{
    auto options = std::make_shared<zlink::framework::detail::framework_options_state_t> ();
    auto handler_groups =
      std::make_shared<zlink::framework::detail::handler_group_options_state_t> ();
    zlink::framework::fanout_channel_builder_t channel ("events", options, handler_groups);
    channel.enable_subscriber ();
    for (auto &topic : topics) {
        channel.subscribe (std::move (topic));
    }

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    options->keyed_zlink_actions.at ("fanout_channel:events") (zlink);
    const auto channels =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ()).channel_snapshots ();
    EXPECT_EQ (channels.size (), 1u);
    if (channels.size () != 1) {
        return {};
    }
    return channels.front ().subscriber.subscription_topics;
}

bool rejects_as_call_argument (const std::function<void ()> &call)
{
    try {
        call ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        return error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    return false;
}

void wait_for_beacon (fanout::raw_fanout_publisher_t &publisher,
                      fanout::raw_fanout_subscriber_t &subscriber,
                      const std::vector<std::uint8_t> &publisher_id,
                      std::chrono::steady_clock::time_point receive_now)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    std::size_t beacon_tick = 1;
    while (!subscriber.ready (publisher_id) && std::chrono::steady_clock::now () < deadline) {
        (void) publisher.tick (receive_now
                               + fanout::fanout_beacon_interval * static_cast<int> (beacon_tick++));
        (void) subscriber.try_receive (receive_now);
        if (!subscriber.ready (publisher_id)) {
            std::this_thread::sleep_for (2ms);
        }
    }
    ASSERT_TRUE (subscriber.ready (publisher_id));
}

void publish (fanout::raw_fanout_publisher_t &publisher, const std::string &topic)
{
    publisher.publish ("events", topic, {"FanoutEvent", "application/json", bytes (topic)})
      .result ()
      .value ();
}

} // namespace

TEST (CppFrameworkFanoutSubscription, NoApplicationSubscriptionReceivesDifferentTopics)
{
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-default");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    ASSERT_TRUE (subscriber.connect_manual (publisher_id, publisher.endpoint ()));
    const auto receive_now = std::chrono::steady_clock::now ();
    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);

    std::set<std::string> received_topics;
    for (std::size_t attempt = 0; attempt < 100 && received_topics.size () != 2; ++attempt) {
        publish (publisher, "order.created");
        publish (publisher, "payment");
        for (std::size_t receive = 0; receive < 2; ++receive) {
            const auto [status, record] = subscriber.try_receive (receive_now);
            if (status == fanout::fanout_receive_status_t::application) {
                ASSERT_TRUE (record);
                received_topics.insert (record->topic);
            }
        }
        if (received_topics.size () != 2) {
            std::this_thread::sleep_for (2ms);
        }
    }

    EXPECT_EQ (received_topics, (std::set<std::string>{"order.created", "payment"}));
}

TEST (CppFrameworkFanoutSubscription, PrefixSubscriptionReceivesMatchingTopicAndRejectsOtherTopic)
{
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-prefix");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({"order"}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    ASSERT_TRUE (subscriber.connect_manual (publisher_id, publisher.endpoint ()));
    const auto receive_now = std::chrono::steady_clock::now ();
    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);

    std::optional<std::string> received_topic;
    for (std::size_t attempt = 0; attempt < 100 && !received_topic; ++attempt) {
        publish (publisher, "payment");
        publish (publisher, "order.created");
        const auto [status, record] = subscriber.try_receive (receive_now);
        if (status == fanout::fanout_receive_status_t::application) {
            ASSERT_TRUE (record);
            received_topic = record->topic;
        } else {
            std::this_thread::sleep_for (2ms);
        }
    }

    ASSERT_TRUE (received_topic);
    EXPECT_EQ (*received_topic, "order.created");
}

TEST (CppFrameworkFanoutSubscription, RestrictedAutomaticSubscriberStaysReadyFromBeacon)
{
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    const auto publisher_id = bytes ("publisher-liveness");
    fanout::raw_fanout_subscriber_t subscriber (
      nullptr, configured_subscription_topics ({"order"}),
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    subscriber.reconcile_automatic ({fanout::fanout_publisher_intent_t{
      publisher_id, 1, publisher.endpoint (), mesh::service_node_state_t::serving}});
    const auto receive_now = std::chrono::steady_clock::now ();

    wait_for_beacon (publisher, subscriber, publisher_id, receive_now);
    EXPECT_TRUE (subscriber.tick (receive_now + fanout::fanout_receive_deadline - 1ms).empty ());
    EXPECT_TRUE (subscriber.ready (publisher_id));
}

TEST (CppFrameworkFanoutSubscription, ReservedPrefixIsRejectedByPublishAndSubscribe)
{
    const auto reserved = fanout::raw_fanout_publisher_t::reserved_topic ();
    const auto extended = reserved + std::string (1, '\0');
    const auto shorter = reserved.substr (0, reserved.size () - 1);
    auto different = reserved;
    different.back () = static_cast<char> (0x32);

    auto options = std::make_shared<zlink::framework::detail::framework_options_state_t> ();
    auto handler_groups =
      std::make_shared<zlink::framework::detail::handler_group_options_state_t> ();
    zlink::framework::fanout_channel_builder_t channel ("events", options, handler_groups);
    EXPECT_TRUE (rejects_as_call_argument ([&] { channel.subscribe (reserved); }));
    EXPECT_TRUE (rejects_as_call_argument ([&] { channel.subscribe (extended); }));
    EXPECT_FALSE (rejects_as_call_argument ([&] { channel.subscribe (shorter); }));
    EXPECT_FALSE (rejects_as_call_argument ([&] { channel.subscribe (different); }));
    ASSERT_TRUE (options->fanout_subscription_topics.contains ("events"));
    EXPECT_EQ (options->fanout_subscription_topics.at ("events").size (), 2u);

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    auto publisher = zlink.publisher ();
    EXPECT_TRUE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", reserved, std::string ("value")); }));
    EXPECT_TRUE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", extended, std::string ("value")); }));
    EXPECT_FALSE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", shorter, std::string ("value")); }));
    EXPECT_FALSE (rejects_as_call_argument (
      [&] { (void) publisher.publish ("events", different, std::string ("value")); }));
}

TEST (CppFrameworkFanoutSubscription, DuplicateSubscriptionHasSameEffectiveTransportSet)
{
    const auto once = configured_subscription_topics ({"order"});
    const auto twice = configured_subscription_topics ({"order", "order"});

    EXPECT_EQ (once, twice);
    EXPECT_EQ (fanout::fanout_subscription_topics (once),
               fanout::fanout_subscription_topics (twice));
    EXPECT_EQ (fanout::fanout_subscription_topics (twice).size (), 2u);
}

class startup_stop_service_t final : public zlink::framework::hosted_service_t
{
  public:
    explicit startup_stop_service_t (zlink::framework::app_t &app) : _app (app) {}

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &) override
    {
        started = true;
        _app.request_stop ();
        co_return;
    }

    void stop () noexcept override {}

    bool started = false;

  private:
    zlink::framework::app_t &_app;
};

TEST (CppFrameworkFanoutSubscription, StartupBindsBeforeFirstPublish)
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.add_fanout_channel ("startup-events").enable_publisher ("tcp://127.0.0.1:0");
    });
    int exit_code = -1;
    std::jthread host ([&] (std::stop_token stop) {
        std::stop_callback request_stop (stop, [&] { app.request_stop (); });
        exit_code = app.run (0, nullptr);
    });
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (10ms);
    ASSERT_TRUE (app.is_ready ());

    auto provider = app.advanced ().services ().build_provider ();
    const auto listener =
      provider.get_required<zlink::framework::framework_runtime_t> ().listener_status (
        zlink::framework::listener_kind_t::fanout, "startup-events");
    zlink::context_t context;
    zlink::sub_socket_t subscriber (context);
    subscriber.options ().linger (0ms);
    auto monitor = subscriber.monitor_open (zlink::monitor_event::connection_ready);
    zlink::poller_t poller;
    poller.add (monitor, zlink::poll_event_flag_t::pollin, 1);
    subscriber.set_subscription ("startup");
    subscriber.connect (listener.endpoint);
    zlink::poll_event_t event;
    EXPECT_EQ (poller.wait (&event, 1, 2s), 1);
    const auto notification = monitor.recv (zlink::recv_flags_t::dontwait);
    EXPECT_TRUE (notification && notification->event == zlink::monitor_event::connection_ready);
    host.request_stop ();
    host.join ();
    EXPECT_EQ (exit_code, 0);
    provider.close ();
}

TEST (CppFrameworkFanoutSubscription, BindFailureFailsStartupBeforeApplicationService)
{
    zlink::context_t context;
    zlink::xpub_socket_t occupied (context);
    occupied.options ().linger (0ms);
    occupied.bind ("tcp://127.0.0.1:0");
    const auto endpoint = occupied.options ().last_endpoint ();
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([&] (zlink::framework::zlink_framework_options_t &options) {
        options.add_fanout_channel ("startup-events").enable_publisher (endpoint);
    });
    auto service = std::make_unique<startup_stop_service_t> (app);
    const auto *application_service = service.get ();
    app.add_hosted_service (std::move (service));
    EXPECT_NE (app.run (0, nullptr), 0);
    EXPECT_FALSE (application_service->started);
    EXPECT_EQ (app.runtime_state (), zlink::framework::framework_runtime_state_t::error);
}

TEST (CppFrameworkFanoutSubscription, PublishDoesNotBindOutsideStartup)
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.handlers ().group ("startup").add_publish<gate_handler_t> ();
        options.add_fanout_channel ("startup-events").enable_publisher ("tcp://127.0.0.1:0");
    });
    const auto result = app.advanced ()
                          .zlink ()
                          .publisher ()
                          .publish ("startup-events", "startup", gate_event_t{1})
                          .async ()
                          .result ();
    ASSERT_FALSE (result.has_value ());
    EXPECT_EQ (result.error_kind (), zlink::framework::framework_error_kind_t::unavailable);
    auto provider = app.advanced ().services ().build_provider ();
    EXPECT_THROW (provider.get_required<zlink::framework::framework_runtime_t> ().listener_status (
                    zlink::framework::listener_kind_t::fanout, "startup-events"),
                  zlink::framework::framework_exception_t);
    provider.close ();
}

TEST (CppFrameworkFanoutSubscription, AsyncHandlersStartInFifoAndOwnTheirTerminals)
{
    for (bool fail_first : {false, true}) {
        gate_state = std::make_shared<gate_state_t> ();
        gate_state->fail_first = fail_first;
        auto app = zlink::framework::app_t::create ();
        auto store = std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
        configure_gate_app (app, store);
        auto service = std::make_unique<gate_publish_client_t> (app);
        auto *client = service.get ();
        app.add_hosted_service (std::move (service));

        auto prepared = client->prepared.get_future ();
        int exit_code = -1;
        std::jthread host ([&] (std::stop_token stop) {
            std::stop_callback request_stop (stop, [&] { app.request_stop (); });
            exit_code = app.run (0, nullptr);
        });
        const auto observed = prepared.wait_for (5s) == std::future_status::ready;
        EXPECT_TRUE (observed);
        const auto ready_deadline = std::chrono::steady_clock::now () + 2s;
        while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
            std::this_thread::sleep_for (1ms);
        EXPECT_TRUE (app.is_ready ());
        {
            std::lock_guard lock (gate_state->mutex);
            EXPECT_EQ (gate_state->starts, (std::vector<int>{1, 2}));
            EXPECT_EQ (gate_state->completions, (std::vector<int>{2}));
        }
        EXPECT_EQ (gate_state->first_completed.load (), 0);
        EXPECT_EQ (gate_state->scopes_live.load (), 1);
        auto shutdown = app.shutdown ();
        EXPECT_FALSE (shutdown.result_for (20ms));
        client->terminal_completed_once =
          gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ());
        client->terminal_repeated =
          gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ());
        EXPECT_TRUE (shutdown.result_for (5s));
        host.request_stop ();
        host.join ();
        EXPECT_EQ (exit_code, 0);
        client->later_completed = gate_state->later_started.load () == 1;
        {
            std::lock_guard lock (gate_state->mutex);
            EXPECT_EQ (gate_state->completions, (std::vector<int>{2, 1}));
        }
        EXPECT_EQ (gate_state->scopes_live.load (), 0);
        EXPECT_EQ (gate_state->scopes_created.load (), gate_state->scopes_destroyed.load ());
        EXPECT_EQ (gate_state->failures.load (), fail_first ? 1 : 0);
        EXPECT_TRUE (client->first_submitted);
        EXPECT_TRUE (client->first_received);
        EXPECT_TRUE (client->last_error.empty ()) << client->last_error;
        EXPECT_TRUE (client->later_submitted);
        EXPECT_TRUE (client->later_received);
        EXPECT_TRUE (client->other_submitted);
        EXPECT_TRUE (client->other_completed);
        EXPECT_EQ (client->later_before_release, 1);
        EXPECT_TRUE (client->later_completed);
        EXPECT_TRUE (client->terminal_completed_once);
        EXPECT_FALSE (client->terminal_repeated);
        EXPECT_EQ (gate_state->first_completed.load (std::memory_order_acquire), 1);
    }
}


TEST (CppFrameworkFanoutSubscription, ForcedShutdownCancelsOwnedCompletionWait)
{
    gate_state = std::make_shared<gate_state_t> ();
    auto app = zlink::framework::app_t::create ();
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_store_t> ();
    configure_gate_app (app, store);
    auto service = std::make_unique<gate_publish_client_t> (app);
    auto prepared = service->prepared.get_future ();
    app.add_hosted_service (std::move (service));
    int exit_code = -1;
    std::promise<void> run_exited;
    auto exited = run_exited.get_future ();
    std::jthread host ([&] {
        exit_code = app.run (0, nullptr);
        run_exited.set_value ();
    });
    EXPECT_EQ (prepared.wait_for (5s), std::future_status::ready);
    const auto ready_deadline = std::chrono::steady_clock::now () + 2s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < ready_deadline)
        std::this_thread::sleep_for (1ms);
    EXPECT_TRUE (app.is_ready ());
    EXPECT_EQ (gate_state->scopes_live.load (), 1);
    auto shutdown = app.shutdown (100ms);
    EXPECT_FALSE (shutdown.result_for (20ms));
    const auto terminal = shutdown.result_for (5s);
    EXPECT_TRUE (terminal.has_value ());
    if (terminal && *terminal) {
        EXPECT_EQ (terminal->value ().outcome,
                   zlink::framework::termination_outcome_t::force_stopped);
        EXPECT_EQ (terminal->value ().reason,
                   zlink::framework::termination_reason_t::deadline_exceeded);
    }
    const bool run_completed = exited.wait_for (5s) == std::future_status::ready;
    EXPECT_TRUE (run_completed);
    EXPECT_EQ (gate_state->cancelled_waits.load (), 1);
    EXPECT_EQ (gate_state->first_completed.load (), 0);
    EXPECT_EQ (gate_state->scopes_live.load (), 0);
    EXPECT_FALSE (gate_state->first_terminal.task ().result_for (0ms));
    // Completing the original source remains valid after its registered owner ended the wait.
    EXPECT_TRUE (
      gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ()));
    EXPECT_FALSE (
      gate_state->first_terminal.complete (zlink::framework::result_t<void>::success ()));
    host.join ();
    EXPECT_EQ (exit_code, 0);
    EXPECT_EQ (gate_state->scopes_created.load (), gate_state->scopes_destroyed.load ());
}
