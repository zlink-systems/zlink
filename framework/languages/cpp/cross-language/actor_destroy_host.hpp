/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <map>
#include <string>

namespace actor_destroy
{
int run_host (int argc, char **argv, bool owner, const std::map<std::string, std::string> &args);
}
