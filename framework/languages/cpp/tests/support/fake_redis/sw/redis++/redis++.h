/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Only the Redis blob-bound unit target uses this client substitute. The
// production provider still owns validation, payload copying and worker dispatch.
namespace sw::redis
{

using Error = std::runtime_error;

struct ConnectionOptions
{
    std::chrono::milliseconds connect_timeout{};
    std::chrono::milliseconds socket_timeout{};
};

struct ConnectionPoolOptions
{
};

class Uri
{
  public:
    explicit Uri (const std::string &) {}
    ConnectionOptions connection_options () const { return {}; }
    ConnectionPoolOptions connection_pool_options () const { return {}; }
};

class Redis
{
  public:
    using eval_handler_t = std::function<std::vector<std::string> (
      std::string_view, std::span<const std::string>, std::span<const std::string>)>;
    inline static eval_handler_t on_eval;

    Redis (const ConnectionOptions &, const ConnectionPoolOptions &) {}

    template <typename T, typename KeyIterator, typename ArgIterator>
    T eval (const std::string &script,
            KeyIterator keys_begin,
            KeyIterator keys_end,
            ArgIterator args_begin,
            ArgIterator args_end)
    {
        if (!on_eval)
            throw Error ("unexpected Redis EVAL");
        return on_eval (script, {keys_begin, keys_end}, {args_begin, args_end});
    }

    template <typename T, typename... Args> T command (Args &&...)
    {
        throw Error ("unexpected Redis command");
    }

    std::optional<std::string> get (const std::string &)
    {
        throw Error ("unexpected Redis GET");
    }

    std::optional<std::string> hget (const std::string &, const std::string &)
    {
        throw Error ("unexpected Redis HGET");
    }

    long long pttl (const std::string &)
    {
        throw Error ("unexpected Redis PTTL");
    }

    bool pexpire (const std::string &, std::chrono::milliseconds)
    {
        throw Error ("unexpected Redis PEXPIRE");
    }

    long long del (const std::string &)
    {
        throw Error ("unexpected Redis DEL");
    }
};

} // namespace sw::redis
