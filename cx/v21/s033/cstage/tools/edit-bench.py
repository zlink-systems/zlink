exec(open('/root/jobs/fx/cx/v21/s033/cstage/tools/edit-core.py').read().split("new('core/src/api/socket/staged_request_prototype.hpp'")[0])
def bench(s):
 s=s.replace('#include <csignal>','#include <csignal>\n#include <pthread.h>')
 a=s.index('class zlink_raw_driver_t');b=s.index('\nnamespace fw =',a)
 block=s[a:b]
 block=block.replace('        _socket = std::make_unique<zlink::router_socket_t> (_context);','''        _staged = std::getenv ("ZLINK_CPP_BENCH_STAGED") != nullptr;
        _socket = std::make_unique<zlink::router_socket_t> (_context);''')
 block=block.replace('        _deadline = deadline;','        pthread_setname_np (pthread_self (), "cstage-caller");\n        _deadline = deadline;')
 pos=block.index('    task_t request_slot ()')
 block=block[:pos]+'''    zlink::request_submission_t submit_request (size_t size, phase_t phase, uint64_t seq)
    {
        if (_staged) {
            const size_t body_size = std::max (size, k_header_size);
            size_t varint_bytes = 1;
            for (size_t v = body_size; v >= 128; v >>= 7)
                ++varint_bytes;
            return request_operation ().deferred (
              {std::strlen (request_envelope ()), body_size + 1 + varint_bytes},
              [this, size, phase, seq] {
                  auto parts = make_parts (size, phase, seq);
                  std::vector<zlink::message_t> result;
                  result.reserve (2);
                  result.push_back (std::move (parts.first));
                  result.push_back (std::move (parts.second));
                  return result;
              }).timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async ();
        }
        auto parts = make_parts (size, phase, seq);
        return request_operation ().message (parts.first).message (parts.second)
          .timeout (std::chrono::milliseconds (_options.request_timeout_ms)).async ();
    }

'''+block[pos:]
 # Only request_slot and request_submitter share this exact request expression.
 block=block.replace('            auto parts = make_parts (_payload_size, _phase, seq);\n            _counters->enter ();','            _counters->enter ();',2)
 block=block.replace('''                  request_operation ()
                    .message (parts.first)
                    .message (parts.second)
                    .timeout (std::chrono::milliseconds (_options.request_timeout_ms))
                    .async ();''','''                  submit_request (_payload_size, _phase, seq);''')
 block=block.replace('    uint64_t _request_rate = 0;','    uint64_t _request_rate = 0;\n    bool _staged = false;')
 return s[:a]+block+s[b:]
edit('framework/bench/grpc/cpp/client/bench_cpp_client.cpp',bench)
# Preserve EAGAIN while closing already-consumed placeholder shells.
edit('core/src/api/socket/socket_request_reply_submit_api.cpp',lambda s:s.replace('    *stage_ = capture.identity;\n    for','    *stage_ = capture.identity;\n    const int saved_errno = errno;\n    for').replace('        (void) zlink_msg_close (&parts[i]);\n    return result;','        (void) zlink_msg_close (&parts[i]);\n    errno = saved_errno;\n    return result;'))
print('Bench staged and common-rate pacing ready')
