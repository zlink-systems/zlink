/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include <zlink/framework.hpp>

#include <gtest/gtest.h>
#include <zlink/framework/detail/deadline_scheduler.hpp>
#include <future>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using parts_t = runtime::messaging::message_parts_t;

struct spot_packet_t
{
    static constexpr const char *packet_name = "spot.packet";
    int value{};
};

class spot_resolver_t final : public runtime::spot_address_resolver_t
{
  public:
    task_t<std::optional<runtime::spot_address_t>> resolve_spot_address (std::string,
                                                                         std::string spot) override
    {
        co_return runtime::spot_address_t{"spot-route", zlink::routing_id_t::from ("peer"),
                                          std::move (spot)};
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
};

struct public_spot_fixture_t
{
    zlink_builder_t builder = test::runtime_failure_builder ();
    serializer_registry_t serializers;
    detail::channel_runtime_manager_t manager = detail::channel_runtime_manager_t::from (builder);

    public_spot_fixture_t ()
    {
        serializers.add<spot_packet_t> (
          [] (const auto &packet) {
              return encoded_payload_t::from_string (std::to_string (packet.value));
          },
          [] (const encoded_payload_t &payload) {
              return spot_packet_t{std::stoi (payload.to_string ())};
          },
          "application/json");
        builder.route_channel ("spot-route");
        manager.initialize_route_channels (builder);
        detail::channel_runtime_t::from (builder.message_bus ())
          .bind_spot_address_resolver (std::make_shared<spot_resolver_t> ());
    }
    detail::route_channel_runtime_t &route () { return manager.get_route_channel ("spot-route"); }
};

class readiness_turn_t final : public detail::serial_turn_t
{
  public:
    explicit readiness_turn_t (std::shared_ptr<std::promise<void>> destroyed) :
        _destroyed (std::move (destroyed))
    {
    }
    ~readiness_turn_t () override { _destroyed->set_value (); }
    bool release () override
    {
        _released = true;
        return true;
    }
    bool released () const override { return _released; }
    std::stop_token wait_cancellation () const override { return _cancellation.get_token (); }
    void cancel_waits () noexcept override { _cancellation.request_stop (); }
    detail::task_scheduler_t resume_scheduler () override
    {
        return [] (auto work) { work (); };
    }
    bool belongs_to (const void *owner) const noexcept override { return owner == this; }
    bool is_after_active_phase () const noexcept override { return false; }
    bool allows_yield () const noexcept override { return true; }
    bool claim_yield () override { return false; }
    result_t<void> defer (std::function<task_t<void> ()>,
                          std::function<void ()>,
                          std::shared_ptr<detail::deferred_barrier_t>) override
    {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "unexpected defer");
    }
    void cancel_deferred () noexcept override {}
    bool finish_handler (std::function<void ()> completion, bool) override
    {
        completion ();
        return release ();
    }

  private:
    bool _released = false;
    std::stop_source _cancellation;
    std::shared_ptr<std::promise<void>> _destroyed;
};

TEST (CppFrameworkRouteChannelRequest, SubmissionReturnsBeforeBindingTerminal)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> submitted;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        submitted.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission =
      std::async (std::launch::async, [&] { return route.request_reply_parts (target, {}, 1s); });
    submitted.get_future ().wait ();
    const auto status = submission.wait_for (100ms);
    // Always release the old blocking implementation before asserting.
    admitted->complete (result_t<void>::success ());
    reply.complete (result_t<parts_t>::success ({}));
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSubmissionReturnsBeforeBindingTerminal)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    std::promise<void> submitted;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        submitted.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto entered_status = submitted.get_future ().wait_for (1s);
    const auto status = submission.wait_for (100ms);
    admitted->complete (result_t<void>::success ());
    reply.complete (result_t<parts_t>::success ({}));
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, entered_status);
    EXPECT_EQ (std::future_status::ready, status);
}

TEST (CppFrameworkRouteChannelRequest, PendingFakeReturnsBeforeCallerRelease)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    std::promise<void> entered;
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        entered.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission =
      std::async (std::launch::async, [&] { return route.request_reply_parts (target, {}, 1s); });
    const auto entered_status = entered.get_future ().wait_for (1s);
    const auto status = submission.wait_for (100ms);
    reply.complete (result_t<parts_t>::success ({}));
    auto pending = submission.get ();
    EXPECT_EQ (std::future_status::ready, entered_status);
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_TRUE (pending.result ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationReturnsPendingBeforePeerReady)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto status = submission.wait_for (100ms);
    route.mark_peer_ready (target);
    (void) submission.get ();
    EXPECT_EQ (std::future_status::ready, status);
}

struct pending_send_fixture_t : public_spot_fixture_t
{
    const zlink::routing_id_t target = zlink::routing_id_t::from ("peer");
    std::atomic<int> submitted = 0;
    pending_send_fixture_t ()
    {
        route ().bind_endpoint ("tcp://self:7500");
        route ().connect (target, "tcp://peer:7500");
        route ().default_request_timeout (1s);
        route ().set_send_backend ([this] (const auto &, const auto &, const auto &) {
            ++submitted;
            return result_t<void>::success ();
        });
    }
    auto invoke ()
    {
        return std::async (std::launch::async,
                           [this] { return route ().submit_spot_send_parts (target, "spot", {}); });
    }
};

TEST (CppFrameworkRouteChannelRequest, SpotSendReturnsBeforePeerReady)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    std::atomic<int> submitted = 0;
    route.set_send_backend ([&] (const auto &, const auto &, const auto &) {
        ++submitted;
        return result_t<void>::success ();
    });
    std::promise<void> entered;
    auto invocation = std::async (std::launch::async, [&] {
        entered.set_value ();
        return route.submit_spot_send_parts (target, "spot", {});
    });
    entered.get_future ().wait ();
    const auto status = invocation.wait_for (100ms);
    const auto before_ready = submitted.load ();
    std::optional<task_t<void>> pending;
    if (status == std::future_status::ready)
        pending.emplace (invocation.get ());
    const bool unfinished = pending && !pending->await_ready ();
    route.mark_peer_ready (target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_EQ (0, before_ready);
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
    EXPECT_EQ (1, submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, PublicSpotSendReturnsBeforePeerReady)
{
    public_spot_fixture_t fixture;
    auto &route = fixture.route ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    std::atomic<int> submitted = 0;
    route.set_send_backend ([&] (const auto &, const auto &, const auto &) {
        ++submitted;
        return result_t<void>::success ();
    });
    auto client = fixture.builder.route_client (fixture.serializers);
    std::promise<void> entered;
    auto invocation = std::async (std::launch::async, [&] {
        entered.set_value ();
        return client.send_to_spot ("spot", spot_packet_t{79}).async ();
    });
    entered.get_future ().wait ();
    const auto status = invocation.wait_for (100ms);
    const auto before_ready = submitted.load ();
    std::optional<task_t<void>> pending;
    if (status == std::future_status::ready)
        pending.emplace (invocation.get ());
    const bool unfinished = pending && !pending->await_ready ();
    route.mark_peer_ready (target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_EQ (0, before_ready);
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
    EXPECT_EQ (1, submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendIgnoresConfiguredRequestTimeout)
{
    pending_send_fixture_t fixture;
    fixture.route ().default_request_timeout (20ms);
    auto invocation = fixture.invoke ();
    std::this_thread::sleep_for (60ms);
    std::optional<task_t<void>> pending;
    if (invocation.wait_for (0ms) == std::future_status::ready)
        pending.emplace (invocation.get ());
    const bool unfinished = pending && !pending->await_ready ();
    fixture.route ().mark_peer_ready (fixture.target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
    EXPECT_EQ (1, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendRouteRemovalIsUnavailable)
{
    pending_send_fixture_t fixture;
    auto invocation = fixture.invoke ();
    invocation.wait_for (20ms);
    fixture.route ().disconnect (fixture.target, "tcp://peer:7500");
    auto pending = invocation.get ();
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
    EXPECT_EQ (0, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendShutdownIsShuttingDown)
{
    pending_send_fixture_t fixture;
    auto invocation = fixture.invoke ();
    invocation.wait_for (20ms);
    fixture.route ().stop ();
    auto pending = invocation.get ();
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
    EXPECT_EQ (0, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, StoppedPublicSpotSendAndRequestAreShuttingDown)
{
    public_spot_fixture_t fixture;
    fixture.route ().stop ();
    auto client = fixture.builder.route_client (fixture.serializers);
    // Submit §3 admission table and §7: a stopped runtime rejects with ShuttingDown.
    auto sent = client.send_to_spot ("spot", spot_packet_t{7}).async ().result ();
    EXPECT_FALSE (sent);
    EXPECT_EQ (framework_error_kind_t::shutting_down, sent.error_kind ());
    auto reply =
      client.request_to_spot ("spot", spot_packet_t{7}).async<spot_packet_t> ().result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendDoesNotObserveAmbientCallerCancellation)
{
    pending_send_fixture_t fixture;
    std::stop_source caller;
    std::promise<void> entered;
    auto invocation = std::async (std::launch::async, [&] {
        auto context = detail::capture_ambient_context ();
        context.wait_owner = caller.get_token ();
        const auto scope = detail::enter_ambient_context (context, false);
        entered.set_value ();
        return fixture.route ().submit_spot_send_parts (fixture.target, "spot", {});
    });
    entered.get_future ().wait ();
    invocation.wait_for (20ms);
    caller.request_stop ();
    std::this_thread::sleep_for (20ms);
    std::optional<task_t<void>> pending;
    if (invocation.wait_for (0ms) == std::future_status::ready)
        pending.emplace (invocation.get ());
    const bool unfinished = pending && !pending->await_ready ();
    fixture.route ().mark_peer_ready (fixture.target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
    EXPECT_EQ (1, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendInitialNoRouteIgnoresUnrelatedNotification)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from ("peer");
    auto invocation = std::async (
      std::launch::async, [&] { return route.submit_spot_send_parts (target, "spot", {}); });
    const auto status = invocation.wait_for (20ms);
    std::optional<task_t<void>> pending;
    if (status == std::future_status::ready)
        pending.emplace (invocation.get ());
    route.routing_id (zlink::routing_id_t::from ("self"));
    std::this_thread::sleep_for (10ms);
    const bool unfinished = pending && !pending->await_ready ();
    route.connect (target, "tcp://peer:7500");
    route.mark_peer_ready (target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendKeepsTargetWithAnotherEndpoint)
{
    pending_send_fixture_t fixture;
    fixture.route ().connect (fixture.target, "tcp://other-peer:7501");
    auto invocation = fixture.invoke ();
    const auto status = invocation.wait_for (20ms);
    std::optional<task_t<void>> pending;
    if (status == std::future_status::ready)
        pending.emplace (invocation.get ());
    fixture.route ().disconnect (fixture.target, "tcp://peer:7500");
    std::this_thread::sleep_for (10ms);
    const bool unfinished = pending && !pending->await_ready ();
    fixture.route ().mark_peer_ready (fixture.target);
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending->result ());
    EXPECT_EQ (1, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendUnknownRouteRemovalKeepsExplicitTarget)
{
    pending_send_fixture_t fixture;
    fixture.route ().connect ("tcp://unknown-peer:7502");
    auto pending = fixture.route ().submit_spot_send_parts (fixture.target, "spot", {});
    fixture.route ().disconnect ("tcp://unknown-peer:7502");
    const bool unfinished = !pending.await_ready ();
    fixture.route ().mark_peer_ready (fixture.target);
    EXPECT_TRUE (unfinished);
    EXPECT_TRUE (pending.result ());
    EXPECT_EQ (1, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendRemovalSurvivesDelayedGenericNotification)
{
    pending_send_fixture_t fixture;
    completion_owner_t owner;
    auto *dispatcher = detail::borrow_completion_dispatcher ();
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    detail::dispatch_completion (
      [&] {
          entered.set_value ();
          released.wait ();
      },
      false, nullptr, dispatcher);
    entered.get_future ().wait ();
    auto pending = fixture.route ().submit_spot_send_parts (fixture.target, "spot", {});
    fixture.route ().routing_id (zlink::routing_id_t::from ("self"));
    fixture.route ().disconnect (fixture.target, "tcp://peer:7500");
    fixture.route ().connect (fixture.target, "tcp://peer:7500");
    fixture.route ().mark_peer_ready (fixture.target);
    release.set_value ();
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
    EXPECT_EQ (0, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendStateSurvivesWrapperDestruction)
{
    std::optional<task_t<void>> pending;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        pending.emplace (
          route.submit_spot_send_parts (zlink::routing_id_t::from ("peer"), "spot", {}));
        EXPECT_FALSE (pending->await_ready ());
    }
    const auto result = pending->result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendShutdownSurvivesDelayedResumeAndRestart)
{
    pending_send_fixture_t fixture;
    completion_owner_t owner;
    auto *dispatcher = detail::borrow_completion_dispatcher ();
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    detail::dispatch_completion (
      [&] {
          entered.set_value ();
          released.wait ();
      },
      false, nullptr, dispatcher);
    entered.get_future ().wait ();
    auto pending = fixture.route ().submit_spot_send_parts (fixture.target, "spot", {});
    fixture.route ().routing_id (zlink::routing_id_t::from ("self"));
    fixture.route ().stop ();
    fixture.route ().start ();
    fixture.route ().mark_peer_ready (fixture.target);
    release.set_value ();
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
    EXPECT_EQ (0, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendPreservesExecutionTurnShutdown)
{
    pending_send_fixture_t fixture;
    auto destroyed = std::make_shared<std::promise<void>> ();
    auto turn = std::make_shared<readiness_turn_t> (destroyed);
    std::optional<task_t<void>> pending;
    {
        detail::serial_turn_scope_t scope (turn);
        pending.emplace (fixture.route ().submit_spot_send_parts (fixture.target, "spot", {}));
    }
    EXPECT_FALSE (pending->await_ready ());
    turn->cancel_waits ();
    const auto result = pending->result ();
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
    EXPECT_EQ (0, fixture.submitted.load ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendPreparationDoesNotRegisterDeadline)
{
    pending_send_fixture_t fixture;
    auto &scheduler = detail::deadline_scheduler_t::instance ();
    const auto before = scheduler.scheduled_count ();
    auto pending = fixture.route ().submit_spot_send_parts (fixture.target, "spot", {});
    EXPECT_EQ (before, scheduler.scheduled_count ());
    EXPECT_FALSE (pending.await_ready ());
    fixture.route ().mark_peer_ready (fixture.target);
    EXPECT_TRUE (pending.result ());
    EXPECT_EQ (before, scheduler.scheduled_count ());
}

TEST (CppFrameworkRouteChannelRequest, SpotSendSubmissionPreservesBackendException)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.set_send_backend ([] (const auto &, const auto &, const auto &) -> result_t<void> {
        throw std::runtime_error ("send submission failed");
    });
    auto pending = route.submit_spot_send_parts (zlink::routing_id_t::from ("peer"), "spot", {});
    const auto result = pending.result ();
    EXPECT_FALSE (result);
    try {
        result.value ();
        FAIL () << "expected original backend exception";
    }
    catch (const std::runtime_error &error) {
        EXPECT_EQ (typeid (std::runtime_error), typeid (error));
        EXPECT_STREQ ("send submission failed", error.what ());
    }
}

TEST (CppFrameworkRouteChannelRequest, SpotRequestReturnsBeforeCallerCompletesReply)
{
    public_spot_fixture_t fixture;
    auto &route = fixture.route ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    task_completion_source_t<parts_t> reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (result_t<void>::success ());
    std::promise<void> entered;
    std::optional<std::string> observed_spot;
    std::string observed_target;
    std::string observed_body;
    route.set_request_backend ([&] (const auto &node, const auto &spot, const auto &parts, auto) {
        observed_spot = spot;
        observed_target = node.to_string ();
        runtime::messaging::envelope_codec_t codec;
        observed_body =
          detail::encoded_payload_from_raw (codec.decode_body (parts).value ()).to_string ();
        entered.set_value ();
        return request_task_t<parts_t> (reply.task (), admitted);
    });
    auto client = fixture.builder.route_client (fixture.serializers);
    auto invocation = std::async (std::launch::async, [&] {
        return client.request_to_spot ("spot", spot_packet_t{79})
          .timeout (1s)
          .async<spot_packet_t> ();
    });
    const auto backend_entered = entered.get_future ().wait_for (1s);
    const auto status = invocation.wait_for (100ms);
    std::optional<request_task_t<spot_packet_t>> pending;
    if (status == std::future_status::ready)
        pending.emplace (invocation.get ());
    const bool unfinished = pending && !pending->await_ready ();
    const auto framework_pending = route.pending_request_count ();
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::response;
    header.channel_name = "spot-route";
    header.message_name = spot_packet_t::packet_name;
    header.content_type = "application/json";
    runtime::messaging::envelope_codec_t codec;
    reply.complete (result_t<parts_t>::success (
      codec.encode_raw_body_parts (std::move (header), zlink::message_t::from ("79"))));
    if (!pending)
        pending.emplace (invocation.get ());
    EXPECT_EQ (std::optional<std::string> ("spot"), observed_spot);
    EXPECT_EQ ("peer", observed_target);
    EXPECT_EQ ("79", observed_body);
    EXPECT_EQ (std::future_status::ready, backend_entered);
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_TRUE (unfinished);
    EXPECT_EQ (0u, framework_pending);
    const auto result = pending->result ();
    ASSERT_TRUE (result);
    EXPECT_EQ (79, result.value ().value);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, PublicSpotPreparationConsumesSingleBudget)
{
    public_spot_fixture_t fixture;
    auto &route = fixture.route ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    std::chrono::milliseconds submitted_timeout{};
    std::chrono::steady_clock::time_point submitted_at;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &parts, auto timeout) {
        submitted_at = std::chrono::steady_clock::now ();
        submitted_timeout = timeout;
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (parts).value ();
        header.kind = runtime::messaging::message_kind_t::response;
        return request_task_t<parts_t> (result_t<parts_t>::success (
          codec.encode_raw_body_parts (header, codec.decode_body (parts).value ())));
    });
    auto client = fixture.builder.route_client (fixture.serializers);
    const auto started = std::chrono::steady_clock::now ();
    auto pending =
      client.request_to_spot ("spot", spot_packet_t{79}).timeout (1s).async<spot_packet_t> ();
    const auto returned = std::chrono::steady_clock::now ();
    const bool unfinished = !pending.await_ready ();
    std::this_thread::sleep_for (30ms);
    route.mark_peer_ready (target);
    const auto result = pending.result ();
    ASSERT_TRUE (result);
    EXPECT_TRUE (unfinished);
    EXPECT_EQ (79, result.value ().value);
    EXPECT_GT (submitted_timeout, 0ms);
    const auto minimum_remaining =
      std::chrono::duration_cast<std::chrono::milliseconds> (started + 1s - submitted_at);
    const auto maximum_remaining =
      std::chrono::ceil<std::chrono::milliseconds> (returned + 1s - submitted_at);
    EXPECT_GE (submitted_timeout, minimum_remaining);
    EXPECT_LE (submitted_timeout, maximum_remaining);
    EXPECT_LT (submitted_timeout, 1s);
}

TEST (CppFrameworkRouteChannelRequest, ThrowingSubmissionRemovesPendingEntry)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    route.set_request_backend (
      [] (const auto &, const auto &, const auto &, auto) -> request_task_t<parts_t> {
          throw std::runtime_error ("submission failed");
      });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    EXPECT_THROW ((void) route.request_reply_parts (target, {}, 1s), std::runtime_error);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, PhysicalTerminalRetainsBackendAfterAdmissionFailureAndStop)
{
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto backend_lifetime = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = backend_lifetime;
    std::optional<request_task_t<result_t<parts_t>>> pending;
    const zlink::routing_id_t *borrowed_target = nullptr;
    const parts_t *borrowed_parts = nullptr;
    {
        detail::route_channel_runtime_t route ("route");
        route.start ();
        route.set_request_backend (
          [held = backend_lifetime, &physical_reply, admitted, &borrowed_target,
           &borrowed_parts] (const auto &target, const auto &, const auto &parts, auto) {
              (void) held;
              borrowed_target = &target;
              borrowed_parts = &parts;
              return request_task_t<parts_t> (physical_reply.task (), admitted);
          });
        pending.emplace (
          route.request_reply_parts (zlink::routing_id_t::from (std::string ("peer")), {}, 1s));
        route.stop ();
    }
    backend_lifetime.reset ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    EXPECT_FALSE (pending->admitted ().result ());
    EXPECT_FALSE (pending->await_ready ());
    EXPECT_FALSE (lifetime.expired ());
    EXPECT_EQ ("peer", borrowed_target->to_string ());
    EXPECT_EQ (0u, borrowed_parts->size ());
    physical_reply.complete (result_t<parts_t>::success ({}));
    ASSERT_TRUE (pending->result ());
    EXPECT_TRUE (pending->result ().value ());
    EXPECT_TRUE (lifetime.expired ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationConsumesBindingBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    std::chrono::milliseconds binding_budget{};
    std::chrono::steady_clock::duration elapsed{};
    const auto started = std::chrono::steady_clock::now ();
    std::thread ready ([&] {
        std::this_thread::sleep_for (40ms);
        route.connect (target, "tcp://peer:7500");
        route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto timeout) {
            binding_budget = timeout;
            elapsed = std::chrono::steady_clock::now () - started;
            return request_task_t<parts_t> (result_t<parts_t>::success ({}));
        });
        std::this_thread::sleep_for (40ms);
        route.mark_peer_ready (target);
    });
    auto pending = route.request_reply_spot_parts (target, "spot", {}, started + 500ms);
    auto reply = pending.result ();
    ready.join ();
    ASSERT_TRUE (reply);
    ASSERT_GE (elapsed, 80ms);
    EXPECT_LE (binding_budget,
               500ms - std::chrono::duration_cast<std::chrono::milliseconds> (elapsed) + 1ms);
    EXPECT_GT (binding_budget, 0ms);
}

TEST (CppFrameworkRouteChannelRequest, SpotDeadlineBeforeBindingIsTimedOut)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto reply =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 20ms)
        .result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (0, submitted);
    EXPECT_EQ (0u, route.pending_request_count ());
}
TEST (CppFrameworkRouteChannelRequest, ExpiredCallerDeadlineDoesNotStartBinding)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto reply =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () - 1ms)
        .result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (0, submitted);
    EXPECT_EQ (0u, route.pending_request_count ());
}

TEST (CppFrameworkRouteChannelRequest, ThrowingSpotSubmissionLeavesNoPendingEntry)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.set_request_backend (
      [] (const auto &, const auto &, const auto &, auto) -> request_task_t<parts_t> {
          throw std::runtime_error ("submission failed");
      });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto pending =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 1s);
    auto reply = pending.result ();
    EXPECT_FALSE (reply);
    try {
        (void) reply.value ();
        ADD_FAILURE () << "backend exception was not preserved";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ ("submission failed", error.what ());
    }
    EXPECT_EQ (0u, route.pending_request_count ());
}


TEST (CppFrameworkRouteChannelRequest, SpotAdmissionFailureEndsWaitBeforePhysicalTerminal)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    auto submission = std::async (std::launch::async, [&] {
        return route.request_reply_spot_parts (target, "spot", {},
                                               std::chrono::steady_clock::now () + 1s);
    });
    const auto status = submission.wait_for (100ms);
    physical_reply.complete (result_t<parts_t>::success ({}));
    auto reply = submission.get ().result ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::unavailable, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationStopAndDestructionCompletePending)
{
    std::optional<request_task_t<parts_t>> pending;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        route.bind_endpoint ("tcp://self:7500");
        const auto target = zlink::routing_id_t::from (std::string ("peer"));
        route.connect (target, "tcp://peer:7500");
        route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
            return request_task_t<parts_t> (result_t<parts_t>::success ({}));
        });
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
        EXPECT_FALSE (pending->await_ready ());
        route.stop ();
    }
    auto reply = pending->result ();
    EXPECT_FALSE (reply);
    // Submit §3 admission table and §7: runtime shutdown settles preparation.
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationDeadlineKeepsOriginalBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    route.set_request_backend ([] (const auto &, const auto &, const auto &, auto) {
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto pending =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 20ms);
    EXPECT_FALSE (pending.await_ready ());
    auto terminal = std::async (std::launch::async, [&] { return pending.result (); });
    const auto status = terminal.wait_for (1s);
    // Release on failure as well, so a broken expiry cannot hang test teardown.
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, SpotPhysicalTerminalRetainsArgumentsAfterStopAndDestruction)
{
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    auto held = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = held;
    std::optional<request_task_t<parts_t>> pending;
    const zlink::routing_id_t *borrowed_target = nullptr;
    const parts_t *borrowed_parts = nullptr;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        route.set_request_backend (
          [held, &physical_reply, admitted, &borrowed_target,
           &borrowed_parts] (const auto &target, const auto &, const auto &parts, auto) {
              borrowed_target = &target;
              borrowed_parts = &parts;
              return request_task_t<parts_t> (physical_reply.task (), admitted);
          });
        pending.emplace (
          route.request_reply_spot_parts (zlink::routing_id_t::from (std::string ("peer")), "spot",
                                          {}, std::chrono::steady_clock::now () + 1s));
        route.stop ();
    }
    held.reset ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "not admitted"));
    EXPECT_FALSE (pending->result ());
    EXPECT_FALSE (lifetime.expired ());
    EXPECT_EQ ("peer", borrowed_target->to_string ());
    EXPECT_EQ (0u, borrowed_parts->size ());
    physical_reply.complete (result_t<parts_t>::success ({}));
    EXPECT_TRUE (lifetime.expired ());
}

TEST (CppFrameworkRouteChannelRequest, DisconnectedNodeSendIsUnavailableWithoutRegisteringDeadline)
{
    detail::route_channel_runtime_t route ("route");
    route.start ();
    const auto before = detail::deadline_scheduler_t::instance ().scheduled_count ();
    auto result = route.submit_send_parts (zlink::routing_id_t::from ("peer"), {});
    EXPECT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
    EXPECT_EQ (before, detail::deadline_scheduler_t::instance ().scheduled_count ());
}

TEST (CppFrameworkRouteChannelRequest, ExpiredSpotDeadlineRemainsTimedOutWhenStopped)
{
    detail::route_channel_runtime_t route ("spot-route");
    auto pending =
      route.request_reply_spot_parts (zlink::routing_id_t::from (std::string ("peer")), "spot", {},
                                      std::chrono::steady_clock::now () - 1ms);
    auto reply = pending.result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest,
      SpotPreparationTimeoutReleasesApplicationTurnWithoutStateChange)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    auto destroyed = std::make_shared<std::promise<void>> ();
    auto destruction = destroyed->get_future ();
    std::optional<request_task_t<parts_t>> pending;
    {
        auto turn = std::make_shared<readiness_turn_t> (destroyed);
        detail::serial_turn_scope_t scope (turn);
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 20ms));
    }
    auto reply = pending->result ();
    pending.reset ();
    const auto status = destruction.wait_for (100ms);
    // Stop only after observing cleanup; the old observer is released by this event.
    route.stop ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (std::future_status::ready, status);
}

TEST (CppFrameworkRouteChannelRequest, SpotPreparationPreservesParentTurnCancellation)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    auto turn = std::make_shared<readiness_turn_t> (std::make_shared<std::promise<void>> ());
    std::optional<request_task_t<parts_t>> pending;
    {
        detail::serial_turn_scope_t scope (turn);
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
    }
    turn->cancel_waits ();
    auto terminal = std::async (std::launch::async, [&] { return pending->result (); });
    const auto status = terminal.wait_for (100ms);
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

}
