from pathlib import Path
p=Path('framework/languages/cpp/framework/src/runtime/channels/channel_host_service.cpp');s=p.read_text();s=s.replace('''                while (auto claim =
                         mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                  owner, dispatch_limits::receive_batch_messages,
                                                  dispatch_limits::receive_batch_bytes)) {
                    try {
                        for (std::size_t index = 0; index != claim->claimed_messages; ++index) {
                            if (auto record = mailbox.take ({claim, index}))
                                report_dispatch_rejection (
                                  messaging::message_parts_t (record->take_parts ()));
                        }
                    }''','''                for (;;) {
                    std::optional<mesh::service_mailbox_record_t> original;
                    auto claim = mailbox.try_claim_owner (
                      mesh::service_mailbox_domain_t::application, owner,
                      dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes,
                      [&] (const auto &claim) {
                          original.emplace (std::move (*claim->records.front ()));
                          claim->records.front ()->application.reset ();
                      });
                    if (!claim)
                        break;
                    try {
                        report_dispatch_rejection (
                          messaging::message_parts_t (original->take_parts ()));
                    }''')
s=s.replace('''                claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner,
                                                 dispatch_limits::receive_batch_messages,
                                                 dispatch_limits::receive_batch_bytes);''','''                std::optional<mesh::service_mailbox_record_t> original;
                claim = mailbox.try_claim_owner (
                  mesh::service_mailbox_domain_t::application, owner,
                  dispatch_limits::receive_batch_messages, dispatch_limits::receive_batch_bytes,
                  [&] (const auto &claim) {
                      original.emplace (std::move (*claim->records.front ()));
                      claim->records.front ()->application.reset ();
                  });''')
s=s.replace('''                for (std::size_t index = 0; index != claim->claimed_messages; ++index) {
                    if (auto record = mailbox.take ({claim, index})) {''','''                {''')
s=s.replace('dispatch_application (std::move (*record));\n                    }','dispatch_application (std::move (*original));')
p.write_text(s)
p=Path('framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp');s=p.read_text().replace('queue.records.empty () && queue.kind == application_owner_kind (owner)','queue.records.empty () && (!queue.kind || queue.kind == application_owner_kind (owner))');p.write_text(s)
# Only keep the regression and explicit gated control as regular test selectors.
p=Path('framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp');s=p.read_text();a=s.index('    if (argc == 2 && std::string (argv[1]) == "--same-owner-blocking-repro")');b=s.index('    if (argc == 2 && std::string (argv[1]) == "--same-owner-gated-control")',a);s=s[:a]+s[b:];p.write_text(s)
