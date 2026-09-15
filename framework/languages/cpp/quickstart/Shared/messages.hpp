#pragma once

#include <nlohmann/json.hpp>
#include <string>

// Request/reply contract shared by the server and client processes.
struct hello_t
{
    static constexpr const char *packet_name = "Hello";
    std::string name;
};

struct greeting_t
{
    static constexpr const char *packet_name = "Greeting";
    std::string text;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (hello_t, name)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (greeting_t, text)
