/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include <service_wire_codec.hpp>
#include <service_wire_pilot_codec.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    return nlohmann::json::parse(input);
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

outcome command(std::uint8_t id, const std::vector<std::vector<std::uint8_t>>& frames)
{
    switch (id) {
        case 16:
            return exact_frames(codec::decode_nodeSend_16_frames(frames), codec::encode_nodeSend_16_frames, frames);
        case 24:
            return exact_frames(codec::decode_actorSend_24_frames(frames), codec::encode_actorSend_24_frames, frames);
        case 28:
            return exact_frames(codec::decode_actorJoin_28_frames(frames), codec::encode_actorJoin_28_frames, frames);
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

outcome type(std::string_view name, const std::vector<std::uint8_t>& input)
{
    if (name == "authority-payload-v1")
        return exact(codec::decode_durable_authority_payload_v1(input), codec::encode_durable_authority_payload_v1, input);
    if (name == "instance-activation-recovery-v1")
        return exact(codec::decode_durable_instance_activation_recovery_v1(input), codec::encode_durable_instance_activation_recovery_v1, input);
    if (name == "relocation-data-chunk-v1")
        return exact(codec::decode_durable_relocation_data_chunk_v1(input), codec::encode_durable_relocation_data_chunk_v1, input);
    if (name == "relocation-manifest-v1")
        return exact(codec::decode_durable_relocation_manifest_v1(input), codec::encode_durable_relocation_manifest_v1, input);
    if (name == "relocation-envelope-v1")
        return exact(codec::decode_relocation_envelope_v1(input), codec::encode_relocation_envelope_v1, input);
    if (name == "descriptor-extension")
        return exact(codec::decode_descriptor_extension(input), codec::encode_descriptor_extension, input);
    if (name == "text8")
        return exact(codec::decode_text8(input), codec::encode_text8, input);
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

outcome operation_case(const nlohmann::json& item)
{
    if (item.at("operation") == "runtime-predicate") {
        codec::reply_20_t value{};
        value.terminalResult = codec::request_terminal_result_t::ok;
        value.failureCode = codec::framework_error_code_t::actorRouteNotFound;
        const auto accepted = codec::validate_reply_20_runtime_predicates(value);
        return {accepted, accepted ? codec::error_code::ok : codec::error_code::predicate};
    }
    const auto frames = bytes(nlohmann::json::object(), item);
    const auto& surface = item.at("surface");
    if (surface.at("format") == "command")
        return command(surface.at("commandId").get<std::uint8_t>(), frames);
    return frames.size() == 1
      ? type(surface.at("type").get<std::string>(), frames.front())
      : outcome{false, codec::error_code::trailing};
}

} // namespace

int main()
{
    const std::filesystem::path indexPath = ZLINK_SERVICE_WIRE_FIXTURE_INDEX_PATH;
    const auto index = load_json(indexPath);
    const auto protocolRoot = indexPath.parent_path().parent_path().parent_path();
    bool passed = true;

    for (const auto& entry : index.at("fixtures")) {
        const auto fixture = load_json(protocolRoot / entry.at("goldenFixture").get<std::string>());
        const auto kind = entry.at("kind").get<std::string>();
        const auto& surface = entry.at("surface");
        const auto verify = [&](const nlohmann::json& item, bool expected) {
            const auto input = bytes(fixture, item);
            const auto result = kind == "command"
              ? command(surface.at("commandId").get<std::uint8_t>(), input)
              : (input.size() == 1
                  ? type(surface.at("type").get<std::string>(), input.front())
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

    for (const auto& item : index.at("operationCases")) {
        const auto result = operation_case(item);
        const auto expected = item.at("expect") == "accept";
        if (result.accepted != expected) {
            std::cerr << "operation-case:" << item.at("name").get<std::string>()
                      << ": operation=" << item.at("operation").get<std::string>()
                      << ", expected " << (expected ? "accept" : "reject")
                      << ", error-code=" << static_cast<int>(result.error) << '\n';
            passed = false;
        }
    }
    return passed ? 0 : 1;
}
