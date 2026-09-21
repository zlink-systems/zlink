/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <service_wire_codec.hpp>
#include <service_wire_pilot_codec.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace codec = zlink::framework::runtime::protocol::generated::detail;
namespace pilot = zlink::framework::runtime::protocol;

namespace {

struct outcome {
    bool accepted{};
    codec::error_code error{codec::error_code::ok};
};

codec::codec_context_t maximum_codec_context()
{
    codec::codec_context_t context{};
    context.effectiveCompleteMessageBytesMinusActualEnvelopeOverhead = 4294966774ll;
    context.effectiveCompleteMessageBytes = 4294967295ll;
    return context;
}

codec::codec_context_t operation_context(const nlohmann::json& item)
{
    codec::codec_context_t context{};
    if (!item.contains("context"))
        return context;
    const auto& values = item.at("context");
    if (values.contains("effectiveCompleteMessageBytesMinusActualEnvelopeOverhead"))
        context.effectiveCompleteMessageBytesMinusActualEnvelopeOverhead =
          values.at("effectiveCompleteMessageBytesMinusActualEnvelopeOverhead").get<std::int64_t>();
    if (values.contains("effectiveCompleteMessageBytes"))
        context.effectiveCompleteMessageBytes =
          values.at("effectiveCompleteMessageBytes").get<std::int64_t>();
    return context;
}

std::vector<std::uint8_t> from_hex(std::string_view hex)
{
    const auto digit = [](char value) -> std::uint8_t {
        if (value >= '0' && value <= '9')
            return static_cast<std::uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f')
            return static_cast<std::uint8_t>(value - 'a' + 10);
        throw std::invalid_argument("hex digit");
    };
    if (hex.size() % 2 != 0)
        throw std::invalid_argument("hex length");
    std::vector<std::uint8_t> result;
    result.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2)
        result.push_back(static_cast<std::uint8_t>((digit(hex[index]) << 4) | digit(hex[index + 1])));
    return result;
}

nlohmann::json load_json(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.good())
        throw std::runtime_error("fixture open failed: " + path.string());
    std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    for (std::size_t position = 0; (position = source.find("\\ud800", position)) != std::string::npos;
         position += 7)
        source.replace(position, 6, "\\\\ud800");
    return nlohmann::json::parse(source);
}

const nlohmann::json& pointer(
  const nlohmann::json& fixture,
  const nlohmann::json& pointers,
  const char* name)
{
    return fixture.at(nlohmann::json::json_pointer(pointers.at(name).get<std::string>()));
}

std::vector<std::vector<std::uint8_t>> bytes(
  const nlohmann::json& fixture,
  const nlohmann::json& item)
{
    if (item.contains("byteRecipe")) {
        std::vector<std::uint8_t> result;
        const auto& recipe = item.at("byteRecipe");
        result.reserve(recipe.at("encodedBytes").get<std::size_t>());
        for (const auto& segment : recipe.at("segments")) {
            if (segment.contains("hex")) {
                const auto part = from_hex(segment.at("hex").get<std::string>());
                result.insert(result.end(), part.begin(), part.end());
            } else {
                result.insert(result.end(), segment.at("count").get<std::size_t>(),
                  segment.at("repeatByte").get<std::uint8_t>());
            }
        }
        if (result.size() != recipe.at("encodedBytes").get<std::size_t>())
            throw std::runtime_error("byte recipe size mismatch");
        std::vector<std::vector<std::uint8_t>> frames;
        frames.emplace_back(std::move(result));
        return frames;
    }
    if (item.contains("chunksHex")) {
        std::vector<std::uint8_t> result;
        for (const auto& chunk : item.at("chunksHex")) {
            const auto part = from_hex(chunk.get<std::string>());
            result.insert(result.end(), part.begin(), part.end());
        }
        std::vector<std::vector<std::uint8_t>> frames;
        frames.emplace_back(std::move(result));
        return frames;
    }
    if (item.contains("hex"))
        return {from_hex(item.at("hex").get<std::string>())};
    if (item.contains("framesHex")) {
        std::vector<std::vector<std::uint8_t>> result;
        for (const auto& frame : item.at("framesHex"))
            result.push_back(from_hex(frame.get<std::string>()));
        return result;
    }
    const auto& pointers = item.at("pointers");
    if (pointers.contains("framesHex")) {
        std::vector<std::vector<std::uint8_t>> result;
        for (const auto& frame : pointer(fixture, pointers, "framesHex"))
            result.push_back(from_hex(frame.get<std::string>()));
        return result;
    }
    for (const char* name : {"encodedHex", "logicalHex", "hex"}) {
        if (pointers.contains(name))
            return {from_hex(pointer(fixture, pointers, name).get<std::string>())};
    }
    throw std::runtime_error("encoded pointer missing");
}

template<class Result, class Encode>
outcome exact(Result decoded, Encode encode, const std::vector<std::uint8_t>& input)
{
    if (!decoded)
        return {false, decoded.error};
    const auto encoded = encode(decoded.value);
    if (!encoded)
        return {false, encoded.error};
    return {encoded.value == input, encoded.value == input ? codec::error_code::ok : codec::error_code::constraint};
}

template<class Result, class Encode>
outcome exact_frames(Result decoded, Encode encode, const std::vector<std::vector<std::uint8_t>>& input)
{
    if (!decoded)
        return {false, decoded.error};
    const auto encoded = encode(decoded.value);
    if (!encoded)
        return {false, encoded.error};
    return {encoded.value == input, encoded.value == input ? codec::error_code::ok : codec::error_code::constraint};
}

template<class Result>
outcome encoded(Result result, const std::vector<std::uint8_t>& input)
{
    if (!result)
        return {false, result.error};
    return {result.value == input,
      result.value == input ? codec::error_code::ok : codec::error_code::constraint};
}

outcome command(
  std::uint8_t id,
  const std::vector<std::vector<std::uint8_t>>& frames,
  const codec::codec_context_t& context)
{
    switch (id) {
        case 16:
            return exact_frames(codec::decode_nodeSend_16_frames(frames, context), [&](const auto& value) { return codec::encode_nodeSend_16_frames(value, context); }, frames);
        case 24:
            return exact_frames(codec::decode_actorSend_24_frames(frames, context), [&](const auto& value) { return codec::encode_actorSend_24_frames(value, context); }, frames);
        case 28:
            return exact_frames(codec::decode_actorJoin_28_frames(frames, context), [&](const auto& value) { return codec::encode_actorJoin_28_frames(value, context); }, frames);
        case 47:
            return exact_frames(codec::decode_userSpotCreate_47_frames(frames), codec::encode_userSpotCreate_47_frames, frames);
        case 48:
            return exact_frames(codec::decode_userSpotClose_48_frames(frames), codec::encode_userSpotClose_48_frames, frames);
        case 49:
            return exact_frames(codec::decode_actorCreate_49_frames(frames), codec::encode_actorCreate_49_frames, frames);
        default:
            return {false, codec::error_code::header};
    }
}

outcome type(
  std::string_view name,
  const std::vector<std::uint8_t>& input,
  const codec::codec_context_t& context)
{
    if (name == "application-version")
        return exact(codec::decode_application_version(input), codec::encode_application_version, input);
    if (name == "bool8")
        return exact(codec::decode_bool8(input), codec::encode_bool8, input);
    if (name == "rid")
        return exact(codec::decode_rid(input), codec::encode_rid, input);
    if (name == "optional-actor-ref")
        return exact(codec::decode_optional_actor_ref(input), codec::encode_optional_actor_ref, input);
    if (name == "actor-ref")
        return exact(codec::decode_actor_ref(input), codec::encode_actor_ref, input);
    if (name == "sorted-text8-vector")
        return exact(codec::decode_sorted_text8_vector(input), codec::encode_sorted_text8_vector, input);
    if (name == "metadata-frame")
        return exact(codec::decode_metadata_frame(input), codec::encode_metadata_frame, input);
    if (name == "authority-payload-v1")
        return exact(codec::decode_durable_authority_payload_v1(input), codec::encode_durable_authority_payload_v1, input);
    if (name == "instance-activation-recovery-v1")
        return exact(codec::decode_durable_instance_activation_recovery_v1(input, context), [&](const auto& value) { return codec::encode_durable_instance_activation_recovery_v1(value, context); }, input);
    if (name == "relocation-data-chunk-v1")
        return exact(codec::decode_durable_relocation_data_chunk_v1(input), codec::encode_durable_relocation_data_chunk_v1, input);
    if (name == "relocation-manifest-v1")
        return exact(codec::decode_durable_relocation_manifest_v1(input), codec::encode_durable_relocation_manifest_v1, input);
    if (name == "relocation-envelope-v1")
        return exact(codec::decode_relocation_envelope_v1(input, context), [&](const auto& value) { return codec::encode_relocation_envelope_v1(value, context); }, input);
    if (name == "descriptor-extension")
        return exact(codec::decode_descriptor_extension(input), codec::encode_descriptor_extension, input);
    if (name == "relocation-object-identity")
        return exact(codec::decode_relocation_object_identity(input), codec::encode_relocation_object_identity, input);
    if (name == "aggregate-participant-vector")
        return exact(codec::decode_aggregate_participant_vector(input), codec::encode_aggregate_participant_vector, input);
    if (name == "text8")
        return exact(codec::decode_text8(input), codec::encode_text8, input);
    if (name == "application-payload-bytes")
        return exact(codec::decode_application_payload_bytes(input, context), [&](const auto& value) { return codec::encode_application_payload_bytes(value, context); }, input);
    if (name == "application-payload-envelope-v1")
        return exact(codec::decode_application_payload_envelope_v1(input, context), [&](const auto& value) { return codec::encode_application_payload_envelope_v1(value, context); }, input);
    if (name == "creation-operation-terminal-v1")
        return exact(codec::decode_creation_operation_terminal_v1(input, context), [&](const auto& value) { return codec::encode_creation_operation_terminal_v1(value, context); }, input);
    return {false, codec::error_code::header};
}

bool pilot_oracle(
  const nlohmann::json& surface,
  const std::vector<std::vector<std::uint8_t>>& frames,
  bool& available)
{
    available = true;
    try {
        const auto format = surface.at("format").get<std::string>();
        if (format == "actor-join-request-v1")
            return pilot::encode_actor_join_28(pilot::decode_actor_join_28(frames)) == frames;
        if (format == "user-spot-create-v1")
            return pilot::encode_user_spot_create_47(pilot::decode_user_spot_create_47(frames.front())) == frames.front();
        if (format == "user-spot-close-v1")
            return pilot::encode_user_spot_close_48(pilot::decode_user_spot_close_48(frames.front())) == frames.front();
        if (format == "actor-create-v1")
            return pilot::encode_actor_create_49(pilot::decode_actor_create_49(frames.front())) == frames.front();
        if (format == "relocation-envelope-v1")
            return pilot::encode_relocation_envelope_v1(pilot::decode_relocation_envelope_v1(frames)) == frames.front();
        if (format == "relocation-data-chunk-v1")
            return pilot::encode_relocation_data_chunk_v1(pilot::decode_relocation_data_chunk_v1(frames.front())) == frames.front();
        if (format == "relocation-manifest-v1")
            return pilot::encode_relocation_manifest_v1(pilot::decode_relocation_manifest_v1(frames.front())) == frames.front();
        available = false;
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

outcome negotiated_case(
  const nlohmann::json& item,
  std::string_view direction,
  const std::vector<std::uint8_t>& input)
{
    const auto context = operation_context(item);
    const auto maximum = maximum_codec_context();
    const auto name = item.at("surface").at("type").get<std::string>();
    if (name == "application-payload-bytes") {
        if (direction == "decode") {
            const auto result = codec::decode_application_payload_bytes(input, context);
            return {static_cast<bool>(result), result.error};
        }
        const auto decoded = codec::decode_application_payload_bytes(input, maximum);
        if (!decoded)
            return {false, decoded.error};
        const auto encoded = codec::encode_application_payload_bytes(decoded.value, context);
        return {encoded && encoded.value == input, encoded ? codec::error_code::ok : encoded.error};
    }
    if (name == "application-payload-envelope-v1") {
        if (direction == "decode") {
            const auto result = codec::decode_application_payload_envelope_v1(input, context);
            return {static_cast<bool>(result), result.error};
        }
        const auto decoded = codec::decode_application_payload_envelope_v1(input, maximum);
        if (!decoded)
            return {false, decoded.error};
        const auto encoded = codec::encode_application_payload_envelope_v1(decoded.value, context);
        return {encoded && encoded.value == input, encoded ? codec::error_code::ok : encoded.error};
    }
    return {false, codec::error_code::header};
}

codec::relocation_object_identity_t relocation_identity(const nlohmann::json& input)
{
    codec::relocation_object_identity_t value{};
    const auto kind = input.at("objectKind").get<std::string>();
    value.objectKind = kind == "actor" ? codec::stateful_object_kind_t::actor
      : codec::stateful_object_kind_t::userSpot;
    if (input.at("variant") == "actor") {
        value.tag = codec::relocation_object_identity_t::tag_t::case_0;
        codec::relocation_object_identity_t::case_0_t selected{};
        selected.actor.actorId.value = input.at("actor").at("actorId").get<std::string>();
        selected.actor.objectGeneration.value = input.at("actor").at("objectGeneration").get<std::uint64_t>();
        selected.expectedAuthorityOwnerGeneration.value =
          input.at("expectedAuthorityOwnerGeneration").get<std::uint64_t>();
        value.value = std::move(selected);
    }
    return value;
}

codec::descriptor_extension_t descriptor(const nlohmann::json& input)
{
    codec::descriptor_extension_t value{};
    if (!input.at("runtimeState").is_null())
        value.runtimeState = codec::runtime_state_t::serving;
    value.applicationVersion = codec::application_version_t{
      input.at("applicationVersion").get<std::int64_t>()};
    codec::sorted_text8_vector_t capabilities{};
    for (const auto& capability : input.at("protocolCapabilities"))
        capabilities.items.push_back(codec::text8_t{capability.get<std::string>()});
    value.protocolCapabilities = std::move(capabilities);
    value.objectRole = codec::object_role_t::none;
    value.placementWeight = codec::u32_t{input.at("placementWeight").get<std::uint32_t>()};
    value.activeCapacityLimit = codec::object_capacity_limit_t{
      input.at("activeCapacityLimit").get<std::uint32_t>()};
    value.pendingCapacityLimit = codec::object_pending_capacity_limit_t{
      input.at("pendingCapacityLimit").get<std::uint32_t>()};
    value.activeCapacityUsed = codec::u32_t{input.at("activeCapacityUsed").get<std::uint32_t>()};
    value.pendingCapacityUsed = codec::u32_t{input.at("pendingCapacityUsed").get<std::uint32_t>()};
    return value;
}

outcome semantic_encode(
  const nlohmann::json& item,
  const std::vector<std::uint8_t>& expected,
  const codec::codec_context_t& context)
{
    const auto name = item.at("surface").at("type").is_null()
      ? std::string{} : item.at("surface").at("type").get<std::string>();
    const auto& input = item.at("input");
    if (name == "text8") {
        codec::text8_t value{};
        value.value = std::string("\xed\xa0\x80", 3);
        return encoded(codec::encode_text8(value), expected);
    }
    if (name == "optional-actor-ref") {
        codec::optional_actor_ref_t value{};
        if (!input.at("actorId").is_null())
            value.actorId.value = input.at("actorId").get<std::string>();
        if (!input.at("generation").is_null())
            value.generation = codec::nonzero_u64_t{input.at("generation").get<std::uint64_t>()};
        return encoded(codec::encode_optional_actor_ref(value), expected);
    }
    if (name == "relocation-object-identity")
        return encoded(codec::encode_relocation_object_identity(relocation_identity(input)), expected);
    if (name == "sorted-text8-vector") {
        codec::sorted_text8_vector_t value{};
        for (const auto& text : input)
            value.items.push_back(codec::text8_t{text.get<std::string>()});
        return encoded(codec::encode_sorted_text8_vector(value), expected);
    }
    if (name == "descriptor-extension")
        return encoded(codec::encode_descriptor_extension(descriptor(input)), expected);
    if (name == "aggregate-participant-vector") {
        codec::aggregate_participant_vector_t value{};
        for (const auto& entry : input) {
            codec::maintenance_aggregate_participant_v1_t participant{};
            auto object = entry.at("object");
            object["variant"] = "actor";
            participant.object = relocation_identity(object);
            participant.expectedStoreVersion.value = entry.at("expectedStoreVersion").get<std::string>();
            participant.mutation.value = from_hex(entry.at("mutationHex").get<std::string>());
            value.items.push_back(std::move(participant));
        }
        return encoded(codec::encode_aggregate_participant_vector(value), expected);
    }
    if (name == "application-payload-bytes") {
        codec::application_payload_bytes_t value{};
        value.value.assign(input.at("count").get<std::size_t>(), input.at("repeatByte").get<std::uint8_t>());
        return encoded(codec::encode_application_payload_bytes(value, context), expected);
    }
    if (name == "creation-operation-terminal-v1") {
        codec::creation_operation_terminal_v1_t value{};
        value.terminalResult = codec::request_terminal_result_t::timedOut;
        value.failureCode = codec::framework_error_code_t::requestFailed;
        value.hasCreation = codec::bool8_t::false_;
        value.hasApplicationPayload = codec::bool8_t::false_;
        return encoded(codec::encode_creation_operation_terminal_v1(value, context), expected);
    }
    return {false, codec::error_code::header};
}

outcome operation_case(const nlohmann::json& item, std::string_view direction)
{
    if (item.at("operation") == "runtime-predicate") {
        codec::reply_20_t value{};
        value.terminalResult = codec::request_terminal_result_t::ok;
        value.failureCode = item.at("input").at("failureCode") == "none"
          ? codec::framework_error_code_t::none
          : codec::framework_error_code_t::actorRouteNotFound;
        const auto accepted = codec::validate_reply_20_runtime_predicates(value);
        return {accepted, accepted ? codec::error_code::ok : codec::error_code::predicate};
    }
    const auto frames = bytes(nlohmann::json::object(), item);
    const auto& surface = item.at("surface");
    if (item.at("operation") == "negotiated-bound")
        return negotiated_case(item, direction, frames.front());
    if (direction == "encode" && item.contains("input"))
        return semantic_encode(item, frames.front(), operation_context(item));
    const auto context = maximum_codec_context();
    if (surface.at("format") == "command")
        return command(surface.at("commandId").get<std::uint8_t>(), frames, context);
    return frames.size() == 1
      ? type(surface.at("type").get<std::string>(), frames.front(), context)
      : outcome{false, codec::error_code::trailing};
}

} // namespace

int main()
{
    const std::filesystem::path indexPath = ZLINK_SERVICE_WIRE_FIXTURE_INDEX_PATH;
    const auto index = load_json(indexPath);
    const auto protocolRoot = indexPath.parent_path().parent_path().parent_path();
    bool passed = true;
    const auto context = maximum_codec_context();

    for (const auto& entry : index.at("fixtures")) {
        const auto fixture = load_json(protocolRoot / entry.at("goldenFixture").get<std::string>());
        const auto kind = entry.at("kind").get<std::string>();
        const auto& surface = entry.at("surface");
        const auto verify = [&](const nlohmann::json& item, bool expected) {
            const auto input = bytes(fixture, item);
            const auto result = kind == "command"
              ? command(surface.at("commandId").get<std::uint8_t>(), input, context)
              : (input.size() == 1
                  ? type(surface.at("type").get<std::string>(), input.front(), context)
                  : outcome{false, codec::error_code::trailing});
            if (result.accepted != expected) {
                std::cerr << kind << ':' << surface.at("format").get<std::string>() << ':'
                          << item.at("name").get<std::string>() << ": operation=fixture, expected "
                          << (expected ? "accept" : "reject") << ", error-code="
                          << static_cast<int>(result.error) << '\n';
                passed = false;
            }
            bool oracleAvailable{};
            const auto oracleAccepted = pilot_oracle(surface, input, oracleAvailable);
            if (oracleAvailable && oracleAccepted != result.accepted) {
                std::cerr << kind << ':' << surface.at("format").get<std::string>() << ':'
                          << item.at("name").get<std::string>() << ": pilot oracle disagreement\n";
                passed = false;
            }
        };
        for (const auto& item : entry.at("canonical"))
            verify(item, true);
        for (const auto& item : entry.at("malformed"))
            verify(item, false);
    }

    std::map<std::string, unsigned> boundaryPairs;
    std::size_t acceptedCases{};
    std::size_t rejectedCases{};
    for (const auto& item : index.at("operationCases")) {
        const auto expected = item.at("expect") == "accept";
        expected ? ++acceptedCases : ++rejectedCases;
        if (item.contains("boundaryPair"))
            boundaryPairs[item.at("boundaryPair").get<std::string>()] |= expected ? 1u : 2u;
        const auto directions = item.contains("directions")
          ? item.at("directions")
          : nlohmann::json::array({"decode"});
        for (const auto& directionValue : directions) {
            const auto direction = directionValue.get<std::string>();
            const auto result = operation_case(item, direction);
            if (result.accepted != expected) {
                std::cerr << "operation-case:" << item.at("name").get<std::string>()
                          << ": operation=" << item.at("operation").get<std::string>()
                          << ", direction=" << direction
                          << ", expected " << (expected ? "accept" : "reject")
                          << ", error-code=" << static_cast<int>(result.error) << '\n';
                passed = false;
            }
        }
    }
    if (index.at("version") != 3 || acceptedCases != 29 || rejectedCases != 50
        || boundaryPairs.size() != 25) {
        std::cerr << "fixture catalog v3 coverage mismatch: accept=" << acceptedCases
                  << ", reject=" << rejectedCases << ", boundary-pairs=" << boundaryPairs.size() << '\n';
        passed = false;
    }
    for (const auto& [operation, coverage] : boundaryPairs) {
        if (coverage != 3u) {
            std::cerr << "operation boundary pair incomplete: operation=" << operation << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
