from pathlib import Path
p=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_claim_dispatch_cost.cpp');s=p.read_text()
s=s.replace('''    return zlink::framework::test::application_owner_result (
             target.dispatch_application_owner (claim_owner (channel), callback, noop, noop))
      .value ();''','''    bool handled = false;
    while (zlink::framework::test::application_owner_result (
             target.dispatch_application_owner (claim_owner (channel), callback, noop, noop)).value ())
        handled = true;
    return handled;''')
s=s.replace('''runtime::dispatch_limits::receive_batch_bytes);
        original = mailbox.take ({claim, 0});''','''runtime::dispatch_limits::receive_batch_bytes, [&] (const auto &claim) {
                                                  original.emplace (std::move (*claim->records.front ()));
                                                  claim->records.front ()->application.reset ();
                                              });''',1)
s=s.replace('''                auto claim = mailbox.try_claim_owner (
                  domain, owner, size, runtime::dispatch_limits::receive_batch_bytes);
                require (static_cast<bool> (claim), "batch cost claim failed");
                const auto count = claim->claimed_messages;
                for (std::size_t index = 0; index < count; ++index)
                    (void) mailbox.take ({claim, index});
                mailbox.release (*claim);''','''                for (std::size_t i = 0; i != size; ++i) {
                    std::optional<mesh::service_mailbox_record_t> original;
                    auto claim = mailbox.try_claim_owner (
                      domain, owner, size, runtime::dispatch_limits::receive_batch_bytes,
                      [&] (const auto &claim) {
                          original.emplace (std::move (*claim->records.front ()));
                          claim->records.front ()->application.reset ();
                      });
                    require (claim && claim->claimed_messages == 1, "batch cost claim failed");
                    original->transferred ();
                }''')
a=s.index('    enqueue (mailbox);\n    const dispatch_t next',s.index('void verify_retained_batch'));b=s.index('\n}',a)
s=s[:a]+'''    enqueue (mailbox);
    const dispatch_t next = [] (const auto &, const auto &, auto, auto permit) {};
    require (dispatch (target, next), "retained independent claim blocked the next record");
    releases[1]();
    releases[1]();
    releases[0]();
    require (mailbox.pending_messages (domain) == 0, "next record was lost");'''+s[b:]
a=s.index('    auto claim =',s.index('void verify_original_transfer'));b=s.index('\n}',a)
s=s[:a]+'''    std::optional<mesh::service_mailbox_record_t> first;
    auto claim = mailbox.try_claim_owner (
      domain, owner, 2, runtime::dispatch_limits::receive_batch_bytes, [&] (const auto &claim) {
          first.emplace (std::move (*claim->records.front ()));
          claim->records.front ()->application.reset ();
      });
    require (claim && claim->claimed_messages == 1, "transfer fixture claim failed");
    require (first && !claim->records[0]->application && !mailbox.take ({claim, 0}),
             "local original remained present after transfer");
    auto cut = mailbox.cut_owners ({owner}, [] (const auto &) { return true; }, [] {});
    require (cut && cut->size () == 1 && cut->front ().size () == 1,
             "relocation cut included an already transferred original");
    require (!cut->front ().front ().transferred, "unclaimed cut minted claim credit");
    require (first->transferred (), "transferred original lost claim credit");
    require (mailbox.pending_messages (domain) == 0, "transferred originals lost claim credit");'''+s[b:]
a=s.index('            auto claim =',s.index('void measure_permit_ownership_cost'));b=s.index('\n        });',a)
s=s[:a]+'''            for (std::size_t i = 0; i < count; ++i) {
                std::optional<mesh::service_mailbox_record_t> original;
                auto claim = mailbox.try_claim_owner (
                  domain, owner, count, runtime::dispatch_limits::receive_batch_bytes,
                  [&] (const auto &claim) {
                      original.emplace (std::move (*claim->records.front ()));
                      claim->records.front ()->application.reset ();
                  });
                require (claim && claim->claimed_messages == 1, "cost claim failed");
                auto &permit = admitted ? original->application_permit : external[i];
                runtime::application_dispatch_terminal_owner_t terminal ({}, {}, {},
                                                                         std::move (*permit));
                terminal.enter_handler ();
                original->transferred ();
            }'''+s[b:]
a=s.index('    auto claim =',s.index('void verify_inline_permit'));b=s.index('    require (original',a)
s=s[:a]+'''    std::optional<mesh::service_mailbox_record_t> original;
    auto claim = mailbox.try_claim_owner (
      domain, owner, 1, runtime::dispatch_limits::receive_batch_bytes, [&] (const auto &claim) {
          original.emplace (std::move (*claim->records.front ()));
          claim->records.front ()->application.reset ();
      });
'''+s[b:]
a=s.index('    auto claim =',s.index('void verify_failure_permit'));b=s.index('    require (retained_reply.request',a)
s=s[:a]+'''    host::reply_token_t retained_reply;
    std::size_t calls = 0;
    require (dispatch (*target, [&] (const auto &, const auto &record, auto, auto) {
        retained_reply = record.reply_token;
        ++calls;
    }), "failure fixture dispatch failed");
    require (calls == 1, "malformed original reached its application handler");
    require (mailbox.pending_messages (domain) == 0, "failure fixture retained queued originals");
'''+s[b:]
s=s.replace('''std::cout << "permit-failure claim-destroyed=" << lifetime.expired ()
              << " retained-routing="''','''std::cout << "permit-failure retained-routing="''')
a=s.index('    auto claim =',s.index('void verify_multicast_admission'));b=s.index('    std::promise<void> entered',a)
s=s[:a]+s[b:]
s=s.replace('''        auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
          *target, std::move (claim), [] (const auto &, const auto &, auto, auto) {
              throw std::runtime_error ("multicast source reached application callback");
          });
        require (pending.await_ready () && pending.result ().value () == 1,
                 "multicast source did not finish while target snapshot was suspended");
        source_destroyed = source_claim.expired ();''','''        auto pending = target->dispatch_application_owner (
          owner, [] (const auto &, const auto &, auto, auto) {
              throw std::runtime_error ("multicast source reached application callback");
          }, noop, noop);
        require (std::holds_alternative<bool> (pending) && std::get<bool> (pending),
                 "multicast source did not finish while target snapshot was suspended");
        source_destroyed = mailbox.pending_messages (domain) == 0;''')
# Promotion and cut of a claimed original belong to a serial Actor owner.
a=s.index('void verify_',s.index('void verify_multicast_admission')+10) if False else s.index('    mesh::service_mailbox_t mailbox;',s.index('void verify_promotion') if 'void verify_promotion' in s else s.index('failed promotion consumed'))
# Locate by the unique retained-original publication.
a=s.rfind('    mesh::service_mailbox_t mailbox;',0,s.index('"retained original publication failed"'))
b=s.index('\n}',a);block=s[a:b];block=block.replace('    require (mailbox.try_enqueue','    original.owner = "actor:promotion";\n    require (mailbox.try_enqueue',1).replace('domain, owner, 1,','domain, "actor:promotion", 1,').replace('cut_owners ({owner}','cut_owners ({"actor:promotion"}');s=s[:a]+block+s[b:]
# Run one owner drain across the exception matrix, preserving all original assertions.
a=s.index('    auto pending = target.dispatch_application_owner (',s.index('void verify_exception'));b=s.index('    try {',a)
s=s[:a]+'''    while (mailbox.pending_messages (domain) != 0) {
        auto pending = target.dispatch_application_owner (
          owner,
          [&] (const auto &, const auto &, auto, auto permit) {
              if (++calls == 2)
                  throw std::runtime_error ("handler fixture");
          },
          [&] { ++starts; }, [&] { ++rejects; });
        require (zlink::framework::test::application_owner_result (pending).value (),
                 "record failure aborted claim continuation");
    }
'''+s[b:]
p.write_text(s)
