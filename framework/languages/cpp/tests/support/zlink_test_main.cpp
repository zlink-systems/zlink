/* SPDX-License-Identifier: FSL-1.1-ALv2 */

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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

int main (int argc, char **argv)
{
    testing::InitGoogleMock (&argc, argv);
    return RUN_ALL_TESTS ();
}
