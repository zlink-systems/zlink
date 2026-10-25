/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "request_admission_fixture.hpp"
#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime_failure_fixture.hpp"
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace
{
namespace fw = zlink::framework;
using parts_t = fw::runtime::messaging::message_parts_t;
struct payload_t
{
    static constexpr const char *packet_name = "p";
    int value = 7;
};

TEST (RequestEncoderAllocations, SynchronousTypedEncoderUsesBorrowedFunctionContainer)
{
    fw::serializer_registry_t serializers;
    std::size_t before_serializer = 0;
    unsigned serialized = 0;
    serializers.add<payload_t> (
      [&] (const payload_t &) -> fw::encoded_payload_t {
          ++serialized;
          before_serializer = fw::tests::tracked_allocation_count;
          throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                           "encoder measurement stop");
      },
      [] (const fw::encoded_payload_t &) { return payload_t{}; }, "x/y");
    auto builder = fw::test::runtime_failure_builder ();
    auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
    runtime.bind_serializers (serializers);
    auto unused = [] (auto, auto) -> fw::request_task_t<fw::result_t<parts_t>> {
        co_return fw::result_t<parts_t>::failure (fw::framework_error_kind_t::unavailable,
                                                  "unexpected transport");
    };
    runtime.bind_mesh_node_transport (
      "m", {},
      zlink::framework::test::request_receiver ([&] (const auto &, auto parts, auto timeout) {
          return unused (std::move (parts), timeout);
      }));
    auto client = builder.route_client (serializers);
    const auto payload = std::make_shared<payload_t> ();
    auto encode = [payload] (fw::serializer_registry_t &registry) {
        return registry.get<payload_t> ().serialize_with_content_type (*payload);
    };
    // Match final-consumer preparation and the admission observer before the
    // encoder boundary. The owned-container control checks that this size
    // filter still distinguishes an allocating encoder from a borrowed one.
    bool owning_encoder = false;
    auto baseline = fw::detail::request_call_access_t::make (
      "p", &serializers,
      [&] (const std::string &name, auto timeout, const auto &,
           fw::detail::request_receiver_t<zlink::message_t> receiver) {
          auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
          receiver.admitted = admitted;
          receiver.fail.bind_admission (admitted);
          fw::route_client_t::payload_encoder_t container =
            owning_encoder ? fw::route_client_t::payload_encoder_t (encode)
                           : fw::route_client_t::payload_encoder_t (std::ref (encode));
          fw::runtime::messaging::client_call_codec_t codec;
          auto header = codec.create_envelope (fw::runtime::messaging::message_kind_t::request, "m",
                                               name, timeout);
          (void) header;
          (void) container (serializers);
          return admitted;
      });
    // Some standard libraries can store this owning callable inline. Calibrate
    // that representation before checking its extra heap cost; the typed
    // encoder must still match the borrowed prefix on every implementation.
    std::optional<fw::route_client_t::payload_encoder_t> owning_container;
    const auto calibrated_owned =
      fw::tests::count_allocations ([&] { owning_container.emplace (encode); }, sizeof (encode));
    ASSERT_NE (owning_container->template target<decltype (encode)> (), nullptr);
    owning_container.reset ();
    auto actual = client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{});
    (void) fw::tests::count_allocations ([&] { (void) baseline.async<payload_t> ().result (); },
                                         sizeof (encode));
    const auto expected = before_serializer;
    owning_encoder = true;
    (void) fw::tests::count_allocations ([&] { (void) baseline.async<payload_t> ().result (); },
                                         sizeof (encode));
    const auto owned = before_serializer;
    owning_encoder = false;
    (void) fw::tests::count_allocations ([&] { (void) actual.async<payload_t> ().result (); },
                                         sizeof (encode));
    EXPECT_EQ (serialized, 3u);
    EXPECT_EQ (owned, expected + calibrated_owned);
    EXPECT_EQ (before_serializer, expected);
    std::printf ("node encoder allocations before serializer: borrowed=%zu owned=%zu typed=%zu "
                 "target_size=%zu\n",
                 expected, owned, before_serializer, sizeof (encode));
}

TEST (RequestEncoderAllocations,
      ReentrantEncodingFinishesBeforePendingTransportAndPreservesItsFailure)
{
    for (const bool channel_request : {false, true}) {
        fw::serializer_registry_t serializers;
        unsigned encoded = 0;
        std::function<void ()> reenter;
        serializers.add<payload_t> (
          [&] (const payload_t &payload) {
              ++encoded;
              if (payload.value == 7)
                  reenter ();
              return fw::encoded_payload_t::from_string (std::to_string (payload.value));
          },
          [] (const fw::encoded_payload_t &) { return payload_t{}; }, "x/y");
        auto builder = fw::test::runtime_failure_builder ();
        auto runtime = fw::detail::channel_runtime_t::from (builder.message_bus ());
        runtime.bind_serializers (serializers);
        fw::task_completion_source_t<fw::result_t<parts_t>> release;
        auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
        admitted->complete (fw::result_t<void>::success ());
        const auto request = [&] (auto, auto) {
            return fw::request_task_t<fw::result_t<parts_t>> (release.task (), admitted);
        };
        runtime.bind_mesh_channel_transport (
          "c", {}, zlink::framework::test::channel_request_receiver (request));
        runtime.bind_mesh_node_transport (
          "m", {},
          zlink::framework::test::request_receiver ([&] (const auto &, auto parts, auto timeout) {
              return request (std::move (parts), timeout);
          }));
        auto client = builder.route_client (serializers);
        std::optional<fw::request_task_t<payload_t>> nested;
        reenter = [&] {
            nested.emplace (
              (channel_request
                 ? client.request_to_channel ("c", payload_t{8})
                 : client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{8}))
                .async<payload_t> ());
        };
        auto pending =
          (channel_request
             ? client.request_to_channel ("c", payload_t{})
             : client.request_to_node ("m", zlink::routing_id_t::from ("n"), payload_t{}))
            .async<payload_t> ();
        EXPECT_EQ (encoded, 2u);
        EXPECT_FALSE (pending.await_ready ());
        ASSERT_TRUE (nested.has_value ());
        EXPECT_FALSE (nested->await_ready ());
        EXPECT_TRUE (release.complete (
          fw::result_t<fw::result_t<parts_t>>::success (fw::result_t<parts_t>::failure (
            fw::framework_error_kind_t::unavailable, "delayed request failure"))));
        const auto &result = pending.result ();
        EXPECT_EQ (result.error_kind (), fw::framework_error_kind_t::unavailable);
        ASSERT_NE (result.error (), nullptr);
        EXPECT_STREQ (result.error ()->what (), "delayed request failure");
        EXPECT_EQ (nested->result ().error_kind (), fw::framework_error_kind_t::unavailable);
        EXPECT_EQ (encoded, 2u);
    }
}
} // namespace
