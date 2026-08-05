/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace zlink::samples::bingo
{

// Resolves where a server role writes its message-flow log file. The directory is
// application configuration (sample.topology.logDir); use_file() creates it if missing.
inline std::string flow_log_path (const std::string &log_dir, const std::string &role)
{
    return log_dir + "/bingo-" + role + ".log";
}

// Capture structured metric diagnostics in the sample log. Production
// applications normally forward these records through their logging provider.
inline void observe_runtime_metrics (zlink::framework::app_t &app,
                                    const std::string &log_dir,
                                    const std::string &role)
{
    std::filesystem::create_directories (log_dir);
    auto sink = std::make_shared<std::ofstream> (log_dir + "/bingo-" + role + "-metrics.log",
                                                 std::ios::app);
    auto gate = std::make_shared<std::mutex> ();
    /* Runtime metrics are emitted at debug level. The sample deliberately
     * enables that level when it installs the evidence sink; otherwise the
     * callback exists but the metric records are filtered before delivery. */
    app.logging ().set_min_level (zlink::framework::log_level_t::debug)
      .use_callback_sink (
      [sink, gate] (const zlink::framework::log_record_t &record) {
          if (record.message != "zlink.runtime.metric.recorded") {
              return;
          }
          const std::lock_guard<std::mutex> lock (*gate);
          for (const auto &field : record.fields) {
              (*sink) << field.key << '=' << field.value << ' ';
          }
          (*sink) << '\n';
          sink->flush ();
      });
}

} // namespace zlink::samples::bingo
