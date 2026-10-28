/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

// The test harness owns main so that test registration and RUN_ALL_TESTS
// resolve to the same GoogleTest instance.
//
// vcpkg's x64-windows gmock.dll carries its own copy of GoogleTest and
// exports MakeAndRegisterTestInfo itself, while gtest_main.dll binds to
// gtest.dll and gmock_main.dll is self-contained again. Borrowing a packaged
// main therefore splits the registry: every TEST registers into one instance
// and the borrowed main runs zero tests from another, printing "does NOT link
// in any test case" and exiting 0 - which ctest reports as a pass.
//
// With main compiled into the test binary, both the registrations and this
// call resolve through the same import, so there is one registry.

#include <gtest/gtest.h>
#include <zlink/framework/contracts/dispatch/task.hpp>

#if defined(ZLINK_TEST_CHECK_CORE_CONTEXTS) && defined(__linux__)
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct thread_counts_t
{
    int threads = 0;
    int contexts = 0;
    std::vector<std::string> core_threads;
};

thread_counts_t thread_counts ()
{
    thread_counts_t counts;
    for (const auto &thread : std::filesystem::directory_iterator ("/proc/self/task")) {
        std::ifstream comm (thread.path () / "comm");
        std::string name;
        if (std::getline (comm, name)) {
            ++counts.threads;
            if (name.find ("core-ct") != std::string::npos) {
                std::ifstream stat (thread.path () / "stat");
                std::string state = "<exited before stat>";
                bool exiting = false;
                if (std::getline (stat, state)) {
                    // Field 2 is parenthesized and may contain spaces or ')'.
                    // Flags are field 9; count fields only after the final ')'.
                    const auto comm_end = state.rfind (')');
                    if (comm_end != std::string::npos) {
                        std::istringstream fields (state.substr (comm_end + 1));
                        std::string ignored;
                        for (int field = 3; field < 9; ++field)
                            fields >> ignored;
                        unsigned long flags = 0;
                        if (fields >> flags) {
                            // Linux PF_EXITING can remain visible in /proc after join.
                            constexpr unsigned long task_exiting_flag = 0x4;
                            exiting = (flags & task_exiting_flag) != 0;
                            if (!exiting)
                                ++counts.contexts;
                        }
                    }
                }
                counts.core_threads.push_back (thread.path ().filename ().string ()
                                               + (exiting ? " exiting " : " ") + state);
            }
        }
    }
    return counts;
}

class core_context_listener_t final : public testing::EmptyTestEventListener
{
  public:
    void OnTestProgramStart (const testing::UnitTest &) override
    {
        _baseline = thread_counts ().contexts;
    }

    void OnTestStart (const testing::TestInfo &) override { _before = thread_counts ().contexts; }

    void OnTestEnd (const testing::TestInfo &test) override
    {
        const auto counts = thread_counts ();
        std::cout << "[ CONTEXTS ] " << test.test_suite_name () << '.' << test.name ()
                  << " before=" << _before << " after=" << counts.contexts
                  << " threads=" << counts.threads << std::endl;
        if (counts.contexts != _baseline) {
            for (const auto &thread : counts.core_threads)
                std::cout << "[ CORE THREAD ] " << thread << std::endl;
        }
        EXPECT_EQ (counts.contexts, _baseline) << "Core contexts survived test teardown";
    }

  private:
    int _baseline = 0;
    int _before = 0;
};
} // namespace
#endif

int main (int argc, char **argv)
{
    // The completion owner starts workers. Death-test children need a fresh
    // runtime instead of inheriting a dispatcher whose threads did not survive fork.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    zlink::framework::completion_owner_t completion_owner;
    testing::InitGoogleTest (&argc, argv);
#if defined(ZLINK_TEST_CHECK_CORE_CONTEXTS) && defined(__linux__)
    testing::UnitTest::GetInstance ()->listeners ().Append (new core_context_listener_t);
#endif
    return RUN_ALL_TESTS ();
}
