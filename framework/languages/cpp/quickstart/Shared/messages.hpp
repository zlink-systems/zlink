#pragma once

#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
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

template <> struct zlink::json::dto_traits<hello_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("name", &hello_t::name)};
};

template <> struct zlink::json::dto_traits<greeting_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("text", &greeting_t::text)};
};
