/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector/contracts/connector.hpp>
#include <zlink/framework/codecs/json_stream_connector.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

static_assert (ZLINK_HAS_EXCEPTIONS == 0);

int main ()
{
    using namespace zlink::stream_connector;
    const auto decode = [] (std::string_view text) {
        return detail::decode_typed_message<nlohmann::json> (codec_t::json,
                                                             {text.begin (), text.end ()});
    };

    const auto valid = decode (R"({"value":1})");
    if (!valid || valid.value ().at ("value") != 1)
        return 1;

    for (const auto invalid : {std::string_view ("{"), std::string_view (R"({"x":1,"x":2})"),
                               std::string_view ("\xef\xbb\xbf{}")}) {
        const auto result = decode (invalid);
        if (result || result.error_code () != error_code_t::frame_decode_failed)
            return 2;
    }
    return 0;
}
