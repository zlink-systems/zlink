#include "../common/perf_single_reqrep.hpp"
#include "../common/perf_single_runner.hpp"

bool run_pattern (const std::string &transport, size_t size, const std::string &lib_name)
{
    return perf::single::run_reqrep_pattern ({"DEALER_ROUTER_REQREP", false}, transport, size,
                                             lib_name)
      .get ();
}

int main (int argc, char **argv)
{
    return perf::single::run_standard_bench_main (argc, argv, run_pattern);
}
