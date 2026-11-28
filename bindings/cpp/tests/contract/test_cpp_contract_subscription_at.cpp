/* SPDX-License-Identifier: MPL-2.0 */
#include <zlink.h>
#include <zlink.hpp>

#include <cerrno>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

extern "C" zlink_config_result_t
__real_zlink_subscription_at (void *, size_t, char *, size_t *, int *);

namespace
{
const std::string long_filter (4096, 'x');

enum class scenario_t
{
    missing,
    empty,
    long_entry,
    removed_before_retry,
    invalid_argument
};

scenario_t scenario;
std::vector<std::pair<size_t, zlink_config_result_t>> calls;
}

extern "C" zlink_config_result_t __wrap_zlink_subscription_at (
  void *socket_, size_t index_, char *filter_, size_t *size_, int *pattern_)
{
    const size_t capacity = filter_ ? *size_ : 0;
    zlink_config_result_t result;
    if (scenario == scenario_t::invalid_argument) {
        *size_ = long_filter.size ();
        errno = EINVAL;
        result = ZLINK_CONFIG_INVALID_ARGUMENT;
    } else {
        if (scenario == scenario_t::removed_before_retry && calls.size () == 1)
            zlink_unset_subscription (socket_, long_filter.c_str ());
        result = __real_zlink_subscription_at (socket_, index_, filter_, size_, pattern_);
    }
    const int saved_errno = errno;
    calls.emplace_back (capacity, result);
    errno = saved_errno;
    return result;
}

namespace
{
struct case_t
{
    const char *name;
    scenario_t scenario;
    std::string filter;
    size_t index;
    std::vector<zlink_config_result_t> results;
};

template <typename Socket> bool check_query (const case_t &test_)
{
    zlink::context_t context;
    Socket socket (context);
    socket.set_subscription (test_.filter);

    scenario = test_.scenario;
    calls.clear ();
    bool correct_entry = false;
    try {
        const std::optional<zlink::subscription_filter_t> entry =
          socket.subscription_at (test_.index);
        if (test_.scenario == scenario_t::missing
            || test_.scenario == scenario_t::removed_before_retry)
            correct_entry = !entry;
        else if (test_.scenario != scenario_t::invalid_argument)
            correct_entry = entry && entry->filter == test_.filter && !entry->is_pattern;
    }
    catch (const zlink::config_error_t &error_) {
        correct_entry = test_.scenario == scenario_t::invalid_argument
                        && error_.result () == zlink::config_result_t::invalid_argument
                        && error_.internal_errno () == EINVAL;
    }

    std::vector<zlink_config_result_t> results;
    for (const auto &call : calls)
        results.push_back (call.second);
    if (correct_entry && results == test_.results && calls.front ().first > 0
        && (calls.size () != 2 || calls.back ().first == test_.filter.size ()))
        return true;

    std::fprintf (stderr, "%s: expected entry or error matched=%d, Core calls (capacity, result):",
                  test_.name, correct_entry);
    for (const auto &call : calls)
        std::fprintf (stderr, " (%zu, %d)", call.first, static_cast<int> (call.second));
    std::fprintf (stderr, "\n");
    return false;
}
}

int main ()
{
    const case_t cases[] = {
      {"missing", scenario_t::missing, "", 1, {ZLINK_CONFIG_NOT_FOUND}},
      {"empty", scenario_t::empty, "", 0, {ZLINK_CONFIG_OK}},
      {"long",
       scenario_t::long_entry,
       long_filter,
       0,
       {ZLINK_CONFIG_BUFFER_TOO_SMALL, ZLINK_CONFIG_OK}},
      {"removed before retry",
       scenario_t::removed_before_retry,
       long_filter,
       0,
       {ZLINK_CONFIG_BUFFER_TOO_SMALL, ZLINK_CONFIG_NOT_FOUND}},
      {"invalid argument", scenario_t::invalid_argument, "", 0, {ZLINK_CONFIG_INVALID_ARGUMENT}},
    };
    bool passed = true;
    for (const auto &test : cases) {
        std::fprintf (stdout, "SUB %s\n", test.name);
        passed = check_query<zlink::sub_socket_t> (test) && passed;
        std::fprintf (stdout, "XSUB %s\n", test.name);
        passed = check_query<zlink::xsub_socket_t> (test) && passed;
    }
    return passed ? 0 : 1;
}
