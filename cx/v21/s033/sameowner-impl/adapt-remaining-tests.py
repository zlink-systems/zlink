from pathlib import Path
r=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests')
p=r/'mailbox_claim_fixture.hpp';s=p.read_text();a=s.index('};\n\ninline mailbox_claim_t claim_mailbox');s=s[:a]+'''
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
'''+s[a:];a=s.index('    result.claim = mailbox.try_claim');b=s.index('    return result;',a);s=s[:a]+'''    result.claim = mailbox.try_claim (domain, messages, bytes,
                                     [&] (const auto &claim) { result.take_ordinary (claim); });
'''+s[b:];pos=s.rfind('\n}');s=s[:pos]+'''
inline mailbox_claim_t claim_mailbox_owner (runtime::mesh::service_mailbox_t &mailbox,
                                             runtime::mesh::service_mailbox_domain_t domain,
                                             const std::string &owner,
                                             std::size_t messages, std::size_t bytes)
{
    mailbox_claim_t result;
    result.claim = mailbox.try_claim_owner (
      domain, owner, messages, bytes, [&] (const auto &claim) { result.take_ordinary (claim); });
    return result;
}
'''+s[pos:];p.write_text(s)
p=r/'test_cpp_framework_reply_submit_claim.cpp';s=p.read_text().replace('#include ', '#include "mailbox_claim_fixture.hpp"\n#include ',1);a=s.index('submit_client_server_reply_and_release_claim');b=s.index('void verify_stateful_claim',a);block=s[a:b].replace('std::shared_ptr<mesh::service_mailbox_claim_t> claim','std::shared_ptr<zlink::framework::test::mailbox_claim_t> claim').replace('release (*claim)','release (**claim)');s=s[:a]+block+s[b:];a=s.index('void verify_client_server_claim_releases_after_one_shot_reply_terminal');b=s.index('\nvoid ',a+10);block=s[a:b];a1=block.index('    auto claim =');b1=block.index('    assert (claim &&',a1);block=block[:a1]+'''    auto claim = std::make_shared<zlink::framework::test::mailbox_claim_t> (
      zlink::framework::test::claim_mailbox_owner (
        server.mailbox (), mesh::service_mailbox_domain_t::application, mailbox_owner, 1, 1024));
'''+block[b1:];block=block.replace('auto released = server.mailbox ().try_claim_owner (','auto released = zlink::framework::test::claim_mailbox_owner (server.mailbox (),');s=s[:a]+block+s[b:];p.write_text(s)
p=r/'test_cpp_framework_m6b_runtime.cpp';s=p.read_text().replace('''auto claim = target.mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                    "channel:independent-channel", 1, 1024);''','''auto claim = zlink::framework::test::claim_mailbox_owner (
      target.mailbox (), mesh::service_mailbox_domain_t::application,
      "channel:independent-channel", 1, 1024);''');p.write_text(s)
# Read-only friendship preserves the existing exact claim-lifetime assertion while
# exercising the production owner dispatch rather than a preclaimed Node slot.
job=Path('/root/jobs/fx/cx/v21/s033/sameowner-impl')
f='framework/languages/cpp/framework/src/runtime/dispatch/application_record.hpp'
p=job/'product-save'/f;p.parent.mkdir(parents=True,exist_ok=True);s=Path(f).read_text();pos=s.index('namespace zlink::framework::runtime::mesh');s=s[:pos]+'''namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t;
}

'''+s[pos:];s=s.replace('    friend class service_mailbox_t;','    friend class service_mailbox_t;\n    friend struct host::public_host_runtime_test_access_t;',1);a=s.index('class mailbox_callback_t');pos=s.index('  private:',a)+len('  private:');s=s[:pos]+'\n    friend struct public_host_runtime_test_access_t;'+s[pos:];p.write_text(s);p=job/'product-files.txt';s=p.read_text();s+=f+'\n';p.write_text(s)
p=r/'test_cpp_framework_handler_entry_permit.cpp';s=p.read_text();a=s.index('    static task_t<std::size_t>');b=s.index('\n};',a);s=s[:a]+'''    static std::weak_ptr<mesh::service_mailbox_claim_t> claim_of (const mailbox_callback_t &callback)
    {
        const auto *settlement = std::get_if<mesh::service_mailbox_settlement_t> (&callback._value);
        return settlement ? settlement->_claim : std::weak_ptr<mesh::service_mailbox_claim_t>{};
    }
'''+s[b:];a=s.index('        auto claim =',s.index('    void node_case'));b=s.index('              EXPECT_EQ (host::record_kind_t::node_send',a);s=s[:a]+'''        std::weak_ptr<mesh::service_mailbox_claim_t> claim_lifetime;
        std::size_t dispatched_records = 0;
        std::optional<task_t<std::optional<detail::route_dispatch_reply_t>>> pending;
        auto dispatched = target.dispatch_application_owner (
          owner,
          [&] (const auto &, const auto &received, auto payload, auto permit) {
              ++dispatched_records;
              claim_lifetime = host::public_host_runtime_test_access_t::claim_of (
                received.release_mailbox_reservation);
              EXPECT_FALSE (claim_lifetime.expired ());
'''+s[b:];a=s.index('        auto dispatched = target.dispatch_application_owner',s.index('void node_case'));b=s.index('        EXPECT_TRUE (claim_lifetime.expired ());',a);block=s[a:b].replace('''          });
        ASSERT_TRUE (dispatched.await_ready ());
        ASSERT_EQ (1u, dispatched.result ().value ());''','''          }, [] {}, [] {});
        ASSERT_TRUE (test::application_owner_ready (dispatched));
        ASSERT_TRUE (test::application_owner_result (dispatched).value ());
        ASSERT_EQ (1u, dispatched_records);''');s=s[:a]+block+s[b:];p.write_text(s)
