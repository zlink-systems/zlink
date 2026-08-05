/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/client_support.hpp"

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace zlink::framework::e2e::registration_codec::client
{

inline std::string read_text_file (const std::filesystem::path &path)
{
    std::ifstream input (path);
    std::ostringstream text;
    text << input.rdbuf ();
    return text.str ();
}

inline void run_invalid_registration_mode (const client_options_t &options,
                                           const std::string &mode,
                                           const std::string &expected_error)
{
    const auto &executable = options.invalid_server_executable;
    const auto &endpoint = options.invalid_endpoint;
    const auto log_dir = std::filesystem::path (options.log_dir);
    ensure (!executable.empty (), "RC-A6 invalid server executable is required");
    ensure (!endpoint.empty (), "RC-A6 invalid server endpoint is required");
    std::filesystem::create_directories (log_dir);

    const auto stdout_path = log_dir / ("invalid-" + mode + ".stdout.log");
    const auto stderr_path = log_dir / ("invalid-" + mode + ".stderr.log");
    const auto config_path = std::filesystem::path (options.config_dir)
                             / ("invalid-" + mode + ".json");
    std::filesystem::create_directories (options.config_dir);
    std::ofstream config (config_path);
    config << nlohmann::json{{"e2e",
                              {{"logDir", options.log_dir},
                               {"apiEndpoint", endpoint},
                               {"invalidMode", mode},
                               {"serverMode", "invalid"}}}}
               .dump (2);
    config.close ();
    std::filesystem::permissions (
      config_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);
    const auto pid = ::fork ();
    ensure (pid >= 0, "RC-A6 failed to start invalid mode " + mode);
    if (pid == 0) {
        const auto stdout_fd = ::open (stdout_path.c_str (), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        const auto stderr_fd = ::open (stderr_path.c_str (), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (stdout_fd >= 0) {
            (void) ::dup2 (stdout_fd, STDOUT_FILENO);
            (void) ::close (stdout_fd);
        }
        if (stderr_fd >= 0) {
            (void) ::dup2 (stderr_fd, STDERR_FILENO);
            (void) ::close (stderr_fd);
        }
        const auto argument = "--config=" + config_path.string ();
        ::execl (executable.c_str (), executable.c_str (), argument.c_str (),
                 static_cast<char *> (nullptr));
        _exit (127);
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (::waitpid (pid, &status, WNOHANG) == 0
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    if (::waitpid (pid, &status, WNOHANG) == 0) {
        (void) ::kill (pid, SIGKILL);
        (void) ::waitpid (pid, &status, 0);
        throw std::runtime_error ("RC-A6 invalid mode " + mode
                                  + " did not fail during startup");
    }

    ensure (WIFEXITED (status) && WEXITSTATUS (status) != 0 && WEXITSTATUS (status) != 127,
            "RC-A6 invalid mode " + mode + " did not report a validation failure");
    const auto error = read_text_file (stderr_path);
    ensure (error.find (expected_error) != std::string::npos,
            "RC-A6 invalid mode " + mode + " did not report: " + expected_error);
    std::cout << "scenario RC-A6 " << mode << " passed\n";
}

inline void run_invalid_registration_scenario (const client_options_t &options)
{
    run_invalid_registration_mode (options, "duplicate", "duplicate handler registration");
    run_invalid_registration_mode (
      options,
      "wrong-group", "maps handler group 'registration-codec' with an incompatible handler kind");
    run_invalid_registration_mode (options, "unsupported-channel",
                                   "server must map a request or send handler group");
    std::cout << "scenario RC-A6 passed\n";
}

} // namespace zlink::framework::e2e::registration_codec::client
