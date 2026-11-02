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
    auto dispatcher = detail::borrow_completion_dispatcher ();
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
    auto dispatcher = detail::borrow_completion_dispatcher ();
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

TEST (CppFrameworkRouteChannelRequest, PublicSpotDelegatesAdmissionWithSingleBudget)
{
    public_spot_fixture_t fixture;
    auto &route = fixture.route ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    parts_t reply_parts;
    int submitted = 0;
    std::chrono::milliseconds submitted_timeout{};
    route.set_request_backend ([&] (const auto &, const auto &, const auto &parts, auto timeout) {
        ++submitted;
        submitted_timeout = timeout;
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (parts).value ();
        header.kind = runtime::messaging::message_kind_t::response;
        reply_parts = codec.encode_raw_body_parts (header, codec.decode_body (parts).value ());
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    auto client = fixture.builder.route_client (fixture.serializers);
    auto pending =
      client.request_to_spot ("spot", spot_packet_t{79}).timeout (1s).async<spot_packet_t> ();
    // Submit §5 assigns service admission to the backend owner, before peer readiness.
    EXPECT_EQ (1, submitted);
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_GT (submitted_timeout, 0ms);
    EXPECT_LE (submitted_timeout, 1s);
    route.mark_peer_ready (target);
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_EQ (1, submitted);
    admitted->complete (result_t<void>::success ());
    physical_reply.complete (result_t<parts_t>::success (std::move (reply_parts)));
    const auto result = pending.result ();
    ASSERT_TRUE (result);
    EXPECT_EQ (79, result.value ().value);
    EXPECT_EQ (0u, route.pending_request_count ());
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
    const auto reply = route.request_reply_parts (target, {}, 1s).result ().value ();
    ASSERT_FALSE (reply);
    EXPECT_EQ (reply.error (), nullptr);
    ASSERT_NE (reply.exception (), nullptr);
    try {
        (void) reply.value ();
        ADD_FAILURE () << "backend exception was not preserved";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ ("submission failed", error.what ());
    }
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

TEST (CppFrameworkRouteChannelRequest, SpotSubmissionConsumesOriginalCallerBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    std::chrono::milliseconds binding_budget{};
    std::chrono::steady_clock::duration elapsed{};
    const auto started = std::chrono::steady_clock::now ();
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto timeout) {
        ++submitted;
        binding_budget = timeout;
        elapsed = std::chrono::steady_clock::now () - started;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    std::this_thread::sleep_for (80ms);
    auto reply = route.request_reply_spot_parts (target, "spot", {}, started + 500ms).result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (1, submitted);
    ASSERT_GE (elapsed, 80ms);
    EXPECT_LE (binding_budget,
               500ms - std::chrono::duration_cast<std::chrono::milliseconds> (elapsed) + 1ms);
    EXPECT_GT (binding_budget, 0ms);
}


TEST (CppFrameworkRouteChannelRequest, SpotBackendAdmissionDeadlineIsPreserved)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    int submitted = 0;
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::deadline_exceeded, "backend deadline"));
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto timeout) {
        ++submitted;
        EXPECT_GT (timeout, 0ms);
        EXPECT_LE (timeout, 20ms);
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    auto reply =
      route.request_reply_spot_parts (target, "spot", {}, std::chrono::steady_clock::now () + 20ms)
        .result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    EXPECT_EQ (1, submitted);
    EXPECT_EQ (0u, route.pending_request_count ());
    physical_reply.complete (result_t<parts_t>::success ({}));
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

TEST (CppFrameworkRouteChannelRequest, SpotBackendOwnsShutdownAfterWrapperDestruction)
{
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    std::optional<request_task_t<parts_t>> pending;
    int submitted = 0;
    {
        detail::route_channel_runtime_t route ("spot-route");
        route.start ();
        route.bind_endpoint ("tcp://self:7500");
        const auto target = zlink::routing_id_t::from (std::string ("peer"));
        route.connect (target, "tcp://peer:7500");
        route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
            ++submitted;
            return request_task_t<parts_t> (physical_reply.task (), admitted);
        });
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
        EXPECT_EQ (1, submitted);
        EXPECT_FALSE (pending->await_ready ());
        route.stop ();
    }
    EXPECT_FALSE (pending->await_ready ());
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::shutting_down, "backend shutdown"));
    auto reply = pending->result ();
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
    physical_reply.complete (result_t<parts_t>::success ({}));
}


TEST (CppFrameworkRouteChannelRequest, SpotBackendDeadlineKeepsOriginalBudget)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto timeout) {
        ++submitted;
        EXPECT_GT (timeout, 0ms);
        EXPECT_LE (timeout, 20ms);
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    const auto deadline = std::chrono::steady_clock::now () + 20ms;
    auto pending = route.request_reply_spot_parts (target, "spot", {}, deadline);
    EXPECT_EQ (1, submitted);
    EXPECT_FALSE (pending.await_ready ());
    std::thread backend_deadline ([&] {
        std::this_thread::sleep_until (deadline);
        admitted->complete (
          result_t<void>::failure (framework_error_kind_t::deadline_exceeded, "backend deadline"));
    });
    auto terminal = std::async (std::launch::async, [&] { return pending.result (); });
    const auto status = terminal.wait_for (1s);
    backend_deadline.join ();
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
    physical_reply.complete (result_t<parts_t>::success ({}));
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

class v21_controlled_spot_resolver_t final : public runtime::spot_address_resolver_t
{
  public:
    task_completion_source_t<std::optional<runtime::spot_address_t>> resolved;
    task_t<std::optional<runtime::spot_address_t>> resolve_spot_address (std::string,
                                                                         std::string) override
    {
        return resolved.task ();
    }
    void invalidate_spot_address (std::string_view) override {}
    void invalidate_all_routes_after_store_recovery () override {}
    void finish ()
    {
        resolved.complete (result_t<std::optional<runtime::spot_address_t>>::success (
          runtime::spot_address_t{"spot-route", zlink::routing_id_t::from ("peer"), "spot"}));
    }
};

// Submit/completion §9: resolving the address consumes the original deadline.
TEST (CppFrameworkRouteChannelRequest, V21RedSpotResolveKeepsOriginalDeadline)
{
    public_spot_fixture_t fixture;
    auto resolver = std::make_shared<v21_controlled_spot_resolver_t> ();
    detail::channel_runtime_t::from (fixture.builder.message_bus ())
      .bind_spot_address_resolver (resolver);
    int submitted = 0;
    fixture.route ().set_request_backend (
      [&] (const auto &, const auto &, const auto &parts, auto) {
          ++submitted;
          runtime::messaging::envelope_codec_t codec;
          auto header = codec.decode_header (parts).value ();
          header.kind = runtime::messaging::message_kind_t::response;
          return request_task_t<parts_t> (result_t<parts_t>::success (
            codec.encode_raw_body_parts (header, codec.decode_body (parts).value ())));
      });
    auto client = fixture.builder.route_client (fixture.serializers);
    const auto started = std::chrono::steady_clock::now ();
    auto pending =
      client.request_to_spot ("spot", spot_packet_t{79}).timeout (20ms).async<spot_packet_t> ();
    // Keep the dependency unresolved beyond the deadline; release it for teardown.
    std::this_thread::sleep_until (started + 120ms);
    const bool completed_before_resolve = pending.await_ready ();
    resolver->finish ();
    const auto reply = pending.result ();
    EXPECT_TRUE (completed_before_resolve);
    EXPECT_EQ (0, submitted);
    ASSERT_FALSE (reply) << "late address resolution succeeded after the original deadline";
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, V21RedSpotResolveShutdownEndsOriginalWait)
{
    public_spot_fixture_t fixture;
    auto resolver = std::make_shared<v21_controlled_spot_resolver_t> ();
    auto runtime = detail::channel_runtime_t::from (fixture.builder.message_bus ());
    runtime.bind_spot_address_resolver (resolver);
    auto client = fixture.builder.route_client (fixture.serializers);
    auto pending =
      client.request_to_spot ("spot", spot_packet_t{79}).timeout (5s).async<spot_packet_t> ();
    runtime.shutdown ();
    // Observe shutdown without using the unresolved dependency to complete it.
    const auto observe_until = std::chrono::steady_clock::now () + 100ms;
    while (!pending.await_ready () && std::chrono::steady_clock::now () < observe_until)
        std::this_thread::yield ();
    const bool completed_before_resolve = pending.await_ready ();
    resolver->finish ();
    const auto reply = pending.result ();
    EXPECT_TRUE (completed_before_resolve);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, V21SpotResolveWaitOwnerStopPreservesTerminalAfterLateAddress)
{
    public_spot_fixture_t fixture;
    auto resolver = std::make_shared<v21_controlled_spot_resolver_t> ();
    detail::channel_runtime_t::from (fixture.builder.message_bus ())
      .bind_spot_address_resolver (resolver);
    auto client = fixture.builder.route_client (fixture.serializers);
    std::stop_source parent;
    std::optional<request_task_t<spot_packet_t>> pending;
    {
        detail::ambient_context_scope_t scope (nullptr, nullptr, parent.get_token ());
        pending.emplace (
          client.request_to_spot ("spot", spot_packet_t{79}).timeout (5s).async<spot_packet_t> ());
    }
    parent.request_stop ();
    const auto observe_until = std::chrono::steady_clock::now () + 100ms;
    while (!pending->await_ready () && std::chrono::steady_clock::now () < observe_until)
        std::this_thread::yield ();
    const bool completed_before_resolve = pending->await_ready ();
    resolver->finish ();
    const auto reply = pending->result ();
    EXPECT_TRUE (completed_before_resolve);
    // Cancellation/shutdown §5.1: ending the runtime wait owner is ShuttingDown.
    ASSERT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
}

// Positive control: the legacy Spot adapter does debit its local encoding time.
// The node adapter must preserve the same request-entry deadline.
void verify_v21_encoding_budget (bool spot)
{
    auto builder = test::runtime_failure_builder ();
    serializer_registry_t serializers;
    serializers.add<spot_packet_t> (
      [] (const auto &packet) {
          std::this_thread::sleep_for (80ms);
          return encoded_payload_t::from_string (std::to_string (packet.value));
      },
      [] (const encoded_payload_t &payload) {
          return spot_packet_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    builder.route_channel ("spot-route");
    auto manager = detail::channel_runtime_manager_t::from (builder);
    manager.initialize_route_channels (builder);
    detail::channel_runtime_t::from (builder.message_bus ())
      .bind_spot_address_resolver (std::make_shared<spot_resolver_t> ());
    auto &route = manager.get_route_channel ("spot-route");
    int submitted = 0;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &parts, auto) {
        ++submitted;
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (parts).value ();
        header.kind = runtime::messaging::message_kind_t::response;
        return request_task_t<parts_t> (result_t<parts_t>::success (
          codec.encode_raw_body_parts (header, codec.decode_body (parts).value ())));
    });
    auto client = builder.route_client (serializers);
    auto pending =
      spot
        ? client.request_to_spot ("spot", spot_packet_t{79}).timeout (20ms).async<spot_packet_t> ()
        : client
            .request_to_node ("spot-route", zlink::routing_id_t::from ("peer"), spot_packet_t{79})
            .timeout (20ms)
            .async<spot_packet_t> ();
    const auto reply = pending.result ();
    EXPECT_EQ (0, submitted);
    ASSERT_FALSE (reply) << "request encoding restarted the original deadline";
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, reply.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, V21RedNodeEncodingKeepsOriginalDeadline)
{
    verify_v21_encoding_budget (false);
}

TEST (CppFrameworkRouteChannelRequest, V21SpotEncodingBudgetPositiveControl)
{
    verify_v21_encoding_budget (true);
}

struct v21_lane_packet_t
{
    static constexpr const char *packet_name = "v21.lane.request";
    int value;
};

TEST (CppFrameworkRouteChannelRequest, V21NodeLaneWaitConsumesDeadlineWithoutBlockingSubmit)
{
    public_spot_fixture_t fixture;
    std::promise<std::chrono::steady_clock::time_point> request_started;
    fixture.serializers.add<v21_lane_packet_t> (
      [&] (const auto &packet) {
          request_started.set_value (std::chrono::steady_clock::now ());
          return encoded_payload_t::from_string (std::to_string (packet.value));
      },
      [] (const encoded_payload_t &payload) {
          return v21_lane_packet_t{std::stoi (payload.to_string ())};
      },
      "application/json");
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    unsigned submitted = 0;
    fixture.route ().set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        entered.set_value ();
        released.wait ();
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    const auto target = zlink::routing_id_t::from ("peer");
    auto first = std::async (std::launch::async, [&] {
        return fixture.route ().request_reply_spot_parts (target, "spot", {},
                                                          std::chrono::steady_clock::now () + 5s);
    });
    entered.get_future ().wait ();
    auto client = fixture.builder.route_client (fixture.serializers);
    auto second = std::async (std::launch::async, [&] {
        return client.request_to_node ("spot-route", target, v21_lane_packet_t{79})
          .timeout (20ms)
          .async<spot_packet_t> ();
    });
    const auto deadline_observation = request_started.get_future ().get () + 100ms;
    const auto status = second.wait_for (100ms);
    (void) released.wait_until (deadline_observation);
    // Release the occupied owner on either implementation before assertions.
    release.set_value ();
    (void) first.get ().result ();
    const auto terminal = second.get ().result ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_EQ (1u, submitted);
    ASSERT_FALSE (terminal);
    EXPECT_EQ (framework_error_kind_t::deadline_exceeded, terminal.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, V21FinalReceiverDecodesLatePhysicalSpotReplyOnce)
{
    for (const bool node : {false, true}) {
        for (const bool decode_failure : {false, true}) {
            SCOPED_TRACE (node);
            SCOPED_TRACE (decode_failure);
            auto builder = test::runtime_failure_builder ();
            serializer_registry_t serializers;
            unsigned decoded = 0;
            serializers.add<spot_packet_t> (
              [] (const auto &packet) {
                  return encoded_payload_t::from_string (std::to_string (packet.value));
              },
              [&] (const encoded_payload_t &payload) {
                  ++decoded;
                  if (decode_failure)
                      throw framework_exception_t (framework_error_kind_t::protocol_error,
                                                   "typed reply decode failed");
                  return spot_packet_t{std::stoi (payload.to_string ())};
              },
              "application/json");
            builder.route_channel ("spot-route");
            auto manager = detail::channel_runtime_manager_t::from (builder);
            manager.initialize_route_channels (builder);
            auto runtime = detail::channel_runtime_t::from (builder.message_bus ());
            runtime.bind_spot_address_resolver (std::make_shared<spot_resolver_t> ());
            task_completion_source_t<parts_t> physical;
            auto admitted = std::make_shared<task_completion_source_t<void>> ();
            parts_t response;
            auto held = std::make_shared<int> (1);
            std::weak_ptr<int> retained = held;
            manager.get_route_channel ("spot-route")
              .set_request_backend (
                [&, held] (const auto &, const auto &, const auto &parts, auto) {
                    runtime::messaging::envelope_codec_t codec;
                    auto header = codec.decode_header (parts).value ();
                    header.kind = runtime::messaging::message_kind_t::response;
                    response =
                      codec.encode_raw_body_parts (header, codec.decode_body (parts).value ());
                    return request_task_t<parts_t> (physical.task (), admitted);
                });
            auto client = builder.route_client (serializers);
            auto pending =
              node ? client
                       .request_to_node ("spot-route", zlink::routing_id_t::from ("peer"),
                                         spot_packet_t{79})
                       .async<spot_packet_t> ()
                   : client.request_to_spot ("spot", spot_packet_t{79}).async<spot_packet_t> ();
            admitted->complete (
              result_t<void>::failure (framework_error_kind_t::unavailable, "admission refused"));
            ASSERT_FALSE (pending.result ());
            EXPECT_EQ (framework_error_kind_t::unavailable, pending.result ().error_kind ());
            runtime.shutdown ();
            held.reset ();
            EXPECT_FALSE (retained.expired ());
            EXPECT_EQ (0u, decoded);
            physical.complete (result_t<parts_t>::success (std::move (response)));
            const auto terminal = detail::request_task_access_t::physical_reply (pending).result ();
            EXPECT_EQ (1u, decoded);
            EXPECT_TRUE (retained.expired ());
            EXPECT_EQ (framework_error_kind_t::unavailable, pending.result ().error_kind ());
            if (decode_failure) {
                ASSERT_FALSE (terminal);
                EXPECT_EQ (framework_error_kind_t::protocol_error, terminal.error_kind ());
            } else {
                ASSERT_TRUE (terminal);
                EXPECT_EQ (79, terminal.value ().value);
            }
        }
    }
}

TEST (CppFrameworkRouteChannelRequest, V21QueuedSubmissionRetainsOriginalWaitOwner)
{
    public_spot_fixture_t fixture;
    std::promise<void> entered, release, second_entered;
    auto released = release.get_future ().share ();
    task_completion_source_t<parts_t> physical;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    using cancellation_t = std::stop_callback<std::function<void ()>>;
    std::unique_ptr<cancellation_t> cancellation;
    unsigned submitted = 0;
    bool owner_preserved = false;
    fixture.route ().set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        if (++submitted == 1) {
            entered.set_value ();
            released.wait ();
            return request_task_t<parts_t> (result_t<parts_t>::success ({}));
        }
        const auto owner = detail::current_wait_owner ();
        owner_preserved = owner.stop_possible ();
        cancellation = std::make_unique<cancellation_t> (owner, [admitted] {
            admitted->complete (result_t<void>::failure (framework_error_kind_t::shutting_down,
                                                         "backend wait owner stopped"));
        });
        second_entered.set_value ();
        return request_task_t<parts_t> (physical.task (), admitted);
    });
    const auto target = zlink::routing_id_t::from ("peer");
    auto first = std::async (std::launch::async, [&] {
        return fixture.route ().request_reply_spot_parts (target, "spot", {},
                                                          std::chrono::steady_clock::now () + 5s);
    });
    entered.get_future ().wait ();
    auto client = fixture.builder.route_client (fixture.serializers);
    std::stop_source parent;
    auto second = std::async (std::launch::async, [&] {
        detail::ambient_context_scope_t invocation (nullptr, nullptr, parent.get_token ());
        return client.request_to_node ("spot-route", target, spot_packet_t{79})
          .timeout (5s)
          .async<spot_packet_t> ();
    });
    const auto submission_status = second.wait_for (100ms);
    release.set_value ();
    (void) first.get ().result ();
    auto pending = second.get ();
    const auto backend_status = second_entered.get_future ().wait_for (1s);
    parent.request_stop ();
    auto terminal = std::async (std::launch::async, [&] { return pending.result (); });
    const auto terminal_status = terminal.wait_for (100ms);
    // Finish teardown even if a regression discarded the backend's wait owner.
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::shutting_down, "backend teardown"));
    const auto result = terminal.get ();
    physical.complete (result_t<parts_t>::success ({}));
    EXPECT_EQ (std::future_status::ready, submission_status);
    EXPECT_EQ (std::future_status::ready, backend_status);
    EXPECT_TRUE (owner_preserved);
    EXPECT_EQ (std::future_status::ready, terminal_status);
    ASSERT_FALSE (result);
    EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
}

TEST (CppFrameworkRouteChannelRequest, V21SpotEncodingExceptionKeepsAdmissionFailure)
{
    public_spot_fixture_t fixture;
    fixture.serializers.add<v21_lane_packet_t> (
      [] (const auto &) -> encoded_payload_t { throw std::runtime_error ("encoding failed"); },
      [] (const encoded_payload_t &) { return v21_lane_packet_t{}; }, "application/json");
    unsigned submitted = 0;
    fixture.route ().set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        ++submitted;
        return request_task_t<parts_t> (result_t<parts_t>::success ({}));
    });
    auto pending = fixture.builder.route_client (fixture.serializers)
                     .request_to_spot ("spot", v21_lane_packet_t{79})
                     .async<spot_packet_t> ();
    const auto admitted = pending.admitted ().result ();
    const auto terminal = pending.result ();
    EXPECT_EQ (0u, submitted);
    ASSERT_FALSE (admitted);
    ASSERT_NE (admitted.error (), nullptr);
    EXPECT_EQ (framework_error_kind_t::protocol_error, admitted.error_kind ());
    EXPECT_STREQ ("payload serialization failed", admitted.error ()->what ());
    EXPECT_EQ (detail::failure_origin_t::payload_encode,
               detail::failure_origin (*admitted.error ()));
    ASSERT_FALSE (terminal);
    ASSERT_NE (terminal.error (), nullptr);
    EXPECT_EQ (framework_error_kind_t::protocol_error, terminal.error_kind ());
    EXPECT_STREQ ("payload serialization failed", terminal.error ()->what ());
    EXPECT_EQ (detail::failure_origin_t::payload_encode,
               detail::failure_origin (*terminal.error ()));
}

TEST (CppFrameworkRouteChannelRequest, V21SubmissionExceptionReachesPreparedReceiver)
{
    for (const bool node : {false, true}) {
        SCOPED_TRACE (node);
        public_spot_fixture_t fixture;
        fixture.route ().set_request_backend (
          [] (const auto &, const auto &, const auto &, auto) -> request_task_t<parts_t> {
              throw std::runtime_error ("submission failed");
          });
        auto client = fixture.builder.route_client (fixture.serializers);
        auto pending =
          node ? client
                   .request_to_node ("spot-route", zlink::routing_id_t::from ("peer"),
                                     spot_packet_t{79})
                   .async<spot_packet_t> ()
               : client.request_to_spot ("spot", spot_packet_t{79}).async<spot_packet_t> ();
        auto admitted = pending.admitted ().result ();
        const auto terminal = pending.result ();
        ASSERT_FALSE (admitted);
        ASSERT_FALSE (terminal);
        EXPECT_EQ (terminal.error (), nullptr);
        try {
            (void) terminal.value ();
            ADD_FAILURE () << "backend exception was not preserved";
        }
        catch (const std::runtime_error &error) {
            EXPECT_STREQ ("submission failed", error.what ());
        }
        EXPECT_EQ (0u, fixture.route ().pending_request_count ());
    }
}

TEST (CppFrameworkRouteChannelRequest, V21ResolutionFailureKeepsResolverInvalidationOwner)
{
    class failed_resolver_t final : public runtime::spot_address_resolver_t
    {
      public:
        unsigned invalidations = 0;
        task_t<std::optional<runtime::spot_address_t>> resolve_spot_address (std::string,
                                                                             std::string) override
        {
            return task_t<std::optional<runtime::spot_address_t>> (
              result_t<std::optional<runtime::spot_address_t>>::failure (
                framework_error_kind_t::unavailable, "lookup failed"));
        }
        void invalidate_spot_address (std::string_view) override { ++invalidations; }
        void invalidate_all_routes_after_store_recovery () override {}
    };
    auto builder = test::runtime_failure_builder ();
    serializer_registry_t serializers;
    auto resolver = std::make_shared<failed_resolver_t> ();
    detail::channel_runtime_t::from (builder.message_bus ()).bind_spot_address_resolver (resolver);
    auto pending = builder.route_client (serializers)
                     .request_to_spot ("spot", spot_packet_t{79})
                     .async<spot_packet_t> ();
    const auto terminal = pending.result ();
    ASSERT_FALSE (terminal);
    EXPECT_EQ (framework_error_kind_t::unavailable, terminal.error_kind ());
    ASSERT_NE (terminal.error (), nullptr);
    EXPECT_STREQ (terminal.error ()->what (), "lookup failed");
    EXPECT_EQ (0u, resolver->invalidations);
}

TEST (CppFrameworkRouteChannelRequest, SpotBackendPreservesParentTurnCancellation)
{
    detail::route_channel_runtime_t route ("spot-route");
    route.start ();
    route.bind_endpoint ("tcp://self:7500");
    const auto target = zlink::routing_id_t::from (std::string ("peer"));
    route.connect (target, "tcp://peer:7500");
    task_completion_source_t<parts_t> physical_reply;
    auto admitted = std::make_shared<task_completion_source_t<void>> ();
    using cancellation_t = std::stop_callback<std::function<void ()>>;
    std::unique_ptr<cancellation_t> cancellation;
    route.set_request_backend ([&] (const auto &, const auto &, const auto &, auto) {
        auto owner = detail::current_wait_owner ();
        EXPECT_TRUE (owner.stop_possible ());
        cancellation = std::make_unique<cancellation_t> (owner, [admitted] {
            admitted->complete (
              result_t<void>::failure (framework_error_kind_t::shutting_down, "backend cancelled"));
        });
        return request_task_t<parts_t> (physical_reply.task (), admitted);
    });
    auto turn = std::make_shared<readiness_turn_t> (std::make_shared<std::promise<void>> ());
    std::optional<request_task_t<parts_t>> pending;
    {
        detail::serial_turn_scope_t scope (turn);
        detail::ambient_context_scope_t invocation (nullptr, nullptr, turn->wait_cancellation ());
        pending.emplace (route.request_reply_spot_parts (target, "spot", {},
                                                         std::chrono::steady_clock::now () + 1s));
    }
    EXPECT_FALSE (pending->await_ready ());
    turn->cancel_waits ();
    auto terminal = std::async (std::launch::async, [&] { return pending->result (); });
    const auto status = terminal.wait_for (100ms);
    // Ensure a broken propagation cannot block test teardown.
    admitted->complete (
      result_t<void>::failure (framework_error_kind_t::shutting_down, "backend teardown"));
    route.stop ();
    auto reply = terminal.get ();
    EXPECT_EQ (std::future_status::ready, status);
    EXPECT_FALSE (reply);
    EXPECT_EQ (framework_error_kind_t::shutting_down, reply.error_kind ());
    physical_reply.complete (result_t<parts_t>::success ({}));
}


}
