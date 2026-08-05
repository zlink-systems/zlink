#include "stream_client_core.hpp"

#include <cstdio>

int main (int argc, char **argv)
{
    if (argc <= 1) {
        std::printf ("test_scenario_stream_client: no args -> skip (scenario runner only)\n");
        return 0;
    }
    return stream_client::run_stream_client (argc, argv);
}
