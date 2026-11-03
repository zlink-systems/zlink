/* SPDX-License-Identifier: Apache-2.0 */
#include <zlink/json_profile.hpp>
#include "allocation_counter.hpp"
#include <cassert>
#include <iostream>

enum class status_t
{
    ready,
    closed
};
struct allocation_dto_t
{
    std::string name = std::string (64, 'a');
    std::string description = std::string (96, 'b');
    int count = 42;
    short rank = -12;
    unsigned flags = 7;
    std::int64_t id = 9223372036854775807LL;
    status_t status = status_t::closed;
    std::optional<int> optional = 8;
};
template <> struct zlink::json::enum_traits<status_t>
{
    static constexpr auto values = std::array{zlink::json::enum_value (status_t::ready, "Ready"),
                                              zlink::json::enum_value (status_t::closed, "Closed")};
};
template <> struct zlink::json::dto_traits<allocation_dto_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("status", &allocation_dto_t::status),
                 zlink::json::field ("name", &allocation_dto_t::name),
                 zlink::json::field ("description", &allocation_dto_t::description),
                 zlink::json::field ("id", &allocation_dto_t::id),
                 zlink::json::field ("flags", &allocation_dto_t::flags),
                 zlink::json::field ("count", &allocation_dto_t::count),
                 zlink::json::field ("rank", &allocation_dto_t::rank),
                 zlink::json::field ("optional", &allocation_dto_t::optional)};
};
struct nested_allocation_dto_t
{
    std::map<std::string, std::vector<std::optional<int>>> values;
    std::vector<std::byte> bytes;
};
template <> struct zlink::json::dto_traits<nested_allocation_dto_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("values", &nested_allocation_dto_t::values),
                 zlink::json::field ("bytes", &nested_allocation_dto_t::bytes)};
};

int main (int argc, char **argv)
{
    const std::string_view injection = argc > 1 ? argv[1] : "";
    const auto inject = [&] (std::string_view direction) {
        if (injection == direction) {
            auto *memory = ::operator new (1);
            ::operator delete (memory);
        }
    };
    using namespace zlink::detail::json_profile;
    using nlohmann::detail::zlink_dom_constructions;
    allocation_dto_t input;
    auto wire = encode (input);
    const auto expected = std::string ("{\"count\":42,\"description\":\"") + std::string (96, 'b')
                          + "\",\"flags\":7,\"id\":\"9223372036854775807\",\"name\":\""
                          + std::string (64, 'a')
                          + "\",\"optional\":8,\"rank\":-12,\"status\":\"Closed\"}";
    assert (wire == expected);
    const auto before_control = zlink_dom_constructions;
    auto control = zlink::framework::tests::count_allocations ([&] {
        auto dom = try_parse (wire.begin (), wire.end ());
        assert (dom);
    });
    assert (zlink_dom_constructions > before_control);
    std::string encoded;
    auto before = zlink_dom_constructions;
    auto enc = zlink::framework::tests::count_allocations ([&] {
        encoded = encode (input);
        inject ("--inject-encode-allocation");
    });
    auto enc_dom = zlink_dom_constructions - before;
    std::optional<allocation_dto_t> decoded;
    before = zlink_dom_constructions;
    auto dec = zlink::framework::tests::count_allocations ([&] {
        decoded = try_decode<allocation_dto_t> (wire.begin (), wire.end ());
        inject ("--inject-decode-allocation");
    });
    auto dec_dom = zlink_dom_constructions - before;
    assert (decoded && encoded == wire && decoded->name == input.name && decoded->id == input.id);
    std::cout << "encode_new=" << enc << " decode_new=" << dec << " encode_dom=" << enc_dom
              << " decode_dom=" << dec_dom << " positive_control_new=" << control << std::endl;
    // Measure the required writer/parser work on this STL. The public helpers
    // must add neither a buffer copy nor a DOM to those primitives.
    std::string reference_encoded;
    const auto reference_enc = zlink::framework::tests::count_allocations ([&] {
        reference_encoded.reserve (initial_output_capacity);
        assert (write_value (input, reference_encoded, nullptr));
    });
    std::optional<allocation_dto_t> reference_decoded;
    const auto parse_reference = [&] () -> std::optional<allocation_dto_t> {
        allocation_dto_t value;
        typed_sax handler (value, nullptr);
        if (!nlohmann::json::sax_parse (wire.begin (), wire.end (), &handler)
            || !handler.complete ())
            return std::nullopt;
        return value;
    };
    const auto reference_dec =
      zlink::framework::tests::count_allocations ([&] { reference_decoded = parse_reference (); });
    std::cout << "reference_encode_new=" << reference_enc
              << " reference_decode_new=" << reference_dec << std::endl;
    assert (reference_encoded == encoded && reference_decoded);
    assert (enc == reference_enc);
    assert (dec == reference_dec);
    std::vector<std::uint8_t> payload;
    const auto payload_allocations = zlink::framework::tests::count_allocations ([&] {
        byte_output out (payload);
        out.reserve (initial_output_capacity);
        assert (write_value (input, out, nullptr));
    });
    assert (std::string (payload.begin (), payload.end ()) == wire);
    std::vector<std::uint8_t> reference_payload;
    const auto reference_payload_allocations = zlink::framework::tests::count_allocations ([&] {
        reference_payload.reserve (initial_output_capacity);
        reference_payload.insert (reference_payload.end (), wire.begin (), wire.end ());
    });
    assert (payload_allocations == reference_payload_allocations);
    assert (decoded->description == input.description && decoded->count == input.count
            && decoded->rank == input.rank && decoded->flags == input.flags
            && decoded->status == input.status && decoded->optional == input.optional);
#ifdef ZLINK_JSON_EXPECT_DOM_FREE
    const nested_allocation_dto_t nested{{{"z", {1, std::nullopt, 2}}, {"a", {3}}},
                                         {std::byte{0}, std::byte{255}}};
    before = zlink_dom_constructions;
    const auto nested_wire = encode (nested);
    assert (nested_wire == "{\"bytes\":\"AP8=\",\"values\":{\"a\":[3],\"z\":[1,null,2]}}");
    const auto nested_back =
      try_decode<nested_allocation_dto_t> (nested_wire.begin (), nested_wire.end ());
    assert (nested_back && nested_back->values == nested.values
            && nested_back->bytes == nested.bytes);
    const std::string invalid = "{\"bytes\":\"AP8=\",\"values\":{},\"unknown\":{\"x\":1,\"x\":2}}";
    assert (!try_decode<nested_allocation_dto_t> (invalid.begin (), invalid.end ()));
    assert (zlink_dom_constructions == before);
#endif
#ifdef ZLINK_JSON_EXPECT_DOM_FREE
    assert (enc_dom == 0 && dec_dom == 0);
#endif
}
