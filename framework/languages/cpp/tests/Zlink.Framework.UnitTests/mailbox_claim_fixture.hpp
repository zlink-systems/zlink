/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/mesh/service_mailbox.hpp"

namespace zlink::framework::test
{
// Raw transport tests own their received originals at the same claim boundary as
// application drains. Views below refer to that consumer storage, not mailbox slots.
struct mailbox_claim_t
{
    std::shared_ptr<runtime::mesh::service_mailbox_claim_t> claim;
    std::vector<runtime::mesh::service_mailbox_record_t> originals;
    std::vector<runtime::mesh::service_mailbox_record_t *> records;
    std::string owner;
    std::size_t claimed_messages = 0;
    std::size_t claimed_bytes = 0;
    explicit operator bool () const noexcept { return static_cast<bool> (claim); }
    mailbox_claim_t *operator->() noexcept { return this; }
    const mailbox_claim_t *operator->() const noexcept { return this; }
    runtime::mesh::service_mailbox_claim_t &operator* () const { return *claim; }
    std::size_t outstanding () const { return claim->outstanding (); }

    void take_ordinary (const std::shared_ptr<runtime::mesh::service_mailbox_claim_t> &value)
    {
        originals.reserve (value->claimed_messages);
        records.reserve (value->claimed_messages);
        for (const auto slot : value->records) {
            originals.push_back (std::move (*slot));
            slot->application.reset ();
            records.push_back (&originals.back ());
        }
        owner = value->owner;
        claimed_messages = value->claimed_messages;
        claimed_bytes = value->claimed_bytes;
    }
};

inline mailbox_claim_t claim_mailbox (runtime::mesh::service_mailbox_t &mailbox,
                                      runtime::mesh::service_mailbox_domain_t domain,
                                      std::size_t messages,
                                      std::size_t bytes)
{
    mailbox_claim_t result;
    result.claim = mailbox.try_claim (domain, messages, bytes,
                                      [&] (const auto &claim) { result.take_ordinary (claim); });
    return result;
}
inline mailbox_claim_t claim_mailbox_owner (runtime::mesh::service_mailbox_t &mailbox,
                                            runtime::mesh::service_mailbox_domain_t domain,
                                            const std::string &owner,
                                            std::size_t messages,
                                            std::size_t bytes)
{
    mailbox_claim_t result;
    result.claim = mailbox.try_claim_owner (
      domain, owner, messages, bytes, [&] (const auto &claim) { result.take_ordinary (claim); });
    return result;
}

}
