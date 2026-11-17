/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <iostream>
#include <string>

namespace zlink::framework::tests
{
struct gate_t
{
    int failures = 0;

    void require (bool condition, const std::string &ledger_id, const std::string &message)
    {
        if (condition) {
            return;
        }
        std::cerr << ledger_id << ": " << message << '\n';
        ++failures;
    }
};

}
