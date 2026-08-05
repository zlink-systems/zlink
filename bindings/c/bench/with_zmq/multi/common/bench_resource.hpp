#ifndef BENCH_RESOURCE_HPP
#define BENCH_RESOURCE_HPP

#include "bench_multi_resource.hpp"

using bench_resource_metrics_t = bench_multi_resource_metrics_t;
using bench_cpu_sample_t = bench_multi_cpu_sample_t;

inline bool bench_resource_metrics_enabled ()
{
    return bench_multi_resource_metrics_enabled ();
}

inline bench_cpu_sample_t bench_capture_cpu_sample ()
{
    return bench_multi_capture_cpu_sample ();
}

inline bench_resource_metrics_t bench_finish_resource_probe (const bench_cpu_sample_t &start)
{
    return bench_multi_finish_resource_probe (start);
}

#endif
