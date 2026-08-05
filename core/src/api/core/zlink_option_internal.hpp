/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_ZLINK_OPTION_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_ZLINK_OPTION_INTERNAL_HPP_INCLUDED__

#include "zlink.h"

#include <cstddef>

#include "sockets/common/socket_base.hpp"

struct option_descriptor_t
{
    int public_option;
    int internal_option;
    bool unsupported_on_socket;
};

namespace zlink
{
class socket_base_t;

enum option_target_kind_t
{
    option_target_invalid = 0,
    option_target_socket
};

struct option_target_t
{
    option_target_t ();

    option_target_kind_t kind;
    socket_base_t *socket;
};

option_target_t resolve_option_target (void *handle_);
}

zlink::socket_base_t *as_socket (void *handle_);
int socket_type_of (zlink::socket_base_t *socket_);

int map_common_option (zlink_option_t option_);
int map_router_option (zlink_router_option_t option_);
int map_dealer_option (zlink_dealer_option_t option_);
int map_stream_option (zlink_stream_option_t option_);
int map_pub_option (int option_);
int map_sub_option (int option_);
const option_descriptor_t *lookup_common_option (zlink_option_t option_);

int set_socket_option_checked (zlink::socket_base_t *socket_,
                               int type_,
                               int expected_a_,
                               int expected_b_,
                               int option_,
                               const void *optval_,
                               size_t optvallen_);
int get_socket_option_checked (zlink::socket_base_t *socket_,
                               int type_,
                               int expected_a_,
                               int expected_b_,
                               int option_,
                               void *optval_,
                               size_t *optvallen_);

#endif
