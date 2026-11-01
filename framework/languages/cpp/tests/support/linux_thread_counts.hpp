/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace zlink::framework::test
{
struct thread_counts_t
{
    int threads = 0;
    int contexts = 0;
    std::vector<std::string> core_threads;
};

inline thread_counts_t thread_counts ()
{
    thread_counts_t counts;
    for (const auto &thread : std::filesystem::directory_iterator ("/proc/self/task")) {
        std::ifstream comm (thread.path () / "comm");
        std::string name;
        if (std::getline (comm, name)) {
            std::ifstream stat (thread.path () / "stat");
            std::string state = "<exited before stat>";
            bool exiting = false;
            if (std::getline (stat, state)) {
                // Field 2 is parenthesized and may contain spaces or ')'.
                // Flags are field 9; count fields only after the final ')'.
                const auto comm_end = state.rfind (')');
                if (comm_end != std::string::npos) {
                    std::istringstream fields (state.substr (comm_end + 1));
                    std::string ignored;
                    for (int field = 3; field < 9; ++field)
                        fields >> ignored;
                    unsigned long flags = 0;
                    if (fields >> flags) {
                        // Linux PF_EXITING can remain visible in /proc after join.
                        constexpr unsigned long task_exiting_flag = 0x4;
                        exiting = (flags & task_exiting_flag) != 0;
                        if (!exiting) {
                            ++counts.threads;
                            if (name.find ("core-ct") != std::string::npos)
                                ++counts.contexts;
                        }
                    }
                }
            }
            if (name.find ("core-ct") != std::string::npos)
                counts.core_threads.push_back (thread.path ().filename ().string ()
                                               + (exiting ? " exiting " : " ") + state);
        }
    }
    return counts;
}

} // namespace zlink::framework::test
