/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_RECV_INTERNAL_HPP_INCLUDED__
#define __ZLINK_CORE_RECV_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <vector>

namespace zlink
{
class socket_base_t;

int recv_msg_socket (socket_base_t *socket_, int socket_type_, zlink_msg_t *msg_, int flags_);
int recv_msg_internal (void *socket_, zlink_msg_t *msg_, int flags_);
int recv_msg_routed_socket (socket_base_t *socket_,
                            zlink_msg_t *msg_,
                            zlink_routing_id_t *source_rid_out_,
                            int flags_,
                            uint64_t *connection_id_out_ = NULL);
int recv_msg_routed_internal (void *socket_,
                              zlink_msg_t *msg_,
                              zlink_routing_id_t *source_rid_out_,
                              int flags_,
                              uint64_t *connection_id_out_ = NULL);
int recv_followup_msg_socket (socket_base_t *socket_, zlink_msg_t *msg_);
int recv_followup_msg_internal (void *socket_, zlink_msg_t *msg_);
int recv_followup_msg_socket_wait (socket_base_t *socket_, zlink_msg_t *msg_, int flags_);
bool msg_frame_has_more (const zlink_msg_t &msg_);
void close_msg_frames (std::vector<zlink_msg_t> *frames_);

struct scoped_msg_frames_t : public std::vector<zlink_msg_t>
{
    scoped_msg_frames_t () {}
    ~scoped_msg_frames_t () { close_msg_frames (this); }

  private:
    scoped_msg_frames_t (const scoped_msg_frames_t &);
    scoped_msg_frames_t &operator= (const scoped_msg_frames_t &);
};

bool recv_msg_sequence_socket_wait (socket_base_t *socket_,
                                    std::vector<zlink_msg_t> *frames_,
                                    long followup_timeout_ms_);
int drain_followup_msg_sequence (socket_base_t *socket_, zlink_msg_t *frame_, bool wait_for_input_);
int export_payload_msg_sequence (socket_base_t *socket_,
                                 zlink_msg_t *first_payload_,
                                 zlink_msg_t **parts_out_,
                                 size_t *part_count_out_,
                                 bool allow_single_);
int export_reserved_followup_msg_sequence (socket_base_t *socket_,
                                           zlink_msg_t **parts_out_,
                                           size_t *part_count_out_,
                                           bool wait_for_input_);
int recv_buffer_internal (void *socket_, void *buf_, size_t len_, int flags_);
int wait_socket_events_internal (void *socket_, short events_, long timeout_ms_);
}

#endif
