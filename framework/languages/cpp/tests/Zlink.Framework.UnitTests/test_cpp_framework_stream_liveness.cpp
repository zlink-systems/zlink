/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "runtime/streams/session_liveness.hpp"
#include <iostream>
using namespace std::chrono_literals;
using zlink::framework::runtime::session_liveness_t;
int main ()
{
    using decision = session_liveness_t::decision_t;
    const auto established = session_liveness_t::clock_t::time_point{};
    session_liveness_t silent (established);
    silent.record_inbound (established - 1s);
    if (silent.evaluate (established + 4999ms) == decision::heartbeat_timeout)
        return 1;
    if (silent.evaluate (established + 5s) != decision::heartbeat_timeout) {
        std::cerr << "FAIL a: first inbound absent at 5s must heartbeat_timeout\n";
        return 1;
    }
    std::cout << "PASS a: first inbound absent heartbeat_timeout\n";
    for (const bool core_transport : {false, true}) {
        for (const bool application_data : {false, true}) {
            session_liveness_t active (established);
            for (int cycle = 1; cycle <= 5; ++cycle) {
                const auto received = established + cycle * 4999ms;
                active.record_inbound (received, core_transport && application_data);
                if (active.evaluate (received + 1ms) == decision::heartbeat_timeout) {
                    std::cerr << "FAIL b: inbound must refresh heartbeat\n";
                    return 1;
                }
                if (!core_transport && application_data)
                    active.record_application_inbound (received + 1ms);
            }
            std::cout << "PASS b: " << (core_transport ? "Core " : "raw ")
                      << (application_data ? "data" : "pong") << " inbound refreshes heartbeat\n";
        }
    }
    session_liveness_t delayed_factory (established, established + 10s);
    delayed_factory.record_inbound (established + 1s);
    if (delayed_factory.evaluate (established + 6s) != decision::heartbeat_timeout)
        return 1;
    session_liveness_t core_data_idle (established);
    core_data_idle.record_inbound (established + 30s, true);
    if (core_data_idle.evaluate (established + 30s) == decision::idle_timeout)
        return 1;
    session_liveness_t raw_data_idle (established);
    raw_data_idle.record_inbound (established + 30s);
    if (raw_data_idle.evaluate (established + 30s) != decision::idle_timeout)
        return 1;
    raw_data_idle.record_application_inbound (established + 31s);
    if (raw_data_idle.evaluate (established + 31s) == decision::idle_timeout)
        return 1;
    session_liveness_t periodic (established);
    for (int tick = 1; tick < 5; ++tick) {
        if (periodic.evaluate (established + tick * 1s) != decision::send_heartbeat) {
            std::cerr << "FAIL: periodic ping must not wait for pong\n";
            return 1;
        }
    }
    if (periodic.evaluate (established + 5s) != decision::heartbeat_timeout)
        return 1;
    session_liveness_t controls (established);
    controls.record_inbound (established + 30s);
    if (controls.evaluate (established + 30s) != decision::idle_timeout)
        return 1;
    session_liveness_t both_expired (established);
    if (both_expired.evaluate (established + 30s) != decision::heartbeat_timeout)
        return 1;
    std::cout << "PASS periodic ping and unchanged idle precedence\n";
}
