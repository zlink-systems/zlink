/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_POLLER_HPP_INCLUDED__
#define __ZLINK_POLLER_HPP_INCLUDED__

//  ASIO is the only supported I/O poller backend
#include "engine/asio/asio_poller.hpp"

#if (defined ZLINK_POLL_BASED_ON_SELECT + defined ZLINK_POLL_BASED_ON_POLL) > 1
#error More than one of the ZLINK_POLL_BASED_ON_* macros defined
#elif (defined ZLINK_POLL_BASED_ON_SELECT + defined ZLINK_POLL_BASED_ON_POLL) == 0
#error None of the ZLINK_POLL_BASED_ON_* macros defined
#endif

#endif
