/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "sample_topology.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace zlink::samples::supportchat
{

inline volatile std::sig_atomic_t keep_running = 1;

inline void stop_role (int)
{
    keep_running = 0;
}

inline int run_role_process (const std::string &role_name)
{
    std::signal (SIGTERM, stop_role);
    std::signal (SIGINT, stop_role);

    std::cout << "supportchat " << role_name << " role=ready" << std::endl;
    while (keep_running != 0) {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    std::cout << "supportchat " << role_name << " role=stopped" << std::endl;
    return 0;
}

} // namespace zlink::samples::supportchat
