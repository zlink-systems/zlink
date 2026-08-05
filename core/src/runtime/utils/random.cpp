/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if !defined ZLINK_HAVE_WINDOWS
#include <unistd.h>
#include <fcntl.h>
#if defined ZLINK_HAVE_GETRANDOM
#include <sys/random.h>
#endif
#endif

#include "utils/random.hpp"
#include "utils/stdint.hpp"
#include "utils/clock.hpp"
#include "utils/mutex.hpp"
#include "utils/macros.hpp"

namespace
{
zlink::mutex_t random_init_sync;
unsigned int random_init_refcount = 0;
bool random_seeded = false;
}

void zlink::seed_random ()
{
#if defined ZLINK_HAVE_WINDOWS
    const int pid = static_cast<int> (GetCurrentProcessId ());
#else
    int pid = static_cast<int> (getpid ());
#endif
    srand (static_cast<unsigned int> (clock_t::now_us () + pid));
}

uint32_t zlink::generate_random ()
{
    const uint32_t low = static_cast<uint32_t> (rand ());
    uint32_t high = static_cast<uint32_t> (rand ());
    high <<= (sizeof (int) * 8 - 1);
    return high | low;
}

void zlink::generate_random_bytes (unsigned char *out_, size_t size_)
{
    if (!out_ || size_ == 0)
        return;

    size_t offset = 0;

#if !defined ZLINK_HAVE_WINDOWS && defined ZLINK_HAVE_GETRANDOM
    while (offset < size_) {
        const ssize_t rc = getrandom (out_ + offset, size_ - offset, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (rc == 0)
            break;
        offset += static_cast<size_t> (rc);
    }
#endif

#if !defined ZLINK_HAVE_WINDOWS
    if (offset < size_) {
        const int fd = open ("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            while (offset < size_) {
                const ssize_t rc = read (fd, out_ + offset, size_ - offset);
                if (rc < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (rc == 0)
                    break;
                offset += static_cast<size_t> (rc);
            }
            close (fd);
        }
    }
#endif

    if (offset < size_) {
        static mutex_t random_sync;
        scoped_lock_t lock (random_sync);
        while (offset < size_) {
            const uint32_t rnd = generate_random ();
            const size_t remaining = size_ - offset;
            const size_t chunk = remaining < sizeof (rnd) ? remaining : sizeof (rnd);
            memcpy (out_ + offset, &rnd, chunk);
            offset += chunk;
        }
    }
}

void zlink::random_open ()
{
    scoped_lock_t lock (random_init_sync);
    if (!random_seeded) {
        seed_random ();
        random_seeded = true;
    }
    ++random_init_refcount;
}

void zlink::random_close ()
{
    scoped_lock_t lock (random_init_sync);
    if (random_init_refcount > 0)
        --random_init_refcount;
}
