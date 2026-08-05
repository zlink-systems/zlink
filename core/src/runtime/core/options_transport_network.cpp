/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/options_dispatch_internal.hpp"

#ifndef ZLINK_HAVE_WINDOWS
#include <net/if.h>
#endif

#if defined IFNAMSIZ
#define BINDDEVSIZ IFNAMSIZ
#else
#define BINDDEVSIZ 16
#endif

int zlink::options_setsockopt_transport_network (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_SNDBUF:
            if (is_int_ && value_ >= -1) {
                self_->sndbuf = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RCVBUF:
            if (is_int_ && value_ >= -1) {
                self_->rcvbuf = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TOS:
            if (is_int_ && value_ >= 0) {
                self_->tos = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_LINGER:
            if (is_int_ && value_ >= -1) {
                self_->linger.store (value_);
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_CONNECT_TIMEOUT:
            if (is_int_ && value_ >= 0) {
                self_->connect_timeout = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_MAXRT:
            if (is_int_ && value_ >= 0) {
                self_->tcp_maxrt = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RECONNECT_IVL:
            if (is_int_ && value_ >= -1) {
                self_->reconnect_ivl = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RECONNECT_IVL_MAX:
            if (is_int_ && value_ >= 0) {
                self_->reconnect_ivl_max = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_BACKLOG:
            if (is_int_ && value_ >= 0) {
                self_->backlog = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_RCVTIMEO:
            if (is_int_ && value_ >= -1) {
                self_->rcvtimeo = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_SNDTIMEO:
            if (is_int_ && value_ >= -1) {
                self_->sndtimeo = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE:
            if (is_int_ && (value_ == -1 || value_ == 0 || value_ == 1)) {
                self_->tcp_keepalive = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_CNT:
            if (is_int_ && (value_ == -1 || value_ >= 0)) {
                self_->tcp_keepalive_cnt = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_IDLE:
            if (is_int_ && (value_ == -1 || value_ >= 0)) {
                self_->tcp_keepalive_idle = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_INTVL:
            if (is_int_ && (value_ == -1 || value_ >= 0)) {
                self_->tcp_keepalive_intvl = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_TCP_NODELAY:
            if (is_int_ && (value_ == -1 || value_ == 0 || value_ == 1)) {
                self_->tcp_nodelay = value_;
                return 0;
            }
            break;

        case ZLINK_INTERNAL_OPT_BINDTODEVICE:
            return options_do_setsockopt_string_allow_empty_strict (
              optval_, optvallen_, &self_->bound_device, BINDDEVSIZ);

        default:
            break;
    }

    return -1;
}

int zlink::options_getsockopt_transport_network (
  const options_t *self_, int option_, void *optval_, size_t *optvallen_, bool is_int_, int *value_)
{
    switch (option_) {
        case ZLINK_INTERNAL_OPT_SNDBUF:
            if (is_int_) {
                *value_ = self_->sndbuf;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RCVBUF:
            if (is_int_) {
                *value_ = self_->rcvbuf;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TOS:
            if (is_int_) {
                *value_ = self_->tos;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_LINGER:
            if (is_int_) {
                *value_ = self_->linger.load ();
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_CONNECT_TIMEOUT:
            if (is_int_) {
                *value_ = self_->connect_timeout;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_MAXRT:
            if (is_int_) {
                *value_ = self_->tcp_maxrt;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RECONNECT_IVL:
            if (is_int_) {
                *value_ = self_->reconnect_ivl;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RECONNECT_IVL_MAX:
            if (is_int_) {
                *value_ = self_->reconnect_ivl_max;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_BACKLOG:
            if (is_int_) {
                *value_ = self_->backlog;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_RCVTIMEO:
            if (is_int_) {
                *value_ = self_->rcvtimeo;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_SNDTIMEO:
            if (is_int_) {
                *value_ = self_->sndtimeo;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE:
            if (is_int_) {
                *value_ = self_->tcp_keepalive;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_CNT:
            if (is_int_) {
                *value_ = self_->tcp_keepalive_cnt;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_IDLE:
            if (is_int_) {
                *value_ = self_->tcp_keepalive_idle;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_KEEPALIVE_INTVL:
            if (is_int_) {
                *value_ = self_->tcp_keepalive_intvl;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TCP_NODELAY:
            if (is_int_) {
                *value_ = self_->tcp_nodelay;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_BINDTODEVICE:
            return do_getsockopt (optval_, optvallen_, self_->bound_device);
        default:
            break;
    }

    return -1;
}
