/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_WSS_ADDRESS_HPP_INCLUDED__
#define __ZLINK_WSS_ADDRESS_HPP_INCLUDED__

#include "platform.hpp"

#ifdef ZLINK_HAVE_WSS

#include "transports/ws/ws_address.hpp"

namespace zlink
{

//  Secure WebSocket address class for wss:// protocol
//  Inherits from ws_address_t, only differs in the protocol prefix
class wss_address_t : public ws_address_t
{
  public:
    wss_address_t ();
    wss_address_t (const sockaddr *sa_, socklen_t sa_len_);
    ~wss_address_t ();

    //  Override to_string to use wss:// prefix
    int to_string (std::string &addr_) const;
};

} // namespace zlink

#endif // ZLINK_HAVE_WSS

#endif // __ZLINK_WSS_ADDRESS_HPP_INCLUDED__
