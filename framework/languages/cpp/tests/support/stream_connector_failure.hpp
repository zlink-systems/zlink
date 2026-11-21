/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <iostream>

namespace stream_connector_fixture
{
/* Reports the failed check by its id and ends the run with exit code 1. The id
 * does not travel in the exit code: POSIX keeps only its low 8 bits, so an id
 * such as 256 would read as success and two ids 256 apart as the same check. */
inline int fail (int check_id)
{
    std::cerr << "test_cpp_stream_connector: check " << check_id << " failed\n";
    return 1;
}
} // namespace stream_connector_fixture
