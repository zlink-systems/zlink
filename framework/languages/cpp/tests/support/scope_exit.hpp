/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#pragma once

#include <functional>
#include <utility>

namespace zlink::framework::test
{

// Runs its action once, from run () or when the scope ends, including early
// returns after a failed check. Declared after the executor, queue or thread
// and every local that queued work references, its action releases blockers
// and drains or joins before those locals are destroyed.
class scope_exit_t final
{
  public:
    explicit scope_exit_t (std::function<void ()> action) : _action (std::move (action)) {}
    scope_exit_t (const scope_exit_t &) = delete;
    scope_exit_t &operator= (const scope_exit_t &) = delete;
    ~scope_exit_t () { run (); }

    void run ()
    {
        if (auto action = std::exchange (_action, nullptr))
            action ();
    }

  private:
    std::function<void ()> _action;
};

} // namespace zlink::framework::test
