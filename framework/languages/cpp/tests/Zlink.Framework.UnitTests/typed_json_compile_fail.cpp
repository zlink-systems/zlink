/* SPDX-License-Identifier: Apache-2.0 */
#include <zlink/json_profile.hpp>

struct base
{
    int member;
};
struct dto : base
{
    int value;
    const int immutable = 0;
    long double unsupported;
#if CASE == 12
    nlohmann::json dom;
#elif CASE == 13
    std::optional<nlohmann::json> dom;
#elif CASE == 14
    std::vector<nlohmann::json> dom;
#elif CASE == 15
    std::map<std::string, nlohmann::json> dom;
#endif
};
enum class state
{
    Ready,
    Alias = Ready,
    Closed
};
#if CASE == 0
void to_json (nlohmann::json &, const dto &)
{
}
void from_json (const nlohmann::json &, dto &)
{
}
#elif CASE == 1
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("x", &dto::value), zlink::json::field ("x", &dto::value)};
};
#elif CASE == 2
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("x", &dto::immutable)};
};
#elif CASE == 3
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("x", &dto::member)};
};
#elif CASE == 4
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("x", &dto::unsupported)};
};
#elif CASE == 6
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "Ready"),
                                              zlink::json::enum_value (state::Closed, "Ready")};
};
#elif CASE == 7
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "Ready"),
                                              zlink::json::enum_value (state::Alias, "Alias")};
};
#elif CASE == 8
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "1")};
};
#elif CASE == 9
template <> struct zlink::json::enum_traits<state>
{
    static constexpr auto values = std::array{zlink::json::enum_value (state::Ready, "")};
};
#elif CASE == 10
template <> struct zlink::json::dto_traits<dto>
{
};
#elif CASE >= 12 && CASE <= 15
template <> struct zlink::json::dto_traits<dto>
{
    static constexpr auto fields = std::tuple{zlink::json::field ("dom", &dto::dom)};
};
#elif CASE == 11
template <> struct zlink::json::enum_traits<state>
{
};
#endif
int main ()
{
#if CASE < 5 || CASE == 10 || (CASE >= 12 && CASE <= 15)
    static_assert (zlink::detail::json_profile::is_json_candidate<dto> () == (CASE != 0));
    return zlink::detail::json_profile::try_encode (dto{}).has_value ();
#else
    static_assert (zlink::detail::json_profile::is_json_candidate<state> () == (CASE != 5));
    return zlink::detail::json_profile::try_encode (state::Ready).has_value ();
#endif
}