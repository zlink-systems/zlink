/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/locations/relocation_completion_store.hpp"
#include "../support/opaque_store_record_fixture.hpp"

namespace
{
using namespace zlink::framework::test::opaque_store;
using namespace zlink::framework::runtime::relocation_completion;

class observed_store_t final : public location_store_t
{
  public:
    bool lose_next_write = false;
    unsigned reply_writes = 0;
    unsigned reply_reads = 0;
    std::chrono::system_clock::time_point clock{};

    task_t<store_read_result_t> read (store_key_t key) override
    {
        auto result = co_await _store.read (key);
        if (!is_reply (key))
            co_return result;
        ++reply_reads;
        if (auto *found = std::get_if<store_found_t> (&result)) {
            if (_expires <= clock)
                co_return store_missing_t{clock};
            found->value.store_now = clock;
            found->value.expires_at = _expires;
        }
        co_return result;
    }

    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        std::optional<std::chrono::milliseconds> ttl;
        for (const auto &mutation : request.mutations)
            if (const auto *put = std::get_if<store_put_t> (&mutation); put && is_reply (put->key))
                ttl = put->retention;
        auto result = co_await _store.write (std::move (request));
        if (ttl && std::holds_alternative<store_write_applied_t> (result)) {
            ++reply_writes;
            _expires = clock + *ttl;
            if (std::exchange (lose_next_write, false))
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "Lost row CAS response");
        }
        co_return result;
    }

    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (request.prefix.starts_with ("$zlink/relocation-reply/") && _expires <= clock)
            co_return store_scan_page_t{{}, {}, clock};
        co_return co_await _store.scan (std::move (request));
    }

  private:
    static bool is_reply (const store_key_t &key)
    {
        return key.value.starts_with ("$zlink/relocation-reply/");
    }

    std::chrono::system_clock::time_point _expires{};
    in_memory_location_store_t _store;
};

class RelocationReplyRow : public ::testing::Test
{
  protected:
    observed_store_t store;
    provider_location_repository_t locations{store};
    repository_t replies{locations};
    row_t row;

    void SetUp () override
    {
        const auto lease = [&] (std::string id) {
            return std::get<owner_lease_claimed_t> (
                     locations.claim_owner_lease (std::move (id), 30s).result ().value ())
              .token;
        };
        const auto source = lease ("requester");
        row = {"00000000-0000-0001-0000-000000000002",
               actor_authority_key ("actor").value,
               {source.owner_id, static_cast<std::uint64_t> (source.lease_generation), "73", 3},
               "00000000000000010000000000000002",
               17,
               lease ("target")};
        store_write_request_t batch;
        const auto put = accepted (row);
        ASSERT_TRUE (append_accepted (batch, std::span (&put, 1)));
        ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (
          store.write (std::move (batch)).result ().value ()));
    }

    row_t record (std::vector<std::byte> reply = bytes ("result"))
    {
        return replies.record_result (row, std::move (reply), bytes ("failure"))
          .result ()
          .value ()
          .value ();
    }

    store_value_t read ()
    {
        return std::get<store_found_t> (store.read (key (row)).result ().value ()).value;
    }
};

TEST_F (RelocationReplyRow, LostResultCASIsConfirmedByRereading)
{
    EXPECT_EQ (key (row).value, "$zlink/relocation-reply/"
                                "00000000-0000-0001-0000-000000000002/requester."
                                  + std::to_string (row.source.lease_generation)
                                  + ".73.3/00000000000000010000000000000002");
    EXPECT_TRUE (replies.pending (row.relocation_id).result ().value ().empty ());
    store.lose_next_write = true;
    const auto reads = store.reply_reads;
    const auto stored = record ();
    EXPECT_GT (store.reply_reads, reads + 1);
    EXPECT_EQ (stored.state, delivery_t::pending);
    EXPECT_EQ (record (bytes ("different result")).reply, stored.reply);
    EXPECT_EQ (store.reply_writes, 2u);
    const auto snapshot = replies.pending (row.relocation_id).result ().value ();
    ASSERT_EQ (snapshot.size (), 1u);
    EXPECT_EQ (snapshot.front ().reply, stored.reply);
}

TEST_F (RelocationReplyRow, AuthenticatedAckKeepsFirstTerminalAndExpiresInFiveMinutes)
{
    record ();
    store.lose_next_write = true;
    EXPECT_TRUE (replies.confirm (row, delivery_t::terminal_received).result ().value ());
    EXPECT_TRUE (replies.confirm (row, delivery_t::already_terminal).result ().value ());
    EXPECT_EQ (record ().state, delivery_t::terminal_received);
    EXPECT_EQ (store.reply_writes, 3u);
    EXPECT_EQ (*read ().expires_at - store.clock, 5min);
    store.clock += 5min - 1ms;
    EXPECT_TRUE (std::holds_alternative<store_found_t> (store.read (key (row)).result ().value ()));
    store.clock += 1ms;
    EXPECT_TRUE (
      std::holds_alternative<store_missing_t> (store.read (key (row)).result ().value ()));
}

TEST_F (RelocationReplyRow, SourceLeaseExpiryRequiresExactFence)
{
    record ();
    EXPECT_FALSE (replies.confirm (row, delivery_t::source_lease_expired).result ().value ());
    locations
      .release_owner_lease (
        {row.source.owner_id, static_cast<std::int64_t> (row.source.lease_generation)})
      .result ()
      .value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      locations.claim_owner_lease (row.source.owner_id, 30s).result ().value ());
    EXPECT_NE (static_cast<std::uint64_t> (replacement.token.lease_generation),
               row.source.lease_generation);
    EXPECT_TRUE (replies.confirm (row, delivery_t::source_lease_expired).result ().value ());
    EXPECT_EQ (record ().state, delivery_t::source_lease_expired);
}

TEST_F (RelocationReplyRow, WriterLeaseExpiryPreventsResultAndAcknowledgement)
{
    record ();
    locations.release_owner_lease (row.writer).result ().value ();
    const auto replacement = std::get<owner_lease_claimed_t> (
      locations.claim_owner_lease (row.writer.owner_id, 30s).result ().value ());
    EXPECT_NE (replacement.token.lease_generation, row.writer.lease_generation);
    EXPECT_FALSE (
      replies.record_result (row, bytes ("result"), bytes ("failure")).result ().value ());
    EXPECT_TRUE (replies.confirm (row, delivery_t::terminal_received).result ().value ());
    EXPECT_EQ (store.reply_writes, 2u);
}

TEST_F (RelocationReplyRow, PendingRenewsAtTwelveHoursOfStoreTime)
{
    record ();
    store.clock += 12h - 1ms;
    EXPECT_EQ (record ().reply, bytes ("result"));
    EXPECT_EQ (store.reply_writes, 2u);
    store.clock += 1ms;
    EXPECT_EQ (record ().reply, bytes ("result"));
    EXPECT_EQ (store.reply_writes, 3u);
    EXPECT_EQ (*read ().expires_at - store.clock, 24h);
}

TEST_F (RelocationReplyRow, OversizedResultStoresFailure)
{
    const auto saved = record (std::vector<std::byte> (location_store_limits::value_bytes));
    EXPECT_EQ (saved.reply, bytes ("failure"));
    EXPECT_EQ (store.reply_writes, 2u);
}

}
