/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_RANDOM_HPP_INCLUDED__
#define __ZLINK_RANDOM_HPP_INCLUDED__

#include "utils/stdint.hpp"
#include <stddef.h>

namespace zlink
{
//  Seeds the random number generator.
void seed_random ();

//  Generates random value.
uint32_t generate_random ();

//  Fills a byte buffer with random data.
void generate_random_bytes (unsigned char *out_, size_t size_);

//  [De-]Initialise crypto library, if needed.
//  Serialised and refcounted, so that it can be called
//  from multiple threads, each with its own context, and from
//  the various zlink_utils helpers safely.
void random_open ();
void random_close ();
}

#endif
