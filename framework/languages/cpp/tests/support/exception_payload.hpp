/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <gtest/gtest.h>
#include <exception>
#include <memory>
#include <typeinfo>
#include <utility>

namespace zlink::framework::test
{
template <typename Base> class marked_exception_t final : public Base
{
  public:
    template <typename... Args>
    explicit marked_exception_t (std::shared_ptr<const int> marker, Args &&...args) :
        Base (std::forward<Args> (args)...), marker (std::move (marker))
    {
    }
    std::shared_ptr<const int> marker;
};

// Exception capture may copy the exception; its type and application payload must survive.
template <typename Error, typename Check>
::testing::AssertionResult exception_payload_matches (std::exception_ptr error, Check check)
{
    if (!error)
        return ::testing::AssertionFailure () << "missing exception";
    try {
        std::rethrow_exception (error);
    }
    catch (const Error &cause) {
        if (typeid (cause) != typeid (Error))
            return ::testing::AssertionFailure () << "exception dynamic type changed";
        if (!check (cause))
            return ::testing::AssertionFailure () << "exception payload changed";
        return ::testing::AssertionSuccess ();
    }
    catch (...) {
        return ::testing::AssertionFailure () << "exception type changed";
    }
}
}
