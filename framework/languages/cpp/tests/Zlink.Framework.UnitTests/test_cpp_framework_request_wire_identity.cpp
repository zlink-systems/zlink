/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <zlink/framework/detail/binary_text_codec.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace
{
namespace fw = zlink::framework;
namespace messaging = fw::runtime::messaging;
namespace protocol = fw::runtime::protocol;

const nlohmann::json &wire_fixtures ()
{
    static const auto fixtures = [] {
        std::ifstream input (ZLINK_REQUEST_WIRE_FIXTURE_PATH);
        return nlohmann::json::parse (input);
    }();
    return fixtures.at ("cases");
}

messaging::envelope_header_t request_header (int scenario)
{
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::request;
    header.channel_name = scenario == 1 ? "b\\\"ench" : "bench";
    header.message_name = "BenchPayload";
    header.content_type = "application/x-protobuf";
    header.correlation_id = "123abc";
    header.deadline = "2026-11-07T12:34:56Z";
    if (scenario == 2)
        header.metadata = {{"escape\n", "v\\\"\t"}, {"utf8", "한글"}};
    if (scenario == 3) {
        header.flow_id = "019a55d5-0000-7000-8000-000000000001";
        header.flow_origin = fw::flow_origin_t::application;
    }
    if (scenario == 4) {
        header.topic = "topic";
        header.source = "source";
    }
    if (scenario == 5)
        header.deadline.reset ();
    return header;
}

fw::encoded_payload_t request_payload ()
{
    // BenchPayload { bytes body = 1; }: protobuf tag and length, then the 1 KiB body.
    std::string body ("\x0a\x80\x08", 3);
    body.append (1024, '\xab');
    return fw::encoded_payload_t::from_string (body);
}

messaging::message_parts_t encode_request (messaging::envelope_header_t header, bool service)
{
    auto type = std::make_shared<const fw::detail::request_type_plan_t> (
      fw::detail::prepare_request_type (std::string_view (header.message_name),
                                        std::string (std::string_view (header.content_type))));
    auto channel = std::make_shared<const fw::detail::prepared_header_fragment_t> (
      fw::detail::prepare_request_channel (std::string_view (header.channel_name)));
    messaging::client_call_codec_t codec;
    return service ? codec.encode_service_payload_parts (std::move (header), request_payload (),
                                                         std::move (type), std::move (channel))
                   : codec.encode_payload_parts (std::move (header), request_payload (),
                                                 std::move (type), std::move (channel));
}

std::string frame_hex (const zlink::message_t &frame)
{
    return fw::detail::encode_hex (frame.bytes ());
}

class RequestWireIdentity : public testing::TestWithParam<int>
{
};

TEST_P (RequestWireIdentity, PreparedServiceAndRouteMatchMainEncoderBytes)
{
    const auto scenario = GetParam ();
    const auto &fixture = wire_fixtures ().at (scenario);
    SCOPED_TRACE (fixture.at ("name").get<std::string> ());
    const auto expected = fixture.at ("application_payload_hex").get<std::string> ();
    EXPECT_EQ (frame_hex (protocol::encode_node_request_header_message (0x123abc)),
               fixture.at ("service_header_hex").get<std::string> ());
    for (const bool service : {false, true}) {
        SCOPED_TRACE (service ? "MeshNode" : "RouteChannel");
        auto parts = encode_request (request_header (scenario), service);
        auto application = std::move (parts).take_application_payload ();
        auto frame = protocol::encode_application_payload_message (application);
        EXPECT_EQ (frame_hex (frame), expected);
        EXPECT_EQ (fw::detail::encode_hex (std::span<const std::uint8_t> (
                     protocol::encode_application_payload (application))),
                   expected);
    }
}

INSTANTIATE_TEST_SUITE_P (MainEncoderFixtures, RequestWireIdentity, testing::Range (0, 6));

TEST (RequestWireIdentity, ChangedByteDoesNotMatchFixture)
{
    auto parts = encode_request (request_header (0), true);
    auto frame =
      protocol::encode_application_payload_message (std::move (parts).take_application_payload ());
    const auto expected =
      wire_fixtures ().at (0).at ("application_payload_hex").get<std::string> ();
    ASSERT_EQ (frame_hex (frame), expected);
    reinterpret_cast<std::byte *> (frame.data ())[frame.size () - 1] ^= std::byte{1};
    EXPECT_NE (frame_hex (frame), expected);
}

TEST (RequestWireIdentity, InvalidMetadataFailsBeforeServicePacking)
{
    for (const bool service : {false, true}) {
        auto header = request_header (0);
        header.metadata = {{std::string ("bad\0key", 7), "value"}};
        try {
            (void) encode_request (std::move (header), service);
            FAIL () << "Invalid metadata reached transport packing";
        }
        catch (const fw::framework_exception_t &error) {
            EXPECT_EQ (error.kind (), fw::framework_error_kind_t::protocol_error);
        }
    }
}


TEST (RequestWireIdentity, InvalidStaticTextFailsBeforeServicePacking)
{
    for (const bool service : {false, true}) {
        for (int field = 0; field != 3; ++field) {
            auto header = request_header (0);
            if (field == 0)
                header.channel_name = std::string (1, '\xff');
            else if (field == 1)
                header.message_name = std::string (1, '\xff');
            else
                header.content_type = std::string (1, '\xff');
            EXPECT_THROW (encode_request (std::move (header), service), nlohmann::json::type_error);
        }
    }
}

TEST (RequestWireIdentity, ServiceRetainsSubmitFlowAfterAmbientScopeEnds)
{
    auto parts = [] {
        const fw::runtime::flow_context_t::scope_t flow (fw::runtime::flow_value_t{
          "019a55d5-0000-7000-8000-000000000001", fw::flow_origin_t::application,
          fw::message_flow_log_mode_t::normal});
        return encode_request (request_header (0), true);
    }();
    auto frame =
      protocol::encode_application_payload_message (std::move (parts).take_application_payload ());
    EXPECT_EQ (frame_hex (frame),
               wire_fixtures ().at (3).at ("application_payload_hex").get<std::string> ());
}

TEST (RequestWireIdentity, CopiesCanMaterializePartsAndReleaseWithoutEncoding)
{
    auto parts = encode_request (request_header (2), true);
    auto copied = parts;
    EXPECT_EQ (copied.size (), 2u);
    auto application = std::move (parts).take_application_payload ();
    auto application_copy = application;
    auto materialized =
      protocol::application_payload_t::from_parts (std::move (copied).take_items ());
    EXPECT_EQ (application_copy, materialized);
    EXPECT_EQ (application, application_copy);
    EXPECT_EQ (frame_hex (protocol::encode_application_payload_message (materialized)),
               wire_fixtures ().at (2).at ("application_payload_hex").get<std::string> ());

    unsigned encoded = 0;
    auto discarded =
      protocol::application_payload_t::from_envelope_encoder ([&] (const auto &, auto) {
          ++encoded;
          return zlink::message_t::from ("discarded");
      });
    discarded.release_body ();
    EXPECT_EQ (encoded, 0u);
    ASSERT_NE (discarded.parts (), nullptr);
    EXPECT_TRUE (discarded.parts ()->empty ());
}

TEST (RequestWireIdentity, OuterFlowUsesTheSharedWireValidationAndWriter)
{
    for (const bool service : {false, true}) {
        auto parts = encode_request (request_header (0), service);
        auto application = std::move (parts).take_application_payload ();
        application.flow_id = "019a55d5-0000-7000-8000-000000000001";
        EXPECT_THROW (protocol::encode_application_payload_message (application),
                      protocol::service_wire_error_t);
        EXPECT_THROW (protocol::encode_application_payload (application),
                      protocol::service_wire_error_t);
        application.flow_origin = fw::flow_origin_t::application;
        auto frame = protocol::encode_application_payload_message (application);
        auto decoded = protocol::decode_application_payload (protocol::message_bytes (frame));
        EXPECT_EQ (decoded.flow_id, application.flow_id);
        EXPECT_EQ (decoded.flow_origin, application.flow_origin);
        EXPECT_EQ (protocol::encode_application_payload (decoded), frame.to_bytes ());
        EXPECT_EQ (protocol::encode_application_payload (application), frame.to_bytes ());
        application.flow_id = "invalid";
        EXPECT_THROW (protocol::encode_application_payload_message (application),
                      protocol::service_wire_error_t);
    }
}
} // namespace
