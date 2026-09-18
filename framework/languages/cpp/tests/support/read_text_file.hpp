/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

namespace zlink::framework::tests
{
/* Reads a whole file as text with every newline normalized to '\n'.
 *
 * Tests that assert on file contents spell multi-line needles with '\n'.
 * A plain std::ifstream makes that assertion depend on the checkout: the
 * Windows CRT folds CRLF away before find() sees it, Linux and macOS do not,
 * so the same needle matches on one platform and misses on another. A missed
 * needle is not always a visible failure — a negative assertion
 * (EXPECT_EQ (find (...), npos)) passes precisely because the needle was
 * never found, so it stops checking anything at all.
 *
 * Reading in binary mode and normalizing here keeps that decision in one
 * place, the way core/tests/contract/check_public_surface.py relies on
 * Path.read_text(). Call sites keep writing '\n'. */
inline std::string read_text_file (const std::filesystem::path &path)
{
    std::ifstream input (path, std::ios::binary);
    const std::string raw ((std::istreambuf_iterator<char> (input)),
                           std::istreambuf_iterator<char> ());

    std::string text;
    text.reserve (raw.size ());
    for (std::size_t index = 0; index < raw.size (); ++index) {
        if (raw[index] != '\r') {
            text.push_back (raw[index]);
            continue;
        }
        /* CRLF and a lone CR both denote one line break. */
        if (index + 1 < raw.size () && raw[index + 1] == '\n') {
            ++index;
        }
        text.push_back ('\n');
    }
    return text;
}
}
