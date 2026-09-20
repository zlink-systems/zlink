/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <string>

namespace zlink::samples::bingo
{

// Resolves where a server role writes its message-flow log file. The directory is
// application configuration (sample.topology.logDir); use_file() creates it if missing.
inline std::string flow_log_path (const std::string &log_dir, const std::string &role)
{
    return log_dir + "/bingo-" + role + ".log";
}

} // namespace zlink::samples::bingo
