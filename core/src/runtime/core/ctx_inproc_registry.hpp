/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_INPROC_REGISTRY_HPP_INCLUDED__
#define __ZLINK_CTX_INPROC_REGISTRY_HPP_INCLUDED__

#include <map>
#include <string>
#include <vector>

#include "core/options.hpp"
#include "utils/mutex.hpp"

namespace zlink
{
class pipe_t;
class socket_base_t;

//  Information associated with inproc endpoint. Note that endpoint options
//  are registered as well so that the peer can access them without a need
//  for synchronisation, handshaking or similar.
struct endpoint_t
{
    endpoint_t ();
    endpoint_t (socket_base_t *socket_, const options_t &options_);

    socket_base_t *socket;
    options_t options;
};

class ctx_inproc_registry_t
{
  public:
    ctx_inproc_registry_t ();

    int register_endpoint (const char *addr_, const endpoint_t &endpoint_);
    int unregister_endpoint (const std::string &addr_, const socket_base_t *socket_);
    void unregister_endpoints (const socket_base_t *socket_);
    endpoint_t find_endpoint (const char *addr_);
    bool pend_connection (const std::string &addr_, const endpoint_t &endpoint_, pipe_t **pipes_);
    void connect_pending (const char *addr_, socket_base_t *bind_socket_);
    bool has_pending_for_socket (const std::string &addr_, const socket_base_t *socket_) const;
    size_t materialize_pending_for_socket (const std::string &addr_,
                                           const socket_base_t *socket_,
                                           socket_base_t *bind_socket_);
    void collect_pending_addresses (std::vector<std::string> *out_) const;

  private:
    struct pending_connection_t
    {
        pending_connection_t ();
        pending_connection_t (const endpoint_t &endpoint_,
                              pipe_t *connect_pipe_,
                              pipe_t *bind_pipe_);

        endpoint_t endpoint;
        pipe_t *connect_pipe;
        pipe_t *bind_pipe;
    };

    enum side
    {
        connect_side,
        bind_side
    };

    static void connect_inproc_sockets (socket_base_t *bind_socket_,
                                        const options_t &bind_options_,
                                        const pending_connection_t &pending_connection_,
                                        side side_,
                                        bool materialization_ = false);

    typedef std::map<std::string, endpoint_t> endpoints_t;
    typedef std::multimap<std::string, pending_connection_t> pending_connections_t;

    mutable recursive_mutex_t _sync;
    endpoints_t _endpoints;
    pending_connections_t _pending_connections;
};
}

#endif
