#ifndef PERF_SINGLE_REPORT_HPP
#define PERF_SINGLE_REPORT_HPP

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

void print_result (const std::string &lib_type,
                   const std::string &pattern,
                   const std::string &transport,
                   size_t size,
                   double throughput,
                   double latency,
                   double latency_p95,
                   double latency_p99);

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency,
                          double latency_p95,
                          double latency_p99)
{
    const double latency_ms = latency / 1000000.0;
    const double latency_p95_ms = latency_p95 / 1000000.0;
    const double latency_p99_ms = latency_p99 / 1000000.0;
    const double bandwidth_mb_s = (throughput * static_cast<double> (size)) / 1000000.0;

    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (2) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (2) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (3) << latency_ms << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (3) << latency_p95_ms
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (3) << latency_p99_ms
              << std::endl;
}

inline void print_fail_result (const std::string &lib_type,
                               const std::string &pattern,
                               const std::string &transport,
                               size_t size)
{
    std::cerr << "FAIL," << lib_type << "," << pattern << "," << transport << "," << size
              << std::endl;
}
#endif
