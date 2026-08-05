#ifndef PERF_MULTI_TLS_HPP
#define PERF_MULTI_TLS_HPP

// Migrated to unified header.  This redirect exists only for
// any stale includes that have not yet been updated.
#include "../../common/perf_tls.hpp"

namespace perf
{
namespace multi
{

typedef ::perf::socket_t perf_socket_t;

using ::perf::perf_tls_file_exists;
using ::perf::resolve_perf_tls_dir_from;
using ::perf::setup_tls_client;
using ::perf::setup_tls_server;
using ::perf::try_resolve_tls_paths;

} // namespace multi
} // namespace perf

#endif
