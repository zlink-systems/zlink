#ifndef PERF_SINGLE_RUNNER_HPP
#define PERF_SINGLE_RUNNER_HPP

#include <cstddef>
#include <string>

namespace perf
{
namespace single
{

typedef bool (*run_fn_t) (const std::string &transport, size_t size, const std::string &lib_name);

int run_standard_bench_main (int argc, char **argv, run_fn_t fn);

} // namespace single
} // namespace perf

#endif
