/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/locations/redis.hpp>

#include <gtest/gtest.h>

namespace
{

using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::redis;

class ZLinkFrameworkRedisBlobBound : public ::testing::Test
{
  protected:
    void TearDown () override { sw::redis::Redis::on_eval = {}; }
};

TEST_F (ZLinkFrameworkRedisBlobBound, Accepts64MiBPlus23ByteEnvelope)
{
    const std::vector<std::byte> payload (64u * 1024u * 1024u + 23u, std::byte{0xa5});
    std::size_t eval_calls = 0;
    sw::redis::Redis::on_eval = [&] (std::string_view,
                                    std::span<const std::string> keys,
                                    std::span<const std::string> args) {
        ++eval_calls;
        EXPECT_EQ (keys.size (), 1u);
        EXPECT_EQ (args.size (), 2u);
        if (keys.size () != 1 || args.size () != 2)
            throw sw::redis::Error ("unexpected relocation PUT arguments");
        EXPECT_EQ (keys[0], "zlink:test:bound:{zlink-relocation-v1}:blob:max-encoded");
        EXPECT_EQ (args[0].size (), payload.size ());
        EXPECT_EQ (std::string_view (args[0]),
                   (std::string_view {reinterpret_cast<const char *> (payload.data ()),
                                      payload.size ()}));
        EXPECT_EQ (args[1], "30000");
        return std::vector<std::string>{"stored", "1000", "31000"};
    };
    redis_relocation_store_t store ({
      .connection_string = "tcp://fake-redis:6379",
      .key_prefix = "zlink:test:bound"});
    relocation_store_t &provider = store;

    const auto result = provider.put ({"max-encoded"}, payload, 30s).result ();

    ASSERT_TRUE (result.has_value ());
    const auto *stored = std::get_if<blob_stored_t> (&result.value ());
    ASSERT_NE (stored, nullptr);
    EXPECT_EQ (stored->store_now, std::chrono::system_clock::time_point (1000ms));
    EXPECT_EQ (stored->expires_at, std::chrono::system_clock::time_point (31000ms));
    EXPECT_EQ (eval_calls, 1u);
}

TEST_F (ZLinkFrameworkRedisBlobBound, Rejects64MiBPlus24BytesBeforeRedisCall)
{
    const std::vector<std::byte> payload (64u * 1024u * 1024u + 24u, std::byte{0xa5});
    std::size_t eval_calls = 0;
    sw::redis::Redis::on_eval = [&] (std::string_view,
                                    std::span<const std::string>,
                                    std::span<const std::string>) {
        ++eval_calls;
        return std::vector<std::string>{"stored", "1000", "31000"};
    };
    redis_relocation_store_t store ({
      .connection_string = "tcp://fake-redis:6379",
      .key_prefix = "zlink:test:bound"});
    relocation_store_t &provider = store;

    EXPECT_THROW ((void) provider.put ({"oversized"}, payload, 30s).result (),
                  std::invalid_argument);
    EXPECT_EQ (eval_calls, 0u);
}

} // namespace
