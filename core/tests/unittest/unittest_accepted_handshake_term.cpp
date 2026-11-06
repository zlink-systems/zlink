/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <cstdlib>
#include <future>
#include <new>
#include <thread>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
std::atomic<size_t> live_allocations (0);
}

// Unit tests link the static test-core archive on every platform. Keep every
// allocation form in the same malloc/free family so ASan observes matching
// allocation and deallocation origins.
namespace
{
void *allocate (size_t size_)
{
    void *const memory = std::malloc (size_ ? size_ : 1);
    if (!memory)
        throw std::bad_alloc ();
    live_allocations.fetch_add (1, std::memory_order_relaxed);
    return memory;
}

void *allocate_aligned (size_t size_, size_t alignment_)
{
#if defined(_WIN32)
    void *const memory = _aligned_malloc (size_ ? size_ : 1, alignment_);
#else
    void *memory = NULL;
    if (posix_memalign (&memory, alignment_, size_ ? size_ : 1) != 0)
        memory = NULL;
#endif
    if (!memory)
        throw std::bad_alloc ();
    live_allocations.fetch_add (1, std::memory_order_relaxed);
    return memory;
}

void free_aligned (void *memory_) noexcept
{
#if defined(_WIN32)
    _aligned_free (memory_);
#else
    std::free (memory_);
#endif
}
}

void *operator new (size_t size_)
{
    return allocate (size_);
}

void *operator new[] (size_t size_)
{
    return allocate (size_);
}

void *operator new (size_t size_, const std::nothrow_t &) noexcept
{
    try {
        return allocate (size_);
    }
    catch (...) {
        return NULL;
    }
}

void *operator new[] (size_t size_, const std::nothrow_t &) noexcept
{
    return ::operator new (size_, std::nothrow);
}

void *operator new (size_t size_, std::align_val_t alignment_)
{
    return allocate_aligned (size_, static_cast<size_t> (alignment_));
}

void *operator new[] (size_t size_, std::align_val_t alignment_)
{
    return ::operator new (size_, alignment_);
}

void *operator new (size_t size_, std::align_val_t alignment_, const std::nothrow_t &) noexcept
{
    try {
        return allocate_aligned (size_, static_cast<size_t> (alignment_));
    }
    catch (...) {
        return NULL;
    }
}

void *operator new[] (size_t size_, std::align_val_t alignment_, const std::nothrow_t &) noexcept
{
    return ::operator new (size_, alignment_, std::nothrow);
}

void operator delete (void *memory_) noexcept
{
    if (memory_) {
        live_allocations.fetch_sub (1, std::memory_order_relaxed);
        std::free (memory_);
    }
}

void operator delete[] (void *memory_) noexcept
{
    ::operator delete (memory_);
}

void operator delete (void *memory_, const std::nothrow_t &) noexcept
{
    ::operator delete (memory_);
}

void operator delete[] (void *memory_, const std::nothrow_t &) noexcept
{
    ::operator delete (memory_);
}

void operator delete (void *memory_, std::align_val_t) noexcept
{
    if (memory_) {
        live_allocations.fetch_sub (1, std::memory_order_relaxed);
        free_aligned (memory_);
    }
}

void operator delete[] (void *memory_, std::align_val_t alignment_) noexcept
{
    ::operator delete (memory_, alignment_);
}

void operator delete (void *memory_, std::align_val_t alignment_, const std::nothrow_t &) noexcept
{
    ::operator delete (memory_, alignment_);
}

void operator delete[] (void *memory_, std::align_val_t alignment_, const std::nothrow_t &) noexcept
{
    ::operator delete (memory_, alignment_);
}

#if defined(__cpp_sized_deallocation)
void operator delete (void *memory_, size_t) noexcept
{
    ::operator delete (memory_);
}

void operator delete[] (void *memory_, size_t) noexcept
{
    ::operator delete (memory_);
}

void operator delete (void *memory_, size_t, std::align_val_t alignment_) noexcept
{
    ::operator delete (memory_, alignment_);
}

void operator delete[] (void *memory_, size_t, std::align_val_t alignment_) noexcept
{
    ::operator delete (memory_, alignment_);
}
#endif

namespace
{
struct handshake_context_t
{
    void *context;
    void *server;
    fd_t peers[8];
};

handshake_context_t accept_handshakes (int io_threads_)
{
    handshake_context_t fixture;
    fixture.context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (fixture.context);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (fixture.context, ZLINK_IO_THREADS, io_threads_));
    fixture.server = zlink_socket (fixture.context, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (fixture.server);
    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (fixture.server, ZLINK_OPT_LINGER, &zero, sizeof zero));
    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (fixture.server, endpoint, sizeof endpoint);

    for (size_t i = 0; i != sizeof fixture.peers / sizeof fixture.peers[0]; ++i) {
        fixture.peers[i] = connect_socket (endpoint, AF_INET, IPPROTO_TCP);
        // The first greeting byte proves that accept, session launch, engine
        // attach and engine plug have completed. The peer sends nothing, so
        // the Core handshake cannot finish. Default handshake timers stay on.
        char greeting;
        TEST_ASSERT_EQUAL_INT (1, recv (fixture.peers[i], &greeting, 1, 0));
    }
    return fixture;
}

void close_peers (const handshake_context_t &fixture_)
{
    // Keep peers open through term: peer disconnect must not perform the
    // engine cleanup that socket/context termination is testing.
    for (size_t i = 0; i != sizeof fixture_.peers / sizeof fixture_.peers[0]; ++i)
        TEST_ASSERT_SUCCESS_RAW_ERRNO (close (fixture_.peers[i]));
}

void accepted_handshake_term (int io_threads_)
{
    const size_t baseline = live_allocations.load (std::memory_order_relaxed);
    for (int iteration = 0; iteration != 16; ++iteration) {
        const handshake_context_t fixture = accept_handshakes (io_threads_);
        TEST_ASSERT_TRUE (live_allocations.load (std::memory_order_relaxed) > baseline);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (fixture.server));
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (fixture.context));
        close_peers (fixture);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE (
          baseline, live_allocations.load (std::memory_order_relaxed),
          "Accepted connection allocations remain after zlink_ctx_term");
    }
}
}

void setUp ()
{
    // Warm up first-use Core/Asio caches with a throwaway context before any
    // allocation snapshot; process-lifetime allocations are not connection leaks.
    const handshake_context_t warmup = accept_handshakes (4);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (warmup.server));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (warmup.context));
    close_peers (warmup);
}

void tearDown ()
{
}

void test_accepted_handshake_term_single_io_thread ()
{
    accepted_handshake_term (1);
}

void test_accepted_handshake_term_multiple_io_threads ()
{
    accepted_handshake_term (4);
}

void test_accepted_handshake_term_parallel_contexts ()
{
    for (int iteration = 0; iteration != 16; ++iteration) {
        handshake_context_t fixtures[8];
        std::promise<int> shutdown[8];
        std::future<int> ready[8];
        std::thread terminators[8];
        int shutdown_results[8];
        zlink_close_result_t close_results[8];
        zlink_close_result_t term_results[8];
        // Promise shared states exist at both snapshots; futures are consumed
        // and thread workers have been joined before the final count.
        const size_t baseline = live_allocations.load (std::memory_order_relaxed);

        for (size_t i = 0; i != 8; ++i) {
            fixtures[i] = accept_handshakes (4);
            ready[i] = shutdown[i].get_future ();
        }
        TEST_ASSERT_TRUE (live_allocations.load (std::memory_order_relaxed) > baseline);
        for (size_t i = 0; i != 8; ++i) {
            terminators[i] = std::thread ([&, i] () {
                shutdown[i].set_value (zlink_ctx_shutdown (fixtures[i].context));
                term_results[i] = zlink_ctx_term (fixtures[i].context);
            });
        }
        for (size_t i = 0; i != 8; ++i)
            shutdown_results[i] = ready[i].get ();
        for (size_t i = 0; i != 8; ++i)
            close_results[i] = zlink_close (fixtures[i].server);
        for (size_t i = 0; i != 8; ++i)
            terminators[i].join ();

        // Unity assertions run only on the test thread, after every term
        // worker has returned and all engines must have been reclaimed.
        for (size_t i = 0; i != 8; ++i) {
            TEST_ASSERT_SUCCESS_ERRNO (shutdown_results[i]);
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, close_results[i]);
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, term_results[i]);
            close_peers (fixtures[i]);
        }
        TEST_ASSERT_EQUAL_UINT64_MESSAGE (
          baseline, live_allocations.load (std::memory_order_relaxed),
          "Accepted connection allocations remain after zlink_ctx_term");
    }
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_accepted_handshake_term_single_io_thread);
    RUN_TEST (test_accepted_handshake_term_multiple_io_threads);
    RUN_TEST (test_accepted_handshake_term_parallel_contexts);
    return UNITY_END ();
}
