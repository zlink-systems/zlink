/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include <nlohmann/json.hpp>
#include <service_wire_constants.hpp>
#include <fstream>
#include <iostream>
#include <map>

using kind_t = zlink::framework::framework_error_kind_t;
int main ()
{
    const std::map<std::string, kind_t> kinds{{"NotFound", kind_t::not_found},
                                              {"AlreadyExists", kind_t::already_exists},
                                              {"TypeMismatch", kind_t::type_mismatch},
                                              {"NotConfigured", kind_t::not_configured},
                                              {"Rejected", kind_t::rejected},
                                              {"Unavailable", kind_t::unavailable},
                                              {"DeadlineExceeded", kind_t::deadline_exceeded},
                                              {"ShuttingDown", kind_t::shutting_down},
                                              {"ProtocolError", kind_t::protocol_error},
                                              {"InvalidOperation", kind_t::invalid_operation},
                                              {"DataLost", kind_t::data_lost},
                                              {"InternalFailure", kind_t::internal_failure}};
    std::ifstream input (ZLINK_ERROR_MAPPING_CONFORMANCE_PATH);
    const auto fixture = nlohmann::json::parse (input);
    const zlink::framework::runtime::messaging::request_failure_mapper_t mapper;
    int failures = 0;
    for (const auto &row : fixture.at ("receive")) {
        if (mapper
              .reply_header_exception (row.at ("terminalResult"), row.at ("failureCode"), "test")
              .kind ()
            != kinds.at (row.at ("kind"))) {
            std::cerr << "receive mismatch: " << row.at ("code") << '\n';
            ++failures;
        }
    }
    for (const auto &row : fixture.at ("receive")) {
        const auto kind = kinds.at (row.at ("kind"));
        const auto code = row.at ("failureCode").get<std::uint32_t> ();
        if (mapper.failure_code_kind (code) != kind) {
            std::cerr << "code-only receive mismatch: " << row.at ("code") << '\n';
            ++failures;
        }
        const auto received =
          mapper.reply_header_exception (row.at ("terminalResult"), code, "roundtrip");
        zlink::framework::runtime::messaging::envelope_header_t header;
        header.metadata.emplace (
          std::string (zlink::framework::runtime::messaging::failure_origin_metadata_key),
          std::string (zlink::framework::runtime::messaging::failure_origin_wire_name (
            zlink::framework::detail::failure_origin_t::payload_decode)));
        const auto restored =
          zlink::framework::runtime::messaging::restore_failure_origin (header, received);
        const auto roundtrip = mapper.target_failure_reply (restored);
        if (!roundtrip || roundtrip->failure_code != code
            || roundtrip->terminal_result != row.at ("terminalResult")) {
            std::cerr << "exception roundtrip mismatch: " << row.at ("code") << '\n';
            ++failures;
        }
        for (const auto &[name, target_kind] : kinds) {
            const auto sent = mapper.target_failure_reply (target_kind, code);
            const auto code_only = mapper.target_failure_code (target_kind, code);
            if (code_only
                != (kind == target_kind ? code : mapper.target_failure_code (target_kind))) {
                std::cerr << "code-only cause preservation mismatch: " << name << " / "
                          << row.at ("code") << '\n';
                ++failures;
            }
            const auto representative = mapper.target_failure_reply (target_kind);
            const auto expected = kind == target_kind ? code : representative->failure_code;
            if (!sent
                || !zlink::framework::runtime::protocol::valid_terminal_failure (
                  sent->terminal_result,
                  static_cast<zlink::framework::runtime::protocol::framework_error_code> (
                    sent->failure_code))) {
                std::cerr << "invalid terminal/failure combination\n";
                ++failures;
            }
            if (!sent || sent->failure_code != expected
                || sent->terminal_result
                     != (kind == target_kind ? row.at ("terminalResult").get<std::uint32_t> ()
                                             : representative->terminal_result)) {
                std::cerr << "cause preservation mismatch: " << name << " / " << row.at ("code")
                          << '\n';
                ++failures;
            }
        }
    }
    if (mapper.failure_code_kind (0) != kind_t::internal_failure
        || mapper.failure_code_kind (9999) != kind_t::internal_failure) {
        std::cerr << "unknown failure code mismatch\n";
        ++failures;
    }
    for (const auto &row : fixture.at ("send")) {
        const auto result = mapper.target_failure_reply (kinds.at (row.at ("kind")));
        if (!result || result->terminal_result != row.at ("terminalResult")
            || result->failure_code != row.at ("failureCode")) {
            std::cerr << "send mismatch: " << row.at ("kind") << '\n';
            ++failures;
        }
    }
    for (const auto &row : fixture.at ("send")) {
        const auto code = mapper.target_failure_code (kinds.at (row.at ("kind")));
        if (code
            != row.value ("codeOnlyFailureCode", row.at ("failureCode").get<std::uint32_t> ())) {
            std::cerr << "code-only send mismatch: " << row.at ("kind") << '\n';
            ++failures;
        }
    }
    using namespace zlink::framework::runtime::protocol;
    if (valid_terminal_failure (static_cast<std::uint32_t> (request_terminal_result::internalError),
                                framework_error_code::actorAlreadyExists)
        || valid_terminal_failure (static_cast<std::uint32_t> (request_terminal_result::conflict),
                                   framework_error_code::none)) {
        std::cerr << "malformed terminal/failure accepted\n";
        ++failures;
    }
    std::cout << "receive=" << fixture.at ("receive").size ()
              << " send=" << fixture.at ("send").size ()
              << " code-only-send=" << fixture.at ("send").size () << " failures=" << failures
              << '\n';
    return failures == 0 ? 0 : 1;
}
