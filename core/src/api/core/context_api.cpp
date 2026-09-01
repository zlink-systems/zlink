/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/core/close_result_internal.hpp"
#include "api/core/config_result_internal.hpp"
#include "core/ctx.hpp"
#include "utils/err.hpp"
#include "utils/ip.hpp"

#include <algorithm>

int zlink_ctx_set_ext (void *ctx_, int option_, const void *optval_, size_t optvallen_);

namespace
{
struct public_ctx_option_descriptor_t
{
    zlink_ctx_option_t option;
    bool can_set;
    bool can_get;
};

static const public_ctx_option_descriptor_t public_ctx_options[] = {
  {ZLINK_IO_THREADS, true, true},
  {ZLINK_MAX_SOCKETS, true, true},
  {ZLINK_SOCKET_LIMIT, false, true},
  {ZLINK_THREAD_PRIORITY, true, true},
  {ZLINK_THREAD_SCHED_POLICY, true, true},
  {ZLINK_MAX_MSGSZ, true, true},
  {ZLINK_MSG_T_SIZE, false, true},
  {ZLINK_THREAD_AFFINITY_CPU_ADD, true, true},
  {ZLINK_THREAD_AFFINITY_CPU_REMOVE, true, true},
  {ZLINK_THREAD_NAME_PREFIX, false, false},
  {ZLINK_CTX_OPT_BLOCKY, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_ENABLE, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_PROFILE, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES, true, true},
  {ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES, true, true},
};

static const public_ctx_option_descriptor_t *find_public_ctx_option (int option_)
{
    for (size_t i = 0; i < sizeof (public_ctx_options) / sizeof (public_ctx_options[0]); ++i) {
        if (public_ctx_options[i].option == option_)
            return &public_ctx_options[i];
    }
    return NULL;
}

static bool is_public_ctx_set_option (int option_)
{
    const public_ctx_option_descriptor_t *descriptor = find_public_ctx_option (option_);
    return descriptor && descriptor->can_set;
}

static bool is_public_ctx_get_option (int option_)
{
    const public_ctx_option_descriptor_t *descriptor = find_public_ctx_option (option_);
    return descriptor && descriptor->can_get;
}

static bool is_auto_hwm_u64_option (int option_)
{
    return option_ == ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES
           || option_ == ZLINK_CTX_OPT_AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES
           || option_ == ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES;
}

static bool is_ctx_data_option (int option_)
{
    return option_ == ZLINK_THREAD_NAME_PREFIX
           || is_auto_hwm_u64_option (option_);
}
}

void zlink_version (int *major_, int *minor_, int *patch_)
{
    *major_ = ZLINK_VERSION_MAJOR;
    *minor_ = ZLINK_VERSION_MINOR;
    *patch_ = ZLINK_VERSION_PATCH;
}

const char *zlink_strerror (int errnum_)
{
    return zlink::errno_to_string (errnum_);
}

int zlink_errno (void)
{
    return errno;
}

void *zlink_ctx_new (void)
{
    if (!zlink::initialize_network ()) {
        return NULL;
    }

    zlink::ctx_t *ctx = new (std::nothrow) zlink::ctx_t;
    if (!ctx) {
        errno = ENOMEM;
        return NULL;
    }
    if (!ctx->valid ()) {
        delete ctx;
        errno = ENOMEM;
        return NULL;
    }
    return ctx;
}

zlink_close_result_t zlink_ctx_term (void *ctx_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CLOSE_INVALID_HANDLE;
    }

    const int rc = (static_cast<zlink::ctx_t *> (ctx_))->terminate ();
    const int en = errno;

    if (!rc || en != EINTR) {
        zlink::shutdown_network ();
    }

    errno = en;
    return zlink::close_result_internal::from_rc (rc);
}

zlink_close_result_t zlink_ctx_shutdown (void *ctx_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CLOSE_INVALID_HANDLE;
    }
    return zlink::close_result_internal::from_rc (
      (static_cast<zlink::ctx_t *> (ctx_))->shutdown ());
}

zlink_config_result_t zlink_ctx_set (void *ctx_, zlink_ctx_option_t option_, int optval_)
{
    if (!is_public_ctx_set_option (option_) || is_auto_hwm_u64_option (option_)) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    return zlink::config_result_internal::from_rc (
      zlink_ctx_set_ext (ctx_, option_, &optval_, sizeof (int)));
}

zlink_config_result_t
zlink_ctx_set_data (void *ctx_, zlink_ctx_option_t option_, const void *optval_, size_t optvallen_)
{
    if (!is_ctx_data_option (option_) || !optval_
        || (is_auto_hwm_u64_option (option_)
            && optvallen_ != sizeof (uint64_t))
        || (option_ == ZLINK_THREAD_NAME_PREFIX
            && (optvallen_ == 0
                || static_cast<const unsigned char *> (optval_)[optvallen_ - 1]
                     != '\0'))) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    return zlink::config_result_internal::from_rc (
      zlink_ctx_set_ext (ctx_, option_, optval_, optvallen_));
}

zlink_config_result_t
zlink_ctx_get_data (void *ctx_, zlink_ctx_option_t option_, void *optval_, size_t *optvallen_)
{
    if (!is_ctx_data_option (option_) || !optval_ || !optvallen_) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    if (is_auto_hwm_u64_option (option_) && *optvallen_ != sizeof (uint64_t)) {
        *optvallen_ = sizeof (uint64_t);
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    const int rc = (static_cast<zlink::ctx_t *> (ctx_))->get (option_, optval_, optvallen_);
    if (rc == 0 && is_auto_hwm_u64_option (option_))
        *optvallen_ = sizeof (uint64_t);
    return zlink::config_result_internal::from_rc (rc);
}

int zlink_ctx_set_ext (void *ctx_, int option_, const void *optval_, size_t optvallen_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return -1;
    }
    //  C ABI seal: option handling may reach allocating runtime paths
    //  (auto-HWM recalc task registration); allocation failure surfaces as
    //  a config error, never as an escaped exception.
    try {
        return (static_cast<zlink::ctx_t *> (ctx_))->set (option_, optval_, optvallen_);
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return -1;
    }
}

int zlink_ctx_get (void *ctx_, zlink_ctx_option_t option_, zlink_config_result_t *error_out_)
{
    if (!is_public_ctx_get_option (option_) || is_auto_hwm_u64_option (option_)) {
        errno = EINVAL;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_ARGUMENT;
        return -1;
    }
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_INVALID_HANDLE;
        return -1;
    }
    const int result = (static_cast<zlink::ctx_t *> (ctx_))->get (option_);
    if (result < 0) {
        if (error_out_)
            *error_out_ = zlink::config_result_internal::from_errno (errno);
    } else {
        if (error_out_)
            *error_out_ = ZLINK_CONFIG_OK;
    }
    return result;
}

zlink_config_result_t zlink_ctx_auto_hwm_recalculate (void *ctx_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    //  Same C ABI seal as zlink_ctx_set_ext.
    try {
        return zlink::config_result_internal::from_rc (
          (static_cast<zlink::ctx_t *> (ctx_))->auto_hwm_recalculate_now ());
    }
    catch (const std::bad_alloc &) {
        errno = ENOMEM;
        return ZLINK_CONFIG_INTERNAL_ERROR;
    }
}

zlink_config_result_t zlink_ctx_get_auto_hwm_budget_snapshot (
  void *ctx_, zlink_auto_hwm_budget_snapshot_t *snapshot_)
{
    if (!snapshot_ || snapshot_->struct_size < sizeof (uint32_t) * 2u) {
        errno = EINVAL;
        return ZLINK_CONFIG_INVALID_ARGUMENT;
    }
    if (snapshot_->abi_version != ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1) {
        errno = ENOTSUP;
        return ZLINK_CONFIG_NOT_SUPPORTED;
    }
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }

    zlink_auto_hwm_budget_snapshot_t current;
    const uint32_t caller_size = snapshot_->struct_size;
    const int rc =
      (static_cast<zlink::ctx_t *> (ctx_))->auto_hwm_budget_snapshot (&current);
    if (rc != 0)
        return zlink::config_result_internal::from_errno (errno);

    const size_t copy_size =
      std::min<size_t> (caller_size, sizeof (zlink_auto_hwm_budget_snapshot_t));
    memcpy (snapshot_, &current, copy_size);
    return ZLINK_CONFIG_OK;
}

zlink_config_result_t zlink_ctx_reset_auto_hwm_budget_metrics (void *ctx_)
{
    if (!ctx_ || !(static_cast<zlink::ctx_t *> (ctx_))->check_tag ()) {
        errno = EFAULT;
        return ZLINK_CONFIG_INVALID_HANDLE;
    }
    return zlink::config_result_internal::from_rc (
      (static_cast<zlink::ctx_t *> (ctx_))->reset_auto_hwm_budget_metrics ());
}
