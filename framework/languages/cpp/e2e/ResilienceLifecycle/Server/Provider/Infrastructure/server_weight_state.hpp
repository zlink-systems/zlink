/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <mutex>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

class server_weight_state_t
{
  public:
    void set (std::uint32_t weight)
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _weight = weight;
    }

    std::uint32_t get () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        return _weight;
    }

  private:
    mutable std::mutex _mutex;
    std::uint32_t _weight = 100;
};

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
