#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::string read_file (const std::filesystem::path &path)
{
    std::ifstream in (path);
    assert (in.good ());
    std::ostringstream out;
    out << in.rdbuf ();
    return out.str ();
}

void collect_files (const std::filesystem::path &dir, std::vector<std::filesystem::path> &out)
{
    for (const auto &entry : std::filesystem::recursive_directory_iterator (dir)) {
        if (!entry.is_regular_file ())
            continue;
        const auto ext = entry.path ().extension ().string ();
        if (ext == ".cpp" || ext == ".hpp" || ext == ".h")
            out.push_back (entry.path ());
    }
}

bool contains_aggregate_call (const std::string &body, const std::string &symbol)
{
    const std::string needle = symbol + " (";
    std::size_t pos = 0;
    while ((pos = body.find (needle, pos)) != std::string::npos) {
        if (body.compare (pos, symbol.size () + 6, symbol + "_part") != 0)
            return true;
        pos += needle.size ();
    }

    const std::string compact_needle = symbol + "(";
    pos = 0;
    while ((pos = body.find (compact_needle, pos)) != std::string::npos) {
        if (body.compare (pos, symbol.size () + 5, symbol + "_part") != 0)
            return true;
        pos += compact_needle.size ();
    }
    return false;
}

void assert_public_contract_includes_only (const std::filesystem::path &root)
{
    const std::vector<std::string> forbidden_include_fragments = {
      "<zlink.h>",
      "\"zlink.h\"",
      "Runtime/",
      "Native/",
      "native_",
      "_access.hpp",
      "detail.hpp",
      "operation_detail.hpp",
      "socket_handle.hpp",
      "request_progress.hpp",
    };

    std::vector<std::filesystem::path> files;
    collect_files (root, files);
    for (const auto &file : files) {
        std::istringstream lines (read_file (file));
        std::string line;
        while (std::getline (lines, line)) {
            const auto first = line.find_first_not_of (" \t");
            if (first == std::string::npos || line.compare (first, 8, "#include") != 0)
                continue;
            for (const auto &fragment : forbidden_include_fragments)
                assert (line.find (fragment) == std::string::npos);
        }
    }
}
}

int main ()
{
    const std::filesystem::path this_file = __FILE__;
    const std::filesystem::path cpp_root = this_file.parent_path ().parent_path ().parent_path ();

    std::vector<std::filesystem::path> files;
    collect_files (cpp_root / "src" / "Runtime", files);

    std::string all;
    for (const auto &file : files)
        all += read_file (file) + "\n";

    const std::vector<std::string> required_part_symbols = {
      "zlink_send_part",
      "zlink_recv_part",
      "zlink_publish_part",
      "zlink_subscribe_part",
      "zlink_router_recv_part",
      "zlink_dealer_request_part",
      "zlink_router_request_part",
      "zlink_router_reply_part",
      "submit_borrowed_message_array",
    };
    for (const auto &symbol : required_part_symbols)
        assert (all.find (symbol) != std::string::npos);

    const std::vector<std::string> aggregate_symbols = {
      "zlink_send",
      "zlink_recv",
      "zlink_publish",
      "zlink_subscribe",
      "zlink_router_recv",
      "zlink_dealer_request",
      "zlink_router_request",
      "zlink_router_reply",
    };

    for (const auto &symbol : aggregate_symbols)
        assert (!contains_aggregate_call (all, symbol));

    assert (all.find ("std::this_thread::sleep_for") == std::string::npos);

    assert_public_contract_includes_only (cpp_root / "samples");
    assert_public_contract_includes_only (cpp_root / "perf");
    return 0;
}
