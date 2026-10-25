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
    payload value;
    value.status = static_cast<state> (9);
    assert (!detail::to_packet_payload (value, 0));
}