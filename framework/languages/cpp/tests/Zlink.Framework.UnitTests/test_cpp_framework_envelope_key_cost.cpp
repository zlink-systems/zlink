/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "runtime/messaging/envelope_codec.hpp"
#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
bool counting = false;
std::size_t key_scans = 0;
constexpr std::array<std::string_view, 14> field_names = {
  "kind",     "channelName", "messageName",  "contentType",  "correlationId",
  "deadline", "topic",       "errorCode",    "errorMessage", "source",
  "flowId",   "flowOrigin",  "formatMarker", "metadata"};
namespace msg = zlink::framework::runtime::messaging;

constexpr std::string_view fields =
  R"("formatMarker":242,"kind":1,"channelName":"c","messageName":"m","contentType":"x/y","correlationId":"corr","deadline":"d","topic":"t","errorCode":null,"errorMessage":null,"source":"s","metadata":{"a":"b"})";

std::string header (std::string_view extra = {})
{
    return "{" + std::string (fields) + std::string (extra) + "}";
}

auto decode (const std::string &json)
{
    return msg::envelope_codec_t{}.decode_header (zlink::message_t::from (json), false);
}

std::size_t decode_scans (const std::string &json)
{
    const auto wire = zlink::message_t::from (json);
    key_scans = 0;
    counting = true;
    const auto result = msg::envelope_codec_t{}.decode_header (wire, false);
    counting = false;
    EXPECT_TRUE (result);
    return key_scans;
}
} // namespace

extern "C" std::size_t __real_strlen (const char *text);
extern "C" std::size_t __wrap_strlen (const char *text)
{
    const auto size = __real_strlen (text);
    if (counting) {
        // Attribute actual libc calls by their argument, not by a source symbol.
        // Other required scans (content type, diagnostics) are outside this cost.
        const std::string_view value (text, size);
        for (const auto name : field_names) {
            if (value == name) {
                ++key_scans;
                break;
            }
        }
    }
    return size;
}

TEST (EnvelopeKeyCost, CounterObservesRealLengthScans)
{
    std::size_t (*volatile scan) (const char *) = &std::strlen;
    key_scans = 0;
    counting = true;
    const auto unrelated = scan ("not-a-header-field");
    counting = false;
    EXPECT_EQ (unrelated, 18u);
    EXPECT_EQ (key_scans, 0u);
    for (const auto name : field_names) {
        counting = true;
        const auto length = scan (name.data ());
        counting = false;
        EXPECT_EQ (length, name.size ());
    }
    EXPECT_EQ (key_scans, field_names.size ());
}

TEST (EnvelopeKeyCost, HeaderClassificationDoesNotRescanFieldLengths)
{
    const auto ordinary = decode_scans (header ());
    const auto unknown = decode_scans (header (R"(,"unrecognized":0)"));
    // An unknown top-level key visits every field comparison. This differential
    // isolates classification from scans needed by the same valid header.
    EXPECT_EQ (unknown, ordinary);
    EXPECT_EQ (ordinary, 0u);
}

TEST (EnvelopeKeyCost, ExistingFieldAndUnknownKeySemanticsArePreserved)
{
    const auto result = decode (header ());
    ASSERT_TRUE (result);
    EXPECT_EQ (result.value ().channel_name, "c");
    EXPECT_EQ (result.value ().message_name, "m");
    EXPECT_EQ (result.value ().content_type, "x/y");
    EXPECT_EQ (result.value ().correlation_id, "corr");
    EXPECT_EQ (result.value ().deadline, "d");
    EXPECT_EQ (result.value ().topic, "t");
    EXPECT_EQ (result.value ().source, "s");
    EXPECT_EQ (result.value ().metadata.at ("a"), "b");
    for (const auto &extra : {std::string (R"(,"unknown":{"kind":false,"channelName":[]})"),
                              ",\"" + std::string (4096, 'x') + "\":0",
                              std::string (R"(,"kind\u0000suffix":false,"channelName\u0000":[])"),
                              std::string (R"(,"flowId":{},"flowOrigin":[])"),
                              std::string (R"(,"kind":false,"kind":1)"),
                              std::string (R"(,"channelName":false,"channelName":"c")")}) {
        const auto decoded = decode (header (extra));
        ASSERT_TRUE (decoded) << extra;
        EXPECT_EQ (decoded.value ().kind, msg::message_kind_t::request);
        EXPECT_EQ (decoded.value ().channel_name, "c");
        EXPECT_EQ (decoded.value ().message_name, "m");
    }
    EXPECT_FALSE (decode (header (R"(,"kind":false)")));
    EXPECT_FALSE (decode (header (R"(,"channelName":false)")));
    EXPECT_FALSE (
      decode (R"({"formatMarker":242,"kind\u0000":1,"channelName":"c","messageName":"m"})"));
}
