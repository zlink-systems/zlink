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

//  rand() is the last-resort source only. It carries no cryptographic
//  strength anywhere, and on Windows the CRT keeps its state per thread, so
//  two threads that never called srand() emit the identical sequence. Every
//  platform therefore resolves a real entropy source first.
uint32_t weak_random ()
{
    const uint32_t low = static_cast<uint32_t> (rand ());
    uint32_t high = static_cast<uint32_t> (rand ());
    high <<= (sizeof (int) * 8 - 1);
    return high | low;
}

#if defined ZLINK_HAVE_WINDOWS
typedef BOOLEAN (WINAPI *rtl_gen_random_fn_t) (PVOID, ULONG);

rtl_gen_random_fn_t resolve_rtl_gen_random ()
{
    //  RtlGenRandom is the system entropy source every Windows CRT and
    //  bcrypt path ultimately reaches. Resolving it dynamically keeps the
    //  library free of an extra link dependency.
    const HMODULE advapi = LoadLibraryA ("advapi32.dll");
    if (!advapi)
        return NULL;
    return reinterpret_cast<rtl_gen_random_fn_t> (
      reinterpret_cast<void *> (GetProcAddress (advapi, "SystemFunction036")));
}
#endif
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
    uint32_t value = 0;
    generate_random_bytes (reinterpret_cast<unsigned char *> (&value),
                           sizeof (value));
    return value;
}

void zlink::generate_random_bytes (unsigned char *out_, size_t size_)
{
    if (!out_ || size_ == 0)
        return;

    size_t offset = 0;

#if defined ZLINK_HAVE_WINDOWS
    {
        static const rtl_gen_random_fn_t rtl_gen_random =
          resolve_rtl_gen_random ();
        while (rtl_gen_random && offset < size_) {
            const size_t remaining = size_ - offset;
            const ULONG chunk = remaining > 0x40000000u
                                  ? 0x40000000u
                                  : static_cast<ULONG> (remaining);
            if (!rtl_gen_random (out_ + offset, chunk))
                break;
            offset += chunk;
        }
    }
#endif

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
            const uint32_t rnd = weak_random ();
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
