from pathlib import Path
p=Path('/root/worktree/zlink-3867-creply2/framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_reply_close.cpp');s=p.read_text()
s=s.replace('#include <memory>','#include <memory>\n#include <map>\n#include <cstdlib>')
s=s.replace('void *last_socket = nullptr;','''void *last_socket = nullptr;
bool hold_reply = true;
std::map<int, std::uint64_t> submit_codes;
std::atomic_int allocated_messages{0}, released_messages{0};

std::vector<zlink::message_t> reply_messages ()
{
    auto *data = new std::uint8_t[256]{};
    ++allocated_messages;
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::advanced::external_message_t::from (
      std::span<std::uint8_t> (data, 256), [] (void *data, void *) {
          delete[] static_cast<std::uint8_t *> (data);
          ++released_messages;
      }));
    return parts;
}''')
s=s.replace('''    while (!allow_native_entry.load (std::memory_order_acquire))
        std::this_thread::yield ();
    raw_reply_probe_arm ();''','''    if (hold_reply) {
        while (!allow_native_entry.load (std::memory_order_acquire))
            std::this_thread::yield ();
        raw_reply_probe_arm ();
    }''')
s=s.replace('    raw_reply_probe_disarm ();\n    return result;','    raw_reply_probe_disarm ();\n    ++submit_codes[static_cast<int> (result)];\n    return result;')
s=s.replace('''    if (injected_failures.load () > 0) {
        --injected_failures;''','''    if (injected_failures.load () != 0) {
        if (injected_failures > 0)
            --injected_failures;''')
s=s.replace('bool verify_race (bool destructor_only, int iteration)','bool verify_race (bool destructor_only, int iteration, bool controlled = true)')
s=s.replace('    raw_reply_probe_reset ();','    hold_reply = controlled;\n    raw_reply_probe_reset ();\n    allocated_messages = released_messages = 0;')
s=s.replace('    source.set_receive_flow_state (zlink::receive_flow_state_t::paused);','    if (controlled)\n        source.set_receive_flow_state (zlink::receive_flow_state_t::paused);')
s=s.replace('''                backend::raw_message_t body{backend::raw_bytes_t (256, 42)};
                if (!port.reply (request, std::move (body)))''','''                if (!port.reply (request, reply_messages ()))''')
s=s.replace('    const bool held = until ([] { return raw_reply_probe_entered (); });','    const bool held = !controlled || until ([] { return raw_reply_probe_entered (); });')
s=s.replace('        release_on_busy = true;','        release_on_busy = controlled;')
s=s.replace('    assert (!port.reply (requests.front (), backend::raw_message_t{{42}}));','''    const auto native_calls = submit_codes;
    assert (!port.reply (requests.front (), reply_messages ()));
    assert (submit_codes == native_calls);''')
s=s.replace('''        assert (busy_attempts == 1);
        assert (close_attempts == 2);
        assert (held_submit_result == ZLINK_SUBMIT_OK);
        assert (retry_gap_ns >= 1'000'000);''','''        if (controlled) {
            assert (busy_attempts == 1);
            assert (close_attempts == 2);
            assert (held_submit_result >= 0);
            assert (retry_gap_ns >= 1'000'000);
        }''')
s=s.replace('    source.close ();\n    return closed;','''    source.close ();
    context.term ();
    assert (allocated_messages == released_messages);
    return closed;''')
a=s.index('void verify_failure_retention ()');b=s.index('\n}\nint main',a)
s=s[:a]+'''void verify_failure_retention ()
{
    zlink::context_t context;
    auto socket = std::make_unique<zlink::router_socket_t> (context);
    auto second = std::make_unique<zlink::router_socket_t> (context);
    reply_socket = last_socket;
    std::swap (socket, second);
    close_attempts = 0;
    injected_failures = -1;
    const auto started = std::chrono::steady_clock::now ();
    const auto deadline = started + 20ms;
    bool failed = false;
    try { backend::close_route_socket (socket, deadline); }
    catch (const zlink::close_error_t &error) {
        failed = error.result () == zlink::close_result_t::busy;
    }
    assert (failed && socket && socket->valid () && close_attempts > 1);
    assert (std::chrono::steady_clock::now () >= deadline);
    const auto attempts = close_attempts.load ();
    injected_failures = 0;
    backend::close_route_socket (socket);
    assert (!socket && close_attempts == attempts + 1);

    // The same expired deadline is not renewed for the next socket.
    reply_socket = nullptr;
    auto third = std::make_unique<zlink::router_socket_t> (context);
    reply_socket = last_socket;
    close_attempts = 0;
    injected_failures = -1;
    failed = false;
    try { backend::close_route_socket (third, deadline); }
    catch (const zlink::close_error_t &error) {
        failed = error.result () == zlink::close_result_t::busy;
    }
    assert (failed && third && third->valid () && close_attempts == 1);
    injected_failures = 0;
    backend::close_route_socket (third);
    assert (!third && close_attempts == 2);

    reply_socket = nullptr;
    socket = std::make_unique<zlink::router_socket_t> (context);
    reply_socket = last_socket;
    close_attempts = 0;
    injected_failures = 1;
    injected_result = ZLINK_CLOSE_INTERNAL_ERROR;
    failed = false;
    try { backend::close_route_socket (socket); }
    catch (const zlink::close_error_t &error) {
        failed = error.result () == zlink::close_result_t::internal_error;
    }
    assert (failed && socket && socket->valid () && close_attempts == 1);
    backend::close_route_socket (socket);
    assert (!socket && close_attempts == 2);
    injected_result = ZLINK_CLOSE_BUSY;
    std::printf ("BUSY deadline: 20 ms, attempts=%d, retained then cleaned; "
                 "shared expired deadline: one attempt; non-BUSY: no retry\\n", attempts);
}
''' +s[b:]
s=s.replace('const int repetitions = mode == "race" || mode == "destructor-control" ? 100 : 1;','''const int repetitions = argc > 2 ? std::atoi (argv[2])
      : mode == "race" || mode == "destructor-control" ? 100 : 1;
    assert (repetitions > 0);''')
s=s.replace('verify_race (mode == "destructor-control", iteration);','verify_race (mode == "destructor-control", iteration, mode != "race");')
s=s.replace('    if (leaks) {','''    for (const auto &[code, count] : submit_codes)
        std::printf ("submit_result=%d count=%llu\\n", code,
                     static_cast<unsigned long long> (count));
    std::printf ("message ownership: all %d iterations consumed or released\\n", repetitions);
    if (leaks) {''')
p.write_text(s)
