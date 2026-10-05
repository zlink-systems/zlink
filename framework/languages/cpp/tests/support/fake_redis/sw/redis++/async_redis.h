/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <sw/redis++/redis++.h>

#include <future>

namespace sw::redis
{

template <typename T> using Future = std::future<T>;

class AsyncRedis : public Redis
{
  public:
    using Redis::Redis;
    inline static std::function<void (std::function<void ()>)> dispatch;

    template <typename T, typename TKeyIterator, typename TArgIterator>
    Future<T> eval (const std::string &script,
                    TKeyIterator keys_begin,
                    TKeyIterator keys_end,
                    TArgIterator args_begin,
                    TArgIterator args_end)
    {
        std::promise<T> completion;
        auto result = completion.get_future ();
        try {
            completion.set_value (
              Redis::eval<T> (script, keys_begin, keys_end, args_begin, args_end));
        }
        catch (const std::exception &) {
            completion.set_exception (std::current_exception ());
        }
        return result;
    }

    template <typename T, typename TKeyIterator, typename TArgIterator, typename TCallback>
    void eval (const std::string &script,
               TKeyIterator keys_begin,
               TKeyIterator keys_end,
               TArgIterator args_begin,
               TArgIterator args_end,
               TCallback callback)
    {
        auto complete = [this, script, keys = std::vector<std::string> (keys_begin, keys_end),
                         args = std::vector<std::string> (args_begin, args_end),
                         callback = std::move (callback)] () mutable {
            callback (eval<T> (script, keys.begin (), keys.end (), args.begin (), args.end ()));
        };
        if (dispatch)
            dispatch (std::move (complete));
        else
            complete ();
    }
};

} // namespace sw::redis
