#include "send_metrics.hpp"

#include <cassert>

int main ()
{
    zlink_c_bench::send_metrics_t metrics;
    metrics.start (100);
    char body[zlink_c_bench::k_header_size];
    zlink_c_bench::stamp_payload (body, sizeof body, 1, zlink_c_bench::phase_active, 0);
    zlink_c_bench::write_u64_le (reinterpret_cast<unsigned char *> (body) + 21, 50);
    metrics.record (body, sizeof body, 75);
    metrics.close (100);
    metrics.record (body, sizeof body, 125);
    auto result = metrics.snapshot (125);
    assert (result.completed == 1);
    assert (result.server_received == 2);
    assert (result.mean_us == 0.025);
    body[0] = 0;
    metrics.record (body, sizeof body, 130);
    assert (metrics.snapshot (130).server_errors == 1);
}
