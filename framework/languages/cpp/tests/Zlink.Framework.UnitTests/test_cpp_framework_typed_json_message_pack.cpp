/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/codecs/json_stream_connector.hpp>
#include <cassert>
#include <limits>

enum class state
{
    Ready,
    Closed
};
struct dto
{
    std::int64_t signed64 = std::numeric_limits<std::int64_t>::min ();
    std::uint64_t unsigned64 = std::numeric_limits<std::uint64_t>::max ();
    state status = state::Closed;
};
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "Ready"),
                                              zlink::json::enum_value (state::Closed, "Closed")};
};
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("signed64", &dto::signed64),
                                              zlink::json::field ("unsigned64", &dto::unsigned64),
                                              zlink::json::field ("status", &dto::status)};
};
int main ()
{
    namespace p = zlink::detail::json_profile;
    const auto wire = nlohmann::json{{"signed64", "-9223372036854775808"},
                                     {"unsigned64", "18446744073709551615"},
                                     {"status", "Closed"}};
    const auto packed = nlohmann::json::to_msgpack (wire);
    const auto unpacked =
      zlink::stream_connector::codecs::codec_traits<dto>::decode_message_pack (packed);
    assert (p::encode_value (unpacked) == wire);
    dto stream_value{};
    assert (zlink::stream_connector::from_stream_payload (
      zlink::stream_connector::codec_t::message_pack, packed, stream_value));
    assert (p::encode_value (stream_value) == wire);
    const auto received = zlink::stream_connector::detail::decode_typed_message<dto> (
      zlink::stream_connector::codec_t::message_pack, packed);
    assert (received && p::encode_value (received.value ()) == wire);
    for (const auto &bad :
         {nlohmann::json (7), nlohmann::json ("01"), nlohmann::json ("9223372036854775808")}) {
        auto invalid = wire;
        invalid["signed64"] = bad;
        assert (!zlink::stream_connector::detail::decode_typed_message<dto> (
          zlink::stream_connector::codec_t::message_pack, nlohmann::json::to_msgpack (invalid)));
    }
    for (const auto &bad : {nlohmann::json (1), nlohmann::json ("1"), nlohmann::json ("Other")}) {
        auto invalid = wire;
        invalid["status"] = bad;
        assert (!zlink::stream_connector::detail::decode_typed_message<dto> (
          zlink::stream_connector::codec_t::message_pack, nlohmann::json::to_msgpack (invalid)));
    }
    assert (!zlink::stream_connector::detail::decode_typed_message<dto> (
      zlink::stream_connector::codec_t::message_pack, {0xc1}));

    dto value{};
    zlink::stream_connector::codecs::decode_payload (zlink::stream_connector::codec_t::message_pack,
                                                     packed, value);
    assert (p::encode_value (value) == wire);
}
