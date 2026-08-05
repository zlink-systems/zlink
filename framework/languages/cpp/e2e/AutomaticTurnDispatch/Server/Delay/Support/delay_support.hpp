/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <zlink/framework.hpp>

#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay {

struct delay_options_t
{
    std::string log_dir;
    std::string node_rid;
    std::string http_endpoint;
    std::string delay_endpoint;

    static delay_options_t bind (const configuration_section_t &section)
    {
        return {.log_dir = section.require ("logDir"),
                .node_rid = section.require ("nodeRid"),
                .http_endpoint = section.require ("httpEndpoint"),
                .delay_endpoint = section.require ("delayEndpoint")};
    }
};

class delay_evidence_store_t
{
  public:
    explicit delay_evidence_store_t (std::string file_path) :
        _file_path (std::move (file_path))
    {
    }

    void add (std::string entry)
    {
        std::lock_guard lock (_mutex);
        _entries.push_back (entry);
        std::ofstream out (_file_path, std::ios::app);
        out << entry << '\n';
    }

    std::vector<std::string> snapshot () const
    {
        std::lock_guard lock (_mutex);
        return _entries;
    }

  private:
    std::string _file_path;
    mutable std::mutex _mutex;
    std::vector<std::string> _entries;
};

struct delay_state_t
{
    explicit delay_state_t (std::string rid, std::string evidence_file) :
        node_rid (std::move (rid)), evidence (std::move (evidence_file))
    {
    }

    std::string node_rid;
    delay_evidence_store_t evidence;
};

} // namespace zlink::framework::e2e::automatic_turn_dispatch::server::delay
