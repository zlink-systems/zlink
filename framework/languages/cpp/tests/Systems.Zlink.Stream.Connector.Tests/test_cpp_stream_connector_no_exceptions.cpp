/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector/contracts/connector.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

static_assert (ZLINK_HAS_EXCEPTIONS == 0);

struct failed_protobuf_t
{
    static constexpr const char *packet_name = "failed.protobuf";
    bool succeeds = false;
    bool SerializeToString (std::string *bytes) const
    {
        *bytes = "valid";
        return succeeds;
    }
    bool ParseFromString (const std::string &bytes) { return bytes == "valid"; }
};

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
    const auto protobuf = detail::decode_typed_message<failed_protobuf_t> (codec_t::protobuf, {});
    if (protobuf || protobuf.error_code () != error_code_t::frame_decode_failed)
        return 3;
    const auto valid_protobuf = detail::decode_typed_message<failed_protobuf_t> (
      codec_t::protobuf, {'v', 'a', 'l', 'i', 'd'});
    if (!valid_protobuf)
        return 5;
    connector_t connector;
    const auto encoded = connector.request (failed_protobuf_t{true}).submit<failed_protobuf_t> ();
    if (encoded || encoded.error_code () != error_code_t::disconnected)
        return 6;
    int errors = 0;
    auto subscription = connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
        if (error.code == error_code_t::send_failed)
            ++errors;
    });
    connector.send (failed_protobuf_t{}).submit ();
    const auto request = connector.request (failed_protobuf_t{}).submit<failed_protobuf_t> ();
    int callbacks = 0;
    connector.request (failed_protobuf_t{})
      .submit<failed_protobuf_t> ([&] (result_t<failed_protobuf_t> result) {
          if (!result && result.error_code () == error_code_t::send_failed)
              ++callbacks;
      });
    (void) connector.dispatch ();
    if (request || request.error_code () != error_code_t::send_failed || errors != 1
        || callbacks != 1)
        return 4;
    connector.close ();
    return 0;
}
