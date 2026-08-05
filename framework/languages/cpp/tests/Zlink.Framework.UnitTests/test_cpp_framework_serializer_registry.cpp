/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/messaging/envelope_codec.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace
{

struct payload_t
{
    int value{};
};

struct missing_t
{
    int value{};
};

struct json_payload_t
{
    int value{};
};

struct custom_payload_codec_extension_t
{
    template <typename TRegistrar> void register_framework_codecs (TRegistrar &registrar) const
    {
        registrar.template add_serializer<payload_t> (
          [] (const payload_t &payload) {
              return zlink::framework::encoded_payload_t::from_string (
                "avro:" + std::to_string (payload.value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              const std::string text = payload.to_string ();
              return payload_t{std::stoi (text.substr (std::string ("avro:").size ()))};
          },
          "application/avro");
    }
};

void to_json (nlohmann::json &json, const json_payload_t &payload)
{
    json = nlohmann::json{{"value", payload.value}};
}

void from_json (const nlohmann::json &json, json_payload_t &payload)
{
    payload.value = json.at ("value").get<int> ();
}

} // namespace

int main ()
{
    zlink::framework::serializer_registry_t serializers;
    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      });

    const auto encoded = serializers.get<payload_t> ().serialize ({42});
    if (encoded.to_string () != "42") {
        return 1;
    }
    if (serializers.content_type (std::type_index (typeid (payload_t)))
        != "application/octet-stream") {
        return 10;
    }

    const auto decoded = serializers.get<payload_t> ().deserialize (encoded);
    if (decoded.value != 42) {
        return 2;
    }
    const auto json_encoded = serializers.get<json_payload_t> ().serialize ({77});
    const auto json_decoded = serializers.get<json_payload_t> ().deserialize (json_encoded);
    if (json_decoded.value != 77) {
        return 3;
    }
    if (serializers.content_type (std::type_index (typeid (json_payload_t)))
        != "application/json") {
        return 11;
    }

    zlink::framework::payload_view_t view (encoded);
    if (view.to_string () != "42") {
        return 4;
    }

    bool duplicate_failed = false;
    try {
        serializers.add<payload_t> ([] (const payload_t &) { return zlink::framework::encoded_payload_t{}; },
                                    [] (const zlink::framework::encoded_payload_t &) { return payload_t{}; });
    }
    catch (const zlink::framework::framework_exception_t &error) {
        duplicate_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!duplicate_failed) {
        return 5;
    }

    bool decode_failed = false;
    try {
        (void) serializers.get<payload_t> ().deserialize (
          zlink::framework::encoded_payload_t::from_string ("not-an-int"));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        decode_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!decode_failed) {
        return 7;
    }

    bool json_decode_failed = false;
    try {
        (void) serializers.get<json_payload_t> ().deserialize (
          zlink::framework::encoded_payload_t::from_string ("not-json"));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        json_decode_failed =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!json_decode_failed) {
        return 13;
    }

    // Application codec configuration uses extensions. The extension registrar
    // installs custom serializers into the registry at startup.
    zlink::framework::serializer_registry_t config_serializers;
    zlink::framework::codec_options_builder_t codecs (config_serializers);
    codecs.use (custom_payload_codec_extension_t{});

    const auto custom_encoded = config_serializers.get<payload_t> ().serialize ({9});
    if (custom_encoded.to_string () != "avro:9") {
        return 8;
    }
    if (config_serializers.get<payload_t> ().deserialize (custom_encoded).value != 9) {
        return 9;
    }
    zlink::framework::runtime::messaging::envelope_codec_t envelope;
    zlink::framework::runtime::messaging::envelope_header_t header;
    header.kind = zlink::framework::runtime::messaging::message_kind_t::request;
    header.channel_name = "codec";
    header.message_name = "custom";
    payload_t envelope_payload{12};
    const auto custom_parts =
      envelope.encode_parts (header, std::type_index (typeid (payload_t)), &envelope_payload,
                             config_serializers);
    const auto custom_header = envelope.decode_header (custom_parts);
    if (!custom_header || custom_header.value ().content_type != "application/avro") {
        return 12;
    }

    return 0;
}
