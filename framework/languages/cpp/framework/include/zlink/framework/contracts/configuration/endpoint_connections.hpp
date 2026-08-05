/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace zlink::framework
{

namespace detail
{
class endpoint_connections_state_t;
class endpoint_connections_runtime_t;
} // namespace detail

/* Runtime handle over one manual role's endpoint set. Discovery-mode roles
 * freeze the handle: the endpoint set can only change while the role owns its
 * peers manually. */
class endpoint_connections_t
{
  public:
    endpoint_connections_t ();

    void connect (std::string endpoint);
    void disconnect (std::string endpoint);
    std::vector<std::string> list_connections () const;

  private:
    friend class detail::endpoint_connections_runtime_t;

    std::shared_ptr<detail::endpoint_connections_state_t> _state;
};

} // namespace zlink::framework
