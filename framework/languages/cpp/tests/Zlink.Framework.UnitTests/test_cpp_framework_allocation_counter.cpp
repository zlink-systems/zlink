/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/allocation_counter.hpp"
#include <cassert>
#include <new>
#include <sstream>

int main ()
{
    std::ostringstream stream;
    stream << std::string (256, 'x');
    const auto count = zlink::framework::tests::count_allocations ([&] {
        const auto text = stream.str ();
        assert (text.size () == 256);
        assert (text.front () == 'x');
        assert (text.back () == 'x');
    });
    assert (count == 1);

    struct allocation_pair_t
    {
        void *(*allocate) ();
        void (*release) (void *);
    };
    const allocation_pair_t pairs[] = {
      {[] () -> void * { return ::operator new (64, std::nothrow); },
       [] (void *memory) { ::operator delete (memory); }},
      {[] () -> void * { return ::operator new[] (64, std::nothrow); },
       [] (void *memory) { ::operator delete[] (memory); }},
      {[] () -> void * { return ::operator new (64, std::align_val_t (64), std::nothrow); },
       [] (void *memory) { ::operator delete (memory, std::align_val_t (64)); }},
      {[] () -> void * { return ::operator new[] (64, std::align_val_t (64), std::nothrow); },
       [] (void *memory) { ::operator delete[] (memory, std::align_val_t (64)); }}};
    for (const auto &pair : pairs) {
        const auto measured = zlink::framework::tests::count_allocations ([&] {
            void *memory = pair.allocate ();
            assert (memory != nullptr);
            pair.release (memory);
        });
        assert (measured == 1);
        zlink::framework::tests::allocations_before_failure = 0;
        const auto failed = zlink::framework::tests::count_allocations ([&] {
            void *memory = pair.allocate ();
            assert (memory == nullptr);
        });
        assert (failed == 0);
        assert (!zlink::framework::tests::allocations_before_failure.has_value ());
    }
}
