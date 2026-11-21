/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/json_profile.hpp>

#include <cstdio>
#include <string_view>
#include <thread>
#include <tuple>

struct payload_t
{
    int value = 0;
};

template <> struct zlink::json::dto_traits<payload_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("value", &payload_t::value)};
};

bool decode ()
{
    constexpr std::string_view input = R"({"value":42})";
    const auto value =
      zlink::detail::json_profile::try_decode<payload_t> (input.begin (), input.end ());
    if (!value || value->value != 42)
        return false;
    std::fputs ("typed JSON decoded; returning through thread exit\n", stderr);
    return true;
}

int main (int argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (std::string_view (argv[1]) == "main")
        return decode () ? 0 : 1;
    if (std::string_view (argv[1]) != "worker")
        return 2;
    bool decoded = false;
    std::thread worker ([&] { decoded = decode (); });
    worker.join ();
    return decoded ? 0 : 1;
}
