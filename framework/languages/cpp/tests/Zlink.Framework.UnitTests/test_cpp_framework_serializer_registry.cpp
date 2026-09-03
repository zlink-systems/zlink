/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/codecs/serializer_test_access.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef ZLINK_CODEC_SELECTION_CONFORMANCE_PATH
#error "codec selection conformance fixture path is required"
#endif

#ifndef ZLINK_PAYLOAD_OWNERSHIP_CONFORMANCE_PATH
#error "payload ownership conformance fixture path is required"
#endif

namespace serializer_registry_test
{

struct cache_overflow_registration_key_t
{
};

struct cache_overflow_payload_t
{
};

} // namespace serializer_registry_test

namespace zlink::framework::detail
{

template <>
struct extension_serializer_traits_t<
  serializer_registry_test::cache_overflow_payload_t,
  void>
{
    static constexpr bool available = true;
    using registration_key_type =
      serializer_registry_test::cache_overflow_registration_key_t;
    using payload_type = serializer_registry_test::cache_overflow_payload_t;

    static serializer_t<payload_type> make_serializer ()
    {
        return serializer_t<payload_type> (
          [] (const payload_type &) {
              return encoded_payload_t::from_string ("cache-payload");
          },
          [] (const encoded_payload_t &) { return payload_type{}; },
          "application/x-cache-overflow");
    }
};

} // namespace zlink::framework::detail

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

struct decode_source_t
{
    int value{};
};

struct decode_once_t
{
    int value{};
};

struct decode_other_t
{
    int value{};
};

struct decode_failure_t
{
    int value{};
};

struct nested_inner_t
{
    int value{};
};

struct nested_outer_t
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
        registrar.template add_serializer<missing_t> (
          [] (const missing_t &payload) {
              return zlink::framework::encoded_payload_t::from_string (
                "avro-missing:" + std::to_string (payload.value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              const std::string text = payload.to_string ();
              return missing_t{std::stoi (
                text.substr (std::string ("avro-missing:").size ()))};
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

const nlohmann::json &codec_selection_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_CODEC_SELECTION_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "codec selection conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    } ();
    return fixture;
}

const nlohmann::json &payload_ownership_fixture ()
{
    static const auto fixture = [] {
        std::ifstream input (ZLINK_PAYLOAD_OWNERSHIP_CONFORMANCE_PATH);
        if (!input)
            throw std::runtime_error (
              "payload ownership conformance fixture could not be opened");
        return nlohmann::json::parse (input);
    } ();
    return fixture;
}

} // namespace

int main ()
{
    const auto &fixture = codec_selection_fixture ();
    if (fixture.at ("fixture") != "zlink.framework.codec-selection"
        || fixture.at ("version") != 1)
        return 20;
    if (fixture.at ("limits").at ("sendTypeCacheCapacity")
          != zlink::framework::detail::serializer_send_type_cache_capacity)
        return 25;

    const auto &ownership = payload_ownership_fixture ();
    if (ownership.at ("fixture") != "zlink.framework.payload-ownership"
        || ownership.at ("version") != 1
        || ownership.at ("copyBudget").at ("frameworkCopiesAfterOwnership") != 0
        || ownership.at ("copyBudget").at ("readonlyAccessorCopies") != 0
        || ownership.at ("copyBudget")
             .at ("maximumDeserializationsAfterAdmission") != 1
        || ownership.at ("accessorScenario").at ("reads") != 3)
        return 27;

    const auto raw_payload = zlink::message_t::from (std::string ("borrowed"));
    const auto borrowed_payload =
      zlink::framework::detail::encoded_payload_from_raw (raw_payload);
    const auto first_view = borrowed_payload.bytes ();
    const auto second_view = borrowed_payload.bytes ();
    const auto third_view = borrowed_payload.bytes ();
    if (first_view.data () != raw_payload.bytes ().data ()
        || second_view.data () != first_view.data ()
        || third_view.data () != first_view.data ()
        || borrowed_payload.to_string () != "borrowed")
        return 28;

    // Copying a borrowed bridge value preserves the public value semantics and
    // is the only point at which this test intentionally requests a copy.
    const auto copied_payload = borrowed_payload;
    if (copied_payload.bytes ().data () == raw_payload.bytes ().data ()
        || copied_payload.to_string () != "borrowed")
        return 29;

    for (const auto &scenario : fixture.at ("normalizationScenarios")) {
        zlink::framework::serializer_registry_t registry;
        bool rejected = false;
        try {
            registry.add<payload_t> (
              [] (const payload_t &payload) {
                  return zlink::framework::encoded_payload_t::from_string (
                    std::to_string (payload.value));
              },
              [] (const zlink::framework::encoded_payload_t &payload) {
                  return payload_t{std::stoi (payload.to_string ())};
              },
              scenario.at ("input").get<std::string> ());
        }
        catch (const zlink::framework::framework_exception_t &error) {
            rejected = error.kind ()
                       == zlink::framework::framework_error_kind_t::protocol_error;
        }
        if (scenario.contains ("expectedError")) {
            if (!rejected)
                return 21;
        }
        else if (rejected
                 || registry.content_type (
                      std::type_index (typeid (payload_t)))
                      != scenario.at ("expected").get<std::string> ()) {
            return 22;
        }
    }

    const auto &duplicate = fixture.at ("normalizedDuplicateScenario");
    zlink::framework::serializer_registry_t duplicate_registry;
    duplicate_registry.add<payload_t> (
      [] (const payload_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      duplicate.at ("registrationInputs").at (0).get<std::string> ());
    duplicate_registry.add<missing_t> (
      [] (const missing_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return missing_t{std::stoi (payload.to_string ())};
      },
      duplicate.at ("registrationInputs").at (1).get<std::string> ());
    if (duplicate.at ("finalEntryCount") != 1
        || duplicate.at ("selectedRegistrationIndex") != 1
        || duplicate_registry.contains (
          std::type_index (typeid (payload_t)))
        || !duplicate_registry.contains (
          std::type_index (typeid (missing_t)))
        || duplicate_registry.content_type (
             std::type_index (typeid (missing_t)))
             != "application/x-base") {
        return 23;
    }

    zlink::framework::detail::serializer_registry_access_t::freeze (
      duplicate_registry);
    bool frozen_registry_rejected = false;
    try {
        duplicate_registry.add<payload_t> (
          [] (const payload_t &payload) {
              return zlink::framework::encoded_payload_t::from_string (
                std::to_string (payload.value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return payload_t{std::stoi (payload.to_string ())};
          },
          "application/x-after-startup");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        frozen_registry_rejected = error.kind ()
                                   == zlink::framework::framework_error_kind_t::invalid_operation;
    }
    if (!frozen_registry_rejected)
        return 26;

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
          != "application/octet-stream"
        || serializers.get<payload_t> ().content_type ()
             != "application/octet-stream") {
        return 10;
    }

    const auto decoded = serializers.get<payload_t> ().deserialize (encoded);
    if (decoded.value != 42) {
        return 2;
    }
    const auto json_encoded = serializers.get<json_payload_t> ().serialize ({77});
    const auto json_decoded = serializers.get<json_payload_t> ().deserialize (json_encoded);
    if (json_encoded.to_string () != R"({"value":77})"
        || json_decoded.value != 77) {
        return 3;
    }
    if (serializers.content_type (std::type_index (typeid (json_payload_t)))
          != "application/json"
        || serializers.get<json_payload_t> ().content_type ()
             != "application/json") {
        return 11;
    }

    if (encoded.to_string () != "42") {
        return 4;
    }

    serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "application/x-replaced");
    if (serializers.content_type (std::type_index (typeid (payload_t)))
          != "application/x-replaced") {
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

    for (const auto &invalid_json : {
           std::string ("\xef\xbb\xbf{\"value\":1}"),
           std::string (R"({"value":1,"value":2})"),
           std::string (R"({"nested":{"value":1,"value":2},"value":3})")}) {
        bool profile_rejected = false;
        try {
            (void) serializers.get<json_payload_t> ().deserialize (
              zlink::framework::encoded_payload_t::from_string (invalid_json));
        }
        catch (const zlink::framework::framework_exception_t &error) {
            profile_rejected =
              error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
        }
        if (!profile_rejected) {
            return 14;
        }
    }

    bool non_finite_rejected = false;
    try {
        (void) serializers.get<double> ().serialize (
          std::numeric_limits<double>::infinity ());
    }
    catch (const zlink::framework::framework_exception_t &error) {
        non_finite_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!non_finite_rejected) {
        return 15;
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
    if (config_serializers.get<payload_t> ().content_type ()
        != "application/avro") {
        return 16;
    }
    const auto grouped_encoded =
      config_serializers.get<missing_t> ().serialize ({10});
    const auto grouped_decoded =
      config_serializers.get<missing_t> ().deserialize (grouped_encoded);
    if (custom_encoded.to_string () != "avro:9"
        || grouped_encoded.to_string () != "avro-missing:10"
        || grouped_decoded.value != 10
        || config_serializers.get<missing_t> ().content_type ()
             != "application/avro") {
        return 30;
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

    zlink::framework::serializer_registry_t receive_registry;
    receive_registry.add<payload_t> (
      [] (const payload_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "application/x-base");
    for (const auto &scenario : fixture.at ("receiveScenarios")) {
        bool protocol_error = false;
        std::optional<payload_t> received;
        try {
            received = zlink::framework::detail::
              deserialize_typed_payload<payload_t> (
                receive_registry,
                zlink::message_t::from (std::string ("17")),
                scenario.at ("wireContentType").get<std::string> ());
        }
        catch (const zlink::framework::framework_exception_t &error) {
            protocol_error = error.kind ()
                             == zlink::framework::framework_error_kind_t::protocol_error;
        }
        const auto success = scenario.at ("expectedTerminal") == "success";
        if ((success && (protocol_error || !received || received->value != 17))
            || (!success && !protocol_error))
            return 24;
    }

    // A resolved serializer owns the erased functions it invokes. Moving the
    // registry must not leave the cached serializer pointing at the old object.
    zlink::framework::serializer_registry_t movable_serializers;
    movable_serializers.add<payload_t> (
      [] (const payload_t &payload) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (payload.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      });
    const auto retained_serializer = movable_serializers.get<payload_t> ();
    auto moved_serializers = std::move (movable_serializers);
    const auto retained_payload = retained_serializer.serialize ({31});
    if (retained_payload.to_string () != "31"
        || retained_serializer.deserialize (retained_payload).value != 31
        || moved_serializers.get<payload_t> ().deserialize (retained_payload).value
             != 31) {
        return 14;
    }

    zlink::framework::serializer_registry_t decode_serializers;
    int successful_deserializations = 0;
    int other_deserializations = 0;
    int failed_deserializations = 0;
    decode_serializers.add<decode_source_t> (
      [] (const decode_source_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return decode_source_t{std::stoi (payload.to_string ())};
      },
      "application/x-decode-source");
    decode_serializers.add<decode_once_t> (
      [] (const decode_once_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (value.value));
      },
      [&] (const zlink::framework::encoded_payload_t &payload) {
          ++successful_deserializations;
          return decode_once_t{std::stoi (payload.to_string ())};
      },
      "application/x-decode-once");
    decode_serializers.add<decode_other_t> (
      [] (const decode_other_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (value.value));
      },
      [&] (const zlink::framework::encoded_payload_t &payload) {
          ++other_deserializations;
          return decode_other_t{std::stoi (payload.to_string ())};
      },
      "application/x-decode-other");
    decode_serializers.add<decode_failure_t> (
      [] (const decode_failure_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (value.value));
      },
      [&] (const zlink::framework::encoded_payload_t &) -> decode_failure_t {
          ++failed_deserializations;
          throw std::runtime_error ("expected decode failure");
      },
      "application/x-decode-failure");

    const auto decoded_message =
      zlink::framework::message_t::from (decode_source_t{73});
    const auto first_decoded =
      decoded_message.decode<decode_once_t> (decode_serializers);
    const auto copied_message = decoded_message;
    const auto second_decoded =
      copied_message.decode<decode_once_t> (decode_serializers);
    bool different_type_rejected = false;
    try {
        (void) decoded_message.decode<decode_other_t> (
          decode_serializers);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        different_type_rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (first_decoded.value != 73 || second_decoded.value != 73
        || successful_deserializations != 1
        || other_deserializations != 0
        || !different_type_rejected) {
        return 30;
    }

    const auto failed_message =
      zlink::framework::message_t::from (decode_source_t{91});
    std::string first_failure;
    std::string repeated_failure;
    try {
        (void) failed_message.decode<decode_failure_t> (
          decode_serializers);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        first_failure = error.what ();
    }
    try {
        (void) failed_message.decode<decode_other_t> (
          decode_serializers);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        repeated_failure = error.what ();
    }
    if (failed_deserializations != 1 || other_deserializations != 0
        || first_failure.empty ()
        || repeated_failure != first_failure) {
        return 31;
    }

    // The selected serializer owns both outputs. A nested serializer selection
    // must not replace the outer serializer's media type.
    zlink::framework::serializer_registry_t nested_serializers;
    nested_serializers.add<nested_inner_t> (
      [] (const nested_inner_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            "inner:" + std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &) { return nested_inner_t{}; },
      "application/x-nested-inner");
    nested_serializers.add<nested_outer_t> (
      [&nested_serializers] (const nested_outer_t &value) {
          auto nested = nested_serializers.get<nested_inner_t> ()
                          .serialize_with_content_type ({value.value});
          if (nested.content_type != "application/x-nested-inner") {
              throw std::runtime_error ("nested serializer selected the wrong content type");
          }
          return std::move (nested.payload);
      },
      [] (const zlink::framework::encoded_payload_t &) { return nested_outer_t{}; },
      "application/x-nested-outer");
    const auto nested_encoded = nested_serializers.get<nested_outer_t> ()
                                  .serialize_with_content_type ({37});
    if (nested_encoded.content_type != "application/x-nested-outer"
        || nested_encoded.payload.to_string () != "inner:37") {
        return 32;
    }

    // Exercise the same uncached return path used after the default 1,024-entry
    // capacity is full without instantiating 1,025 serializer template types.
    static_assert (zlink::framework::detail::serializer_send_type_cache_capacity
                   == 1024);
    zlink::framework::serializer_registry_t capacity_serializers;
    capacity_serializers.add<
      serializer_registry_test::cache_overflow_registration_key_t> (
      [] (const serializer_registry_test::cache_overflow_registration_key_t &) {
          return zlink::framework::encoded_payload_t{};
      },
      [] (const zlink::framework::encoded_payload_t &) {
          return serializer_registry_test::cache_overflow_registration_key_t{};
      },
      "application/x-cache-extension");
    capacity_serializers.add<payload_t> (
      [] (const payload_t &value) {
          return zlink::framework::encoded_payload_t::from_string (
            std::to_string (value.value));
      },
      [] (const zlink::framework::encoded_payload_t &payload) {
          return payload_t{std::stoi (payload.to_string ())};
      },
      "application/x-cache-filler");
    zlink::framework::detail::serializer_registry_test_access_t::
      set_resolved_serializer_cache_capacity (
        capacity_serializers, 1);
    (void) capacity_serializers.get<payload_t> ();
    const auto overflow_encoded = capacity_serializers
                                    .get<serializer_registry_test::cache_overflow_payload_t> ()
                                    .serialize_with_content_type ({});
    if (overflow_encoded.content_type != "application/x-cache-overflow"
        || overflow_encoded.payload.to_string () != "cache-payload") {
        return 33;
    }

    return 0;
}
