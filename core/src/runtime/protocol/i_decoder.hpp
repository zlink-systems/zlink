/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_I_DECODER_HPP_INCLUDED__
#define __ZLINK_I_DECODER_HPP_INCLUDED__

#include "utils/macros.hpp"
#include "utils/stdint.hpp"

namespace zlink
{
class msg_t;

//  Interface to be implemented by message decoder.

class i_decoder
{
  public:
    virtual ~i_decoder () ZLINK_DEFAULT;

    virtual void get_buffer (unsigned char **data_, size_t *size_) = 0;

    virtual void resize_buffer (size_t) = 0;
    //  Decodes data pointed to by data_.
    //  When a message is decoded, 1 is returned.
    //  When the decoder needs more data, 0 is returned.
    //  On error, -1 is returned and errno is set accordingly.
    virtual int decode (const unsigned char *data_, size_t size_, size_t &processed_) = 0;

    virtual msg_t *msg () = 0;

    //  Notify a framed decoder that its underlying byte stream ended. Stream
    //  decoders that do not need finalization keep the default no-op behavior.
    virtual int stream_end () { return 0; }
};
}

#endif
