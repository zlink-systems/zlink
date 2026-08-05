/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/multipart_send_txn.hpp"

#include "core/msg.hpp"
#include "sockets/common/socket_base.hpp"

namespace zlink
{
struct multipart_send_facade_t
{
    static socket_public_send_scope_t make_scope (socket_base_t *socket_, bool force_sync_)
    {
        return socket_public_send_scope_t (socket_->lifecycle_coordinator (),
                                           force_sync_
                                             || socket_->direct_send_needs_public_api_sync ());
    }

    static int send_scoped (socket_base_t *socket_,
                            msg_t *msg_,
                            int flags_,
                            socket_public_send_scope_t &scope_,
                            pipe_t **pipe_out_ = NULL)
    {
        return socket_->send_direct_with_retry (
          NULL, msg_, flags_, scope_, NULL, 0, true, pipe_out_);
    }

    static int rollback_scoped (socket_base_t *socket_, socket_public_send_scope_t &scope_)
    {
        return socket_->rollback_scoped (scope_);
    }
};
}

namespace
{
struct prefix_frame_t
{
    const void *data;
    size_t size;
    int frame_flags;
};

static void consume_frames_from (zlink_msg_t *parts_, size_t start_index_, size_t part_count_)
{
    for (size_t i = start_index_; i < part_count_; ++i) {
        zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        if (!msg->check ())
            continue;

        const int close_rc = msg->close ();
        errno_assert (close_rc == 0);
        const int init_rc = msg->init ();
        errno_assert (init_rc == 0);
    }
}

static int send_frames_once (zlink::socket_base_t *socket_,
                             zlink_msg_t *parts_,
                             size_t part_count_,
                             int flags_,
                             bool rollback_started_,
                             zlink::socket_public_send_scope_t &scope_,
                             zlink::pipe_t **application_pipe_out_ = NULL,
                             zlink::multipart_pipe_selected_fn selected_fn_ = NULL,
                             void *selected_userdata_ = NULL)
{
    bool started = rollback_started_;
    if (application_pipe_out_)
        *application_pipe_out_ = NULL;

    for (size_t i = 0; i < part_count_; ++i) {
        const bool more = i + 1 < part_count_;
        zlink::pipe_t *selected_pipe = NULL;
        if (zlink::multipart_send_facade_t::send_scoped (
              socket_, reinterpret_cast<zlink::msg_t *> (&parts_[i]),
              (more ? ZLINK_SNDMORE : 0) | flags_, scope_,
              application_pipe_out_ && i == 0 ? &selected_pipe : NULL)
            != 0) {
            const int err = errno;
            if (started)
                (void) zlink::multipart_send_facade_t::rollback_scoped (socket_, scope_);
            consume_frames_from (parts_, i, part_count_);
            errno = err;
            return -1;
        }
        if (application_pipe_out_ && i == 0)
            *application_pipe_out_ = selected_pipe;
        if (selected_fn_ && i == 0)
            selected_fn_ (selected_pipe, selected_userdata_);
        started = more;
    }

    errno = 0;
    return 0;
}

static int send_prefixed_once (zlink::socket_base_t *socket_,
                               const void *prefix_data_,
                               size_t prefix_size_,
                               int prefix_frame_flags_,
                               bool rollback_started_,
                               zlink_msg_t *parts_,
                               size_t part_count_,
                               int flags_,
                               zlink::socket_public_send_scope_t &scope_);

static int send_prefix_sequence_once (zlink::socket_base_t *socket_,
                                      const prefix_frame_t *prefixes_,
                                      size_t prefix_count_,
                                      bool rollback_started_,
                                      zlink_msg_t *parts_,
                                      size_t part_count_,
                                      int flags_,
                                      zlink::socket_public_send_scope_t &scope_,
                                      zlink::multipart_pipe_selected_fn selected_fn_ = NULL,
                                      void *selected_userdata_ = NULL)
{
    std::vector<zlink_msg_t> prepared_prefixes (prefix_count_);
    size_t prepared_count = 0;
    for (; prepared_count < prefix_count_; ++prepared_count) {
        if (zlink_msg_init_size (&prepared_prefixes[prepared_count],
                                 prefixes_[prepared_count].size)
            != 0) {
            const int err = errno;
            for (size_t i = 0; i < prepared_count; ++i)
                (void) zlink_msg_close (&prepared_prefixes[i]);
            errno = err;
            return -1;
        }

        if (prefixes_[prepared_count].size > 0)
            memcpy (zlink_msg_data (&prepared_prefixes[prepared_count]),
                    prefixes_[prepared_count].data, prefixes_[prepared_count].size);
    }

    bool started = rollback_started_;

    for (size_t i = 0; i < prefix_count_; ++i) {
        const bool has_following = (i + 1 < prefix_count_) || part_count_ > 0;
        if (zlink::multipart_send_facade_t::send_scoped (
              socket_,
              reinterpret_cast<zlink::msg_t *> (&prepared_prefixes[i]),
              (has_following ? ZLINK_SNDMORE : 0) | flags_ | prefixes_[i].frame_flags,
              scope_)
            != 0) {
            const int err = errno;
            if (started)
                (void) zlink::multipart_send_facade_t::rollback_scoped (socket_, scope_);
            for (size_t close_index = i; close_index < prefix_count_; ++close_index)
                (void) zlink_msg_close (&prepared_prefixes[close_index]);
            errno = err;
            return -1;
        }
        (void) zlink_msg_close (&prepared_prefixes[i]);
        started = has_following;
    }

    if (part_count_ == 0) {
        errno = 0;
        return 0;
    }

    zlink::pipe_t *application_pipe = NULL;
    return send_frames_once (
      socket_, parts_, part_count_, flags_, started, scope_,
      selected_fn_ ? &application_pipe : NULL, selected_fn_, selected_userdata_);
}

static int send_frames_once_routed (zlink::socket_base_t *socket_,
                                    const zlink_routing_id_t *routing_id_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    int flags_,
                                    zlink::socket_public_send_scope_t &scope_,
                                    zlink::multipart_pipe_selected_fn selected_fn_ = NULL,
                                    void *selected_userdata_ = NULL)
{
    const prefix_frame_t routing_prefix = {routing_id_->data, routing_id_->size, 0};
    return send_prefix_sequence_once (socket_, &routing_prefix, 1, false, parts_, part_count_,
                                      flags_, scope_, selected_fn_, selected_userdata_);
}

static int send_publish_once (zlink::socket_base_t *socket_,
                              const char *topic_,
                              zlink_msg_t *parts_,
                              size_t part_count_,
                              int flags_,
                              zlink::socket_public_send_scope_t &scope_)
{
    if (!topic_) {
        if (part_count_ == 0) {
            errno = EINVAL;
            return -1;
        }
        return send_frames_once (socket_, parts_, part_count_, flags_, false, scope_);
    }

    zlink::msg_t topic_msg;
    const size_t topic_size = strlen (topic_);
    if (topic_msg.init_size (topic_size) != 0)
        return -1;

    if (topic_size > 0)
        memcpy (topic_msg.data (), topic_, topic_size);

    const bool has_payload = part_count_ > 0;
    if (zlink::multipart_send_facade_t::send_scoped (
          socket_, &topic_msg, (has_payload ? ZLINK_SNDMORE : 0) | flags_, scope_)
        != 0) {
        const int err = errno;
        (void) topic_msg.close ();
        errno = err;
        return -1;
    }
    (void) topic_msg.close ();

    if (part_count_ == 0) {
        errno = 0;
        return 0;
    }

    return send_frames_once (socket_, parts_, part_count_, flags_, true, scope_);
}

static int send_publish_frame_once (zlink::socket_base_t *socket_,
                                    zlink_msg_t *topic_frame_,
                                    zlink_msg_t *parts_,
                                    size_t part_count_,
                                    int flags_,
                                    zlink::socket_public_send_scope_t &scope_)
{
    if (!topic_frame_ || !reinterpret_cast<zlink::msg_t *> (topic_frame_)->check ()) {
        errno = EINVAL;
        return -1;
    }

    const bool has_payload = part_count_ > 0;
    if (zlink::multipart_send_facade_t::send_scoped (
          socket_, reinterpret_cast<zlink::msg_t *> (topic_frame_),
          (has_payload ? ZLINK_SNDMORE : 0) | flags_, scope_)
        != 0) {
        return -1;
    }

    if (part_count_ == 0) {
        errno = 0;
        return 0;
    }

    return send_frames_once (socket_, parts_, part_count_, flags_, true, scope_);
}

static int send_prefixed_once (zlink::socket_base_t *socket_,
                               const void *prefix_data_,
                               size_t prefix_size_,
                               int prefix_frame_flags_,
                               bool rollback_started_,
                               zlink_msg_t *parts_,
                               size_t part_count_,
                               int flags_,
                               zlink::socket_public_send_scope_t &scope_)
{
    const prefix_frame_t prefix = {prefix_data_, prefix_size_, prefix_frame_flags_};
    return send_prefix_sequence_once (socket_, &prefix, 1, rollback_started_, parts_, part_count_,
                                      flags_, scope_);
}

}

int zlink::logical_multipart_send (socket_base_t *socket_,
                                   zlink_msg_t *parts_,
                                   size_t part_count_,
                                   int flags_)
{
    if (!socket_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    const int rc = send_frames_once (socket_, parts_, part_count_, flags_, false, send_scope);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_send_tracked (socket_base_t *socket_,
                                           zlink_msg_t *parts_,
                                           size_t part_count_,
                                           int flags_,
                                           multipart_pipe_selected_fn selected_fn_,
                                           void *selected_userdata_)
{
    if (!socket_ || !parts_ || part_count_ == 0 || !selected_fn_) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    zlink::pipe_t *application_pipe = NULL;
    const int rc = send_frames_once (
      socket_, parts_, part_count_, flags_, false, send_scope,
      &application_pipe, selected_fn_, selected_userdata_);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_send_routed_tracked (
  socket_base_t *socket_,
  const zlink_routing_id_t *routing_id_,
  zlink_msg_t *parts_,
  size_t part_count_,
  int flags_,
  multipart_pipe_selected_fn selected_fn_,
  void *selected_userdata_)
{
    if (!socket_ || !routing_id_ || !parts_ || part_count_ == 0
        || !selected_fn_) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    return send_frames_once_routed (
      socket_, routing_id_, parts_, part_count_, flags_, send_scope,
      selected_fn_, selected_userdata_);
}

int zlink::logical_multipart_send_routed (socket_base_t *socket_,
                                          const zlink_routing_id_t *routing_id_,
                                          zlink_msg_t *parts_,
                                          size_t part_count_,
                                          int flags_)
{
    if (!socket_ || !routing_id_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    const int rc =
      send_frames_once_routed (socket_, routing_id_, parts_, part_count_, flags_, send_scope);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_send_prefixed (socket_base_t *socket_,
                                            const void *prefix_data_,
                                            size_t prefix_size_,
                                            zlink_msg_t *parts_,
                                            size_t part_count_,
                                            int flags_)
{
    if (!socket_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, false);
    if (!send_scope.acquired ())
        return -1;

    return send_prefixed_once (socket_, prefix_data_, prefix_size_, 0, false, parts_, part_count_,
                               flags_, send_scope);
}

int zlink::logical_multipart_send_prefixed_frame (socket_base_t *socket_,
                                                  const void *prefix_data_,
                                                  size_t prefix_size_,
                                                  int prefix_frame_flags_,
                                                  zlink_msg_t *parts_,
                                                  size_t part_count_,
                                                  int flags_)
{
    if (!socket_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, false);
    if (!send_scope.acquired ())
        return -1;

    return send_prefixed_once (socket_, prefix_data_, prefix_size_, prefix_frame_flags_, false,
                               parts_, part_count_, flags_, send_scope);
}

int zlink::logical_multipart_send_routed_prefixed_frame (socket_base_t *socket_,
                                                         const zlink_routing_id_t *routing_id_,
                                                         const void *prefix_data_,
                                                         size_t prefix_size_,
                                                         int prefix_frame_flags_,
                                                         zlink_msg_t *parts_,
                                                         size_t part_count_,
                                                         int flags_)
{
    if (!socket_ || !routing_id_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    zlink::msg_t routing_msg;
    if (routing_msg.init_size (routing_id_->size) != 0)
        return -1;
    if (routing_id_->size > 0)
        memcpy (routing_msg.data (), routing_id_->data, routing_id_->size);

    if (zlink::multipart_send_facade_t::send_scoped (socket_, &routing_msg, ZLINK_SNDMORE | flags_,
                                                     send_scope)
        != 0) {
        const int err = errno;
        (void) routing_msg.close ();
        errno = err;
        return -1;
    }
    (void) routing_msg.close ();

    if (send_prefixed_once (socket_, prefix_data_, prefix_size_, prefix_frame_flags_, true, parts_,
                            part_count_, flags_, send_scope)
        != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_send_prefixed_frames (socket_base_t *socket_,
                                                   const void *prefix1_data_,
                                                   size_t prefix1_size_,
                                                   int prefix1_frame_flags_,
                                                   const void *prefix2_data_,
                                                   size_t prefix2_size_,
                                                   int prefix2_frame_flags_,
                                                   zlink_msg_t *parts_,
                                                   size_t part_count_,
                                                   int flags_)
{
    if (!socket_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, false);
    if (!send_scope.acquired ())
        return -1;

    const prefix_frame_t prefixes[] = {{prefix1_data_, prefix1_size_, prefix1_frame_flags_},
                                       {prefix2_data_, prefix2_size_, prefix2_frame_flags_}};
    return send_prefix_sequence_once (socket_, prefixes, 2, false, parts_, part_count_, flags_,
                                      send_scope);
}

int zlink::logical_multipart_send_routed_prefixed_frames (socket_base_t *socket_,
                                                          const zlink_routing_id_t *routing_id_,
                                                          const void *prefix1_data_,
                                                          size_t prefix1_size_,
                                                          int prefix1_frame_flags_,
                                                          const void *prefix2_data_,
                                                          size_t prefix2_size_,
                                                          int prefix2_frame_flags_,
                                                          zlink_msg_t *parts_,
                                                          size_t part_count_,
                                                          int flags_)
{
    if (!socket_ || !routing_id_ || !parts_ || part_count_ == 0) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    const prefix_frame_t prefixes[] = {{routing_id_->data, routing_id_->size, 0},
                                       {prefix1_data_, prefix1_size_, prefix1_frame_flags_},
                                       {prefix2_data_, prefix2_size_, prefix2_frame_flags_}};
    if (send_prefix_sequence_once (socket_, prefixes, 3, false, parts_, part_count_, flags_,
                                   send_scope)
        != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_publish (socket_base_t *socket_,
                                      const char *topic_,
                                      zlink_msg_t *parts_,
                                      size_t part_count_,
                                      int flags_)
{
    if (!socket_ || (part_count_ > 0 && !parts_) || (!topic_ && part_count_ == 0)) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    const int rc = send_publish_once (socket_, topic_, parts_, part_count_, flags_, send_scope);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}

int zlink::logical_multipart_publish_frame (socket_base_t *socket_,
                                            zlink_msg_t *topic_frame_,
                                            zlink_msg_t *parts_,
                                            size_t part_count_,
                                            int flags_)
{
    if (!socket_ || !topic_frame_ || (part_count_ > 0 && !parts_)) {
        errno = EINVAL;
        return -1;
    }

    zlink::socket_public_send_scope_t send_scope =
      zlink::multipart_send_facade_t::make_scope (socket_, true);
    if (!send_scope.acquired ())
        return -1;

    const int rc =
      send_publish_frame_once (socket_, topic_frame_, parts_, part_count_, flags_, send_scope);
    if (rc != 0)
        return -1;

    errno = 0;
    return 0;
}
