/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/options_dispatch_internal.hpp"

int zlink::options_setsockopt_protocol_metadata (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_)
{
    switch (option_) {
#ifdef ZLINK_HAVE_TLS
        case ZLINK_INTERNAL_OPT_TLS_CERT:
            return options_do_setsockopt_string_allow_empty_strict (optval_, optvallen_,
                                                                    &self_->tls_cert, 256);
        case ZLINK_INTERNAL_OPT_TLS_KEY:
            return options_do_setsockopt_string_allow_empty_strict (optval_, optvallen_,
                                                                    &self_->tls_key, 256);
        case ZLINK_INTERNAL_OPT_TLS_CA:
            return options_do_setsockopt_string_allow_empty_strict (optval_, optvallen_,
                                                                    &self_->tls_ca, 256);
        case ZLINK_INTERNAL_OPT_TLS_VERIFY:
            if (is_int_ && (value_ == 0 || value_ == 1)) {
                self_->tls_verify = value_;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT:
            if (is_int_ && (value_ == 0 || value_ == 1)) {
                self_->tls_require_client_cert = value_;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_HOSTNAME:
            return options_do_setsockopt_string_allow_empty_strict (optval_, optvallen_,
                                                                    &self_->tls_hostname, 256);
        case ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM:
            if (is_int_ && (value_ == 0 || value_ == 1)) {
                self_->tls_trust_system = value_;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_PASSWORD:
            return options_do_setsockopt_string_allow_empty_strict (optval_, optvallen_,
                                                                    &self_->tls_password, 256);
#endif

        default:
            break;
    }

    return -1;
}

int zlink::options_getsockopt_protocol_metadata (
  const options_t *self_, int option_, void *optval_, size_t *optvallen_, bool is_int_, int *value_)
{
    switch (option_) {
#ifdef ZLINK_HAVE_TLS
        case ZLINK_INTERNAL_OPT_TLS_CERT:
            return do_getsockopt (optval_, optvallen_, self_->tls_cert);
        case ZLINK_INTERNAL_OPT_TLS_KEY:
            return do_getsockopt (optval_, optvallen_, self_->tls_key);
        case ZLINK_INTERNAL_OPT_TLS_CA:
            return do_getsockopt (optval_, optvallen_, self_->tls_ca);
        case ZLINK_INTERNAL_OPT_TLS_VERIFY:
            if (is_int_) {
                *value_ = self_->tls_verify;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_REQUIRE_CLIENT_CERT:
            if (is_int_) {
                *value_ = self_->tls_require_client_cert;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_HOSTNAME:
            return do_getsockopt (optval_, optvallen_, self_->tls_hostname);
        case ZLINK_INTERNAL_OPT_TLS_TRUST_SYSTEM:
            if (is_int_) {
                *value_ = self_->tls_trust_system;
                return 0;
            }
            break;
        case ZLINK_INTERNAL_OPT_TLS_PASSWORD:
            return do_getsockopt (optval_, optvallen_, self_->tls_password);
#endif

        default:
            break;
    }

    return -1;
}
