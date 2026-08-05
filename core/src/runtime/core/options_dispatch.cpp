/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include <string.h>

#include "core/options_dispatch_internal.hpp"
#include "core/options_owner.hpp"

int zlink::options_t::setsockopt (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_owner_of (option_)) {
        case options_owner_core_socket:
            if (options_setsockopt_core_socket (this, option_, optval_, optvallen_, is_int, value)
                == 0)
                return 0;
            break;
        case options_owner_transport_network:
            if (options_setsockopt_transport_network (this, option_, optval_, optvallen_, is_int,
                                                      value)
                == 0)
                return 0;
            break;
        case options_owner_protocol_metadata:
            if (options_setsockopt_protocol_metadata (this, option_, optval_, optvallen_, is_int,
                                                      value)
                == 0)
                return 0;
            break;
        case options_owner_socket_specific:
        case options_owner_unknown:
        default:
            break;
    }

    errno = EINVAL;
    return -1;
}

int zlink::options_t::getsockopt (int option_, void *optval_, size_t *optvallen_) const
{
    const bool is_int = (*optvallen_ == sizeof (int));
    int *value = static_cast<int *> (optval_);

    switch (option_owner_of (option_)) {
        case options_owner_core_socket:
            if (options_getsockopt_core_socket (this, option_, optval_, optvallen_, is_int, value)
                == 0)
                return 0;
            break;
        case options_owner_transport_network:
            if (options_getsockopt_transport_network (this, option_, optval_, optvallen_, is_int,
                                                      value)
                == 0)
                return 0;
            break;
        case options_owner_protocol_metadata:
            if (options_getsockopt_protocol_metadata (this, option_, optval_, optvallen_, is_int,
                                                      value)
                == 0)
                return 0;
            break;
        case options_owner_socket_specific:
        case options_owner_unknown:
        default:
            break;
    }

    errno = EINVAL;
    return -1;
}
