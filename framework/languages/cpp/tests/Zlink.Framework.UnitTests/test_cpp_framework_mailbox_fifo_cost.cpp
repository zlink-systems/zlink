/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/mesh/service_mailbox.hpp"

#include <chrono>
#include <iostream>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace
{
namespace fw = zlink::framework;
namespace mesh = fw::runtime::mesh;
using record_t = mesh::service_mailbox_record_t;
using mailbox_t = mesh::service_mailbox_t;

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

record_t record ()
{
    record_t value;
    value.owner = "node";
    value.domain = mesh::service_mailbox_domain_t::application;
    value.parts.emplace_back ();
    return value;
}

void rotate (mailbox_t &mailbox, std::size_t count)
{
    mailbox.begin_application_receive_turn ();
    for (std::size_t index = 0; index != count; ++index)
        require (mailbox.try_enqueue (record ()), "mailbox enqueue failed");
    auto ready = mailbox.take_application_receive_turn ();
    require (ready && ready->first.size () == 1, "owner ready count differs");
    mailbox_t::notify_application_ready (ready);
    require (mailbox.begin_application_drain ("node"), "owner drain did not begin");
    auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "node",
                                          count, 1024 * count);
    require (claim && claim->claimed_messages == count, "owner claim count differs");
    for (std::size_t index = 0; index != count; ++index) {
        auto original = mailbox.take ({claim, index});
        require (original && original->transferred (), "original did not settle");
    }
    mailbox.end_application_drain ("node");
    require (mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0,
             "mailbox retained records after drain");
}

void verify_cost ()
{
    mailbox_t mailbox;
    std::size_t notifications = 0;
    const mailbox_t::application_ready_t ready_callback = [&] (const auto &) { ++notifications; };
    mailbox.bind_application_dispatch ({}, ready_callback);
    mailbox.begin_application_receive_turn ();
    require (mailbox.try_enqueue (record ()), "cost enqueue failed");
    std::optional<mailbox_t::application_ready_batch_t> batch;
    const auto transfer = fw::tests::measure_allocations (
      [&] { batch.emplace (mailbox.take_application_receive_turn ()); });
    // Calibrate construction/move too: some standard libraries allocate a list
    // sentinel even for an empty/moved-from container.
    std::list<std::string> ready_owners{"node"};
    using ready_reference_t =
      std::optional<std::pair<std::list<std::string>, mailbox_t::application_ready_t>>;
    const auto take_reference = [&] {
        ready_reference_t value (std::in_place);
        value->second = ready_callback;
        value->first.swap (ready_owners);
        return value;
    };
    std::optional<ready_reference_t> reference_batch;
    const auto transfer_reference =
      fw::tests::measure_allocations ([&] { reference_batch.emplace (take_reference ()); });
    std::cout << "transfer_count=" << transfer.count << " transfer_bytes=" << transfer.bytes
              << '\n';
    mailbox_t::notify_application_ready (*batch);
    require (notifications == 1, "ready owner notification was lost or duplicated");
    require (transfer.count == transfer_reference.count
               && transfer.bytes == transfer_reference.bytes,
             "ready FIFO transfer allocates additional storage");
}
} // namespace

int main ()
{
    try {
        verify_cost ();
        mailbox_t mailbox;
        mailbox.bind_application_dispatch ({}, [] (const auto &) {});
        for (auto count : {1u, 128u})
            rotate (mailbox, count);
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

#ifdef ZLINK_MAILBOX_FIFO_COST_DSO
// Isolated copies of the actual mailbox owner run alternately in one process.
extern "C" __attribute__ ((visibility ("default"))) int
zlink_mailbox_fifo_measure (std::size_t iterations,
                            std::size_t batch_size,
                            bool tracking,
                            std::size_t *allocations,
                            std::size_t *bytes,
                            double *elapsed_ns)
{
    try {
        mailbox_t mailbox;
        mailbox.bind_application_dispatch ({}, [] (const auto &) {});
        rotate (mailbox, batch_size);
        const auto start = std::chrono::steady_clock::now ();
        const auto run = [&] {
            for (std::size_t index = 0; index != iterations; ++index)
                rotate (mailbox, batch_size);
        };
        const auto measured = tracking ? fw::tests::measure_allocations (run)
                                       : (run (), fw::tests::allocation_measurement_t{});
        *elapsed_ns =
          std::chrono::duration<double, std::nano> (std::chrono::steady_clock::now () - start)
            .count ();
        *allocations = measured.count;
        *bytes = measured.bytes;
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}

#endif
