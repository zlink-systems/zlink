/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_API_SOCKET_MESSAGE_API_INTERNAL_HPP_INCLUDED__
#define __ZLINK_API_SOCKET_MESSAGE_API_INTERNAL_HPP_INCLUDED__

#include "zlink.h"

#if defined(__cplusplus)
#include <vector>
#include "core/ctx_physical_queue_registry.hpp"
namespace zlink
{
int socket_recv_internal_retained (
  void *socket_, zlink_routing_id_t *source_rid_out_,
  zlink_msg_t **parts_out_, size_t *part_count_out_,
  zlink_send_flags_t flags_,
  std::vector<retained_credit_token_t> *credits_out_);
int socket_subscribe_recv_internal_retained (
  void *socket_, zlink_routing_id_t *source_rid_out_,
  zlink_msg_t **parts_out_, size_t *part_count_out_,
  char *topic_id_out_, size_t *topic_id_len_out_,
  zlink_send_flags_t flags_,
  std::vector<retained_credit_token_t> *credits_out_);
}
#endif

#if defined(__cplusplus)
extern "C" {
#endif

int zlink_socket_xpub_recv_internal (void *socket_,
                                     zlink_routing_id_t *source_rid_out_,
                                     int *subscribed_out_,
                                     char *topic_id_out_,
                                     size_t *topic_id_len_,
                                     zlink_send_flags_t flags_);

int zlink_socket_send_internal (void *socket_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                zlink_send_flags_t flags_);

int zlink_socket_send_rid_internal (void *socket_,
                                    const zlink_routing_id_t *target_rid_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    zlink_send_flags_t flags_);

int zlink_socket_publish_internal (void *socket_,
                                   const char *topic_id_,
                                   zlink_msg_t *parts_,
                                   size_t part_count_,
                                   zlink_send_flags_t flags_);

int zlink_socket_recv_internal (void *socket_,
                                zlink_routing_id_t *source_rid_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_,
                                zlink_send_flags_t flags_);

int zlink_socket_subscribe_recv_internal (void *socket_,
                                          zlink_routing_id_t *source_rid_out_,
                                          zlink_msg_t **parts_out_,
                                          size_t *part_count_out_,
                                          char *topic_id_out_,
                                          size_t *topic_id_len_out_,
                                          zlink_send_flags_t flags_);

#if defined(__cplusplus)
}
#endif

#endif
