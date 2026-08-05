/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "client_options.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::resilience_lifecycle::client
{

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

inline void touch_file (const std::string &path)
{
    if (path.empty ()) {
        return;
    }
    std::ofstream file (path);
    file << "ready\n";
}

inline void wait_for_file (const std::string &path)
{
    if (path.empty ()) {
        return;
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (std::filesystem::exists (path)) {
            return;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }
    throw std::runtime_error ("timed out waiting for " + path);
}

} // namespace zlink::framework::e2e::resilience_lifecycle::client
