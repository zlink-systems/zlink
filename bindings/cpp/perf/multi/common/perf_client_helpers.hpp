#ifndef PERF_MULTI_CLIENT_HELPERS_HPP
#define PERF_MULTI_CLIENT_HELPERS_HPP

#include <cstring>
#include <string>

namespace perf
{
namespace multi
{

inline std::string parse_endpoint_arg (int argc, char **argv)
{
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::strcmp (argv[i], "--endpoint") == 0)
            return argv[i + 1];
    }
    return std::string ();
}

} // namespace multi
} // namespace perf

#endif
