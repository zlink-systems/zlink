/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace zlink::http_client::detail
{

struct cookie_t
{
    std::string host;
    std::string name;
    std::string value;
    std::string path;
    bool secure = false;
};

class cookie_jar_t
{
  public:
    void store (const std::string &host, const std::string &set_cookie_header);
    std::string header_for (const std::string &host, const std::string &path, bool secure) const;

  private:
    mutable std::mutex _mutex;
    std::vector<cookie_t> _cookies;
};

} // namespace zlink::http_client::detail
