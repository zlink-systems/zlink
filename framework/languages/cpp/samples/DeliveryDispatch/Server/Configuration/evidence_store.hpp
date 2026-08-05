/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/Contracts/messages.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace zlink::samples::deliverydispatch
{

class evidence_store_t
{
  public:
    explicit evidence_store_t (std::string path) : _path (std::move (path))
    {
        std::filesystem::create_directories (std::filesystem::path (_path).parent_path ());
    }

    void append (const delivery_status_changed_req_t &status)
    {
        const std::lock_guard lock (_mutex);
        std::ofstream file (_path, std::ios::app);
        file << status.delivery_id << ":" << status.status << ":"
             << status.courier_id.value_or ("none") << "\n";
    }

    std::vector<std::string> read_lines () const
    {
        const std::lock_guard lock (_mutex);
        std::vector<std::string> lines;
        std::ifstream file (_path);
        std::string line;
        while (std::getline (file, line)) {
            if (!line.empty ()) {
                lines.push_back (line);
            }
        }
        return lines;
    }

    bool has_sequence (const std::string &delivery_id,
                       const std::vector<std::string> &expected) const
    {
        const auto lines = read_lines ();
        std::size_t cursor = 0;
        const auto prefix = delivery_id + ":";
        for (const auto &line : lines) {
            if (line.rfind (prefix, 0) != 0 || cursor >= expected.size ()) {
                continue;
            }
            const auto status_start = prefix.size ();
            const auto status_end = line.find (':', status_start);
            const auto status = line.substr (status_start, status_end - status_start);
            if (status == expected[cursor]) {
                ++cursor;
            }
        }
        return cursor == expected.size ();
    }

  private:
    std::string _path;
    mutable std::mutex _mutex;
};

} // namespace zlink::samples::deliverydispatch
