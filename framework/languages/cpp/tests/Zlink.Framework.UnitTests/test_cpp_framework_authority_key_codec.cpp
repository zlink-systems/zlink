/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/locations/authority_key_codec.hpp"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace runtime = zlink::framework::runtime;

namespace
{
std::string from_hex (const std::string &value)
{
    const auto digit = [] (char ch) -> unsigned char {
        if (ch >= '0' && ch <= '9')
            return static_cast<unsigned char> (ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<unsigned char> (ch - 'a' + 10);
        assert (false);
        return 0;
    };
    assert (value.size () % 2 == 0);
    std::string result;
    for (std::size_t index = 0; index < value.size (); index += 2)
        result.push_back (static_cast<char> (
          (digit (value[index]) << 4) | digit (value[index + 1])));
    return result;
}
}

int main ()
{
    std::ifstream fixture (ZLINK_AUTHORITY_KEY_GOLDEN_PATH);
    assert (fixture.good ());
    const auto vectors = nlohmann::json::parse (fixture);
    for (const auto &entry : vectors.at ("cases")) {
        const auto identity = from_hex (
          entry.at ("identityHex").get<std::string> ());
        const auto kind = entry.at ("objectKind") == "actor" ? 'a' : 's';
        const auto encoded = runtime::authority_key_codec_detail::encode_authority_key (
          kind, identity);
        assert (encoded.value == entry.at ("encoded").get<std::string> ());
        const auto decoded =
          runtime::authority_key_codec_detail::decode_authority_key (
            encoded.value);
        assert (decoded && decoded->kind == kind
                && decoded->object_id == identity);
    }

    for (const auto invalid : {
           "zla1:a:01:A",
           "zla1:a:1:%41",
           "zla1:a:1:%2f",
           "zla1:a:1:/",
           "zla1:a:0:",
           "zla1:a:256:A",
           "zla1:a:2:%C0%AF",
           "zla1:a:1:%FF"}) {
        assert (!runtime::authority_key_codec_detail::decode_authority_key (
          invalid));
    }
    assert (!runtime::authority_key_codec_detail::decode_authority_key (
      "zla1:a:255:" + std::string (256, 'A')));

    bool rejected = false;
    try {
        static_cast<void> (
          runtime::authority_key_codec_detail::encode_authority_key ('a', ""));
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);
    return 0;
}
