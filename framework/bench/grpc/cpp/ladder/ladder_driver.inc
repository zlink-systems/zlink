/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
// Included inside the existing bench driver's anonymous namespace.
class ladder_driver_t final : public driver_t
{
  public:
    ladder_driver_t (const options_t &options, int window, bool command, int level) :
        driver_t (options, window, command), _level (level),
        _target (zlink::routing_id_t::from (options.ladder_target_rid))
    {
        const auto rid = zlink::routing_id_t::from ("bench-ladder-" + std::to_string (::getpid ()));
        if (level >= 0) {
            _owner = std::make_unique<ladder::fw::runtime::mesh::raw_mesh_node_owner_t> (
              ladder::owner_options (rid, "tcp://127.0.0.1:0"));
            _owner->ablation_start ();
            _socket = &_owner->ablation_socket ();
        } else {
            _context = std::make_unique<zlink::context_t> ();
            _poller = std::make_unique<zlink::poller_t> ();
            _raw_socket = std::make_unique<zlink::router_socket_t> (*_context);
            _socket = _raw_socket.get ();
            _socket->set_routing_id (rid);
            _socket->bind ("tcp://127.0.0.1:0");
            _poller->add (*_socket, zlink::poll_event_flag_t::pollin
                                 | zlink::poll_event_flag_t::pollcompletion, 1);
            _local = ladder::descriptor (rid, _socket->options ().last_endpoint ());
        }
        _socket->connect (options.framework_endpoint);
        prepare_streams (stream_count ());
        std::fprintf (stderr, "#4350 source L%d: %s\n", level,
                      _owner ? "real raw_mesh_node_owner_t; binding completion"
                             : "raw binding peer; canonical wire/admission adapter");
        _completion_thread = std::thread ([this] {
            pthread_setname_np (pthread_self (), "ladder-binding");
            try {
                if (_owner) {
                    zlink::poll_event_t event{};
                    while (!_stop)
                        _owner->ablation_poller ().wait (&event, 1, std::chrono::milliseconds (50));
                } else {
                    while (!_stop) {
                        zlink::poll_event_t event{};
                        if (!_poller->wait (&event, 1, std::chrono::milliseconds (50)))
                            continue;
                        zlink::received_t received;
                        while (_socket->recv (received, zlink::recv_flags_t::dontwait) == 0)
                            receive_control (received);
                    }
                }
            } catch (const std::exception &error) {
                std::fprintf (stderr, "ladder completion: %s\n", error.what ());
                _failed = true;
            }
        });
    }
    ~ladder_driver_t () override
    {
        _stop = true;
        _completion_thread.join ();
    }
    bool await_ready (int timeout_ms) override
    {
        const auto deadline = clock_t_::now () + std::chrono::milliseconds (timeout_ms);
        if (_level < 0) {
            const auto hello = ladder::protocol::encode_route_mesh_admission (
              ladder::protocol::command::hello, _local);
            while (clock_t_::now () < deadline && !_failed) {
                if (_admitted) {
                    ladder::control (*_socket, _target, ladder::protocol::encode_route_mesh_admission (
                      ladder::protocol::command::admit, _local));
                    return true;
                }
                try { ladder::control (*_socket, _target, hello); }
                catch (const zlink::submit_error_t &error) {
                    if (error.result () != zlink::submit_result_t::not_connected)
                        throw;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
            return false;
        }
        while (clock_t_::now () < deadline && !_failed) {
            auto stream = std::make_shared<stream_t> (0, deadline, _run_id, phase_warmup, 1024);
            stream->counters.enter ();
            ++stream->counters.submitted;
            try { submit (stream, 0, 0); }
            catch (const std::exception &) {
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
                continue;
            }
            std::unique_lock lock (stream->mutex);
            stream->changed.wait_until (lock, deadline, [&] { return stream->counters.outstanding == 0; });
            if (stream->counters.completed > 0)
                return true;
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        }
        return false;
    }
  private:
    void receive_control (zlink::received_t &received)
    {
        if (!ladder::handle_control (*_socket, received, _local, &_admitted))
            throw std::runtime_error ("unexpected binding client receive");
    }
    void prepare_streams (int count) override
    {
        _encoders.clear ();
        if (_level == 0 || _level == 1)
            return;
        for (int i = 0; i < count; ++i)
            _encoders.emplace_back (std::make_unique<ladder::encoder_t> (_level));
    }
    void submit (std::shared_ptr<stream_t> stream, int index, uint64_t seq) override
    {
        if (_level == 0 || _level == 1) {
            submit_raw (*_socket, _target, std::move (stream), seq);
            return;
        }
        auto parts = _encoders[index]->encode (stream->size, stream->run_id, stream->phase, seq, _command,
                                              std::chrono::milliseconds (_options.request_timeout_ms));
        if (_command) {
            auto submission = _socket->send (_target).message (parts[0]).message (parts[1]).async ();
            observe_binding (std::move (submission.admitted),
              [stream, seq] { stream->finish (0, nullptr, seq); stream->admit (); },
              [stream, seq] (const auto &error) {
                  stream->finish (0, nullptr, seq, &error); stream->admit (false);
              });
        } else {
            const auto timeout = std::chrono::milliseconds (_options.request_timeout_ms);
            auto submission = _socket->request (_target).message (parts[0]).message (parts[1])
              .timeout (timeout).async ();
            const auto level = _level;
            observe_binding (std::move (submission.reply),
              [stream, seq, level] (const auto &reply) {
                  try {
                      auto payload = ladder::decode_reply (reply, level, seq);
                      stream->finish (0, &payload, seq);
                  } catch (const std::exception &error) {
                      stream->finish (0, nullptr, seq, &error);
                  }
              },
              [stream, seq] (const auto &error) { stream->finish (0, nullptr, seq, &error); });
            observe_binding (std::move (submission.admitted),
              [stream] { stream->admit (); },
              [stream] (const auto &) { stream->admit (false); });
        }
    }
    int _level;
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<ladder::fw::runtime::mesh::raw_mesh_node_owner_t> _owner;
    std::unique_ptr<zlink::router_socket_t> _raw_socket;
    zlink::router_socket_t *_socket = nullptr;
    zlink::routing_id_t _target;
    ladder::fw::runtime::mesh::service_node_descriptor_t _local;
    std::unique_ptr<zlink::poller_t> _poller;
    std::atomic<bool> _stop{false}, _admitted{false}, _failed{false};
    std::thread _completion_thread;
    std::vector<std::unique_ptr<ladder::encoder_t>> _encoders;
};
