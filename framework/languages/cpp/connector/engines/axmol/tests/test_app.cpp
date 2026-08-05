/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink_axmol_stream_connector.hpp>

#include <cstring>
#include <iostream>

int main (int argc, char **argv)
{
    bool requested = false;
    for (int i = 1; i < argc; ++i) {
        requested = requested || std::strcmp (argv[i], "--zlink-test=stream-connector") == 0;
    }
    if (!requested) {
        return 1;
    }
    std::cout << "engine-required: Axmol runtime gate requires an Axmol application runner\n";
    return 0;
}
