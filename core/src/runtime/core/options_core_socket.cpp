/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <limits.h>

#include "core/auto_hwm_policy.hpp"
#include "core/options_dispatch_internal.hpp"

namespace
{
const int max_submit_retry_attempts = 16;
}

int zlink::options_setsockopt_core_socket (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_SNDHWM:
            if (optvallen_ == sizeof (uint64_t)) {
                memcpy (&self_->sndhwm, optval_, sizeof (self_->sndhwm));
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RCVHWM:
            if (optvallen_ == sizeof (uint64_t)) {
                memcpy (&self_->rcvhwm, optval_, sizeof (self_->rcvhwm));
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES:
            if (optvallen_ == sizeof (uint64_t)) {
                uint64_t value = 0;
                memcpy (&value, optval_, sizeof (value));
                self_->auto_hwm_msg_unit_bytes = value;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_AFFINITY:
            return options_do_setsockopt_value (optval_, optvallen_, &self_->affinity);

        case ZLINK_INTERNAL_OPT_ROUTING_ID:
            if (optvallen_ > 0 && optvallen_ <= UCHAR_MAX) {
                self_->routing_id_size = static_cast<unsigned char> (optvallen_);
                memcpy (self_->routing_id, optval_, self_->routing_id_size);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RATE:
            if (is_int_ && value_ > 0) {
                self_->rate = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RECOVERY_IVL:
            if (is_int_ && value_ >= 0) {
                self_->recovery_ivl = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_MAXMSGSIZE:
            return options_do_setsockopt_value (optval_, optvallen_, &self_->maxmsgsize);

        case ZLINK_INTERNAL_OPT_MULTICAST_HOPS:
            if (is_int_ && value_ > 0) {
                self_->multicast_hops = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_MULTICAST_MAXTPDU:
            if (is_int_ && value_ > 0) {
                self_->multicast_maxtpdu = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_IPV6:
            return do_setsockopt_int_as_bool_strict (optval_, optvallen_, &self_->ipv6);

        case ZLINK_INTERNAL_OPT_IMMEDIATE:
            if (is_int_ && (value_ == 0 || value_ == 1)) {
                self_->immediate = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_CONFLATE:
            return do_setsockopt_int_as_bool_strict (optval_, optvallen_, &self_->conflate);

        case ZLINK_INTERNAL_OPT_HANDSHAKE_IVL:
            if (is_int_ && value_ >= 0) {
                self_->handshake_ivl = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_INVERT_MATCHING:
            return do_setsockopt_int_as_bool_relaxed (optval_, optvallen_, &self_->invert_matching);

        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_MODE:
            if (is_int_
                && (value_ == ZLINK_SUBMIT_RETRY_OFF
                    || value_ == ZLINK_SUBMIT_RETRY_LOCAL_FAILURE)) {
                self_->submit_retry_mode = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_TIMEOUT:
            if (is_int_ && value_ >= 0) {
                self_->submit_retry_timeout = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS:
            if (is_int_ && value_ >= 0 && value_ <= max_submit_retry_attempts) {
                self_->submit_retry_attempts = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_STREAM_NOTIFY:
            return do_setsockopt_int_as_bool_strict (optval_, optvallen_, &self_->stream_notify);

        case ZLINK_INTERNAL_OPT_ZMP_METADATA:
            return do_setsockopt_int_as_bool_strict (optval_, optvallen_, &self_->zmp_metadata);

        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
            if (is_int_
                && (value_ == ZLINK_RID_DUPLICATE_REJECT
                    || value_ == ZLINK_RID_DUPLICATE_HANDOVER)) {
                self_->rid_duplicate_policy = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_PEER_WEIGHT:
            if (is_int_ && value_ >= 0 && value_ <= static_cast<int> (max_peer_weight)) {
                self_->peer_weight = value_;
                return 0;
            }
            break;

        default:
            break;
    }

    return -1;
}

int zlink::options_getsockopt_core_socket (
  const options_t *self_, int option_, void *optval_, size_t *optvallen_, bool is_int_, int *value_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_SNDHWM:
            if (*optvallen_ == sizeof (uint64_t)) {
                memcpy (optval_, &self_->sndhwm, sizeof (self_->sndhwm));
                *optvallen_ = sizeof (self_->sndhwm);
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RCVHWM:
            if (*optvallen_ == sizeof (uint64_t)) {
                memcpy (optval_, &self_->rcvhwm, sizeof (self_->rcvhwm));
                *optvallen_ = sizeof (self_->rcvhwm);
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_AUTO_HWM_MSG_UNIT_BYTES:
            if (*optvallen_ == sizeof (uint64_t)) {
                memcpy (optval_, &self_->auto_hwm_msg_unit_bytes,
                        sizeof (self_->auto_hwm_msg_unit_bytes));
                *optvallen_ = sizeof (self_->auto_hwm_msg_unit_bytes);
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_AFFINITY:
            if (*optvallen_ == sizeof (uint64_t)) {
                *(static_cast<uint64_t *> (optval_)) = self_->affinity;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_ROUTING_ID:
            return do_getsockopt (optval_, optvallen_, self_->routing_id, self_->routing_id_size);
        case ZLINK_INTERNAL_OPT_RATE:
            if (is_int_) {
                *value_ = self_->rate;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RECOVERY_IVL:
            if (is_int_) {
                *value_ = self_->recovery_ivl;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TYPE:
            if (is_int_) {
                *value_ = self_->type;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_MAXMSGSIZE:
            if (*optvallen_ == sizeof (int64_t)) {
                *(static_cast<int64_t *> (optval_)) = self_->maxmsgsize;
                *optvallen_ = sizeof (int64_t);
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_MULTICAST_HOPS:
            if (is_int_) {
                *value_ = self_->multicast_hops;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_MULTICAST_MAXTPDU:
            if (is_int_) {
                *value_ = self_->multicast_maxtpdu;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_IPV6:
            if (is_int_) {
                *value_ = self_->ipv6;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_IMMEDIATE:
            if (is_int_) {
                *value_ = self_->immediate;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_CONFLATE:
            if (is_int_) {
                *value_ = self_->conflate;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_HANDSHAKE_IVL:
            if (is_int_) {
                *value_ = self_->handshake_ivl;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_INVERT_MATCHING:
            if (is_int_) {
                *value_ = self_->invert_matching;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_MODE:
            if (is_int_) {
                *value_ = self_->submit_retry_mode;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_TIMEOUT:
            if (is_int_) {
                *value_ = self_->submit_retry_timeout;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_SUBMIT_RETRY_ATTEMPTS:
            if (is_int_) {
                *value_ = self_->submit_retry_attempts;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_STREAM_NOTIFY:
            if (is_int_) {
                *value_ = self_->stream_notify ? 1 : 0;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_ZMP_METADATA:
            if (is_int_) {
                *value_ = self_->zmp_metadata ? 1 : 0;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RID_DUPLICATE_POLICY:
            if (is_int_) {
                *value_ = self_->rid_duplicate_policy;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_PEER_WEIGHT:
            if (is_int_) {
                *value_ = self_->peer_weight;
                return 0;
            }
            break;
        default:
            break;
    }

    return -1;
}
