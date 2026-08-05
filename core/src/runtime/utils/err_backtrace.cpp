/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/err.hpp"

#if defined(HAVE_LIBUNWIND) && !defined(__SUNPRO_CC)

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <dlfcn.h>
#include <cxxabi.h>

#include "utils/mutex.hpp"

void zlink::print_backtrace (void)
{
    static zlink::mutex_t mtx;
    mtx.lock ();
    Dl_info dl_info;
    unw_cursor_t cursor;
    unw_context_t ctx;
    unsigned frame_n = 0;

    unw_getcontext (&ctx);
    unw_init_local (&cursor, &ctx);

    while (unw_step (&cursor) > 0) {
        unw_word_t offset;
        unw_proc_info_t p_info;
        static const char unknown[] = "?";
        const char *file_name;
        char *demangled_name;
        char func_name[256] = "";
        void *addr;
        int rc;

        if (unw_get_proc_info (&cursor, &p_info))
            break;

        rc = unw_get_proc_name (&cursor, func_name, 256, &offset);
        if (rc == -UNW_ENOINFO)
            memcpy (func_name, unknown, sizeof unknown);

        addr = (void *) (p_info.start_ip + offset);

        if (dladdr (addr, &dl_info) && dl_info.dli_fname)
            file_name = dl_info.dli_fname;
        else
            file_name = unknown;

        demangled_name = abi::__cxa_demangle (func_name, NULL, NULL, &rc);

        printf ("#%u  %p in %s (%s+0x%lx)\n", frame_n++, addr, file_name,
                rc ? func_name : demangled_name, (unsigned long) offset);
        free (demangled_name);
    }
    puts ("");

    fflush (stdout);
    mtx.unlock ();
}

#else

void zlink::print_backtrace ()
{
}

#endif
