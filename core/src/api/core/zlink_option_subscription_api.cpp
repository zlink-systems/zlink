/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/config_result_internal.hpp"
#include "api/core/zlink_option_internal.hpp"

#include "core/msg.hpp"
#include "sockets/pubsub/xsub.hpp"

#include <algorithm>
#include <string>
#include <string.h>
#include <vector>

namespace
{
int xsub_update_subscription (zlink::socket_base_t *socket_,
                              bool subscribe_,
                              const void *optval_,
                              size_t optvallen_)
{
    if (!socket_ || !optval_) {
        errno = EINVAL;
        return -1;
    }

    zlink::msg_t msg;
    if (msg.init_size (optvallen_ + 1) != 0)
        return -1;
    unsigned char *data = static_cast<unsigned char *> (msg.data ());
    data[0] = subscribe_ ? 1 : 0;
    if (optvallen_ > 0)
        memcpy (data + 1, optval_, optvallen_);
    const int rc = socket_->send (&msg, 0);
    if (rc != 0)
        msg.close ();
    return rc;
}

struct subscription_snapshot_entry_t
{
    subscription_snapshot_entry_t () : is_pattern (false) {}

    std::string filter;
    bool is_pattern;
};

struct raw_subscription_less_t
{
    bool operator() (const zlink::xsub_t::subscription_descriptor_t &lhs_,
                     const zlink::xsub_t::subscription_descriptor_t &rhs_) const
    {
        if (lhs_.filter != rhs_.filter)
            return lhs_.filter < rhs_.filter;
        return lhs_.is_pattern < rhs_.is_pattern;
    }
};

int copy_subscription_entry (const subscription_snapshot_entry_t &entry_,
                             char *filter_out_,
                             size_t *filter_len_inout_,
                             int *is_pattern_out_)
{
    if (!filter_len_inout_) {
        errno = EFAULT;
        return -1;
    }
    if (!filter_out_ && *filter_len_inout_ != 0) {
        errno = EFAULT;
        return -1;
    }
    if (*filter_len_inout_ < entry_.filter.size ()) {
        *filter_len_inout_ = entry_.filter.size ();
        errno = EINVAL;
        return -1;
    }
    if (filter_out_ && !entry_.filter.empty ())
        memcpy (filter_out_, entry_.filter.data (), entry_.filter.size ());
    *filter_len_inout_ = entry_.filter.size ();
    if (is_pattern_out_)
        *is_pattern_out_ = entry_.is_pattern ? 1 : 0;
    return 0;
}

int raw_socket_subscription_at (zlink::socket_base_t *socket_,
                                size_t index_,
                                char *filter_out_,
                                size_t *filter_len_inout_,
                                int *is_pattern_out_)
{
    if (!socket_) {
        errno = EFAULT;
        return -1;
    }

    std::vector<zlink::xsub_t::subscription_descriptor_t> entries;
    static_cast<zlink::xsub_t *> (socket_)->snapshot_subscriptions (&entries);
    std::sort (entries.begin (), entries.end (), raw_subscription_less_t ());

    if (index_ >= entries.size ()) {
        errno = ENOENT;
        return -1;
    }

    subscription_snapshot_entry_t entry;
    entry.filter = entries[index_].filter;
    entry.is_pattern = entries[index_].is_pattern;
    return copy_subscription_entry (entry, filter_out_, filter_len_inout_, is_pattern_out_);
}
}

zlink_config_result_t zlink_set_subscription (void *handle_, const char *filter_)
{
    if (zlink::socket_base_t *socket = as_socket (handle_)) {
        if (!filter_) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        const int type = socket_type_of (socket);
        if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        const size_t filter_len = strlen (filter_);
        if (type == ZLINK_CORE_SOCKET_XSUB)
            return zlink::config_result_internal::from_rc (
              xsub_update_subscription (socket, true, filter_, filter_len));
        return zlink::config_result_internal::from_rc (
          socket->setsockopt (ZLINK_INTERNAL_OPT_SUBSCRIBE, filter_, filter_len));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t zlink_unset_subscription (void *handle_, const char *filter_)
{
    if (zlink::socket_base_t *socket = as_socket (handle_)) {
        if (!filter_) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        const int type = socket_type_of (socket);
        if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        const size_t filter_len = strlen (filter_);
        if (type == ZLINK_CORE_SOCKET_XSUB)
            return zlink::config_result_internal::from_rc (
              xsub_update_subscription (socket, false, filter_, filter_len));
        return zlink::config_result_internal::from_rc (
          socket->setsockopt (ZLINK_INTERNAL_OPT_UNSUBSCRIBE, filter_, filter_len));
    }
    return zlink::config_result_internal::from_errno (errno);
}

zlink_config_result_t zlink_subscription_at (
  void *handle_, size_t index_, char *filter_out_, size_t *filter_len_inout_, int *is_pattern_out_)
{
    if (zlink::socket_base_t *socket = as_socket (handle_)) {
        const int type = socket_type_of (socket);
        if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
            errno = EINVAL;
            return ZLINK_CONFIG_INVALID_ARGUMENT;
        }
        return zlink::config_result_internal::from_rc (raw_socket_subscription_at (
          socket, index_, filter_out_, filter_len_inout_, is_pattern_out_));
    }
    return zlink::config_result_internal::from_errno (errno);
}
