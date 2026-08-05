/* SPDX-License-Identifier: MPL-2.0 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{
void require (bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error (message);
}

std::string trim (std::string value)
{
    const auto first = value.find_first_not_of (" \t");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of (" \t\r");
    return value.substr (first, last - first + 1);
}

bool is_forbidden_contract_include (const std::string &source_line)
{
    const std::string line = trim (source_line);
    if (line.rfind ("#include", 0) != 0)
        return false;

    const auto open = line.find_first_of ("<\"");
    if (open == std::string::npos)
        return false;
    const char close_character = line[open] == '<' ? '>' : '"';
    const auto close = line.find (close_character, open + 1);
    if (close == std::string::npos)
        return false;

    std::string target = line.substr (open + 1, close - open - 1);
    std::replace (target.begin (), target.end (), '\\', '/');
    std::transform (target.begin (), target.end (), target.begin (),
                    [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });

    const auto basename = target.substr (target.find_last_of ('/') + 1);
    if (basename == "zlink.h")
        return true;

    const std::string delimited = "/" + target + "/";
    return delimited.find ("/runtime/") != std::string::npos
           || delimited.find ("/native/") != std::string::npos
           || delimited.find ("/src/") != std::string::npos;
}

void assert_mutation_detection ()
{
    require (!is_forbidden_contract_include ("#include \"../Core/routing_id.hpp\""),
             "contract include mutation rejected a valid contract dependency");
    require (!is_forbidden_contract_include ("#include <string>"),
             "contract include mutation rejected a standard library dependency");
    require (is_forbidden_contract_include ("#include <zlink.h>"),
             "contract include mutation did not detect raw zlink.h");
    require (is_forbidden_contract_include ("#include \"../../../src/detail.hpp\""),
             "contract include mutation did not detect src");
    require (is_forbidden_contract_include ("#include \"../../Runtime/Sockets/socket.hpp\""),
             "contract include mutation did not detect Runtime");
    require (is_forbidden_contract_include ("#include \"../../Native/native_api.hpp\""),
             "contract include mutation did not detect Native");
}

void assert_contract_tree_has_no_runtime_dependency (const std::filesystem::path &root)
{
    for (const auto &entry : std::filesystem::recursive_directory_iterator (root)) {
        if (!entry.is_regular_file ())
            continue;
        const auto extension = entry.path ().extension ().string ();
        if (extension != ".hpp" && extension != ".h")
            continue;

        std::ifstream input (entry.path ());
        require (input.good (), "failed to read contract header: " + entry.path ().string ());
        std::string line;
        std::size_t line_number = 0;
        while (std::getline (input, line)) {
            ++line_number;
            require (!is_forbidden_contract_include (line),
                     "forbidden contract dependency: " + entry.path ().string () + ":"
                       + std::to_string (line_number));
        }
    }
}
}

int main ()
{
    assert_mutation_detection ();

    const std::filesystem::path this_file = __FILE__;
    const auto binding_root = this_file.parent_path ().parent_path ().parent_path ();
    assert_contract_tree_has_no_runtime_dependency (binding_root / "include" / "zlink" / "Contracts");
    return 0;
}
