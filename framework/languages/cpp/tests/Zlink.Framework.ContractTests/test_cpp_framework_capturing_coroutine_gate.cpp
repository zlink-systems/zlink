/* SPDX-License-Identifier: MPL-2.0 */

#include "../support/capturing_coroutine_scan.hpp"
#include "../support/read_text_file.hpp"

#include <filesystem>
#include <iostream>

#ifndef ZLINK_FRAMEWORK_CPP_SOURCE_DIR
#error "ZLINK_FRAMEWORK_CPP_SOURCE_DIR must be defined"
#endif

int main ()
{
    using zlink::framework::tests::read_text_file;
    const std::filesystem::path root = ZLINK_FRAMEWORK_CPP_SOURCE_DIR;
    const auto include_root = root / "framework/include";
    int failures = 0;

    // CP.51: captures read after suspension depend on the closure lifetime.
    zlink::framework::tests::coroutine_scan_counts_t scan_counts;
    for (const auto &scan_root : {root / "framework/src", include_root}) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator (scan_root)) {
            const auto ext = entry.path ().extension ();
            if (!entry.is_regular_file () || (ext != ".hpp" && ext != ".h" && ext != ".cpp"))
                continue;
            const auto source = read_text_file (entry.path ());
            for (const auto offset :
                 zlink::framework::tests::capturing_coroutine_reads (source, &scan_counts)) {
                const auto line = 1 + std::count (source.begin (), source.begin () + offset, '\n');
                std::cerr << "CPP-CP51-001: capture read after suspension: "
                          << entry.path ().string () << ':' << line << '\n';
                ++failures;
            }
        }
    }
    std::cout << "CP.51 files=" << scan_counts.files << " parsed_files=" << scan_counts.parsed_files
              << " lambdas=" << scan_counts.lambdas << " regex_passes=" << scan_counts.regex_passes
              << " binding_regex_passes=" << scan_counts.binding_regex_passes
              << " declaration_visits=" << scan_counts.declaration_visits << '\n';

    return failures == 0 ? 0 : 1;
}
