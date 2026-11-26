/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework/contracts/configuration/app.hpp>
#include <chrono>
#include <string>
#include <thread>

namespace zlink::framework::test::spot_host_relocation
{
constexpr auto wait_budget = std::chrono::seconds (5);

class host_t
{
  public:
    ~host_t ()
    {
        app.request_stop ();
        if (_worker.joinable ())
            _worker.join ();
    }

    void start (std::string program = {})
    {
        _worker = std::thread ([this, program = std::move (program)] () mutable {
            if (program.empty ()) {
                app.run (0, nullptr);
                return;
            }
            char *argv[]{program.data (), nullptr};
            app.run (1, argv);
        });
    }

    app_t app = app_t::create ();

  private:
    std::thread _worker;
};

template <class Predicate> bool observe (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!predicate () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    return predicate ();
}
} // namespace zlink::framework::test::spot_host_relocation
