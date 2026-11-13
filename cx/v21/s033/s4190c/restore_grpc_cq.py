from pathlib import Path
p=Path('framework/bench/grpc/cpp/client/bench_cpp_client.cpp');s=p.read_text().replace('''        _channel (grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ())) {}''','''        _channel (grpc::CreateChannel (options.grpc_endpoint, grpc::InsecureChannelCredentials ())),
        _completion_thread ([this] {
            pthread_setname_np (pthread_self (), "bench-grpc-cq");
            void *tag = nullptr;
            bool ok = false;
            while (_cq.Next (&tag, &ok)) {
                std::unique_ptr<call_base_t> call (static_cast<call_base_t *> (tag));
                call->finish (ok);
            }
        }) {}
    ~grpc_driver_t () override
    {
        _cq.Shutdown ();
        _completion_thread.join ();
    }''')
a=s.index('    template <typename Reply> struct call_t',s.index('class grpc_driver_t'))
b=s.index('    void submit (std::shared_ptr<stream_t> stream',a)
s=s[:a]+'''    struct call_base_t
    {
        virtual ~call_base_t () = default;
        virtual void finish (bool ok) = 0;
    };
    template <typename Reply> struct call_t final : call_base_t
    {
        grpc::ClientContext context;
        payload_t request;
        Reply reply;
        grpc::Status status;
        std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>> reader;
        std::shared_ptr<stream_t> stream;
        uint64_t seq = 0, sent = 0;

        void finish (bool ok) override
        {
            if (!ok || !status.ok ()) {
                const std::runtime_error error (ok
                  ? "gRPC code " + std::to_string (status.error_code ()) + ": " + status.error_message ()
                  : "gRPC completion queue event was not ok");
                stream->finish (0, nullptr, seq, &error);
            } else {
                const payload_t *body = nullptr;
                if constexpr (std::is_same_v<Reply, payload_t>)
                    body = &reply;
                stream->finish (body ? sent : 0, body, seq);
            }
        }
    };
    template <typename Reply>
    void start (std::shared_ptr<stream_t> stream, int index, uint64_t seq)
    {
        auto call = std::make_unique<call_t<Reply>> ();
        call->stream = stream;
        call->seq = seq;
        call->context.set_deadline (std::chrono::system_clock::now ()
                                   + std::chrono::milliseconds (_options.request_timeout_ms));
        call->request = make_payload (stream->size, stream->phase, seq);
        call->sent = now_ns ();
        if constexpr (std::is_same_v<Reply, payload_t>)
            call->reader = _stubs[index]->PrepareAsyncEcho (&call->context, call->request, &_cq);
        else
            call->reader = _stubs[index]->PrepareAsyncCommand (&call->context, call->request, &_cq);
        call->reader->StartCall ();
        auto *published = call.release ();
        published->reader->Finish (&published->reply, &published->status,
                                   static_cast<call_base_t *> (published));
        stream->admit ();
        // Preserve the uncapped driver's cooperative yield. CQ callbacks stay
        // on its completion resource; no stream waits for a reply here.
        if (_window <= 0)
            std::this_thread::yield ();
    }
'''+s[b:]
a=s.index('    std::shared_ptr<grpc::Channel> _channel;',s.index('class grpc_driver_t'))
s=s[:a]+s[a:].replace('''    std::shared_ptr<grpc::Channel> _channel;
    std::vector<std::unique_ptr<zlink::framework::bench::withgrpc::BenchService::Stub>> _stubs;''','''    std::shared_ptr<grpc::Channel> _channel;
    std::vector<std::unique_ptr<zlink::framework::bench::withgrpc::BenchService::Stub>> _stubs;
    grpc::CompletionQueue _cq;
    std::thread _completion_thread;''',1)
p.write_text(s)
