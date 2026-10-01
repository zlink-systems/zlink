/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/coroutine_executor.hpp"

#include <gtest/gtest.h>

// Tests that drive runtime parts without a host install the runtime context
// hooks the host installs when its runtime starts.
class runtime_context_hooks_environment_t final : public ::testing::Environment
{
  public:
    void SetUp () override { zlink::framework::runtime::install_host_context_hooks (); }
};

inline ::testing::Environment *const runtime_context_hooks_environment =
  ::testing::AddGlobalTestEnvironment (new runtime_context_hooks_environment_t);
