/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{
class cleanup_failure_t final : public std::runtime_error
{
  public:
    explicit cleanup_failure_t (std::vector<std::exception_ptr> failures) :
        std::runtime_error (message (failures)), _failures (std::move (failures))
    {
    }

    const std::vector<std::exception_ptr> &causes () const noexcept { return _failures; }

  private:
    static std::string message (const std::vector<std::exception_ptr> &failures)
    {
        std::string text;
        for (const auto &failure : failures) {
            try {
                std::rethrow_exception (failure);
            }
            catch (const std::exception &error) {
                if (!text.empty ())
                    text += "; ";
                text += error.what ();
            }
        }
        return text;
    }
    std::vector<std::exception_ptr> _failures;
};
}
