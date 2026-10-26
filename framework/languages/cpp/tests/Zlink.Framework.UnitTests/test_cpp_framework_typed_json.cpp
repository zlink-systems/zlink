/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/json.hpp>
#include <zlink/framework.hpp>
#include <zlink/framework/codecs/json_stream_connector.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <limits>
#include <bit>
#include <random>
#include <iostream>
enum class state
{
    Ready,
    Closed
};
struct dto
{
    std::int64_t signed64;
    std::uint64_t unsigned64;
    state status;
    std::optional<std::string> nullable;
    std::vector<std::byte> bytes;
    std::map<std::string, std::vector<std::int64_t>> nested;
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
                                              zlink::json::field ("status", &dto::status),
                                              zlink::json::field ("nullable", &dto::nullable),
                                              zlink::json::field ("bytes", &dto::bytes),
                                              zlink::json::field ("nested", &dto::nested)};
};
struct golden_dto
{
    std::int64_t signed64;
    std::uint64_t unsigned64;
    std::int32_t int32;
    double ratio;
    state status;
    std::vector<std::byte> bytes;
    std::optional<std::string> nullable;
    std::map<std::string, int> labels;
};
template <> struct zlink::json::dto_traits<golden_dto>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("signed64", &golden_dto::signed64),
                 zlink::json::field ("unsigned64", &golden_dto::unsigned64),
                 zlink::json::field ("int32", &golden_dto::int32),
                 zlink::json::field ("ratio", &golden_dto::ratio),
                 zlink::json::field ("state", &golden_dto::status),
                 zlink::json::field ("bytes", &golden_dto::bytes),
                 zlink::json::field ("nullable", &golden_dto::nullable),
                 zlink::json::field ("labels", &golden_dto::labels)};
};
void verify_goldens ()
{
    namespace p = zlink::detail::json_profile;
    std::ifstream input (ZLINK_TYPED_JSON_GOLDEN_PATH);
    assert (input);
    const auto fixture = nlohmann::json::parse (input);
    for (const auto &item : fixture.at ("valid")) {
        const auto text = item.at ("jsonUtf8").get<std::string> ();
        auto value = p::try_decode<golden_dto> (text.begin (), text.end ());
        assert (value);
        const auto wire = p::encode_value (*value);
        auto expected = nlohmann::json::parse (text);
        expected.erase ("unknownFuture");
        assert (wire == expected);
    }
    for (const auto &item : fixture.at ("invalid")) {
        const auto text = item.at ("jsonUtf8").get<std::string> ();
        assert (!p::try_decode<golden_dto> (text.begin (), text.end ()));
    }
}
struct tree_leaf
{
    std::int64_t number = 0;
    state status = state::Ready;
};
struct tree
{
    tree_leaf nested;
    std::vector<tree_leaf> list;
    std::map<std::string, tree_leaf> map;
};
template <> struct zlink::json::dto_traits<tree_leaf>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("number", &tree_leaf::number),
                                              zlink::json::field ("status", &tree_leaf::status)};
};
template <> struct zlink::json::dto_traits<tree>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("nested", &tree::nested),
                 zlink::json::field ("list", &tree::list), zlink::json::field ("map", &tree::map)};
};
void verify_boundaries ()
{
    namespace p = zlink::detail::json_profile;
    for (auto value :
         {INT64_MIN, INT64_MAX, std::int64_t (-1), std::int64_t (0), std::int64_t (1)}) {
        const auto wire = p::encode_value (value);
        assert (wire.is_string ());
        assert (p::decode_value<std::int64_t> (wire) == value);
    }
    for (auto value : {std::uint64_t (0), std::uint64_t (1), UINT64_MAX}) {
        const auto wire = p::encode_value (value);
        assert (wire.is_string ());
        assert (p::decode_value<std::uint64_t> (wire) == value);
    }
    for (const auto &invalid :
         {"-1", "18446744073709551616", "01", "+1", "1.0", "1e0", " 1", "", "-0"}) {
        std::uint64_t out{};
        assert (!p::from_value (nlohmann::json (invalid), out));
    }
    std::int64_t signed_value{};
    std::uint64_t unsigned_value{};
    assert (!p::from_value (nlohmann::json (7), signed_value));
    assert (!p::from_value (nlohmann::json (7), unsigned_value));
    std::int32_t small{};
    std::uint32_t unsigned_small{};
    assert (!p::from_value (nlohmann::json (2147483648LL), small));
    assert (!p::from_value (nlohmann::json (-2147483649LL), small));
    assert (!p::from_value (nlohmann::json (-1), unsigned_small));
    assert (!p::from_value (nlohmann::json (4294967296ULL), unsigned_small));
    assert (!p::from_value (nlohmann::json (1.5), small));
    assert (!p::from_value (nlohmann::json ("1"), small));
    assert (p::encode_value (INT32_MIN).is_number_integer ());
    assert (p::encode_value (UINT32_MAX).is_number_integer ());
    assert (!p::try_encode (std::numeric_limits<double>::infinity ()));
    assert (!p::try_encode (std::numeric_limits<float>::quiet_NaN ()));
    assert (!p::try_encode (std::string ("\xff", 1)));
    std::vector<std::byte> bytes;
    for (auto text : {"AQI", "AB==", "A===", "====", "/x==", "AQI=AAAA", "AA=A"})
        assert (!p::decode_bytes (text, bytes));
    assert (p::decode_bytes ("", bytes));
    assert (p::decode_bytes ("AQI=", bytes));
    assert (bytes.size () == 2);
    tree value{
      {1, state::Closed}, {{INT64_MAX, state::Ready}}, {{"leaf", {INT64_MIN, state::Closed}}}};
    auto wire = p::encode_value (value);
    assert (wire["nested"]["number"] == "1");
    auto decoded = p::decode_value<tree> (wire);
    assert (decoded.map.at ("leaf").number == INT64_MIN);
    wire["list"][0]["status"] = 0;
    tree rejected;
    assert (!p::from_value (wire, rejected));
    std::string bom = "\xef\xbb\xbf{}", duplicate = "{\"outer\":{\"x\":0,\"x\":1}}";
    assert (!p::try_parse (bom.begin (), bom.end ()));
    assert (!p::try_parse (duplicate.begin (), duplicate.end ()));
}
void verify_sax_boundaries ()
{
    namespace p = zlink::detail::json_profile;
    const tree value{};
    const auto wire = p::encode (value);
    for (const auto *suffix : {" true", " {}", " []", " trailing"}) {
        const auto text = wire + suffix;
        assert (!p::try_decode<tree> (text.begin (), text.end ()));
    }
    const auto spaced = wire + " \r\n\t";
    assert (p::try_decode<tree> (spaced.begin (), spaced.end ()));
    const auto prefix = wire.substr (0, wire.size () - 1) + ",\"ignored\":";
    const auto unknown = prefix + "{\"array\":[{\"x\":1},null,42],\"x\":2}}";
    assert (p::try_decode<tree> (unknown.begin (), unknown.end ()));
    for (const auto &bad :
         std::vector<std::string>{"{\"x\":1,\"x\":2}", "[{\"x\":1,\"x\":2}]", "1e999",
                                  R"({"x":"\ud800"})", std::string ("\"") + char (0x80) + '"'}) {
        const auto text = prefix + bad + '}';
        assert (!p::try_decode<tree> (text.begin (), text.end ()));
    }
    const auto bom = std::string ("\xef\xbb\xbf") + wire;
    assert (!p::try_decode<tree> (bom.begin (), bom.end ()));
}

void verify_float_round_trips ()
{
    namespace p = zlink::detail::json_profile;
    std::size_t checked = 0;
    const auto verify = [&] (double value) {
        assert (std::isfinite (value));
        const auto wire = p::encode (value);
        const auto decoded = p::try_decode<double> (wire.begin (), wire.end ());
        assert (decoded);
        assert (std::bit_cast<std::uint64_t> (*decoded) == std::bit_cast<std::uint64_t> (value));
        // Message model §5 fixes only a finite JSON number, not its text form.
        assert (nlohmann::json::parse (wire).is_number ());
        ++checked;
    };
    for (double value :
         {0.0, -0.0, std::numeric_limits<double>::denorm_min (),
          std::numeric_limits<double>::min (), std::numeric_limits<double>::max (), 0.0001, 0.00001,
          1e15, 1e16, 61700135196713504.0, 4.6198674636351205e304, -666672776458465.75}) {
        verify (value);
        verify (-value);
        for (double direction : {-std::numeric_limits<double>::infinity (),
                                 std::numeric_limits<double>::infinity ()}) {
            const auto adjacent = std::nextafter (value, direction);
            if (std::isfinite (adjacent))
                verify (adjacent);
        }
    }
    for (std::uint64_t exponent = 0; exponent < 2047; ++exponent)
        for (std::uint64_t mantissa : {std::uint64_t (0), std::uint64_t (1),
                                       (std::uint64_t (1) << 51), (std::uint64_t (1) << 52) - 1})
            for (std::uint64_t sign : {std::uint64_t (0), std::uint64_t (1) << 63})
                verify (std::bit_cast<double> (sign | (exponent << 52) | mantissa));
    std::mt19937_64 random (1713);
    std::size_t random_checked = 0;
    while (random_checked != 200000) {
        const double value = std::bit_cast<double> (random ());
        if (!std::isfinite (value))
            continue;
        verify (value);
        ++random_checked;
    }
    std::cout << "double_round_trips=" << checked << " random=" << random_checked << '\n';
}

void verify_float_bytes ()
{
    namespace p = zlink::detail::json_profile;
    // Integral doubles within the safe range use the integer form.
    for (const auto &[value, expected] :
         std::array{std::pair{0.0, "0"}, std::pair{120.0, "120"}, std::pair{60.0, "60"},
                    std::pair{9007199254740991.0, "9007199254740991"}})
        assert (p::encode (value) == expected);
    // Every other value is a JSON number that reads back to the same bits.
    for (const auto value :
         std::array{-0.0, 0.0001, 0.00001, 1e16, 1000000.25, 61700135196713504.0,
                    4.6198674636351205e304, -666672776458465.75, 9007199254740994.0}) {
        const auto text = p::encode (value);
        assert (nlohmann::json::parse (text).is_number ());
        const auto decoded = p::decode<double> (text.begin (), text.end ());
        assert (std::bit_cast<std::uint64_t> (decoded) == std::bit_cast<std::uint64_t> (value));
    }
}

int main ()
{
    verify_float_round_trips ();
    verify_goldens ();
    verify_boundaries ();
    namespace p = zlink::detail::json_profile;
    dto d{INT64_MIN,    UINT64_MAX,       state::Closed,
          std::nullopt, {std::byte{255}}, {{"n", {1, INT64_MAX}}}};
    auto text = p::try_encode (d);
    assert (text);
    auto wire = nlohmann::json::parse (*text);
    assert (wire["signed64"] == "-9223372036854775808"
            && wire["unsigned64"] == "18446744073709551615" && wire["status"] == "Closed"
            && wire["bytes"] == "/w==");
    auto result = p::try_decode<dto> (text->begin (), text->end ());
    assert (result && result->signed64 == d.signed64 && result->nested == d.nested);

    zlink::framework::serializer_registry_t registry;
    auto serializer = registry.get<dto> ();
    assert (nlohmann::json::parse (serializer.serialize (d).to_string ()) == wire);
    auto message = zlink::framework::codecs::json::make_message (d);
    assert (zlink::framework::codecs::json::parse_message<dto> (message).signed64 == d.signed64);
    auto connector = zlink::stream_connector::detail::to_packet_payload (d, 0);
    assert (connector);
    assert (nlohmann::json::parse (connector.value ()) == wire);
    assert (nlohmann::json::parse (zlink::stream_connector::codecs::codec_traits<dto>::encode (d))
            == wire);
    auto valid = wire;
    for (auto bad :
         {"01", "-0", "+1", "9223372036854775808", "-9223372036854775809", "1e0", " 1", ""}) {
        wire = valid;
        wire["signed64"] = bad;
        dto v{};
        assert (!p::from_value (wire, v));
    }
    for (auto bad : {nlohmann::json (1), nlohmann::json ("1"), nlohmann::json ("Other"),
                     nlohmann::json (nullptr)}) {
        wire = valid;
        wire["status"] = bad;
        dto v{};
        assert (!p::from_value (wire, v));
    }
    wire = valid;
    wire.erase ("nullable");
    dto v{};
    assert (!p::from_value (wire, v));
    wire = valid;
    wire["unknown"] = 1;
    assert (p::from_value (wire, v));
    std::string dup = "{\"x\":1,\"x\":2}";
    assert (!p::try_parse (dup.begin (), dup.end ()));
    d.status = static_cast<state> (9);
    assert (!p::try_encode (d));
    verify_sax_boundaries ();
    verify_float_bytes ();
}