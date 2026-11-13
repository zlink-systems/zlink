from pathlib import Path
root=Path('/root/worktree/zlink-v21-s033/l3868-next')
b=root/'framework/bench/grpc/cpp'
p=b/'framework/bench_framework_cpp_server.cpp'
s=p.read_text().replace('struct echo_handler_t','template <typename Payload = payload_t> struct echo_handler_t').replace('''    payload_t handle (const payload_t &request)''','''    Payload handle (const Payload &request)''').replace('struct command_handler_t','template <typename Payload = payload_t> struct command_handler_t').replace('''    void handle (const payload_t &request)''','''    void handle (const Payload &request)''')
s=s.replace('''        options.codecs ().use (zlink::framework_codecs::protobuf ());''','''#if defined(ZLINK_CPP_ABLATION_LADDER)
        if (level != 7)
#endif
            options.codecs ().use (zlink::framework_codecs::protobuf ());''')
s=s.replace('''        mesh.add_route_request_handler<echo_handler_t, payload_t, payload_t> ("BenchPayload")
          .add_route_send_handler<command_handler_t, payload_t> ("BenchPayload");''','''#if defined(ZLINK_CPP_ABLATION_LADDER)
        if (level == 7) {
            mesh.add_route_request_handler<echo_handler_t<ladder::typed_payload_t>,
                                          ladder::typed_payload_t, ladder::typed_payload_t> ("BenchPayload")
              .add_route_send_handler<command_handler_t<ladder::typed_payload_t>,
                                      ladder::typed_payload_t> ("BenchPayload");
        } else
#endif
        {
            mesh.add_route_request_handler<echo_handler_t<>, payload_t, payload_t> ("BenchPayload")
              .add_route_send_handler<command_handler_t<>, payload_t> ("BenchPayload");
        }''')
p.write_text(s)
p=b/'client/bench_cpp_client.cpp';s=p.read_text();start=s.index('class framework_driver_t final');end=s.index('\nint pattern_window',start);part=s[start:end];part=part.replace('class framework_driver_t final','template <typename Payload = payload_t>\nclass framework_driver_t final').replace('''        config.codecs ().use (zlink::framework_codecs::protobuf ());''','''        if constexpr (std::is_same_v<Payload, payload_t>)
            config.codecs ().use (zlink::framework_codecs::protobuf ());''').replace('make_payload (1024, phase_warmup, 0)','make_request (1024, phase_warmup, 0)').replace('.async<payload_t> ()','.template async<Payload> ()').replace('''    void submit (std::shared_ptr<stream_t> stream, int, uint64_t seq) override
    {
        const auto sent = _command ? 0 : now_ns ();
        auto payload = make_payload (stream->size, stream->phase, seq);''','''    Payload make_request (size_t size, phase_t phase, uint64_t seq)
    {
#if defined(ZLINK_CPP_ABLATION_LADDER)
        if constexpr (std::is_same_v<Payload, ladder::typed_payload_t>)
            return ladder::make_typed_payload (size, _run_id, phase, seq);
        else
#endif
            return make_payload (size, phase, seq);
    }
    void submit (std::shared_ptr<stream_t> stream, int, uint64_t seq) override
    {
        // The ladder derives latency from the same in-body stamp as L4.
        const auto sent = std::is_same_v<Payload, payload_t> && !_command ? now_ns () : 0;
        auto payload = make_request (stream->size, stream->phase, seq);''').replace('''                stream->finish (sent, result ? &result.value () : nullptr, seq,
                                result ? nullptr : result.error ());''','''                stream->finish_bytes (sent, result ? result.value ().body ().data () : nullptr,
                                      result ? result.value ().body ().size () : 0, seq,
                                      result ? nullptr : result.error ());''');s=s[:start]+part+s[end:];s=s.replace('''                      _options.ladder_raw_mesh ? -1 : fw::detail::ablation::level ());
                else''','''                      _options.ladder_raw_mesh ? -1 : fw::detail::ablation::level ());
                else if (_options.implementation == "zlink-framework-cpp"
                         && fw::detail::ablation::level () == 7)
                    _driver = std::make_unique<framework_driver_t<ladder::typed_payload_t>> (
                      _options, window, command);
                else''').replace('std::make_unique<framework_driver_t>','std::make_unique<framework_driver_t<>>');p.write_text(s)
p=root/'framework/languages/cpp/framework/include/zlink/framework/detail/ablation_ladder.hpp';s=p.read_text().replace('L0/L1/L2/L3/L4 and','L0/L1/L2/L3/L4/L5_7 and').replace('parsed != 4 && parsed != 8','parsed != 4 && parsed != 7 && parsed != 8').replace('L4 (4) and main','L4 (4), L5_7 (7) and main');p.write_text(s)
