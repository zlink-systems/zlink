/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#ifndef ZLINK_FRAMEWORK_CPP_SOURCE_DIR
#error "ZLINK_FRAMEWORK_CPP_SOURCE_DIR must be defined"
#endif

namespace
{

bool require_exists (const std::filesystem::path &path)
{
    if (std::filesystem::exists (path)) {
        return true;
    }
    std::cerr << "missing required path: " << path << '\n';
    return false;
}

bool require_absent (const std::filesystem::path &path, const std::string &reason)
{
    if (!std::filesystem::exists (path)) {
        return true;
    }
    std::cerr << "unexpected path: " << path << " (" << reason << ")\n";
    return false;
}

bool public_headers_do_not_include_runtime (const std::filesystem::path &root)
{
    if (!std::filesystem::exists (root)) {
        return true;
    }
    bool ok = true;
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".h") {
            continue;
        }

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            if (line.find ("src/runtime") != std::string::npos
                || line.find ("/Private/") != std::string::npos
                || line.find ("Private/") != std::string::npos) {
                std::cerr << "public header references runtime implementation: " << entry.path ()
                          << ':' << line_no << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

bool public_headers_do_not_expose_runtime_dependencies (const std::filesystem::path &root)
{
    if (!std::filesystem::exists (root)) {
        return true;
    }
    bool ok = true;
    const std::string forbidden[] = {"#include <boost",
                                     "#include \"boost",
                                     "boost::asio",
                                     "boost::beast",
                                     "boost::asio::awaitable",
                                     "#include <future>",
                                     "std::future",
                                     "std::promise",
                                     "#include <openssl",
                                     "#include <OpenSSL",
                                     "SSL_CTX",
                                     "SSL_CTX_",
                                     "SSL *",
                                     "SSL*",
                                     "ssl::stream",
                                     "#include <gtest",
                                     "#include <gmock",
                                     "testing::",
                                     "#include <spdlog",
                                     "spdlog",
                                     "spdlog::",
                                     "#include <fmt",
                                     "fmt::",
                                     "#include <nlohmann",
                                     "nlohmann::",
                                     "#include <msgpack",
                                     "msgpack::",
                                     "#include <google/protobuf",
                                     "google::protobuf",
                                     "#include <kafka",
                                     "#include <Kafka",
                                     "Kafka::",
                                     "RdKafka",
                                     "#include <grpc",
                                     "#include <gRPC",
                                     "grpc::",
                                     "#include <yaml",
                                     "#include <YAML",
                                     "YAML::",
                                     "#include <flatbuffers",
                                     "#include <FlatBuffers",
                                     "flatbuffers::",
                                     "#include <zlink.hpp",
                                     "#include <zlink/Contracts/Sockets",
                                     "#include <zlink/Contracts/Service",
                                     "zlink::context_t",
                                     "zlink::router_socket_t",
                                     "zlink::stream_socket_t",
                                     "zlink::dealer_socket_t",
                                     "zlink::pub_socket_t",
                                     "zlink::sub_socket_t"};

    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".h") {
            continue;
        }

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            for (const auto &needle : forbidden) {
                const auto relative = std::filesystem::relative (entry.path (), root);
                const auto relative_text = relative.generic_string ();
                if ((relative_text == "framework/include/zlink/framework/codecs/json.hpp"
                     || relative_text
                          == "framework/include/zlink/framework/codecs/json_stream_connector.hpp"
                     || relative_text
                          == "framework/include/zlink/framework/contracts/codecs/serializer.hpp"
                     || relative_text == "zlink/framework/codecs/json.hpp"
                     || relative_text == "zlink/framework/codecs/json_stream_connector.hpp"
                     || relative_text == "zlink/framework/contracts/codecs/serializer.hpp")
                    && (needle == "#include <nlohmann" || needle == "nlohmann::")) {
                    continue;
                }
                if ((relative_text
                       == "connector/core/include/zlink/stream_connector/contracts/calls/"
                          "zlink_stream_calls.hpp"
                     || relative_text
                          == "zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp")
                    && (needle == "#include <future>" || needle == "std::future"
                        || needle == "std::promise")) {
                    continue;
                }
                if ((relative_text
                       == "framework/include/zlink/framework/contracts/locations/location.hpp"
                     || relative_text
                          == "framework/include/zlink/framework/contracts/locations/rows.hpp"
                     || relative_text
                          == "zlink/framework/contracts/locations/location.hpp"
                     || relative_text == "zlink/framework/contracts/locations/rows.hpp")
                    && needle == "#include <zlink/Contracts/Service") {
                    continue;
                }
                if (line.find (needle) != std::string::npos) {
                    std::cerr << "public header exposes runtime/test dependency: " << entry.path ()
                              << ':' << line_no << " contains " << needle << '\n';
                    ok = false;
                }
            }
        }
    }
    return ok;
}

bool file_contains_quiet (const std::filesystem::path &path, const std::string &needle)
{
    std::ifstream input (path);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    return buffer.str ().find (needle) != std::string::npos;
}

bool file_contains (const std::filesystem::path &path, const std::string &needle)
{
    if (file_contains_quiet (path, needle)) {
        return true;
    }
    std::cerr << "file lacks required text: " << path << " :: " << needle << '\n';
    return false;
}

bool is_code_guard_file (const std::filesystem::path &path)
{
    const auto ext = path.extension ().generic_string ();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c"
        || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".h"
        || ext == ".cmake") {
        return true;
    }
    return path.filename () == "CMakeLists.txt";
}

bool path_contains_segment (const std::filesystem::path &path, const std::string &segment)
{
    for (const auto &part : path) {
        if (part == segment) {
            return true;
        }
    }
    return false;
}

bool redesigned_cpp_contract_symbols_do_not_regress (const std::filesystem::path &root)
{
    bool ok = true;
    const std::filesystem::path scan_roots[] = {
      root / "framework", root / "tests", root / "e2e", root / "samples"};
    const std::vector<std::string> forbidden = {
      std::string ("add_actor_") + "packet",
      std::string ("route_request_") + "call_t",
      std::string ("join_spot_") + "raw",
      std::string ("join_entry_spot_") + "raw",
      std::string ("leave") + "Actor",
      std::string ("route_location_") + "resolver_t",
      std::string ("use_registry_") + "spot_resolver",
      std::string ("registry_spot_") + "resolver"};

    for (const auto &scan_root : scan_roots) {
        if (!std::filesystem::exists (scan_root)) {
            std::cerr << "contract scan root is missing: " << scan_root << '\n';
            ok = false;
            continue;
        }
        for (const auto &entry : std::filesystem::recursive_directory_iterator (scan_root)) {
            if (!entry.is_regular_file () || !is_code_guard_file (entry.path ())) {
                continue;
            }
            const auto relative = std::filesystem::relative (entry.path (), root);
            if (path_contains_segment (relative, "Zlink.Framework.ContractTests")) {
                continue;
            }

            std::ifstream input (entry.path ());
            std::string line;
            std::size_t line_no = 0;
            while (std::getline (input, line)) {
                ++line_no;
                for (const auto &needle : forbidden) {
                    if (line.find (needle) != std::string::npos) {
                        std::cerr << "redesigned C++ contract symbol regressed: " << entry.path ()
                                  << ':' << line_no << " contains " << needle << '\n';
                        ok = false;
                    }
                }
            }
        }
    }
    return ok;
}

std::size_t count_occurrences (const std::string &text, const std::string &needle)
{
    std::size_t count = 0;
    std::size_t offset = 0;
    while (true) {
        offset = text.find (needle, offset);
        if (offset == std::string::npos) {
            return count;
        }
        ++count;
        offset += needle.size ();
    }
}

bool non_empty_directories_do_not_keep_gitkeep (const std::filesystem::path &root)
{
    bool ok = true;
    const std::filesystem::path roots[] = {
      root / "framework/include",
      root / "framework/src/runtime",
      root / "connector/core/include",
      root / "connector/core/src/runtime",
      root / "http-client/include",
      root / "http-client/src/runtime",
      root / "extensions/include",
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public",
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Private"};

    for (const auto &scan_root : roots) {
        if (!std::filesystem::exists (scan_root)) {
            continue;
        }
        for (const auto &entry : std::filesystem::recursive_directory_iterator (scan_root)) {
            if (!entry.is_regular_file () || entry.path ().filename () != ".gitkeep") {
                continue;
            }

            const auto dir = entry.path ().parent_path ();
            bool has_real_entry = false;
            for (const auto &sibling : std::filesystem::directory_iterator (dir)) {
                if (sibling.path ().filename () != ".gitkeep") {
                    has_real_entry = true;
                    break;
                }
            }
            if (has_real_entry) {
                std::cerr << "non-empty framework directory still keeps placeholder: "
                          << entry.path () << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

bool actor_model_documents_actor_destroy_lifecycle (const std::filesystem::path &root)
{
    const auto path =
      root.parent_path ().parent_path () / "doc/framework/common/spec/14-actor-model.ko.md";
    std::ifstream input (path);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    const auto text = buffer.str ();

    /* §6.8 prose wraps at column width, so needles that used to span a line break (a space in
     * the needle over a '\n' in the file) never matched. Each needle below is re-pinned to text
     * that stays on one physical source line; the semantic coverage is unchanged. */
    bool ok = true;
    const std::string required[] = {
      "Actor 종료는 새로운 payload admission을 닫고 session binding과 location ownership을",
      "Bound session의 연결이 종료되었다는 이유만으로 Actor를 자동 종료하거나",
      "현재 Spot에서 자동 leave하지 않는다.",
      "Actor destroy는 exact `ActorRef`를 받는다. Actor가 user Spot에 있으면 먼저 leave",
      "또는 Entry Spot join을 완료해야 한다.",
      "Destroy는 membership 이동이 아니다.",
      "성공 과정에서 `OnLeaveActor`를 다시",
      "새로운 payload admission을 닫는다.",
      "Session binding을 제거한다.",
      "Location ownership과 registry entry를 제거한다.",
      "Idempotent `false`를 반환한다.",
      "bound"};
    for (const auto &needle : required) {
        if (text.find (needle) == std::string::npos) {
            std::cerr << "actor model lacks actor destroy lifecycle contract: " << needle << '\n';
            ok = false;
        }
    }
    return ok;
}

bool framework_api_documents_actor_destroy_lifecycle (const std::filesystem::path &root)
{
    const auto path =
      root.parent_path ().parent_path () / "doc/framework/common/spec/06-framework-api.ko.md";
    std::ifstream input (path);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    const auto text = buffer.str ();

    /* The v10 draft cross-referenced 22-actor-model.ko.md by link text and named "transfer
     * adapter" directly; the v11 common/spec consolidation replaced that with the factory
     * declaration, the relocation adapter registration call, and the destroy error
     * classification row below. The underlying facts (destroy-capable factories exist, an
     * adapter is registered for relocation, and destroy has its own error classification) are
     * all still present, just under different wording, so the needles are re-pinned rather than
     * dropped. */
    bool ok = true;
    const std::string required[] = {
      "Object Server의 Entry Spot, user Spot, typed Actor와 actor-free Instance Spot factory",
      "`PreserveStateWith`는 Actor factory에",
      "`ActorRelocationAdapter`, User·Instance Spot factory에 `SpotRelocationAdapter`를 지정한다.",
      "| exact ActorRef destroy | idempotent `false` | `Unavailable` | "
      "`InvalidOperation` | `Unavailable` |"};
    for (const auto &needle : required) {
        if (text.find (needle) == std::string::npos) {
            std::cerr << "framework API spec lacks actor destroy lifecycle contract: " << needle
                      << '\n';
            ok = false;
        }
    }
    return ok;
}

bool session_actor_dispatch_documents_disconnect_destroy_boundary (
  const std::filesystem::path &root)
{
    const auto path =
      root.parent_path ().parent_path ()
      / "doc/framework/common/spec/20-session-actor-dispatch.ko.md";
    std::ifstream input (path);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    const auto text = buffer.str ();

    /* The v10 draft named an explicit "Actor control message"/"control operation" vocabulary
     * that the v11 rewrite retired in favor of naming the exact operations
     * (`OnDisconnectActorAsync`/`NotifyDisconnectedAsync`) directly. The boundary the needles
     * guard -- disconnect never destroys the Actor or changes Spot membership -- is still
     * stated twice (§3 and §4.1), so the needles are re-pinned to that current wording. */
    bool ok = true;
    const std::string required[] = {
      "binding을 해제하지만 Actor를 destroy하거나 Spot membership을 바꾸지 않는다.",
      "두 통지는 connection 종료 사실만 알리며",
      "Actor를 destroy하거나 Spot membership을 변경하지 않는다."};
    for (const auto &needle : required) {
        if (text.find (needle) == std::string::npos) {
            std::cerr << "session actor dispatch spec lacks disconnect/destroy boundary contract: "
                      << needle << '\n';
            ok = false;
        }
    }
    return ok;
}

bool registry_spec_does_not_reintroduce_monitoring_contract (const std::filesystem::path &root)
{
    const auto path = root / "../../doc/framework/cpp/spec/cpp-registry.ko.md";
    std::ifstream input (path);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    const auto text = buffer.str ();

    bool ok = true;
    const std::string stale[] = {"Registry snapshot event는 등록된 monitoring source에만 전달",
                                 "topology나 service summary",
                                 "typed monitoring event",
                                 "Registry snapshot diff event는 설정된 interval을 따르고",
                                 "monitoring 통합 단계에서 별도 regression으로 고정한다"};
    for (const auto &needle : stale) {
        if (text.find (needle) != std::string::npos) {
            std::cerr << "registry spec reintroduced legacy monitoring contract: " << needle
                      << '\n';
            ok = false;
        }
    }
    return ok;
}

bool contract_headers_have_compile_coverage (const std::filesystem::path &root,
                                             const std::filesystem::path &include_dir,
                                             const std::string &include_prefix)
{
    const auto coverage_file =
      root / "tests/Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp";
    std::ifstream coverage_input (coverage_file);
    std::ostringstream coverage_buffer;
    coverage_buffer << coverage_input.rdbuf ();
    const auto coverage_text = coverage_buffer.str ();

    bool ok = true;
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root / include_dir)) {
        if (!entry.is_regular_file () || entry.path ().extension () != ".hpp") {
            continue;
        }
        const auto relative = std::filesystem::relative (entry.path (), root / include_dir);
        const auto include =
          std::string ("#include <") + include_prefix + relative.generic_string () + ">";
        if (coverage_text.find (include) == std::string::npos) {
            std::cerr << "public contract header lacks direct compile coverage: " << entry.path ()
                      << '\n';
            ok = false;
        }
    }
    return ok;
}

bool sample_application_code_uses_message_codec (const std::filesystem::path &root)
{
    bool ok = true;
    const auto samples_root = root / "samples";
    for (const auto &entry : std::filesystem::recursive_directory_iterator (samples_root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".cpp") {
            continue;
        }

        const auto relative =
          std::filesystem::relative (entry.path (), samples_root).generic_string ();
        const bool dto_contract_file = relative.find ("/Shared/Contracts/") != std::string::npos;
        /* Relocation adapters receive an opaque byte sequence by contract. They may use the
         * framework's typed JSON message representation to restore application state. This is
         * not a raw transport message or a per-message codec registration. */
        const bool relocation_adapter_file =
          relative.find ("relocation_adapter") != std::string::npos;

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            if (!dto_contract_file && line.find ("nlohmann::json::parse") != std::string::npos) {
                std::cerr << "sample application code must use message_t/serializer "
                             "instead of direct JSON parse: "
                          << entry.path () << ':' << line_no << '\n';
                ok = false;
            }
            if (!dto_contract_file && line.find ("json.at") != std::string::npos) {
                std::cerr << "sample application code must not extract JSON fields "
                             "outside DTO serializer hooks: "
                          << entry.path () << ':' << line_no << '\n';
                ok = false;
            }
            const bool inline_support_chat_relocation_restore =
              relative == "SupportChat/Server/Support/main.cpp"
              && line.find ("zlink::message_t::from (") != std::string::npos;
            if (!dto_contract_file && !relocation_adapter_file
                && !inline_support_chat_relocation_restore
                && line.find ("zlink::message_t::from (") != std::string::npos) {
                std::cerr << "sample application code must not construct raw zlink::message_t "
                             "payloads outside DTO serializer hooks: "
                          << entry.path () << ':' << line_no << '\n';
                ok = false;
            }
            if (line.find ("join_spot_payload") != std::string::npos
                || line.find ("write_packet_raw") != std::string::npos
                || line.find ("reply_packet_raw") != std::string::npos
                || line.find ("on_create_raw") != std::string::npos
                || line.find ("on_raw_packet") != std::string::npos) {
                std::cerr << "sample application code must not use explicit raw framework APIs: "
                          << entry.path () << ':' << line_no << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

/* 공통 정책 sample-e2e-configuration-policy.ko.md §2.2, §8: sample과 E2E 애플리케이션 코드가
 * 직접 읽을 수 있는 환경 변수는 0개다. 설정은 role별 설정 파일과 typed binding으로만 들어온다. */
bool sample_and_e2e_code_does_not_read_the_environment (const std::filesystem::path &root)
{
    bool ok = true;
    for (const auto *tree : {"samples", "e2e"}) {
        const auto tree_root = root / tree;
        if (!std::filesystem::exists (tree_root)) {
            continue;
        }
        for (const auto &entry : std::filesystem::recursive_directory_iterator (tree_root)) {
            if (!entry.is_regular_file ()) {
                continue;
            }
            const auto ext = entry.path ().extension ();
            if (ext != ".hpp" && ext != ".cpp" && ext != ".sh") {
                continue;
            }

            std::ifstream input (entry.path ());
            std::string line;
            std::size_t line_no = 0;
            while (std::getline (input, line)) {
                ++line_no;
                const bool reads_environment = line.find ("getenv (") != std::string::npos
                                               || line.find ("getenv(") != std::string::npos
                                               || line.find ("load_env (") != std::string::npos
                                               || line.find ("load_env(") != std::string::npos
                                               || line.find ("os.environ") != std::string::npos
                                               || (line.find ("${ZLINK_")
                                                     != std::string::npos
                                                   && line.find (
                                                        "${ZLINK_CPP_BUILD_DIR")
                                                        == std::string::npos)
                                               || (line.find ("$ZLINK_")
                                                     != std::string::npos
                                                   && line.find (
                                                        "$ZLINK_CPP_BUILD_DIR")
                                                        == std::string::npos)
                                               || ((line.find ("${BINGO_") != std::string::npos
                                                    || line.find ("${GAMEQUEST_")
                                                         != std::string::npos
                                                    || line.find ("${SHOPPINGMALL_")
                                                         != std::string::npos
                                                    || line.find ("${TICTACTOE_")
                                                         != std::string::npos)
                                                   && line.find (":-") != std::string::npos);
                if (reads_environment) {
                    std::cerr << "sample/e2e application code must take configuration from its "
                                 "config file, not the environment: "
                              << entry.path () << ":" << line_no << "\n";
                    ok = false;
                }
            }
        }
    }
    return ok;
}

bool runner_generated_config_files_are_private_and_cleaned (
  const std::filesystem::path &root)
{
    bool ok = true;
    for (const auto *tree : {"samples", "e2e"}) {
        const auto tree_root = root / tree;
        for (const auto &entry : std::filesystem::recursive_directory_iterator (tree_root)) {
            if (!entry.is_regular_file () || entry.path ().extension () != ".sh") {
                continue;
            }
            std::ifstream input (entry.path ());
            const std::string content ((std::istreambuf_iterator<char> (input)),
                                       std::istreambuf_iterator<char> ());
            if (content.find ("--config") == std::string::npos) {
                continue;
            }
            if (content.find ("os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)")
                == std::string::npos) {
                std::cerr << "runner-generated application config must use mode 0600: "
                          << entry.path () << '\n';
                ok = false;
            }
            if (content.find ("rm -rf") == std::string::npos) {
                std::cerr << "runner-generated application config must be cleaned on exit: "
                          << entry.path () << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

/* 공통 설정 정책 §2.1과 Config 11 §2: server 역할은 설정 값으로 한 실행 파일을
 * 전환하지 않고 역할별 진입점으로 구성한다. Client 시나리오도 역할 host와 분리한다. */
bool observability_ops_uses_role_specific_entrypoints (const std::filesystem::path &root)
{
    const auto scenario = root / "e2e/ObservabilityOps";
    bool ok = true;
    for (const auto &relative : {
           "Server/Session/main.cpp",
           "Server/Play/main.cpp",
           "Server/OrderWorkflow/main.cpp",
           "Client/main.cpp",
           "Client/Scenarios/obs_a1_scenario.hpp",
         }) {
        if (!std::filesystem::is_regular_file (scenario / relative)) {
            std::cerr << "ObservabilityOps requires a role-specific entrypoint: "
                      << (scenario / relative) << '\n';
            ok = false;
        }
    }

    std::ifstream cmake_input (root / "CMakeLists.txt");
    const std::string cmake ((std::istreambuf_iterator<char> (cmake_input)),
                             std::istreambuf_iterator<char> ());
    for (const auto *target : {
           "zlink_cpp_e2e_observability_ops_session",
           "zlink_cpp_e2e_observability_ops_play",
           "zlink_cpp_e2e_observability_ops_order_workflow",
           "zlink_cpp_e2e_observability_ops_client",
         }) {
        if (cmake.find (target) == std::string::npos) {
            std::cerr << "ObservabilityOps CMake target is missing: " << target << '\n';
            ok = false;
        }
    }

    std::ifstream runner_input (scenario / "run_e2e.sh");
    const std::string runner ((std::istreambuf_iterator<char> (runner_input)),
                              std::istreambuf_iterator<char> ());
    if (runner.find ("zlink_cpp_e2e_observability_ops_server") != std::string::npos
        || runner.find ('\"' + std::string ("role") + '\"') != std::string::npos) {
        std::cerr << "ObservabilityOps must not select server roles through one binary/config key\n";
        ok = false;
    }
    return ok;
}

bool spot_actor_transfer_uses_role_specific_entrypoints (const std::filesystem::path &root)
{
    const auto scenario = root / "e2e/SpotActorTransfer";
    bool ok = true;
    for (const auto &relative : {"Server/ActorNode/main.cpp", "Server/Session/main.cpp"}) {
        if (!std::filesystem::is_regular_file (scenario / relative)) {
            std::cerr << "SpotActorTransfer requires a role-specific entrypoint: "
                      << (scenario / relative) << '\n';
            ok = false;
        }
    }
    std::ifstream cmake_input (root / "CMakeLists.txt");
    const std::string cmake ((std::istreambuf_iterator<char> (cmake_input)),
                             std::istreambuf_iterator<char> ());
    for (const auto *target : {"zlink_cpp_e2e_spot_actor_transfer_actor_node",
                               "zlink_cpp_e2e_spot_actor_transfer_session"}) {
        if (cmake.find (target) == std::string::npos) {
            std::cerr << "SpotActorTransfer CMake target is missing: " << target << '\n';
            ok = false;
        }
    }
    std::ifstream runner_input (scenario / "run_e2e.sh");
    const std::string runner ((std::istreambuf_iterator<char> (runner_input)),
                              std::istreambuf_iterator<char> ());
    if (runner.find ('\"' + std::string ("role") + '\"') != std::string::npos
        || runner.find ("zlink_cpp_e2e_spot_actor_transfer_node") != std::string::npos) {
        std::cerr << "SpotActorTransfer must not select ActorNode/Session through config\n";
        ok = false;
    }
    return ok;
}

/* 공통 E2E README §2.5: 계약 시나리오 ID 하나는 실행과 단언을 소유하는 client
 * scenario 파일 하나에 대응한다. ID 상수만 둔 placeholder는 이 계약을 충족하지 않는다. */
bool affected_e2e_clients_own_each_scenario_in_a_file (const std::filesystem::path &root)
{
    struct config_scenarios_t
    {
        const char *directory;
        std::vector<const char *> ids;
    };
    const std::vector<config_scenarios_t> configs{
      {"ToActorMessaging",
       {"TA-A1", "TA-A2", "TA-A3", "TA-A4", "TA-B1", "TA-B2", "TA-B3"}},
      {"DiscoveryRegistryHa",
       {"SF-A1", "SF-A2", "SF-B1", "SF-B2", "SF-C1", "SF-C2", "SF-D1", "SF-D2",
        "SF-D3", "SF-E1"}},
      {"SpotActorTransfer",
       {"ST-A1", "ST-A2", "ST-A3", "ST-B1", "ST-B2", "ST-B3", "ST-B4", "ST-C1",
        "ST-C2", "ST-C3", "ST-D1", "ST-D2", "ST-E1", "ST-E2", "ST-F1", "ST-F2",
        "ST-F3", "ST-F4", "ST-F5", "ST-F6"}},
      {"ObservabilityOps",
       {"OBS-A1", "OBS-A2", "OBS-A3", "OBS-A4", "OBS-B1", "OBS-B2", "OBS-B3",
        "OBS-B4", "OBS-C1", "OBS-C2", "OBS-C3", "OBS-C4", "OBS-C5"}},
    };

    bool ok = true;
    for (const auto &config : configs) {
        const auto client_root = root / "e2e" / config.directory / "Client";
        std::ifstream main_input (client_root / "main.cpp");
        const std::string main_content ((std::istreambuf_iterator<char> (main_input)),
                                        std::istreambuf_iterator<char> ());
        for (const auto *id : config.ids) {
            std::string key;
            for (const char value : std::string (id)) {
                key.push_back (value == '-' ? '_' : static_cast<char> (std::tolower (value)));
            }
            const auto filename = key + "_scenario.hpp";
            const auto symbol = "run_" + key + "_scenario";
            const auto path = client_root / "Scenarios" / filename;
            if (!std::filesystem::is_regular_file (path)) {
                std::cerr << config.directory << " requires client scenario file for " << id
                          << ": " << path << '\n';
                ok = false;
                continue;
            }
            std::ifstream scenario_input (path);
            const std::string scenario ((std::istreambuf_iterator<char> (scenario_input)),
                                        std::istreambuf_iterator<char> ());
            if (scenario.find (id) == std::string::npos
                || scenario.find (symbol) == std::string::npos) {
                std::cerr << path << " must own " << id << " execution through " << symbol
                          << '\n';
                ok = false;
            }
            if (main_content.find (filename) == std::string::npos
                || main_content.find (symbol) == std::string::npos) {
                std::cerr << client_root / "main.cpp" << " must dispatch " << id << " through "
                          << symbol << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

bool sample_server_code_does_not_block_on_task_result (const std::filesystem::path &root)
{
    bool ok = true;
    const auto samples_root = root / "samples";
    for (const auto &entry : std::filesystem::recursive_directory_iterator (samples_root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".cpp") {
            continue;
        }

        const auto relative =
          std::filesystem::relative (entry.path (), samples_root).generic_string ();
        if (relative.find ("/Server/") == std::string::npos
            && relative.find ("/Shared/") == std::string::npos) {
            continue;
        }

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            if (line.find (".result (") != std::string::npos
                || line.find (".result(") != std::string::npos) {
                std::cerr << "sample server/shared code must use task_t await or "
                             "callback completion instead of blocking result(): "
                          << entry.path () << ':' << line_no << '\n';
                ok = false;
            }
        }
    }
    return ok;
}

bool client_sample_uses_e2e_connector (const std::filesystem::path &root,
                                       const std::filesystem::path &client_file)
{
    const auto path = root / client_file;
    bool ok = true;
    ok &= file_contains (path, "zlink/stream_connector.hpp");
    ok &= file_contains (path, "zlink/stream_e2e_client");
    ok &= file_contains (path, "stream_e2e_client::use");
    ok &= file_contains (path, "connector_factory_t::create");
    ok &= file_contains (path, "dispatch_mode_t::immediate");
    if (!ok) {
        std::cerr << "client sample does not wrap the stream connector with e2e client: " << path
                  << '\n';
    }
    return ok;
}

bool client_sample_does_not_include_server_implementation (const std::filesystem::path &root,
                                                           const std::filesystem::path &client_root)
{
    bool ok = true;
    const std::string forbidden[] = {"../Server/",
                                     "Server/",
                                     "../Shared/E2E/",
                                     "Shared/E2E/",
                                     "zlink/framework",
                                     "framework::",
                                     "app_t",
                                     "config_builder_t",
                                     "configuration_section_t",
                                     "zlink/Contracts/Sockets",
                                     "zlink/Contracts/Service",
                                     "zlink::context_t",
                                     "zlink::stream_socket_t",
                                     "run_client_e2e_stream_server",
                                     "use_embedded_server"};
    const auto path = root / client_root;
    for (const auto &entry : std::filesystem::recursive_directory_iterator (path)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".cpp") {
            continue;
        }

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            for (const auto &needle : forbidden) {
                if (line.find (needle) != std::string::npos) {
                    std::cerr << "client sample references server/test harness or "
                                 "low-level zlink implementation: "
                              << entry.path () << ':' << line_no << " contains " << needle << '\n';
                    ok = false;
                }
            }
        }
    }
    return ok;
}

bool sample_client_targets_do_not_link_framework (const std::filesystem::path &root)
{
    const auto path = root / "CMakeLists.txt";
    std::ifstream input (path);
    std::string text ((std::istreambuf_iterator<char> (input)), std::istreambuf_iterator<char> ());

    bool ok = true;
    const std::string targets[] = {"sample_cpp_framework_bingo_client",
                                   "sample_cpp_framework_tictactoe_client"};
    for (const auto &target : targets) {
        std::istringstream lines (text);
        std::string line;
        std::string block;
        bool found = false;
        while (std::getline (lines, line)) {
            if (!found && line.find ("target_link_libraries(" + target) == std::string::npos) {
                continue;
            }
            found = true;
            block += line;
            block += '\n';
            if (line.find (')') != std::string::npos) {
                break;
            }
        }
        if (!found) {
            std::cerr << "missing client target link declaration: " << target << '\n';
            ok = false;
            continue;
        }
        auto link_block = block;
        const std::string codec_target = "zlink::framework_codec_";
        for (auto pos = link_block.find (codec_target); pos != std::string::npos;
             pos = link_block.find (codec_target, pos)) {
            link_block.erase (pos, codec_target.size ());
        }
        if (link_block.find ("zlink::framework") != std::string::npos) {
            std::cerr << "client target must not link zlink::framework: " << target << '\n';
            ok = false;
        }
    }
    return ok;
}

bool file_does_not_contain (const std::filesystem::path &path,
                            const std::string &needle,
                            const std::string &message)
{
    if (!file_contains_quiet (path, needle)) {
        return true;
    }
    std::cerr << message << ": " << path << '\n';
    return false;
}

bool http_client_public_surface_declares_general_client_features (const std::filesystem::path &root)
{
    bool ok = true;
    const auto contract_header =
      root / "http-client/include/zlink/http_client/contracts/client.hpp";

    std::ifstream input (contract_header);
    std::ostringstream buffer;
    buffer << input.rdbuf ();
    const auto text = buffer.str ();

    const std::string required[] = {"follow_redirects", "retry (", "cookies ()", "proxy (",
                                    "compression ()",   "query (", "form (",     "multipart (",
                                    "multipart_file (", "patch (", "head (",     "options (",
                                    "download ("};
    for (const auto &needle : required) {
        if (text.find (needle) == std::string::npos) {
            std::cerr << "HTTP client public surface lacks general client feature: "
                      << contract_header << " misses " << needle << '\n';
            ok = false;
        }
    }
    return ok;
}

bool stream_connector_public_surface_hides_runtime_internals (const std::filesystem::path &root)
{
    bool ok = true;
    const auto include_root = root / "connector/core/include";
    const std::string forbidden[] = {
      "connector_state_t",      "connector_runtime_t",    "pending_request_t", "pending_requests",
      "transport_connection_t", "stream_connection_t",    "frame_codec_t",     "header_codec_t",
      "metadata_codec_t",       "lz4_compression_codec_t"};

    for (const auto &entry : std::filesystem::recursive_directory_iterator (include_root)) {
        if (!entry.is_regular_file ()) {
            continue;
        }
        const auto ext = entry.path ().extension ();
        if (ext != ".hpp" && ext != ".h") {
            continue;
        }

        std::ifstream input (entry.path ());
        std::string line;
        std::size_t line_no = 0;
        while (std::getline (input, line)) {
            ++line_no;
            for (const auto &needle : forbidden) {
                if (line.find (needle) != std::string::npos) {
                    std::cerr << "Stream Connector public surface exposes runtime "
                                 "implementation type: "
                              << entry.path () << ':' << line_no << " contains " << needle << '\n';
                    ok = false;
                }
            }
        }
    }
    return ok;
}

bool http_hosting_public_surface_excludes_non_goal_features (const std::filesystem::path &root)
{
    bool ok = true;
    const std::filesystem::path include_roots[] = {
      root / "framework/include/zlink/framework/contracts/http"};
    const std::string forbidden[] = {
      "mvc",        "MVC",         "controller",        "Controller",
      "razor",      "Razor",       "websocket",         "WebSocket",
      "web_socket", "view_engine", "template_renderer", "render_template"};

    for (const auto &include_root : include_roots) {
        if (std::filesystem::is_regular_file (include_root)) {
            std::ifstream input (include_root);
            std::string line;
            std::size_t line_no = 0;
            while (std::getline (input, line)) {
                ++line_no;
                for (const auto &needle : forbidden) {
                    if (line.find (needle) != std::string::npos) {
                        std::cerr << "HTTP hosting public surface exposes non-goal "
                                     "feature: "
                                  << include_root << ':' << line_no << " contains " << needle
                                  << '\n';
                        ok = false;
                    }
                }
            }
            continue;
        }

        for (const auto &entry : std::filesystem::recursive_directory_iterator (include_root)) {
            if (!entry.is_regular_file ()) {
                continue;
            }
            const auto ext = entry.path ().extension ();
            if (ext != ".hpp" && ext != ".h") {
                continue;
            }

            std::ifstream input (entry.path ());
            std::string line;
            std::size_t line_no = 0;
            while (std::getline (input, line)) {
                ++line_no;
                for (const auto &needle : forbidden) {
                    if (line.find (needle) != std::string::npos) {
                        std::cerr << "HTTP hosting public surface exposes non-goal "
                                     "feature: "
                                  << entry.path () << ':' << line_no << " contains " << needle
                                  << '\n';
                        ok = false;
                    }
                }
            }
        }
    }
    return ok;
}

bool location_store_public_surface_hides_domain_repositories (
  const std::filesystem::path &root)
{
    bool ok = true;
    const std::filesystem::path include_roots[] = {
      root / "framework/include",
      root / "extensions/framework-locations-redis/include"};
    const std::string forbidden[] = {
      "maintenance_stores.hpp",
      "location_repository_t",
      "relocation_repository_t",
      "authority_key_t",
      "aggregate_prepare_request_t",
      "location_owner_token_t",
      "redis_location_repository_t",
      "redis_relocation_repository_t"};

    for (const auto &include_root : include_roots) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator (include_root)) {
            if (!entry.is_regular_file ()) {
                continue;
            }
            const auto ext = entry.path ().extension ();
            if (ext != ".hpp" && ext != ".h") {
                continue;
            }

            std::ifstream input (entry.path ());
            std::string line;
            std::size_t line_no = 0;
            while (std::getline (input, line)) {
                ++line_no;
                for (const auto &needle : forbidden) {
                    if (line.find (needle) != std::string::npos) {
                        std::cerr << "Store public surface exposes private domain repository: "
                                  << entry.path () << ':' << line_no << " contains " << needle
                                  << '\n';
                        ok = false;
                    }
                }
            }
        }
    }
    return ok;
}

} // namespace

int main ()
{
    const std::filesystem::path root{ZLINK_FRAMEWORK_CPP_SOURCE_DIR};

    bool ok = true;
    ok &= require_exists (root / "framework/include/zlink/framework/contracts");
    ok &=
      require_exists (root / "framework/include/zlink/framework/contracts/detail/message_name.hpp");
    const auto location_values_header =
      root / "framework/include/zlink/framework/contracts/locations/values.hpp";
    ok &= file_does_not_contain (
      location_values_header, std::string ("to_") + "canonical_string",
      "location canonical strings are an internal codec, not public API");
    ok &= file_does_not_contain (
      location_values_header, std::string ("try_parse_") + "location",
      "location canonical string parsing is an internal codec, not public API");
    ok &= file_does_not_contain (
      root / "framework/include/zlink/framework/contracts/locations/resolvers.hpp",
      std::string ("route_") + "location_resolver_t",
      "route resolver is internal-only and must not return to the public location surface");
    const auto redis_store_header =
      root / "extensions/framework-locations-redis/include/zlink/locations/redis.hpp";
    ok &= file_does_not_contain (
      redis_store_header, "options () const",
      "Redis Store concrete classes must not add a public options query");
    ok &= file_does_not_contain (
      redis_store_header, "options = {}",
      "Redis Store constructors require explicit provider options");
    ok &= file_does_not_contain (
      redis_store_header, "127.0.0.1",
      "Redis provider options must not publish a default endpoint");
    const std::string removed_framework_facades[] = {
      "actors.hpp", "app.hpp",           "assembly.hpp",  "call.hpp",       "channels.hpp",
      "config.hpp", "error.hpp",         "execution.hpp", "handlers.hpp",   "health.hpp",
      "http.hpp",   "logging.hpp",       "module.hpp",    "monitoring.hpp", "registry.hpp",
      "result.hpp", "serialization.hpp", "services.hpp",  "spots.hpp",      "streams.hpp",
      "task.hpp",   "timers.hpp",        "transport.hpp"};
    for (const auto &header : removed_framework_facades) {
        ok &= require_absent (root / "framework/include/zlink/framework" / header,
                              "one-line facade wrappers are dead compatibility surface; use "
                              "zlink/framework.hpp or contracts/*");
    }
    ok &= require_exists (root / "framework/src/runtime");
    ok &= require_exists (root / "framework/src/runtime/backend/native_route_backend.cpp");
    ok &= require_exists (root / "framework/src/runtime/backend/native_route_backend.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_packet_dispatcher.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_packet_dispatcher.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_pending_requests.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_pending_requests.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_reply_writer.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_reply_writer.hpp");
    ok &= require_exists (root / "framework/src/runtime/execution");
    ok &= require_exists (root / "framework/src/runtime/execution/serial_execution_queue.cpp");
    ok &= require_exists (root / "framework/src/runtime/execution/serial_execution_queue.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging");
    ok &= require_exists (root / "framework/src/runtime/messaging/client_call_codec.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/client_call_codec.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/envelope_codec.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/envelope_codec.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/pending_operation.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/pending_operation_state.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/pending_submit.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/pending_submit.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/request_failure_mapper.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/request_failure_mapper.hpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/submit_queue.cpp");
    ok &= require_exists (root / "framework/src/runtime/messaging/submit_queue.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_runtime_bundle.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_runtime_bundle.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_bundle_factory.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_bundle_factory.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_runtime_manager.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/channel_runtime_manager.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_connection_set.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_connection_set.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_channel_registration.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_channel_registration.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_channel_runtime.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_channel_runtime.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_handler_registry.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_handler_registry.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_handler_invoker.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_handler_invoker.hpp");
    ok &=
      require_exists (root / "framework/src/runtime/channels/route_internal_packet_dispatcher.cpp");
    ok &=
      require_exists (root / "framework/src/runtime/channels/route_internal_packet_dispatcher.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_packet.hpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_packet_dispatcher.cpp");
    ok &= require_exists (root / "framework/src/runtime/channels/route_packet_dispatcher.hpp");
    ok &= require_exists (root / "framework/src/runtime/configuration/builders");
    ok &= require_exists (
      root / "framework/src/runtime/configuration/builders/configuration_builder.cpp");
    ok &= require_exists (root / "connector/core/include/zlink/stream_connector/contracts");
    ok &= require_exists (root / "connector/core/include/zlink/stream_connector/contracts/calls");
    ok &= require_exists (
      root
      / "connector/core/include/zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp");
    ok &= require_exists (root
                          / "connector/core/include/zlink/stream_connector/contracts/"
                            "zlink_stream_connector_options.hpp");
    ok &= require_exists (
      root / "connector/core/include/zlink/stream_connector/contracts/zlink_stream_models.hpp");
    ok &= require_exists (root / "connector/core/src/runtime");
    ok &= require_exists (root / "connector/core/src/runtime/calls");
    ok &= require_exists (root / "connector/core/src/runtime/protocol");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/compression");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/framing");
    ok &= require_exists (root / "connector/core/src/runtime/transport");
    ok &= require_exists (root / "connector/core/src/runtime/calls/zlink_stream_calls.cpp");
    ok &= require_exists (
      root / "connector/core/src/runtime/protocol/compression/lz4_compression_codec.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/framing/frame_codec.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/framing.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/header_codec.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/metadata_codec.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/protocol/packet_name_resolver.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/transport/stream_connection.cpp");
    ok &=
      require_exists (root / "connector/core/src/runtime/transport/stream_transport_factory.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/transport/websocket_connection.cpp");
    ok &= require_exists (root / "connector/core/src/runtime/backend/contracts");
    ok &= require_exists (root / "http-client/include/zlink/http_client.hpp");
    ok &= require_exists (root / "http-client/include/zlink/http_client/contracts/client.hpp");
    ok &= require_exists (root / "http-client/src/runtime");
    ok &= require_exists (root / "http-client/src/runtime/http_client_runtime.hpp");
    ok &= require_exists (root / "http-client/src/runtime/http_client_runtime.cpp");
    ok &= require_exists (root / "tests/Zlink.Framework.UnitTests");
    ok &= require_exists (root / "tests/Zlink.Framework.ContractTests");
    ok &= require_absent (root / "tests/Zlink.Framework.E2ETests",
                          "sample e2e must not rely on separate fake process runners");
    ok &= require_exists (root / "tests/Systems.Zlink.Stream.Connector.Tests");
    ok &= require_exists (root / "tests/Zlink.Unreal.Stream.Connector.Tests");
    ok &= require_exists (
      root / "tests/Zlink.Framework.ContractTests/test_cpp_framework_contract_headers.cpp");
    ok &= require_exists (
      root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_handler_registry.cpp");
    ok &= require_exists (
      root / "tests/Systems.Zlink.Stream.Connector.Tests/test_cpp_stream_connector.cpp");
    ok &= require_exists (
      root / "tests/Zlink.Unreal.Stream.Connector.Tests/test_unreal_stream_connector.cpp");
    const auto old_unreal_connector_dir = std::string ("unreal") + "-connector";
    ok &= require_absent (root / old_unreal_connector_dir,
                          "Unreal connector belongs under connector/engines/unreal");
    ok &= require_exists (root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public");
    ok &= require_exists (root / "connector/engines/unreal/Source/ZLinkStreamConnector/Private");
    ok &= require_exists (root
                          / "connector/engines/unreal/Source/ZLinkStreamConnectorTests/Private/"
                            "ZLinkStreamConnectorAutomationTests.cpp");
    ok &= require_exists (root
                          / "connector/engines/unreal/Source/ZLinkStreamConnectorTests/"
                            "ZLinkStreamConnectorTests.Build.cs");
    ok &= require_exists (root / "samples/Bingo/Server/Configuration/sample_names.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Configuration/sample_topology.hpp");
    ok &= require_exists (root / "samples/Bingo/Client/Configuration/sample_topology.hpp");
    ok &= require_exists (root / "samples/Bingo/run_sample.sh");
    ok &= require_exists (root / "samples/Bingo/run_sample.ps1");
    ok &= require_exists (root / "samples/Bingo/Shared/Contracts/messages.hpp");
    /* Bingo의 payload codec은 Protobuf다. wire 스키마는 `.proto`가 정본이고, 도메인 타입은 그
     * 스키마가 만든 message로 옮겨 실린다. */
    ok &= require_exists (root / "samples/Bingo/Shared/Contracts/bingo_messages.proto");
    ok &= require_exists (root / "samples/Bingo/Shared/Contracts/protobuf_conversions.hpp");
    ok &= require_absent (root / "samples/Bingo/Shared" / "Configuration",
                          "Bingo Shared must contain message contracts only");
    ok &= require_absent (root / "samples/Bingo/Shared" / "sample.hpp",
                          "Bingo sample umbrella belongs to server code, not Shared");
    ok &= require_absent (root / "samples/Bingo/Shared" / "host_support.hpp",
                          "Bingo host support belongs to server code, not Shared");
    ok &= require_absent (root / "samples/Bingo/Server" / "sample.hpp",
                          "Bingo server code should include role-local headers directly");
    ok &= require_absent (root / "samples/Bingo/Server/E2E",
                          "Bingo sample e2e must run the public sample server roles");
    ok &= require_absent (root / "samples/Bingo/Shared/E2E",
                          "Bingo sample tree must not contain test-only stream harnesses");
    ok &=
      require_exists (root / "samples/Bingo/Server/Api/Handlers/authenticate_player_handler.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Api/api_server_host_factory.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Api/api_server_framework.hpp");
    ok &= file_contains (root / "samples/Bingo/Server/Api/api_server_host_factory.hpp",
                         "app.logging ().use_console ().set_level (\"info\")");
    ok &= file_does_not_contain (
      root / "samples/Bingo/Server/Api/api_server_framework.hpp", "app.logging ().use_console ()",
      "reusable framework setup must not force a host-level logging sink");
    ok &= require_exists (root / "samples/Bingo/Server/Api/Handlers/match_bingo_handler.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Play/Domain/Bingo/bingo_card.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Play/Domain/Bingo/bingo_game.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Play/Domain/Bingo/bingo_room_game.hpp");
    ok &= require_exists (
      root
      / "samples/Bingo/Server/Matchmaking/Application/"
        "bingo_match_reservation_store.hpp");
    ok &= require_exists (
      root / "samples/Bingo/Server/Matchmaking/matchmaking_server_host_factory.hpp");
    ok &= require_exists (
      root
      / "samples/Bingo/Server/Matchmaking/Infrastructure/Redis/"
        "redis_bingo_match_reservation_store.hpp");
    ok &= require_exists (
      root / "samples/Bingo/Server/Play/Infrastructure/ZLink/Actors/player_actor.hpp");
    ok &= require_exists (
      root / "samples/Bingo/Server/Play/Infrastructure/ZLink/Actors/player_actor_factory.hpp");
    ok &= require_absent (root
                            / "samples/Bingo/Server/Play/Infrastructure/ZLink/Notifications/"
                              "bingo_notification_publisher.hpp",
                          "BingoRoom publishes reward events and PlayerActor owns session push");
    ok &= require_exists (
      root
      / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo_room_spot.hpp");
    ok &=
      require_absent (root
                        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/Handlers/"
                          "bingo_room_timer_handler.hpp",
                      "BingoRoom owns draw progression instead of delegating to a shallow handler");
    ok &= require_exists (
      root / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp");
    ok &= require_absent (root
                            / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/Handlers/"
                              "match_bingo_actor_handler.hpp",
                          "SPOT actor packets must be registered as spot member functions");
    ok &= require_absent (
      root
      / "samples/Bingo/Server/Play/Infrastructure/ZLink/Handlers/"
        "allocate_bingo_room_handler.hpp",
      "Matchmaker Instance Spot owns Redis reservation and Play does not allocate rooms");
    ok &= require_absent (
      root
        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/"
          "ensure_player_actor_handler.hpp",
      "Bingo Entry Spot owns Actor creation admission in on_create_actor");
    ok &= file_contains (
      root
        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp",
      "task_t<actor_create_response_t>");
    ok &= file_contains (
      root
        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp",
      "on_create_actor (");
    ok &= file_contains (
      root
        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp",
      "player_actor_t &actor");
    ok &= file_contains (
      root
        / "samples/Bingo/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/bingo_entry_spot.hpp",
      "create_request.decode<ensure_player_actor_req_t> ()");
    ok &= require_exists (root / "samples/Bingo/Server/Play/play_server_host_factory.hpp");
    ok &= require_absent (root / "samples/Bingo/Server/Registry",
                          "Bingo uses Redis location store instead of a registry role");
    ok &= require_exists (root / "samples/Bingo/Server/Session/main.cpp");
    ok &= require_exists (root / "samples/Bingo/Server/Session/session_server_host_factory.hpp");
    ok &= require_exists (root / "samples/Bingo/Server/Session/Sessions/bingo_session.hpp");
    ok &= require_exists (
      root / "samples/Bingo/Server/Session/Sessions/Handlers/authenticate_session_handler.hpp");
    ok &= require_exists (root / "samples/Bingo/Client/bingo_client_options.hpp");
    ok &= require_exists (root / "samples/Bingo/Client/bingo_client_scenario.hpp");
    ok &= require_absent (root / "samples/Bingo/Client/bingo_notification_inbox.hpp",
                          "Bingo client scenario waits on connector messages directly");
    ok &= require_absent (root / "samples/Bingo/Client/bingo_player_client.hpp",
                          "Bingo client scenario must not hide connector calls in a wrapper");
    ok &= require_absent (root / "samples/Bingo/Client/bingo_client_app.hpp",
                          "Bingo client scenario is kept in bingo_client_scenario.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Server/Configuration/sample_names.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Server/Configuration/sample_topology.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Client/Configuration/sample_names.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Client/Configuration/sample_topology.hpp");
    ok &= require_exists (root / "samples/TicTacToe/run_sample.sh");
    ok &= require_exists (root / "samples/TicTacToe/run_sample.ps1");
    ok &= require_exists (root / "samples/run_samples.sh");
    ok &= require_exists (root / "samples/run_samples.ps1");
    const auto sample_shell_aggregate = root / "samples/run_samples.sh";
    const auto sample_powershell_aggregate = root / "samples/run_samples.ps1";
    for (const auto &runner : {"TicTacToe", "Bingo", "DeliveryDispatch", "SupportChat",
                               "GameQuest", "ShoppingMall"}) {
        ok &= file_contains (sample_shell_aggregate,
                             std::string (runner) + "/run_sample.sh");
        ok &= file_contains (sample_powershell_aggregate,
                             std::string (runner) + "/run_sample.sh");
    }
    ok &= file_does_not_contain (
      sample_shell_aggregate, "MAX_ATTEMPTS",
      "the C++ sample aggregate must not retry a bind failure");
    ok &= file_does_not_contain (
      sample_shell_aggregate, "BIND_RETRY_PATTERN",
      "the C++ sample aggregate must not classify bind failure as retryable");
    ok &= require_exists (root / "samples/TicTacToe/Shared/Contracts/messages.hpp");
    ok &= require_absent (root / "samples/TicTacToe/Shared" / "Configuration",
                          "TicTacToe Shared must contain message contracts only");
    ok &= require_absent (root / "samples/TicTacToe/Shared" / "sample.hpp",
                          "TicTacToe sample umbrella belongs to server code, not Shared");
    ok &= require_absent (root / "samples/TicTacToe/Shared" / "host_support.hpp",
                          "TicTacToe host support belongs to server code, not Shared");
    ok &= require_absent (root / "samples/TicTacToe/Server" / "sample.hpp",
                          "TicTacToe server code should include role-local headers directly");
    ok &= require_absent (root / "samples/TicTacToe/Server" / "sample_log.hpp",
                          "TicTacToe server logging should not need a separate helper header");
    ok &= require_absent (root / "samples/TicTacToe/Server/E2E",
                          "TicTacToe sample e2e must run the public sample server roles");
    ok &= require_absent (root / "samples/TicTacToe/Shared/E2E",
                          "TicTacToe sample tree must not contain test-only stream harnesses");
    ok &= require_absent (root / "samples/Shared/stream_frame_server.hpp",
                          "test-only stream frame server code must not live under samples");
    ok &= require_exists (
      root / "samples/TicTacToe/Server/Api/Handlers/authenticate_player_handler.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Server/Api/api_server_host_factory.hpp");
    ok &=
      require_exists (root / "samples/TicTacToe/Server/Api/Handlers/create_game_http_handler.hpp");
    ok &= require_absent (root / "samples/TicTacToe/Server/Api/api_server_framework.hpp",
                          "TicTacToe API framework setup belongs in api_server_host_factory.hpp");
    ok &= require_absent (
      root
        / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/Handlers/join_game_handler.hpp",
      "SPOT actor packets must be registered as spot member functions");
    ok &=
      require_exists (root / "samples/TicTacToe/Server/Play/Domain/TicTacToe/tictactoe_match.hpp");
    ok &= require_absent (
      root / "samples/TicTacToe/Server/Play/Application/GameCreation/tictactoe_game_creator.hpp",
      "TicTacToe room creation belongs to the public Spot manager used by the API role");
    ok &= require_exists (
      root / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Actors/player_actor.hpp");
    ok &= require_exists (
      root
      / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/"
        "Notifications/game_notification_publisher.hpp");
    ok &= require_absent (root
                            / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/"
                              "TicTacToeGameSpot/tictactoe_game_contract_mapper.hpp",
                          "TicTacToe must not keep an identity contract mapper");
    ok &= require_absent (root
                            / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/"
                              "TicTacToeGameSpot/tictactoe_game_models.hpp",
                          "TicTacToe must not keep an unused snapshot wrapper");
    ok &= require_absent (
      root
        / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/Handlers/"
          "tictactoe_game_spot_created_handler.hpp",
      "TicTacToe must not keep an unregistered spot-created handler");
    ok &= require_exists (root
                          / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/"
                            "TicTacToeGameSpot/tictactoe_game_spot.hpp");
    ok &= require_absent (root
                            / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/Handlers/"
                              "tictactoe_game_join_handler.hpp",
                          "SPOT actor joins must be registered as spot member functions");
    ok &= require_absent (root
                            / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/Handlers/"
                              "place_mark_handler.hpp",
                          "SPOT actor packets must be registered as spot member functions");
    /* EnsurePlayerActor는 Bingo 전용 계약이다(공통 sample spec §11): TicTacToe는 인증에서
     * 받은 PlayerInfo를 그대로 actor 생성 payload로 쓴다. */
    ok &= require_absent (root
                            / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Handlers/"
                              "ensure_player_actor_handler.hpp",
                          "TicTacToe must not carry the Bingo-only EnsurePlayerActor contract");
    ok &= require_exists (
      root / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp");
    ok &= require_exists (root
                          / "samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/Handlers/"
                            "authenticate_play_session_handler.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Server/Play/play_server_host_factory.hpp");
    ok &= require_absent (root / "samples/TicTacToe/Server/Registry",
                          "TicTacToe uses manual endpoints and has no Registry role");
    ok &= require_absent (root / "samples/TicTacToe/Server/Session",
                          "TicTacToe Play owns the stream session");
    ok &= require_exists (root / "samples/TicTacToe/Client/tictactoe_client_options.hpp");
    ok &= require_exists (root / "samples/TicTacToe/Client/tictactoe_client_scenario.hpp");
    ok &= require_absent (root / "samples/TicTacToe/Client/session_actor_notification_inbox.hpp",
                          "TicTacToe client scenario waits on connector messages directly");
    ok &= require_absent (root / "samples/TicTacToe/Client/tictactoe_player_client.hpp",
                          "TicTacToe client scenario must not hide connector calls in a wrapper");
    ok &= require_absent (root / "samples/TicTacToe/Client/tictactoe_client.hpp",
                          "TicTacToe client scenario is kept in tictactoe_client_scenario.hpp");

    ok &= client_sample_uses_e2e_connector (root, "samples/Bingo/Client/main.cpp");
    ok &= client_sample_uses_e2e_connector (
      root, "samples/TicTacToe/Client/tictactoe_client_scenario.hpp");
    ok &= client_sample_does_not_include_server_implementation (root, "samples/Bingo/Client");
    ok &= client_sample_does_not_include_server_implementation (root, "samples/TicTacToe/Client");
    ok &= sample_client_targets_do_not_link_framework (root);
    ok &= file_does_not_contain (root / "connector/core/src/runtime/connector_runtime.hpp",
                                 "socket_fd", "C++ connector runtime must not use raw fd state");
    ok &= file_does_not_contain (root / "connector/core/src/runtime/connector_runtime.hpp", "recv(",
                                 "C++ connector runtime must not expose raw recv state");
    ok &= file_contains (root / "framework/src/runtime/handlers/handler_registry.cpp",
                         "runtime::handler_coroutine_executor ().submit");
    ok &= file_contains (root / "framework/src/runtime/handlers/handler_registry.cpp",
                         "co_await runtime::await_task_result");
    ok &= file_contains (root / "framework/src/runtime/http/http_request_pipeline.cpp",
                         "handler_coroutine_executor ().submit");
    ok &= file_contains (root / "framework/src/runtime/http/http_request_pipeline.cpp",
                         "co_await await_task_result");
    ok &= file_contains (root / "framework/src/runtime/spots/spot_runtime.cpp",
                         "runtime::serial_execution_queue_t");
    ok &= file_contains (root / "framework/src/runtime/spots/spot_runtime.cpp",
                         "try_post_serial_async");
    ok &= file_contains (root / "framework/src/runtime/streams/stream_runtime.cpp",
                         "detail::observe_task_completion");
    ok &= file_does_not_contain (
      root / "framework/src/runtime/channels/route_handler_invoker.cpp", ".result (",
      "route handler dispatch must await task_t instead of blocking with result()");
    ok &= file_does_not_contain (
      root / "framework/src/runtime/channels/route_handler_invoker.cpp", ".result(",
      "route handler dispatch must await task_t instead of blocking with result()");
    ok &= file_does_not_contain (
      root / "connector/core/include/zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp",
      ".result", "connector callback submit must observe task completion instead of blocking");
    ok &= file_does_not_contain (
      root / "connector/core/include/zlink/stream_connector/codecs/auto_codec.hpp", ".result",
      "connector auto codec callback submit must observe task completion instead of blocking");
    ok &= file_does_not_contain (
      root / "connector/core/include/zlink/stream_connector/codecs/auto_codec.hpp",
      "on_completed ([&", "connector auto codec must not depend on immediate callback completion");
    ok &= file_does_not_contain (
      root / "connector/core/include/zlink/stream_connector/codecs/auto_codec.hpp",
      "std::optional<result_t", "connector auto codec must let task_t own result storage details");
    ok &= file_does_not_contain (root / "connector/core/include/zlink/stream_connector.hpp",
                                 "stream_e2e_client",
                                 "core connector umbrella must not include the e2e client surface");
    ok &= file_does_not_contain (root / "connector/core/include/zlink/stream_connector.hpp",
                                 "<coroutine>",
                                 "core connector umbrella must not include coroutine support");
    ok &= file_does_not_contain (root / "connector/core/include/zlink/stream_connector.hpp",
                                 "task_t", "core connector umbrella must not expose e2e task_t");
    ok &= file_contains (root / "CMakeLists.txt", "add_library(zlink_stream_e2e_client INTERFACE)");
    ok &= file_contains (root / "CMakeLists.txt",
                         "add_library(zlink::stream_e2e_client ALIAS zlink_stream_e2e_client)");
    ok &= file_contains (root / "CMakeLists.txt", "option(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT");
    ok &= file_contains (root / "CMakeLists.txt", "option(ZLINK_STREAM_CONNECTOR_BUILD_UNREAL");
    ok &= file_contains (root / "CMakeLists.txt", "option(ZLINK_STREAM_CONNECTOR_BUILD_GODOT");
    ok &= file_contains (root / "CMakeLists.txt", "option(ZLINK_STREAM_CONNECTOR_BUILD_AXMOL");
    ok &= file_contains (root / "CMakeLists.txt", "if(ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT)");
    ok &= file_contains (root / "CMakeLists.txt",
                         "add_library(zlink_stream_connector_throwing INTERFACE)");
    ok &= require_exists (
      root / "connector/throwing-adapter/include/zlink/stream_connector_throwing.hpp");
    ok &= file_does_not_contain (
      root / "connector/core/include/zlink/stream_connector.hpp", "stream_connector_throwing",
      "core connector umbrella must not include the throwing adapter surface");
    ok &= file_does_not_contain (
      root / "connector/core/src/runtime/connector_runtime.cpp", ".result",
      "connector send callback submit must observe task completion instead of blocking");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public/ZLinkStreamConnector.h",
      "zlink/stream_connector",
      "Unreal public API must not include the general C++ connector surface");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public/ZLinkStreamConnector.h",
      "task_t", "Unreal public API must not expose connector coroutine task_t");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public/ZLinkStreamConnector.h",
      "<coroutine>", "Unreal public API must not include coroutine support");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public/ZLinkStreamConnector.h",
      "co_await", "Unreal public API must not expose coroutine await syntax");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public/ZLinkStreamConnector.h",
      "submit", "Unreal public API must use delegates instead of connector submit calls");
    ok &= file_does_not_contain (
      root / "CMakeLists.txt",
      "target_link_libraries(zlink_unreal_stream_connector PUBLIC zlink::stream_connector)",
      "Unreal connector target must not expose the general C++ connector publicly");
    ok &= file_contains (root / "CMakeLists.txt",
                         "target_link_libraries(zlink_unreal_stream_connector PRIVATE\n"
                         "    zlink::stream_connector\n"
                         "    zlink::stream_connector_codecs)");
    ok &= file_does_not_contain (
      root / "CMakeLists.txt",
      "target_include_directories(zlink_unreal_stream_connector PRIVATE\n  "
      "${ZLINK_FRAMEWORK_CPP_DIR}/connector/core/src)",
      "Unreal connector target must not include general connector runtime internals");
    ok &= file_contains (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "#include <zlink/stream_connector.hpp>");
    ok &= file_does_not_contain (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "enum class frame_", "Unreal adapter must not define its own STREAM frame enums");
    ok &= file_does_not_contain (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "encode_frame", "Unreal adapter must delegate frame encoding to the core connector");
    ok &= file_does_not_contain (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "decode_frame", "Unreal adapter must delegate frame decoding to the core connector");
    ok &= file_does_not_contain (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "connector/core/src/runtime",
      "Unreal adapter must not include core connector runtime internals");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/ZLinkStreamConnector.Build.cs",
      "\"Sockets\"",
      "Unreal adapter must not use Unreal socket APIs when wrapping the non-Unreal core");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/ZLinkStreamConnector.Build.cs",
      "\"Networking\"",
      "Unreal adapter must not use Unreal networking APIs when wrapping the non-Unreal core");
    ok &= file_does_not_contain (
      root / "CMakeLists.txt", "ZLINK_STREAM_CONNECTOR_WITH_JSON",
      "Stream connector JSON helper is always included and must not expose a fake option");
    ok &= file_does_not_contain (
      root / "CMakePresets.json", "ZLINK_STREAM_CONNECTOR_WITH_JSON",
      "CMake presets must not set the removed Stream Connector JSON helper option");
    ok &= file_does_not_contain (
      root / "CMakeLists.txt", "ZLINK_STREAM_CONNECTOR_WITH_MESSAGEPACK",
      "MessagePack is provided by framework codec extension packages, not connector options");
    ok &= file_does_not_contain (
      root / "CMakeLists.txt", "ZLINK_STREAM_CONNECTOR_WITH_PROTOBUF",
      "Protobuf is provided by framework codec extension packages, not connector options");
    ok &=
      file_does_not_contain (root / "CMakeLists.txt", "zlink::cpp_codec_",
                             "bindings codec targets must not be restored by C++ framework build");
    ok &= file_does_not_contain (
      root / "cmake/zlink_framework_cppConfig.cmake.in",
      "@ZLINK_STREAM_CONNECTOR_EXPORT_MESSAGEPACK_DEPENDENCY@",
      "Framework package config must not restore connector MessagePack dependency");
    ok &= file_does_not_contain (
      root / "cmake/zlink_framework_cppConfig.cmake.in",
      "@ZLINK_STREAM_CONNECTOR_EXPORT_PROTOBUF_DEPENDENCY@",
      "Framework package config must not restore connector Protobuf dependency");
    ok &= file_contains (root / "cmake/zlink_stream_connector_cppConfig.cmake.in",
                         "@ZLINK_STREAM_CONNECTOR_EXPORT_OPENSSL_DEPENDENCY@");
    ok &= file_does_not_contain (
      root / "cmake/zlink_stream_connector_cppConfig.cmake.in", "MESSAGEPACK",
      "Stream connector package config must not export MessagePack codec dependencies");
    ok &= file_does_not_contain (
      root / "cmake/zlink_stream_connector_cppConfig.cmake.in", "PROTOBUF",
      "Stream connector package config must not export Protobuf codec dependencies");
    ok &=
      file_contains (root / "../../../bindings/cpp/CMakeLists.txt", "install(TARGETS zlink_cpp");
    ok &= file_contains (root / "../../../bindings/cpp/CMakeLists.txt", "EXPORT zlink_cppTargets");
    ok &= file_contains (
      root / "CMakeLists.txt",
      "option(ZLINK_STREAM_CONNECTOR_WITH_LZ4 \"Enable Stream Connector LZ4 compression\" ON)");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/ZLinkStreamConnector.Build.cs",
      "\"Sockets\"",
      "Unreal plugin must not depend on Unreal socket APIs when core owns transport");
    ok &= file_does_not_contain (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/ZLinkStreamConnector.Build.cs",
      "\"Networking\"",
      "Unreal plugin must not depend on Unreal networking APIs when core owns transport");
    ok &= file_contains (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "TWeakObjectPtr<UZLinkStreamConnector>");
    ok &= file_contains (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/ZLinkStreamConnector.cpp",
      "DetachOwner");
    ok &=
      require_absent (root
                        / "connector/engines/unreal/Source/ZLinkStreamConnector/Private/"
                          "ZLinkStreamConnectorAutomationTests.cpp",
                      "Unreal Automation Tests must live in the Rider/Editor-visible test module");
    ok &= file_contains (root / "connector/engines/unreal/ZLinkStreamConnector.uplugin",
                         "\"Name\": \"ZLinkStreamConnectorTests\"");
    ok &= file_contains (root / "connector/engines/unreal/ZLinkStreamConnector.uplugin",
                         "\"Type\": \"DeveloperTool\"");
    ok &= file_contains (root
                           / "connector/engines/unreal/Source/ZLinkStreamConnectorTests/"
                             "ZLinkStreamConnectorTests.Build.cs",
                         "\"ZLinkStreamConnector\"");
    ok &= file_contains (root
                           / "connector/engines/unreal/Source/ZLinkStreamConnectorTests/Private/"
                             "ZLinkStreamConnectorAutomationTests.cpp",
                         "IMPLEMENT_SIMPLE_AUTOMATION_TEST");
    ok &= file_does_not_contain (
      root
        / "connector/engines/unreal/Source/ZLinkStreamConnectorTests/Private/"
          "ZLinkStreamConnectorAutomationTests.cpp",
      "FSocket", "Unreal Automation Test must not reimplement STREAM loopback protocol");
    ok &= require_exists (root / "connector/engines/godot/CMakeLists.txt");
    ok &=
      require_exists (root / "connector/engines/godot/include/zlink_godot_stream_connector.hpp");
    ok &= require_exists (root / "connector/engines/godot/src/zlink_godot_stream_connector.cpp");
    ok &= file_does_not_contain (
      root / "connector/engines/godot/include/zlink_godot_stream_connector.hpp",
      "zlink/stream_connector",
      "Godot public API must not expose the general C++ connector surface");
    ok &= file_contains (root / "connector/engines/godot/src/zlink_godot_stream_connector.cpp",
                         "#include <zlink/stream_connector.hpp>");
    ok &= file_does_not_contain (
      root / "connector/engines/godot/src/zlink_godot_stream_connector.cpp", "enum class frame_",
      "Godot adapter must not define its own STREAM frame enums");
    ok &= file_contains (root / "connector/engines/godot/src/zlink_godot_stream_connector.cpp",
                         "main_thread_dispatcher");
    ok &= file_contains (root / "connector/engines/godot/src/zlink_godot_stream_connector.cpp",
                         "emit_request");
    ok &= require_exists (root
                          / "connector/engines/godot/extension/zlink_stream_connector.gdextension");
    ok &= require_exists (root / "connector/engines/godot/tests/zlink_stream_connector_tests.gd");
    ok &= file_contains (root / "connector/engines/godot/tests/zlink_stream_connector_tests.gd",
                         "engine-required");
    ok &= require_exists (root / "connector/engines/axmol/CMakeLists.txt");
    ok &=
      require_exists (root / "connector/engines/axmol/include/zlink_axmol_stream_connector.hpp");
    ok &= require_exists (root / "connector/engines/axmol/src/zlink_axmol_stream_connector.cpp");
    ok &= file_does_not_contain (
      root / "connector/engines/axmol/include/zlink_axmol_stream_connector.hpp",
      "zlink/stream_connector",
      "Axmol public API must not expose the general C++ connector surface");
    ok &= file_contains (root / "connector/engines/axmol/src/zlink_axmol_stream_connector.cpp",
                         "#include <zlink/stream_connector.hpp>");
    ok &= file_does_not_contain (
      root / "connector/engines/axmol/src/zlink_axmol_stream_connector.cpp", "enum class frame_",
      "Axmol adapter must not define its own STREAM frame enums");
    ok &= file_contains (root / "connector/engines/axmol/src/zlink_axmol_stream_connector.cpp",
                         "axmol_thread_dispatcher");
    ok &= file_contains (root / "connector/engines/axmol/src/zlink_axmol_stream_connector.cpp",
                         "emit_request");
    ok &= require_exists (root / "connector/engines/axmol/tests/test_app.cpp");
    ok &= file_contains (root / "connector/engines/axmol/tests/test_app.cpp", "engine-required");
    ok &= require_exists (root / "connector/core/packaging/vcpkg/vcpkg.json");
    ok &= require_exists (root / "connector/core/packaging/vcpkg/portfile.cmake");
    ok &= require_exists (root / "connector/core/packaging/conan/conanfile.py");
    ok &= require_exists (root / "connector/e2e-client/packaging/vcpkg/vcpkg.json");
    ok &= require_exists (root / "connector/e2e-client/packaging/vcpkg/portfile.cmake");
    ok &= require_exists (root / "connector/e2e-client/packaging/conan/conanfile.py");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/vcpkg.json",
                         "\"name\": \"zlink-stream-connector\"");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/vcpkg.json", "\"boost-beast\"");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/vcpkg.json", "\"vcpkg-cmake\"");
    ok &= file_does_not_contain (
      root / "connector/core/packaging/vcpkg/vcpkg.json", "\"msgpack-cxx\"",
      "MessagePack dependency belongs to the framework codec extension package");
    ok &= file_does_not_contain (
      root / "connector/core/packaging/vcpkg/vcpkg.json", "\"protobuf\"",
      "Protobuf dependency belongs to the framework codec extension package");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/vcpkg.json", "\"openssl\"");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/vcpkg.json", "\"lz4\"");
    ok &= file_contains (root / "connector/e2e-client/packaging/vcpkg/vcpkg.json",
                         "\"name\": \"zlink-stream-e2e-client\"");
    ok &= file_contains (root / "connector/core/packaging/conan/conanfile.py",
                         "name = \"zlink-stream-connector\"");
    ok &= file_contains (root / "connector/e2e-client/packaging/conan/conanfile.py",
                         "name = \"zlink-stream-e2e-client\"");
    ok &=
      file_contains (root / "connector/core/packaging/conan/conanfile.py", "version = \"0.1.0\"");
    ok &= file_contains (root / "connector/e2e-client/packaging/conan/conanfile.py",
                         "version = \"0.1.0\"");
    ok &= file_contains (root / "connector/core/packaging/conan/conanfile.py",
                         "\"shared\": [True, False]");
    ok &= file_contains (root / "connector/e2e-client/packaging/conan/conanfile.py",
                         "package_type = \"header-library\"");
    ok &= file_contains (root / "connector/e2e-client/packaging/conan/conanfile.py",
                         "requires = \"zlink-stream-connector/0.1.0\"");
    ok &= file_does_not_contain (root / "connector/e2e-client/packaging/conan/conanfile.py",
                                 "CMake(self).install()",
                                 "e2e Conan package must not reinstall the core connector package");
    ok &= file_contains (root / "CMakeLists.txt", "option(ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK");
    ok &= file_contains (root / "connector/core/packaging/conan/conanfile.py",
                         "ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/portfile.cmake",
                         "-DZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK=OFF");
    ok &= file_contains (root / "connector/e2e-client/packaging/vcpkg/portfile.cmake",
                         "zlink_stream_e2e_clientConfig.cmake");
    ok &= file_contains (root / "connector/e2e-client/packaging/vcpkg/portfile.cmake",
                         "find_dependency(zlink_stream_connector_cpp CONFIG)");
    ok &= file_does_not_contain (root / "connector/e2e-client/packaging/vcpkg/portfile.cmake",
                                 "vcpkg_cmake_install",
                                 "e2e vcpkg package must not reinstall the core connector package");
    ok &= file_does_not_contain (root / "connector/core/packaging/vcpkg/vcpkg.json", "unreal",
                                 "engine adapters must not be part of the core vcpkg package");
    ok &= file_does_not_contain (root / "connector/e2e-client/packaging/vcpkg/vcpkg.json", "unreal",
                                 "engine adapters must not be part of the e2e vcpkg package");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/portfile.cmake",
                         "-DZLINK_STREAM_CONNECTOR_BUILD_UNREAL=OFF");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/portfile.cmake",
                         "-DZLINK_STREAM_CONNECTOR_BUILD_GODOT=OFF");
    ok &= file_contains (root / "connector/core/packaging/vcpkg/portfile.cmake",
                         "-DZLINK_STREAM_CONNECTOR_BUILD_AXMOL=OFF");
    ok &= file_contains (root / "../../doc/framework/cpp/guide/stream-connector/01-overview.ko.md",
                         "TypeScript connector 사용");
    ok &=
      file_does_not_contain (root / "CMakeLists.txt", "cocos-connector",
                             "C++ connector package names must not use ambiguous cocos-connector");
    ok &=
      file_does_not_contain (root / "connector/core/packaging/vcpkg/vcpkg.json", "cocos-connector",
                             "core vcpkg package must not use ambiguous cocos-connector");
    ok &= file_does_not_contain (root / "connector/e2e-client/packaging/vcpkg/vcpkg.json",
                                 "cocos-connector",
                                 "e2e vcpkg package must not use ambiguous cocos-connector");
    ok &= http_client_public_surface_declares_general_client_features (root);
    ok &= stream_connector_public_surface_hides_runtime_internals (root);
    ok &= http_hosting_public_surface_excludes_non_goal_features (root);
    ok &= file_does_not_contain (
      root / "framework/include/zlink/framework/contracts/http/http.hpp",
      "http_options_builder_t &tls",
      "HTTP hosting public API must use configure_tls, not tls compatibility aliases");
    ok &= file_does_not_contain (
      root / "tests/Zlink.Framework.UnitTests/test_cpp_framework_app_host.cpp", ".tls (",
      "HTTP hosting regression tests must use configure_tls final API");
    ok &= file_contains (root / "framework/src/runtime/http/http_listener.cpp",
                         "offload_executor_t _connection_workers");
    ok &= file_does_not_contain (root / "framework/src/runtime/http/http_listener.cpp",
                                 "std::thread connection_thread",
                                 "HTTP hosting must not create one OS thread per connection");
    ok &=
      file_contains (root / "framework/src/runtime/http/http_listener.cpp", "_tls_context.emplace");
    ok &= file_contains (root / "framework/src/runtime/http/http_listener.cpp", "*_tls_context");
    ok &= file_does_not_contain (
      root / "framework/src/runtime/http/http_listener.cpp",
      "asio::ssl::context context (asio::ssl::context::tls_server)",
      "HTTP hosting must reuse listener TLS context instead of creating it per connection");
    ok &= file_contains (root / "framework/include/zlink/framework/contracts/actors/actor.hpp",
                         "class actor_client_t");
    ok &= file_contains (root / "framework/include/zlink/framework/contracts/actors/actor.hpp",
                         "actor_send_call_t send (actor_id_t actor_id");
    ok &= file_contains (root / "framework/include/zlink/framework/contracts/actors/actor.hpp",
                         "actor_request_call_t request (actor_id_t actor_id");
    ok &= file_contains (root / "framework/src/runtime/actors/actor_client.cpp",
                         "authority_key_t{\"1:\" + actor_id}");
    ok &= file_contains (root / "CMakeLists.txt",
                         "framework/src/runtime/actors/actor_client.cpp");

    ok &= public_headers_do_not_include_runtime (root / "framework/include");
    ok &= public_headers_do_not_include_runtime (root / "connector/core/include");
    ok &= public_headers_do_not_include_runtime (root / "http-client/include");
    ok &= public_headers_do_not_include_runtime (root / "extensions/include");
    ok &= public_headers_do_not_include_runtime (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public");
    ok &= public_headers_do_not_include_runtime (root / "connector/engines/godot/include");
    ok &= public_headers_do_not_include_runtime (root / "connector/engines/axmol/include");
    ok &= public_headers_do_not_expose_runtime_dependencies (root / "framework/include");
    ok &= public_headers_do_not_expose_runtime_dependencies (root / "connector/core/include");
    ok &= public_headers_do_not_expose_runtime_dependencies (root / "http-client/include");
    ok &= public_headers_do_not_expose_runtime_dependencies (root / "extensions/include");
    ok &= public_headers_do_not_expose_runtime_dependencies (
      root / "connector/engines/unreal/Source/ZLinkStreamConnector/Public");
    ok &=
      public_headers_do_not_expose_runtime_dependencies (root / "connector/engines/godot/include");
    ok &=
      public_headers_do_not_expose_runtime_dependencies (root / "connector/engines/axmol/include");
    ok &= location_store_public_surface_hides_domain_repositories (root);
    ok &= sample_application_code_uses_message_codec (root);
    ok &= sample_server_code_does_not_block_on_task_result (root);
    ok &= sample_and_e2e_code_does_not_read_the_environment (root);
    ok &= runner_generated_config_files_are_private_and_cleaned (root);
    ok &= observability_ops_uses_role_specific_entrypoints (root);
    ok &= spot_actor_transfer_uses_role_specific_entrypoints (root);
    ok &= affected_e2e_clients_own_each_scenario_in_a_file (root);
    ok &= file_does_not_contain (
      root / "e2e/run_e2e_all.sh", "exec env E2E_START_ORDER=",
      "the aggregate E2E runner must pass start order as a runner option, not an environment variable");
    ok &= file_contains (root / "e2e/RuntimeMonitoring/run_e2e.sh",
                         "$CLIENT\" --config=\"$CONFIG_DIR/client.json\"");
    ok &= file_contains (root / "e2e/RuntimeMonitoring/Client/Support/client_options.hpp",
                         "RuntimeMonitoring client requires --config=<path>");
    ok &= file_does_not_contain (
      root / "e2e/RuntimeMonitoring/Client/Support/client_options.hpp", "--log-dir=",
      "RuntimeMonitoring client file paths and topology must come from typed config");
    ok &= redesigned_cpp_contract_symbols_do_not_regress (root);
    ok &= contract_headers_have_compile_coverage (root, "framework/include", "");
    ok &= contract_headers_have_compile_coverage (root, "connector/core/include", "");
    ok &= contract_headers_have_compile_coverage (root, "http-client/include", "");
    ok &= non_empty_directories_do_not_keep_gitkeep (root);
    ok &= actor_model_documents_actor_destroy_lifecycle (root);
    ok &= framework_api_documents_actor_destroy_lifecycle (root);
    ok &= session_actor_dispatch_documents_disconnect_destroy_boundary (root);
    ok &= registry_spec_does_not_reintroduce_monitoring_contract (root);

    return ok ? 0 : 1;
}
