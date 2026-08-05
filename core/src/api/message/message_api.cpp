/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <zlink.h>

#include "api/core/config_result_internal.hpp"
#include "core/msg.hpp"
#include "core/recv_tls_view.hpp"

namespace
{
zlink::msg_t *checked_message (zlink_msg_t *msg_)
{
    if (!msg_) {
        errno = EFAULT;
        return NULL;
    }
    zlink::msg_t *msg = reinterpret_cast<zlink::msg_t *> (msg_);
    if (unlikely (!msg->check ())) {
        errno = EFAULT;
        return NULL;
    }
    return msg;
}

const zlink::msg_t *checked_message (const zlink_msg_t *msg_)
{
    if (!msg_) {
        errno = EFAULT;
        return NULL;
    }
    const zlink::msg_t *msg = reinterpret_cast<const zlink::msg_t *> (msg_);
    if (unlikely (!msg->check ())) {
        errno = EFAULT;
        return NULL;
    }
    return msg;
}
}

zlink_config_result_t zlink_msg_init (zlink_msg_t *msg_)
{
    if (!msg_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (msg_))->init ());
}

zlink_config_result_t zlink_msg_init_size (zlink_msg_t *msg_, size_t size_)
{
    if (!msg_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (msg_))->init_size (size_));
}

zlink_config_result_t zlink_msg_init_buffer (zlink_msg_t *msg_, const void *buf_, size_t size_)
{
    if (!msg_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (msg_))->init_buffer (buf_, size_));
}

zlink_config_result_t
zlink_msg_init_data (zlink_msg_t *msg_, void *data_, size_t size_, zlink_free_fn *ffn_, void *hint_)
{
    if (!msg_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (msg_))->init_data (data_, size_, ffn_, hint_));
}

zlink_config_result_t zlink_msg_close (zlink_msg_t *msg_)
{
    zlink::msg_t *msg = checked_message (msg_);
    if (!msg)
        return ZLINK_CONFIG_INVALID_HANDLE;
    return zlink::config_result_internal::from_rc (msg->close ());
}

zlink_config_result_t zlink_msg_move (zlink_msg_t *dest_, zlink_msg_t *src_)
{
    if (!dest_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    zlink::msg_t *src = checked_message (src_);
    if (!src)
        return ZLINK_CONFIG_INVALID_HANDLE;
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (dest_))->move (*src));
}

zlink_config_result_t zlink_msg_copy (zlink_msg_t *dest_, zlink_msg_t *src_)
{
    if (!dest_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    zlink::msg_t *src = checked_message (src_);
    if (!src)
        return ZLINK_CONFIG_INVALID_HANDLE;
    return zlink::config_result_internal::from_rc (
      (reinterpret_cast<zlink::msg_t *> (dest_))->copy (*src));
}

zlink_config_result_t zlink_msg_adopt (zlink_msg_t *dest_, zlink_msg_t *src_)
{
    if (!dest_ || !src_) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    zlink::msg_t *dest = reinterpret_cast<zlink::msg_t *> (dest_);
    zlink::msg_t *src = reinterpret_cast<zlink::msg_t *> (src_);
    if (unlikely (!src->check ())) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    *dest = *src;
    return zlink::config_result_internal::from_rc (src->init ());
}

void *zlink_msg_data (zlink_msg_t *msg_)
{
    zlink::msg_t *msg = checked_message (msg_);
    return msg ? msg->data () : NULL;
}

size_t zlink_msg_size (const zlink_msg_t *msg_)
{
    const zlink::msg_t *msg = checked_message (msg_);
    return msg ? msg->size () : 0;
}

int zlink_msg_refcnt (const zlink_msg_t *msg_, zlink_config_result_t *error_out_)
{
    const zlink::msg_t *msg = checked_message (msg_);
    if (!msg) {
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_HANDLE;
        return -1;
    }
    if (error_out_)
        *error_out_ = ZLINK_CONFIG_OK;
    return static_cast<int> (msg->refcnt_value ());
}


void zlink_multipart_close (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return;

    if (zlink::recv_tls_view::release_closed_prefix (parts_, part_count_))
        return;

    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_close (&parts_[i]);
}
