/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_OPTIONS_DISPATCH_INTERNAL_HPP_INCLUDED__
#define __ZLINK_OPTIONS_DISPATCH_INTERNAL_HPP_INCLUDED__

#include <string>
#include <string.h>

#include "core/options.hpp"
#include "utils/err.hpp"

namespace zlink
{
inline int options_sockopt_invalid ()
{
#if defined(ZLINK_ACT_MILITANT)
    zlink_assert (false);
#endif
    errno = EINVAL;
    return -1;
}

template <typename T>
inline int options_do_setsockopt_value (const void *optval_, size_t optvallen_, T *out_value_)
{
    if (optvallen_ == sizeof (T)) {
        memcpy (out_value_, optval_, sizeof (T));
        return 0;
    }
    return options_sockopt_invalid ();
}

inline int options_do_setsockopt_string_allow_empty_strict (const void *optval_,
                                                            size_t optvallen_,
                                                            std::string *out_value_,
                                                            size_t max_len_)
{
    if (optval_ == NULL && optvallen_ == 0) {
        out_value_->clear ();
        return 0;
    }
    if (optval_ != NULL && optvallen_ > 0 && optvallen_ <= max_len_) {
        out_value_->assign (static_cast<const char *> (optval_), optvallen_);
        return 0;
    }
    return options_sockopt_invalid ();
}

int options_setsockopt_core_socket (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_);
int options_getsockopt_core_socket (const options_t *self_,
                                    int option_,
                                    void *optval_,
                                    size_t *optvallen_,
                                    bool is_int_,
                                    int *value_);

int options_setsockopt_transport_network (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_);
int options_getsockopt_transport_network (const options_t *self_,
                                          int option_,
                                          void *optval_,
                                          size_t *optvallen_,
                                          bool is_int_,
                                          int *value_);

int options_setsockopt_protocol_metadata (
  options_t *self_, int option_, const void *optval_, size_t optvallen_, bool is_int_, int value_);
int options_getsockopt_protocol_metadata (const options_t *self_,
                                          int option_,
                                          void *optval_,
                                          size_t *optvallen_,
                                          bool is_int_,
                                          int *value_);
}

#endif
