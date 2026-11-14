/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <cassert>
#include <limits>
enum class state
{
    Ready
};
struct payload
{
    std::int64_t number = 7;
    state status = state::Ready;
};
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "Ready")};
};
template <> struct zlink::json::dto_traits<payload>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("number", &payload::number),
                                              zlink::json::field ("status", &payload::status)};
};
int main ()
{
    using namespace zlink::stream_connector;
    const std::array<std::string_view, 4> byte_wire{"\"\"", "\"AA==\"", "\"AAE=\"", "\"AAEC\""};
    for (std::size_t size = 0; size < byte_wire.size (); ++size) {
        std::vector<std::byte> bytes;
        for (std::size_t i = 0; i < size; ++i)
            bytes.push_back (std::byte (i));
        const auto wire = detail::to_packet_payload (bytes, 0);
        assert (wire);
        assert (std::string (wire.value ().begin (), wire.value ().end ()) == byte_wire[size]);
        const auto back =
          detail::decode_typed_message<std::vector<std::byte>> (codec_t::json, wire.value ());
        assert (back && back.value () == bytes);
    }
    const std::string invalid_bytes = "\"AB==\"";
    assert (!detail::decode_typed_message<std::vector<std::byte>> (
      codec_t::json, {invalid_bytes.begin (), invalid_bytes.end ()}));
    auto encoded = detail::to_packet_payload (payload{}, 0);
    assert (encoded);
    auto decoded = detail::decode_typed_message<payload> (codec_t::json, encoded.value ());
    assert (decoded && decoded.value ().number == 7);
    std::string invalid = "{\"number\":7,\"status\":1}";
    assert (
      !detail::decode_typed_message<payload> (codec_t::json, {invalid.begin (), invalid.end ()}));
    assert (!detail::to_packet_payload (std::string ("\xff", 1), 0));
    assert (!detail::to_packet_payload (std::numeric_limits<double>::infinity (), 0));
    auto raw = detail::decode_typed_message<payload> (codec_t::raw, {'o', 'k'});
    assert (raw && raw.value ().number == 7);
    const auto packed = nlohmann::json::to_msgpack (
      nlohmann::json{{"number", "-9223372036854775808"}, {"status", "Ready"}});
    const auto unpacked = detail::decode_typed_message<payload> (codec_t::message_pack, packed);
    assert (unpacked && unpacked.value ().number == std::numeric_limits<std::int64_t>::min ()
            && unpacked.value ().status == state::Ready);
    for (const auto &invalid : {nlohmann::json{{"number", 7}, {"status", "Ready"}},
                                nlohmann::json{{"number", "01"}, {"status", "Ready"}},
                                nlohmann::json{{"number", "7"}, {"status", 1}},
                                nlohmann::json{{"number", "7"}, {"status", "1"}},
                                nlohmann::json{{"number", "7"}, {"status", "Unknown"}}})
        assert (!detail::decode_typed_message<payload> (codec_t::message_pack,
                                                        nlohmann::json::to_msgpack (invalid)));
    assert (!detail::decode_typed_message<payload> (codec_t::message_pack, {0xc1}));
    payload value;
    value.status = static_cast<state> (9);
    assert (!detail::to_packet_payload (value, 0));
}