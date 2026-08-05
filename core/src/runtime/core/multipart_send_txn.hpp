/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_MULTIPART_SEND_TXN_HPP_INCLUDED__
#define __ZLINK_CORE_MULTIPART_SEND_TXN_HPP_INCLUDED__

#include <zlink.h>

namespace zlink
{
class socket_base_t;
class pipe_t;
typedef void (*multipart_pipe_selected_fn) (pipe_t *pipe_, void *userdata_);

int logical_multipart_send (socket_base_t *socket_,
                            zlink_msg_t *parts_,
                            size_t part_count_,
                            int flags_);

int logical_multipart_send_tracked (socket_base_t *socket_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    int flags_,
                                    multipart_pipe_selected_fn selected_fn_,
                                    void *selected_userdata_);

int logical_multipart_send_routed_tracked (
  socket_base_t *socket_,
  const zlink_routing_id_t *routing_id_,
  zlink_msg_t *parts_,
  size_t part_count_,
  int flags_,
  multipart_pipe_selected_fn selected_fn_,
  void *selected_userdata_);

int logical_multipart_send_routed (socket_base_t *socket_,
                                   const zlink_routing_id_t *routing_id_,
                                   zlink_msg_t *parts_,
                                   size_t part_count_,
                                   int flags_);

int logical_multipart_send_prefixed_frame (socket_base_t *socket_,
                                           const void *prefix_data_,
                                           size_t prefix_size_,
                                           int prefix_frame_flags_,
                                           zlink_msg_t *parts_,
                                           size_t part_count_,
                                           int flags_);

int logical_multipart_send_routed_prefixed_frame (socket_base_t *socket_,
                                                  const zlink_routing_id_t *routing_id_,
                                                  const void *prefix_data_,
                                                  size_t prefix_size_,
                                                  int prefix_frame_flags_,
                                                  zlink_msg_t *parts_,
                                                  size_t part_count_,
                                                  int flags_);

int logical_multipart_send_prefixed_frames (socket_base_t *socket_,
                                            const void *prefix1_data_,
                                            size_t prefix1_size_,
                                            int prefix1_frame_flags_,
                                            const void *prefix2_data_,
                                            size_t prefix2_size_,
                                            int prefix2_frame_flags_,
                                            zlink_msg_t *parts_,
                                            size_t part_count_,
                                            int flags_);

int logical_multipart_send_routed_prefixed_frames (socket_base_t *socket_,
                                                   const zlink_routing_id_t *routing_id_,
                                                   const void *prefix1_data_,
                                                   size_t prefix1_size_,
                                                   int prefix1_frame_flags_,
                                                   const void *prefix2_data_,
                                                   size_t prefix2_size_,
                                                   int prefix2_frame_flags_,
                                                   zlink_msg_t *parts_,
                                                   size_t part_count_,
                                                   int flags_);

int logical_multipart_send_prefixed (socket_base_t *socket_,
                                     const void *prefix_data_,
                                     size_t prefix_size_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     int flags_);

int logical_multipart_publish (socket_base_t *socket_,
                               const char *topic_,
                               zlink_msg_t *parts_,
                               size_t part_count_,
                               int flags_);

int logical_multipart_publish_frame (socket_base_t *socket_,
                                     zlink_msg_t *topic_frame_,
                                     zlink_msg_t *parts_,
                                     size_t part_count_,
                                     int flags_);
}

#endif
