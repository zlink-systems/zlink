/* SPDX-License-Identifier: Elastic-2.0 */
#include "runtime/streams/admission_queue_node.hpp"
#include "runtime/streams/admission_queue_state.hpp"
#include "runtime/streams/stream_host_ingress.hpp"

#include <zlink/framework.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

namespace zlink::framework::detail
{
void configure_stream_dispatch_executor ();
void shutdown_stream_dispatch_executor () noexcept;
}

namespace
{
namespace fw = zlink::framework;
namespace aq = fw::runtime::admission_queue;
using aq::json_t;
using namespace std::chrono_literals;

void require (bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error (std::string (message));
}

struct session_t final : fw::packet_stream_session_t
{
    explicit session_t (bool fail) : fail_handler (fail) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &,
                                const fw::session_message_context_t &,
                                const fw::message_t &) override
    {
        ++calls;
        if (fail_handler)
            throw fw::framework_exception_t (fw::framework_error_kind_t::internal_failure,
                                             "test authentication failure");
        co_return;
    }

    std::atomic_int calls{0};
    bool fail_handler;
};

struct outcome_t
{
    fw::result_t<void> result;
    fw::detail::stream_metadata_t metadata;
};

void exercise (const json_t &fixture)
{
    const auto &given = fixture.at ("given");
    const auto &steps = fixture.at ("steps");
    const auto &expected = steps.at (0).at ("expect");
    const auto expected_auth = expected.value ("handlerCalls", 1);
    const auto spot_outcome = given.value ("spotPermit", std::string{});
    const auto has_report = !given.at ("reportedEpoch").is_null ();
    const auto epoch = has_report ? given.at ("reportedEpoch").get<std::uint64_t> () : 42;
    const auto fail_handler =
      steps.size () > 1 && steps.at (1).value ("outcome", "") == std::string ("failure");
    const auto max = given.value ("globalLimit", json_t{});
    const auto limit = max.is_null () ? std::nullopt : std::optional (max.get<std::int32_t> ());
    auto signer = std::make_shared<aq::ticket_signer_t> (
      "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", std::nullopt);
    const aq::node_t node_identity{{std::byte{1}}, 1};
    std::atomic_int acquisitions{0};
    std::atomic_int releases{0};
    std::atomic_uint64_t submitted_seq{0};
    aq::node_report_t latest_report;
    session_t session (fail_handler);

    aq::queue_node_t::ports_t ports{
      [&] { return node_identity; }, [] { return aq::owner_t{"owner", 1}; },
      [&] (const char *packet, json_t message) -> fw::task_t<json_t> {
          if (std::string_view (packet) == aq::report_packet) {
              latest_report = aq::read_report (message);
              return fw::task_t<json_t> (fw::result_t<json_t>::success (
                aq::write_admission_message (aq::report_reply_t{epoch, {}})));
          }
          require (std::string_view (packet) == aq::admit_packet, "unexpected Spot packet");
          require (session.calls == 0, "handler ran before AdmitPermit");
          ++acquisitions;
          submitted_seq = aq::read_admit (message).request_seq;
          if (spot_outcome == "noResponseWithinSpotRequestTimeout")
              return fw::task_t<json_t> (fw::result_t<json_t>::failure (
                fw::framework_error_kind_t::unavailable, "fake Spot request timeout"));
          return fw::task_t<json_t> (fw::result_t<json_t>::success (
            aq::write_admit_reply (spot_outcome == "granted" ? 1 : 0)));
      },
      [&] (const aq::release_t &message) {
          require (message.node == node_identity && message.permit_id == 1,
                   "ReleasePermit identities changed");
          ++releases;
          return fw::task_t<void> (fw::result_t<void>::success ());
      }};
    auto queue = std::make_shared<aq::queue_node_t> (aq::settings_t{"fixture", signer, limit},
                                                     std::move (ports));
    if (has_report)
        queue->report (1, 0).result ().value ();

    fw::stream_snapshot_t snapshot{.name = "login"};
    snapshot.authentication.packet_name = "login";
    fw::runtime::stream_host_ingress_t ingress ({snapshot});
    ingress.bind_queue (queue);

    fw::zlink_builder_t builder;
    builder.stream (snapshot.name).bind ("tcp://127.0.0.1:*");
    fw::serializer_registry_t serializers;
    fw::detail::bind_stream_serializers (builder, serializers);
    const auto runtime = fw::detail::stream_runtime_t::from (builder);
    auto stream = runtime.open_session (snapshot.name);
    const auto session_rid = zlink::routing_id_t::from ("a");
    runtime.set_session_identity (stream, session_rid);
    const auto identity = queue->connection (snapshot.name, session_rid);
    require (identity.has_value (), "fixture node identity is missing");

    const auto ticket_state = given.at ("ticketState").get<std::string> ();
    fw::detail::stream_metadata_t metadata;
    if (ticket_state != "missing") {
        const auto now = aq::queue_time_t::now ().unix_ms;
        auto signed_for = *identity;
        if (ticket_state == "bad-signature")
            signed_for.session_rid = {std::byte{2}};
        metadata.with (
          aq::client_ticket_key,
          signer->issue (signed_for, ticket_state == "wrong-epoch" ? epoch + 1 : epoch,
                         ticket_state == "expired" ? now - 2001 : now + aq::ticket_lifetime_ms));
    }
    const fw::detail::stream_header_t header (
      fw::detail::stream_message_kind_t::request, fw::stream_codec_t::raw,
      fw::detail::stream_header_flags_t::has_request_seq, 1, "login", std::move (metadata));

    std::promise<outcome_t> completion;
    auto future = completion.get_future ();
    auto settle = [&] (const fw::result_t<void> &result,
                       fw::detail::stream_metadata_t metadata = {}) {
        completion.set_value ({result, std::move (metadata)});
    };
    const auto dispatched = ingress.publish (
      runtime, session, stream, snapshot.name, header, zlink::message_t{}, {}, settle,
      [] { return false; }, settle, [] (auto work) { work ({}); });
    require (dispatched.has_value (), "STREAM ingress failed synchronously");
    if (expected.contains ("handlerCallsBeforeStageOne"))
        require (acquisitions == 0, "valid ticket did not take stage one");
    require (future.wait_for (3s) == std::future_status::ready, "STREAM ingress did not finish");
    const auto result = future.get ();
    runtime.drain_async_dispatch (stream);
    require (session.calls == expected_auth, "unexpected authentication handler count");

    if (expected.contains ("errorCode"))
        require (!result.result
                   && result.result.error_kind () == fw::framework_error_kind_t::unavailable,
                 "expected unavailable authentication response");
    if (expected.contains ("metadata")) {
        const auto &want = expected.at ("metadata");
        require (result.metadata.values () == want.get<std::map<std::string, std::string>> (),
                 "unexpected queueRequired metadata");
    }
    if (expected.contains ("stageOneInFlightDelta"))
        require (ingress.in_flight () == expected.at ("stageOneInFlightDelta").get<unsigned> (),
                 "ticket rejection entered stage one");
    if (expected.contains ("admitPermitRequests"))
        require (acquisitions == expected.at ("admitPermitRequests").get<int> (),
                 "unexpected AdmitPermit count");
    if (expected.contains ("admitPermitRequestSeq"))
        require (submitted_seq == expected.at ("admitPermitRequestSeq").get<unsigned> (),
                 "unexpected AdmitPermit sequence");
    const auto release_count = fail_handler
                                 ? steps.at (1).at ("expect").at ("releasePermitSends").get<int> ()
                                 : expected.value ("releasePermitSends", 0);
    require (releases == release_count, "unexpected ReleasePermit count");

    if (steps.size () > 1 && steps.at (1).at ("action") == "nodeReport") {
        queue->report (1, 0).result ().value ();
        const auto &report_expected = steps.at (1).at ("expect");
        require (latest_report.resolved_request_seq
                     == report_expected.at ("resolvedRequestSeq").get<unsigned> ()
                   && json_t (latest_report.held) == report_expected.at ("heldPermits"),
                 "unexpected resolved permit report");
    }
}
} // namespace

int main ()
{
    fw::detail::configure_stream_dispatch_executor ();
    const auto shutdown = std::shared_ptr<void> (
      nullptr, [] (void *) { fw::detail::shutdown_stream_dispatch_executor (); });
    try {
        const auto fixture = json_t::parse (std::ifstream (ZLINK_ADMISSION_QUEUE_PATH));
        const auto &scenarios = fixture.at ("nodeScenarios");
        require (scenarios.size () == 10, "Expected ten node scenarios");
        for (const auto &scenario : scenarios) {
            try {
                exercise (scenario);
            }
            catch (const std::exception &error) {
                throw std::runtime_error (scenario.at ("name").get<std::string> () + ": "
                                          + error.what ());
            }
            std::cout << "PASS " << scenario.at ("name").get<std::string> () << '\n';
        }
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what () << '\n';
        return 1;
    }
}
