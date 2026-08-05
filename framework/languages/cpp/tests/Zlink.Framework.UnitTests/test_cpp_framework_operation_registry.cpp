/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/foundation/operation_registry.hpp"
#include "runtime/operations/exactly_once_table.hpp"

#include <cassert>
#include <stdexcept>

namespace foundation = zlink::framework::runtime::foundation;
namespace runtime = zlink::framework::runtime;

foundation::operation_id_t id (std::uint8_t value)
{
    return foundation::operation_id_t{0, value};
}

int main ()
{
    runtime::exactly_once_table_t<foundation::operation_id_t,
                                  int,
                                  runtime::operation_id_hash_t>
      completions (2);
    assert (completions.reserve (id (10)));
    assert (completions.claim (id (10)).state
            == runtime::exactly_once_claim_state::pending);
    assert (completions.complete (id (10), 7));
    const auto completed = completions.claim (id (10));
    assert (completed.state == runtime::exactly_once_claim_state::completed
            && completed.value && *completed.value == 7);
    assert (!completions.complete (id (10), 8));
    int completion = 0;
    assert (completions.take (id (10), completion) && completion == 7);
    assert (!completions.take (id (10), completion));
    assert (completions.claim (id (11)).state
            == runtime::exactly_once_claim_state::claimed);
    assert (completions.erase (id (11)));
    assert (completions.claim (id (11)).state
            == runtime::exactly_once_claim_state::claimed);
    assert (completions.complete (id (12), 9));
    const auto unreserved_completion = completions.claim (id (12));
    assert (unreserved_completion.state
              == runtime::exactly_once_claim_state::completed
            && unreserved_completion.value && *unreserved_completion.value == 9);

    foundation::operation_registry_t registry (2);
    const auto now = foundation::operation_registry_t::clock_t::now ();
    int first_terminal = 0;
    int second_terminal = 0;
    assert (registry.register_operation (
      id (1), now + std::chrono::seconds (1),
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t> payload) {
          assert (terminal == foundation::operation_terminal_t::completed);
          assert (payload == std::vector<std::uint8_t> ({7}));
          ++first_terminal;
      }));
    assert (registry.register_operation (
      id (2), now,
      [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
          assert (terminal == foundation::operation_terminal_t::timed_out);
          ++second_terminal;
      }));
    assert (!registry.register_operation (id (3), now, [] (auto, auto) {}));
    assert (registry.complete (id (1), {7}));
    assert (!registry.complete (id (1), {8}));
    assert (registry.expire (now) == 1);
    assert (first_terminal == 1 && second_terminal == 1 && registry.size () == 0);
    assert (registry.shutdown () == 0);
    assert (!registry.register_operation (id (4), now, [] (auto, auto) {}));

    int failed_terminals = 0;
    foundation::operation_registry_t failed_registry (1);
    assert (failed_registry.register_operation (
      id (4), now + std::chrono::seconds (1),
      [&] (foundation::operation_terminal_t terminal,
           std::vector<std::uint8_t> payload) {
          assert (terminal
                  == foundation::operation_terminal_t::transport_failed);
          assert (payload.empty ());
          ++failed_terminals;
      }));
    assert (failed_registry.fail (
      id (4), foundation::operation_terminal_t::transport_failed));
    assert (!failed_registry.fail (
      id (4), foundation::operation_terminal_t::transport_failed));
    assert (failed_terminals == 1);

    int shutdown_terminals = 0;
    {
        foundation::operation_registry_t scoped_registry (2);
        assert (scoped_registry.register_operation (
          id (5), now,
          [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              ++shutdown_terminals;
              throw std::runtime_error ("consumer failure");
          }));
        assert (scoped_registry.register_operation (
          id (6), now,
          [&] (foundation::operation_terminal_t terminal, std::vector<std::uint8_t>) {
              assert (terminal == foundation::operation_terminal_t::shutdown);
              ++shutdown_terminals;
          }));
    }
    assert (shutdown_terminals == 2);
    return 0;
}
