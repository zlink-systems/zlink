/* SPDX-License-Identifier: MPL-2.0 */

#include "../support/read_text_file.hpp"
#include "../support/target_contract_gate.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#ifndef ZLINK_FRAMEWORK_CPP_SOURCE_DIR
#error "ZLINK_FRAMEWORK_CPP_SOURCE_DIR must be defined"
#endif

namespace
{

using zlink::framework::tests::read_text_file;

// GCC 13 evaluates a co_await operand of ||, && or ?: even when the other
// operand already decides the result, so a guarded null dereference runs.
// Runtime sources split such awaits into if statements.
std::vector<std::string> short_circuit_awaits (const std::filesystem::path &root)
{
    static const std::regex same_line (R"((\|\||&&|\?)[^;]*co_await)");
    static const std::regex continued (R"(^\s*(\|\||&&|\?|:)\s*[!(]*\s*co_await)");
    std::vector<std::string> found;
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        const auto ext = entry.path ().extension ();
        if (!entry.is_regular_file () || (ext != ".hpp" && ext != ".cpp"))
            continue;
        const auto text = read_text_file (entry.path ());
        std::size_t line_number = 0;
        std::size_t start = 0;
        while (start < text.size ()) {
            const auto end = text.find ('\n', start);
            const auto line = text.substr (start, end == std::string::npos ? end : end - start);
            ++line_number;
            if (std::regex_search (line, same_line) || std::regex_search (line, continued))
                found.push_back (entry.path ().string () + ":" + std::to_string (line_number));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    return found;
}

// Per-message success traces must not construct even a lazy builder while Off.
// Require an adjacent live guard; a mode snapshot at callback registration is
// insufficient because the level may change before the callback runs.
std::vector<std::string> ungated_message_flow_calls (const std::filesystem::path &root)
{
    const std::regex call (
      R"((?:^|\n)[ \t]*((?:(?:[a-zA-Z_]\w*::)*[a-zA-Z_]\w*(?:\s*\([^;\n]*\))?\s*\.(?:trace|trace_bound_session_send_stage)|(?:detail::)?(?:report_actor_dispatch_stage_trace_lazy|report_actor_handoff_request_trace|trace_detached_bound_session_send_stage|trace_bound_session_send_stage))\s*\())");
    const std::regex guard (
      R"(if\s*\([^;{}]*(?:\.enabled(?:_for)?|trace_bound_session_send_stage_enabled)\s*\([^;{}]*\)[^;{}]*\)\s*\{\s*$)");
    const auto guarded = [&guard] (const std::string &source, std::size_t start) {
        // Only the immediately enclosing statement can guard this builder.
        // Avoid rescanning the entire file prefix for every trace call.
        const auto open = source.rfind ('{', start);
        const auto boundary = open == std::string::npos || open == 0
                                ? std::string::npos
                                : source.find_last_of (";{}", open - 1);
        const auto begin = boundary == std::string::npos ? 0 : boundary + 1;
        return std::regex_search (source.substr (begin, start - begin), guard);
    };
    std::vector<std::string> found;
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        const auto ext = entry.path ().extension ();
        if (!entry.is_regular_file () || (ext != ".hpp" && ext != ".cpp"))
            continue;
        const auto source = read_text_file (entry.path ());
        for (auto it = std::sregex_iterator (source.begin (), source.end (), call);
             it != std::sregex_iterator (); ++it) {
            const auto start = static_cast<std::size_t> (it->position (1));
            const auto args = start + static_cast<std::size_t> (it->length (1));
            const auto builder = source.find ("[", args);
            const auto end = source.find (';', args);
            const auto arguments = source.substr (args, std::min (builder, end) - args);
            // Failure/drop transitions retain their lazy construction contract.
            if (arguments.find ("message_flow_outcome_t::dropped") != std::string::npos
                || arguments.find ("message_flow_outcome_t::backpressured") != std::string::npos
                || arguments.find ("message_flow_result_t::failed") != std::string::npos)
                continue;
            // Forwarding an existing builder constructs no event or lambda.
            if (arguments.find ("std::forward<BuildResult>") != std::string::npos)
                continue;
            // Actor transfer markers are rare lifecycle transitions; their
            // named builder is intentionally lazy, like failure transitions.
            if (arguments.find ("transfer_id, build_event") != std::string::npos)
                continue;
            // RouteMesh's shared builder is checked through its callers below.
            if (entry.path ().filename () == "route_packet_dispatcher.cpp")
                continue;
            if (!guarded (source, start)) {
                const auto line = 1 + std::count (source.begin (), source.begin () + start, '\n');
                found.push_back (entry.path ().string () + ":" + std::to_string (line));
            }
        }
        if (entry.path ().filename () == "route_packet_dispatcher.cpp") {
            const std::regex route_call (R"(\n[ \t]*trace_route_flow\s*\()");
            for (auto it = std::sregex_iterator (source.begin (), source.end (), route_call);
                 it != std::sregex_iterator (); ++it) {
                const auto start = static_cast<std::size_t> (it->position ());
                if (!guarded (source, start)) {
                    const auto line =
                      2 + std::count (source.begin (), source.begin () + start, '\n');
                    found.push_back (entry.path ().string () + ":" + std::to_string (line));
                }
            }
        }
    }
    return found;
}

using zlink::framework::tests::gate_t;

} // namespace

int main ()
{
    const std::filesystem::path root = ZLINK_FRAMEWORK_CPP_SOURCE_DIR;
    const auto include_root = root / "framework/include";

    for (const auto &required : {include_root, root / "framework/src"}) {
        if (!std::filesystem::exists (required)) {
            std::cerr << "source scan root is missing: " << required << '\n';
            return 1;
        }
    }

    gate_t gate;

    /* CPP-OBS-004 — spec 26 §5: the live call-site gate precedes the
     * per-message event builder, including callbacks and shared RouteMesh calls. */
    const auto ungated_flow_calls = ungated_message_flow_calls (root / "framework/src");
    for (const auto &site : ungated_flow_calls)
        gate.require (false, "CPP-OBS-004", "ungated per-message trace: " + site);

    for (const auto &scanned : {root / "framework/src", include_root}) {
        for (const auto &site : short_circuit_awaits (scanned))
            gate.require (false, "GCC-COAWAIT-01",
                          "co_await inside a short-circuit or conditional operand: " + site);
    }

    if (gate.failures != 0) {
        std::cerr << "source scan gate failures: " << gate.failures << '\n';
        return 1;
    }
    std::cout << "source scan gate satisfied\n";
    return 0;
}
