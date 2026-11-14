/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../ladder/ladder_wire.hpp"

using namespace zlink_cpp_bench;
namespace msg = ladder::messaging;
namespace fw = ladder::fw;

namespace
{
void same (std::span<const std::byte> before, std::span<const std::byte> after)
{
    if (!std::ranges::equal (before, after))
        throw std::runtime_error ("ladder wire bytes changed");
}

// Compatibility oracle: the pre-#4352 owning caller, retained only in this test.
msg::message_parts_t before_parts (const msg::envelope_header_t &actual,
                                   std::span<const std::byte> body, int level, bool command)
{
    msg::client_call_codec_t codec;
    auto header = codec.create_envelope (
      command ? msg::message_kind_t::command : msg::message_kind_t::request,
      "bench", "BenchPayload", command ? std::chrono::milliseconds (0) : std::chrono::seconds (2),
      {}, {}, "application/x-protobuf");
    // Correlation and UTC deadline vary per invocation. Give both paths the
    // same per-message values; all bytes, including these fields, are compared.
    header.correlation_id = actual.correlation_id;
    header.deadline = actual.deadline;
    auto payload = fw::detail::encoded_payload_from_raw (body);
    auto type = std::make_shared<fw::detail::request_type_plan_t> (
      fw::detail::prepare_request_type ("BenchPayload", "application/x-protobuf"));
    auto channel = std::make_shared<fw::detail::prepared_header_fragment_t> (
      fw::detail::prepare_request_channel ("bench"));
    if (command)
        return codec.encode_payload_parts (header, std::move (payload), type->content_type);
    if (level == 3)
        return codec.encode_service_payload_parts (std::move (header), std::move (payload), type, channel);
    return codec.encode_payload_parts (std::move (header), std::move (payload), type, channel);
}

void wire_identity ()
{
    for (int level : {2, 3}) {
        ladder::encoder_t encoder (level);
        for (size_t size : {1024, 4096}) {
            for (bool command : {true, false}) {
                auto after = encoder.encode (size, 123, phase_active, 456, command,
                                              std::chrono::seconds (2));
                auto views = ladder::application_views (after);
                const auto actual = msg::envelope_codec_t{}.decode_header_view (
                  msg::message_parts_t::borrow (views), false).value ();
                // Reuse the same stamped body: its send timestamp also varies.
                auto before = before_parts (actual, views[1], level, command);
                if (level == 3) {
                    auto application = std::move (before).take_application_payload ();
                    auto header = command ? ladder::protocol::encode_node_send_header_message ()
                                          : ladder::protocol::encode_node_request_header_message (457);
                    auto body = ladder::protocol::encode_application_payload_message (application);
                    same (header.bytes (), after[0].bytes ());
                    same (body.bytes (), after[1].bytes ());
                } else {
                    const auto items = std::move (before).take_items ();
                    same (items[0].bytes (), after[0].bytes ());
                    same (items[1].bytes (), after[1].bytes ());
                }
                // copy() shares native storage; mutation needs an independent fixture.
                auto changed = zlink::message_t::from (after[0].bytes ());
                changed.data ()[0] ^= std::byte{1};
                bool rejected = false;
                try { same (after[0].bytes (), changed.bytes ()); }
                catch (const std::runtime_error &) { rejected = true; }
                if (!rejected)
                    throw std::runtime_error ("wire identity check accepted a changed frame");
                if (!command) {
                    ladder::payload_t payload;
                    if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ())))
                        throw std::runtime_error ("invalid identity test payload");
                    auto reply = ladder::reply_parts (payload, actual);
                    fw::detail::channel_reply_writer_t writer;
                    auto expected = writer.reply_raw_envelope (
                      writer.create_reply_header (msg::message_kind_t::response, "bench", actual),
                      zlink_cpp_bench::encode_bench_payload_message (payload));
                    const auto items = std::move (expected).take_items ();
                    same (items[0].bytes (), reply[0].bytes ());
                    same (items[1].bytes (), reply[1].bytes ());
                }
                std::printf ("L%d %zu %s: all frame bytes identical\n", level, size,
                             command ? "command" : "request/reply");
            }
        }
    }
    std::puts ("negative identity checks: changed frame byte rejected in every case");
}
}

int main ()
{
    try { wire_identity (); }
    catch (const std::exception &error) {
        std::fprintf (stderr, "%s\n", error.what ());
        return 1;
    }
}
