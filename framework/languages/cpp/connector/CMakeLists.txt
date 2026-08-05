cmake_minimum_required(VERSION 3.20)
project(zlink_stream_connector_family LANGUAGES CXX)

# This entry point is for source-package consumers that check out only the
# connector family. The repository root still owns the full developer build.
add_subdirectory(core)
add_subdirectory(e2e-client)
