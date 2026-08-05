#include "perf_single_runner.hpp"

#include <cstdlib>
#include <iostream>

namespace perf
{
namespace single
{

int run_standard_bench_main (int argc, char **argv, run_fn_t fn)
{
    if (!fn)
        return 1;

    if (argc < 4) {
        std::cerr << "usage: <lib_name> <transport> <size>" << std::endl;
        return 1;
    }

    std::string transport;
    size_t size = 0;
    const std::string lib_name = argv[1];
    transport = argv[2];
    size = static_cast<size_t> (std::strtoull (argv[3], NULL, 10));

    if (size == 0)
        return 1;

    return fn (transport, size, lib_name) ? 0 : 1;
}

} // namespace single
} // namespace perf
