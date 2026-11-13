**Core I/O cost under Framework and raw binding — #3867, #3868, #3870 / #1639**

The largest measured difference is **TCP-call amortization**. Framework and binding send **two physical ZMP parts per application record**, use the **same native Core build**, and maintain **two TCP connections per ROUTER peer pair**. Framework sends fewer messages per TCP write. Java makes the mechanism particularly clear: Framework averages about 1.75 messages per write versus 6.41 for binding, while sampled TCP-send CPU per call is about 15.6 versus 15.3 µs.

For C++ request-backpressure, the hottest source I/O thread costs 21.83 µs/message under Framework versus 2.88 under binding; total source I/O CPU is 38.59 versus 5.43. About 69% of this aggregate excess is below TCP send/receive calls. Smaller batches explain the dominant measured mechanism. **Whether faster Framework submission alone will produce binding-sized batches is not measured**: no rate-matched or patched counterfactual was run.

**Measurement scope and interpretation**

- Read-only investigation on detached HEAD `ee1139025782688e33109455041d8a0222cc8fe3`. The four pre-existing runner pinning changes were preserved. No tracked files, branches, commits, packages, or builds were changed. All new scripts and outputs are in this directory. No agents or other models were invoked.
- Every benchmark and attached tracer held `flock -o /root/jobs/fx/cx/measure.lock`. Source CPUs were 0–7; target CPUs were 8–15. Job-local runner copies preserve the benchmark behavior and use the existing builds with `SKIP_BUILD=1`, `PAYLOAD_SIZES=1024`, one pattern/implementation, `RUNS=1`, warmup 10 s, and active 30–60 s. Request mailbox follow-ups used 45 s active. Lock waits are recorded in [matrix.log](matrix.log).
- CPU: per-thread `/proc/PID/task/TID/schedstat` deltas over approximately 10 s; samples from `perf record -e cpu-clock -F 149 --call-graph dwarf,16384`. CPU sampling ran first, before syscall or mailbox probes. CPU denominators are completed requests, or target received messages for send. I/O totals sum all `ZLINKbg/IO/*` threads; idle contexts are included. These are CPU time, not syscall elapsed time or end-to-end latency.
- Counters: separate 8 s BPF windows, with a start/stop gate and per-process/thread/FD syscall entry, return, successful byte, and epoll timeout/return aggregates. The main tables use the **latest syscall-only window** for each case. To avoid HTTP/proc snapshot boundary skew, counter denominators are forward-data TCP bytes divided by the decoded fixed record size. This is a wire-message estimate; rare small service-control records and partial boundary records produce a small error. Completed/received snapshots remain in the raw evidence. The two processes can also differ by data already in flight at the boundaries.
- Wire and callers: approximately 1 s `strace -f -k -xx -s 131072 -v -yy`, after the counter window. TCP streams were reassembled and ZMP headers decoded. `strace` changes batching substantially; **its batch sizes and timings are not used as production performance measurements**. Core traffic is separated from benchmark HTTP statistics traffic.
- Mailboxes: later uprobes on the existing native `mailbox_t::send/recv` symbols and whole-record `zlink_request`, `zlink_send_rid`, and `zlink_reply` entry points. There is no `zlink_msg_send` entry point in this build. These probes materially slow the workload; their counts describe the probed run and omit compiler-inlined calls. They identify command types and callers, rather than supplying the main batching numbers.
- All inspected native libraries have build ID `cc6989f03587dd8c267953b75b2f40042efec476`, SHA-256 `6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01`. Java loads the same binary from its native cache. BPF CPU deltas are deliberately excluded from savings estimates because tracing executes work on the traced threads. `perf` uses the raw monotonic clock here, about 76 s ahead of Python monotonic time; samples are selected by the active-only recorder lifetime, not by comparing these different clocks directly.

`F / B` below means Framework / binding. “Message” in request-backpressure means one request operation; its request and reply are both shown. TCP byte counts exclude TCP/IP headers and ACKs.

**CPU per message**

| Case | Source I/O total, µs F / B | Source hottest, µs F / B | Target I/O total, µs F / B | Target hottest, µs F / B | CPU-window rate, msg/s F / B |
|---|---:|---:|---:|---:|---:|
| C++ request-backpressure | 38.59 / 5.43 | 21.83 / 2.88 | 38.77 / 5.65 | 21.67 / 3.39 | 36,601 / 267,305 |
| C++ send-saturation | 4.97 / 1.60 | 4.95 / 1.60 | 3.90 / 1.19 | 3.89 / 1.19 | 183,330 / 592,376 |
| Java send-saturation | 10.92 / 3.33 | 10.92 / 3.33 | 8.69 / 2.88 | 8.68 / 2.87 | 88,792 / 294,165 |

The binding C++ request source’s hottest thread receives replies. Its sending I/O thread costs 2.54 µs/message; Framework’s sending thread costs 21.83. Thus the hottest-thread comparison is not a claim that the thread roles are identical. Full TIDs, core shares, sample counts and stacks are in [perf-summary.json](perf-summary.json).

| Case / side | TCP send CPU, µs F / B | TCP receive CPU, µs F / B | epoll CPU, µs F / B | write CPU, µs F / B | Other I/O CPU, µs F / B |
|---|---:|---:|---:|---:|---:|
| C++ request-backpressure / source | 15.82 / 1.54 | 10.77 / 2.02 | 2.60 / 0.20 | 0.66 / 0.00 | 8.74 / 1.67 |
| C++ request-backpressure / target | 15.94 / 2.43 | 10.54 / 1.40 | 2.61 / 0.27 | 0.75 / 0.00 | 8.93 / 1.55 |
| C++ send-saturation / source | 3.46 / 1.03 | 0.00 / 0.00 | 0.10 / 0.02 | 0.00 / 0.00 | 1.41 / 0.55 |
| C++ send-saturation / target | 0.00 / 0.00 | 2.73 / 0.49 | 0.26 / 0.10 | 0.16 / 0.00 | 0.76 / 0.60 |
| Java send-saturation / source | 8.87 / 2.39 | 0.00 / 0.00 | 0.45 / 0.09 | 0.00 / 0.00 | 1.61 / 0.85 |
| Java send-saturation / target | 0.00 / 0.00 | 5.93 / 1.88 | 0.73 / 0.19 | 0.56 / 0.00 | 1.47 / 0.81 |

Stages are mutually exclusive sampled stack groups, scaled to each thread’s measured CPU. TCP groups include kernel networking and loopback processing below the libc calls, including TCP and netfilter paths. “Other” includes Core/Asio userspace, other syscalls, and small unsampled idle-thread totals. The finer division of that remainder between per-frame, per-batch, allocation, locking and cache costs is **not measured**.

**Wire parts, bytes and batching**

| Record | Physical parts F / B | Part body sizes F | Part body sizes B | ZMP bytes/record F / B |
|---|---:|---|---|---:|
| C++ request | 2 / 2 | 13, 1390 | 201, 1027 | 1427 / 1252 |
| C++ reply | 2 / 2 | 21, 1372 | 201, 1027 | 1417 / 1252 |
| C++ one-way send | 2 / 2 | 5, 1368 | 201, 1027 | 1389 / 1244 |
| Java one-way send | 2 / 2 | 5, 1363 | 201, 1027 | 1384 / 1244 |

Each request/reply has 24 ZMP header bytes: two 8-byte headers and an 8-byte sequence extension on the first frame. One-way records have 16 header bytes. Neither the routing ID nor the request sequence becomes an additional multipart payload frame in these captures. Core I/O uses scalar `sendto` buffers here: **one contiguous buffer per call; zero Core-I/O `sendmsg` calls** in all six selected counter windows. A contiguous buffer can contain several records and many ZMP parts. HTTP `sendmsg` calls in whole-process strace are excluded.

Framework carries the JSON application envelope and protobuf body **inside one binary `ZLinkFrameworkMultipart` payload part**, preceded by the service header. C++ request overhead versus binding is 175 bytes: 13 service-header bytes, 75 wrapper/inner-length bytes, and 87 additional JSON-header bytes. Reply overhead is 165 bytes. One-way overhead is 145 bytes in C++ and 140 in Java. These increases are about 11–14%, not additional physical parts. See [C++ application packing](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2688), [C++ service writer](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:2821), and [Java multipart writer](/root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:356).

| Data direction | Messages/successful send F / B | Messages/successful receive F / B | Mean TCP bytes/successful send F / B |
|---|---:|---:|---:|
| C++ request: source → target | 1.18 / 10.53 | 1.26 / 5.99 | 1690 / 13184 |
| C++ reply: target → source | 1.22 / 6.57 | 1.29 / 4.67 | 1727 / 8227 |
| C++ one-way: source → target | 4.48 / 13.11 | 4.17 / 6.59 | 6222 / 16307 |
| Java one-way: source → target | 1.75 / 6.41 | 1.84 / 5.04 | 2428 / 7974 |

These batching values use returned TCP bytes and successful call counts, so they do not depend on HTTP message-counter timing. Rare small control records are negligible in the byte ratios. A successful receive is not necessarily a whole record; the byte ratio is an average over the stream. [Wire decoder results](wire-summary.json) retain the decoded record counts, frame kinds, prefixes, control records and partial-boundary information.

| Case / side | sendto calls/msg F / B | recvfrom calls/msg F / B | recvfrom EAGAIN/msg F / B | TCP TX bytes/msg F / B | TCP RX bytes/msg F / B |
|---|---:|---:|---:|---:|---:|
| C++ request-backpressure / source | 0.844 / 0.095 | 1.162 / 0.286 | 0.383 / 0.072 | 1427.0 / 1252.0 | 1419.8 / 1250.7 |
| C++ request-backpressure / target | 0.822 / 0.152 | 1.191 / 0.205 | 0.396 / 0.038 | 1419.8 / 1250.7 | 1427.0 / 1252.0 |
| C++ send-saturation / source | 0.223 / 0.076 | 0.001 / 0.000 | 0.000 / 0.000 | 1389.0 / 1244.0 | 0.0 / 0.0 |
| C++ send-saturation / target | 0.000 / 0.000 | 0.355 / 0.154 | 0.116 / 0.002 | 0.0 / 0.0 | 1389.0 / 1242.0 |
| Java send-saturation / source | 0.570 / 0.156 | 0.000 / 0.000 | 0.000 / 0.000 | 1384.0 / 1244.0 | 0.0 / 0.0 |
| Java send-saturation / target | 0.000 / 0.000 | 0.777 / 0.220 | 0.233 / 0.022 | 0.0 / 0.0 | 1384.0 / 1244.0 |

**Notifications and epoll returns**

| Case / side | I/O eventfd reads/msg F / B | I/O eventfd writes/msg F / B | Other-thread eventfd reads/msg F / B | Other-thread eventfd writes/msg F / B |
|---|---:|---:|---:|---:|
| C++ request-backpressure / source | 0.000 / 0.000 | 0.219 / 0.000 | 1.300 / 0.031 | 0.383 / 0.016 |
| C++ request-backpressure / target | 0.000 / 0.000 | 0.405 / 0.000 | 1.584 / 0.000 | 0.167 / 0.000 |
| C++ send-saturation / source | 0.000 / 0.000 | 0.000 / 0.001 | 0.002 / 0.007 | 0.000 / 0.001 |
| C++ send-saturation / target | 0.000 / 0.000 | 0.109 / 0.000 | 0.405 / 0.000 | 0.001 / 0.000 |
| Java send-saturation / source | 0.000 / 0.000 | 0.000 / 0.000 | 0.000 / 0.013 | 0.000 / 0.000 |
| Java send-saturation / target | 0.000 / 0.000 | 0.291 / 0.000 | 0.825 / 0.000 | 0.001 / 0.000 |

The measured transport notification FDs are eventfds, not pipes. Core I/O eventfd reads are zero. The large C++ process-wide read count belongs primarily to the Framework receive owner, through Core’s `socket_poller_t` and `signaler_t::recv_failable`; many reads return EAGAIN. Successful notification reads/writes are 8 bytes. `recv_failable` can also write back a surplus counter, which makes the receive owner a writer as well. Thus process-wide read/write totals cannot be charged wholesale to an I/O thread. Per-FD and per-TID counts, including success/EAGAIN splits, are in [metrics.json](metrics.json) and [bpf-summary.json](bpf-summary.json).

| Case / side | epoll_wait calls/msg F / B | Nonempty returns/msg F / B | Nonempty, blocking permitted F / B | Nonempty, timeout=0 F / B | Other-thread Core reactor epoll_ctl/msg F / B |
|---|---:|---:|---:|---:|---:|
| C++ request-backpressure / source | 1.8168 / 0.3245 | 0.6556 / 0.1420 | 0.5716 / 0.0869 | 0.0840 / 0.0550 | 0.1888 / 0.0151 |
| C++ request-backpressure / target | 1.8369 / 0.3430 | 0.6923 / 0.1145 | 0.6141 / 0.0610 | 0.0783 / 0.0536 | 0.2179 / 0.0241 |
| C++ send-saturation / source | 0.2245 / 0.0764 | 0.0008 / 0.0001 | 0.0008 / 0.0001 | 0.0000 / 0.0000 | 0.0004 / 0.0000 |
| C++ send-saturation / target | 0.2410 / 0.1518 | 0.1384 / 0.0232 | 0.1160 / 0.0024 | 0.0224 / 0.0208 | 0.0005 / 0.0016 |
| Java send-saturation / source | 0.5716 / 0.1562 | 0.0007 / 0.0000 | 0.0007 / 0.0000 | 0.0000 / 0.0000 | 0.0007 / 0.0000 |
| Java send-saturation / target | 0.5448 / 0.2027 | 0.2748 / 0.1206 | 0.2320 / 0.0217 | 0.0428 / 0.0990 | 0.0000 / 0.0132 |

A nonempty epoll return is not necessarily a sleeping thread waking. “Blocking permitted” means the input timeout was nonzero; a ready descriptor can still return immediately. Exact `sched_wakeup` counts are **not measured**. In send-saturation, source I/O spends almost all epoll calls polling with no returned event while continuing its write-completion work. Calling all of those “wakeups” would misstate the mechanism. External `epoll_ctl` calls above target the Core I/O reactor FDs, excluding the benchmark HTTP reactor. They arise from Core/Asio mailbox posts, rather than evidence of a Framework socket re-registration per message.

**Caller and ownership attribution**

| Extra work | Observed call path and owner | Classification | Implication |
|---|---|---|---|
| More TCP sends and smaller writes | Framework `raw_mesh_node_owner_t::request_with_header` / `start_send_on_lane` → `raw_route_port_t::request` / `submit_send` → binding operation → Core `asio_engine_t::prepare_output_buffer` / `start_async_write` → `tcp_transport_t::async_write_some` → TCP/kernel | **Batching consequence**, executed by Core | The Core encoder already pulls queued frames until its output target or lack of data stops it. Neither path adds a syscall per part. Faster submission, reply production, or completion handling may change queue occupancy; their causal contributions are not isolated. |
| More TCP receives and EAGAIN calls | Core `asio_engine_t::start_async_read` / `on_read_complete` → `tcp_transport_t::async_read_some` and Asio reactor receive operations | **Core/binding behavior triggered by traffic cadence** | Smaller arriving byte batches amortize receive and read-arm checks over fewer messages. These are native receive/arming calls, not a Framework retry loop. |
| I/O-thread eventfd writes | Core `asio_engine_t::process_input` → `pipe_t::flush_unlocked` → `object_t::send_activate_read` / `send_pipe_command` → `mailbox_t::send` → `signaler_t::send` | **Core/binding behavior**, sensitive to receiver quiescence and batching | Wakes the pipe reader after an inactive-to-pending edge. Framework’s public poller requires descriptor notification; raw target’s blocking receive uses a different wait path. The write bucket is only 0.66–0.75 µs/request across each endpoint’s I/O threads in C++. |
| Receive-owner reads and self-notifications | Framework `node_receive_role_t::rotate` / `raw_route_port_t::poll` → binding `poller_t::wait` → Core `socket_poller_t::check_socket_events` / `collect_socket_event`; Core reply drain calls `notify_request_completion` → mailbox → eventfd | **Framework polling architecture triggers Core/binding behavior** | The native poller owns the descriptor reads. Reply-completion notification can be produced while that same receive owner drains replies. This work is primarily outside I/O threads. |
| Reactor interrupts and handler dispatch | Core `mailbox_t::schedule_if_needed_unlocked` → `boost::asio::post` / `scheduler::post_immediate_completion` → `epoll_reactor::interrupt` (`epoll_ctl`) | **Core/binding behavior**, amortized by batching | The mailbox has an existing scheduled/pending decision. More returns to idle produce more posts and readiness processing. Remaining Core/Asio userspace CPU is measured as a group; its exact per-call/per-frame split is not measured. |
| Additional service/application wrapper bytes | Framework `public_host_runtime_t::encode_application`, `application_payload_t::from_parts`, `encode_application_payload`; Java `ZLinkJavaRawMeshNode.encodeApplicationFrames` / `ZLinkServiceM6AWireCodec.encodeFrameworkMultipartMessage` | **Framework wire-format choice, currently a shared contract** | Adds approximately 11–14% of bytes. The optional metadata part is absent in these application records. It does not multiply the physical frame count. A format change requires contract review; changing benchmark codecs would not establish a runtime fix. |
| Separate Completion connection | Core transport-pair creation in `socket_base_endpoint.cpp`; reply routing in `socket_request_reply_runtime_io.cpp` | **Core/binding behavior common to both** | Two established TCP connections in every case: request/data on the Application lane, replies on the Completion lane. One-way uses the data lane; the second lane is idle or carries control. No extra application copy or Framework-only connection multiplier is observed. |
| Request completion entry | Binding `request_submit_operation_t::async`, `request_completion_bundle_t`, `completion_owner_t::start_async_request`; Core `notify_request_completion` | **Common binding entry, Core notification coalescing** | Both C++ paths use the same async request implementation and native two-part request API. One entry does not force one I/O-thread eventfd write per reply. Pending notification coalescing and drain cadence determine the observed commands. |

Source anchors: [Framework C++ request construction](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1526), [Framework C++ binding request](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:313), [Framework C++ poll](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:365), [binding async request](/root/worktree/zlink-ratio040/bindings/cpp/src/Runtime/Messaging/request_reply.cpp:102), [Core output buffer](/root/worktree/zlink-ratio040/core/src/runtime/engine/asio/asio_engine.cpp:1314), [Core TCP write](/root/worktree/zlink-ratio040/core/src/runtime/transports/tcp/tcp_transport.cpp:519), [Core pipe flush](/root/worktree/zlink-ratio040/core/src/runtime/core/pipe_write.cpp:1430), [Core mailbox](/root/worktree/zlink-ratio040/core/src/runtime/core/mailbox.cpp:72), [Core poller](/root/worktree/zlink-ratio040/core/src/runtime/core/socket_poller.cpp:543), [Core completion notification](/root/worktree/zlink-ratio040/core/src/runtime/sockets/common/socket_base_dispatch.cpp:325), [Core Completion lane creation](/root/worktree/zlink-ratio040/core/src/runtime/sockets/common/socket_base_endpoint.cpp:872), [Java Framework send](/root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:202), [Java binding native calls](/root/worktree/zlink-ratio040/bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java:119).

The C++ request strace directly shows the source receive owner writing its primary eventfd through `complete_reply_from_transport → notify_request_completion`; target I/O writes through `process_input → pipe_t::flush_unlocked`. The C++ send strace also resolves `epoll_ctl` through Core’s scheduled mailbox post, and target flow-control publication through `zlink_socket_set_receive_flow_state`. Full native stack chains are retained beside each `.strace` as `.wire.json`.

**Observed mailbox command calls**

The following counts come from the latest **uprobe window**, normalized by its forward wire-message estimate. They are diagnostic symbol-call counts, not the syscall-only window above; compiler-inlined calls are not counted. `activate_read` is the normal pipe activation command (enum 5), `activate_write` is cumulative credit return (6), and `request_completion` is the coalesced socket-completion notification (9).

| Case / command sender | activate_read/msg F / B | activate_write/msg F / B | request_completion/msg F / B |
|---|---:|---:|---:|
| C++ request-backpressure / source | 1.508 / 0.230 | 0.000 / 0.000 | 0.977 / 0.031 |
| C++ request-backpressure / target | 1.465 / 0.269 | 0.000 / 0.015 | 0.000 / 0.000 |
| C++ send-saturation / source | 0.355 / 0.147 | 0.000 / 0.000 | 0.000 / 0.000 |
| C++ send-saturation / target | 0.182 / 0.098 | 0.000 / 0.139 | 0.000 / 0.000 |
| Java send-saturation / source | 0.734 / 0.363 | 0.000 / 0.000 | 0.000 / 0.000 |
| Java send-saturation / target | 0.360 / 0.196 | 0.000 / 0.268 | 0.000 / 0.000 |

The request uprobes observe two-part `zlink_request` and two-part `zlink_reply` calls in both implementations. Occasional one-part `zlink_send_rid` calls are service-control records. Framework’s request-completion command is close to one per operation in this heavily probed run; binding coalesces many replies per notification. The registered entry is common; the notification cadence differs. The count alone does not imply that a separate Framework entry generates an I/O-thread wake.

**Connections and notification writers**

| Case / implementation | Source → target TCP endpoints | Application/completion usage | Largest notification writers |
|---|---|---|---|
| C++ request-backpressure / Framework | 127.0.0.1:56468 → 127.0.0.1:5294; 127.0.0.1:56476 → 127.0.0.1:5294 | requests on Application, replies on Completion | source: TID 2712281 (non-I/O), fd 29, 0.300/msg, TID 2712278 (Core I/O), fd 29, 0.219/msg, TID 2712802 (non-I/O), fd 29, 0.083/msg; target: TID 2712211 (Core I/O), fd 9, 0.405/msg, TID 2712804 (non-I/O), fd 9, 0.080/msg, TID 2712919 (non-I/O), fd 9, 0.059/msg |
| C++ request-backpressure / binding | 127.0.0.1:36774 → 127.0.0.1:5287; 127.0.0.1:36780 → 127.0.0.1:5287 | requests on Application, replies on Completion | source: TID 2718707 (non-I/O), fd 29, 0.016/msg, TID 2718151 (Core I/O), fd 29, 0.000/msg; target: no eventfd write observed |
| C++ send-saturation / Framework | 127.0.0.1:56914 → 127.0.0.1:5294; 127.0.0.1:56926 → 127.0.0.1:5294 | data on Application; Completion idle/control only | source: TID 2725389 (Core I/O), fd 29, 0.000/msg, TID 2725392 (non-I/O), fd 29, 0.000/msg, TID 2725387 (Core I/O), fd 29, 0.000/msg; target: TID 2725322 (Core I/O), fd 9, 0.109/msg, TID 2725327 (non-I/O), fd 9, 0.001/msg |
| C++ send-saturation / binding | 127.0.0.1:45440 → 127.0.0.1:5288; 127.0.0.1:45442 → 127.0.0.1:5288 | data on Application; Completion idle/control only | source: TID 2684458 (non-I/O), fd 29, 0.001/msg, TID 2683870 (Core I/O), fd 29, 0.001/msg; target: no eventfd write observed |
| Java send-saturation / Framework | 127.0.0.1:35144 → 127.0.0.1:5254; 127.0.0.1:35158 → 127.0.0.1:5254 | data on Application; Completion idle/control only | source: TID 2693752 (Core I/O), fd 87, 0.000/msg, TID 2693754 (Core I/O), fd 87, 0.000/msg, TID 2693761 (non-I/O), fd 88, 0.000/msg; target: TID 2693609 (Core I/O), fd 71, 0.291/msg, TID 2693618 (non-I/O), fd 71, 0.001/msg, TID 2693617 (non-I/O), fd 72, 0.000/msg |
| Java send-saturation / binding | 127.0.0.1:48778 → 127.0.0.1:5248; 127.0.0.1:48782 → 127.0.0.1:5248 | data on Application; Completion idle/control only | source: no eventfd write observed; target: no eventfd write observed |

TCP endpoints are the selected counter window’s established peer connections, not statistics listeners. There are two peer TCP connections in all six cases. The raw C++ server additionally creates an unused logical ROUTER/listener for the other benchmark pattern; that is not a second active peer workload. Listener, eventfd, epoll FD and TID inventories remain in each snapshot.

**Framework changes and conditional I/O savings**

The measurements identify a batching mechanism, not a validated Framework patch. The primary Framework proposal is to reduce work before Core admission and before reply production, preserving the same typed serializer, message bytes, admission/deadline semantics, execution gate and completion context. A concrete compatible scope is to write the service-wire application payload into its final native message buffer, replacing the intermediate byte vector plus `materialize_binding_part` copy in the existing owner. This reduces Framework construction work; **direct I/O-thread savings with unchanged traffic are approximately zero**. Its possible I/O benefit is the change in arrival cadence and batch occupancy.

For C++ requests, also examine the existing `framework_driver_t::run_tasks` admitted-task/completion bookkeeping and the runtime request/operation path, while retaining the public API contract. Java’s corresponding scope is `ZLinkJavaRawMeshNode.encodeApplicationFrames`, `ZLinkJavaRawServicePort.sendMessages`, and their existing state-lane turn. **Caller execution when the lane is idle already exists** in both [C++ `state_lane_t::run_sync`](/root/worktree/zlink-ratio040/framework/languages/cpp/framework/src/runtime/execution/state_lane.hpp:289) and [Java `ZLinkStateLane.awaitTurn`](/root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java:123). Adding that existing behavior is not a new optimization. No isolated construction/admission patch was implemented or timed here.

The frequency-scaling model below estimates the Core I/O opportunity **if Framework reaches binding’s call amortization**. For send/receive stages, it adjusts the binding call rate by the Framework/binding record-byte ratio; for epoll/write it uses the observed call-rate ratio. For each stage: `saving = Framework stage CPU × max(0, 1 − adjusted binding calls/msg ÷ Framework calls/msg)`. This is optimistic: some CPU is per-byte or per-frame, raw target uses a different receive wait form, and profiled call costs can change with batch size. Other Core userspace CPU is not removed by this model. The estimates are not measured patch gains.

| Case | Source currently hottest I/O thread, conditional µs saved/msg | Target currently hottest I/O thread, conditional µs saved/msg | Source / target aggregate I/O estimate, µs/msg |
|---|---:|---:|---:|
| C++ request-backpressure | 14.5 | 13.3 | 24.4 / 23.9 |
| C++ send-saturation | 2.2 | 1.7 | 2.2 / 1.7 |
| Java send-saturation | 6.5 | 5.1 | 6.5 / 5.1 |

These numbers are a shared batching opportunity for producer, reply-producer and receiver improvements; **they must not be added once for each proposed change**. For example, C++ request source’s measured TCP send/receive excess is about 23.0 µs/message across I/O threads, while its eventfd-write bucket is only 0.66. A notification-only proposal cannot recover the TCP difference. [savings-model.json](savings-model.json) contains the per-stage and per-thread arithmetic.

| Framework-side cause / proposed scope | Direct Core I/O saving with unchanged traffic | Conditional or unresolved saving |
|---|---|---|
| Construction/admission/reply-production work: remove duplicate materialization in the existing service writer and simplify already-completed admission bookkeeping where the existing contract permits | Approximately 0 µs/msg; this work executes on Framework/caller resources | The batching model above is the conditional opportunity. The amount recovered by this specific change is **not measured**. Validate both bytes and call amortization after any approved patch. |
| Polling/receive architecture: retain the existing multiplexed owner and consolidate necessary bookkeeping at its existing turn boundary | No independently demonstrated Framework-only I/O saving | Reduced idle-to-active edges may lower Core writes/posts. In C++ requests, removing the entire I/O write bucket would save only about 0.66 µs/source and 0.75 µs/target aggregate. Removing those notifications safely is a Core/poller ownership question; substituting a second owner or skipping required progress is not justified by this evidence. |
| Nested envelope bytes: a more compact common typed-message carrier would be a shared wire-contract design change; the compatible immediate proposal is construction into final storage with the existing bytes | Final-storage construction: approximately 0 µs/msg on I/O directly | Wire-only savings are **not measured**. Even a linear-byte sensitivity of the C++ request hottest-source CPU gives only `21.83 × 175/1427 ≈ 2.7 µs/msg`; fixed per-call TCP costs make that an aggressive sensitivity, not a prediction. No compatible removal of all 175 bytes has been established. |
| Extra physical parts, Framework-only Completion connection, or an extra binding completion entry | 0 attributable saving: these differences were not observed | Two physical parts and two peer connections are common; both C++ request paths use the same binding entry owner. |

The receive/completion and gate contracts remain owned by [Core polling](/root/worktree/zlink-ratio040/core/doc/spec/core/05-polling.ko.md), [Core ROUTER](/root/worktree/zlink-ratio040/core/doc/spec/core/socket/07-router.ko.md), and Framework [submit/completion](/root/worktree/zlink-ratio040/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md) / [messaging hot path](/root/worktree/zlink-ratio040/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md). This investigation proposes no Core decision reimplementation in Framework and makes no runtime or spec change.

**Validation, limitations and evidence**

- All six cases have CPU samples, successful-byte/syscall counters, decoded wire records, native stacks and mailbox/API probe evidence. Final benchmark captures drained with matching completed/received totals and zero workload errors. The original Java trace attempt missed active because its trigger endpoint is synchronous; it was excluded and the job-local collector was corrected. The first C++ Framework request mailbox script failed compilation due to a duplicate `END` block; its prior CPU/counter/wire windows remain valid, and a later clean request run supplies mailbox evidence. These were collection failures, not evidence of a runtime failure.
- Exact TCP packet counts, exact scheduler wakeups, isolated byte-copy CPU cost, finer Core/Asio userspace attribution, rate-matched raw controls, and savings from an implemented Framework change are **not measured**. .NET was not reprofiled; the supplied .NET result is not sufficient to assign the same numerical breakdown to .NET.
- The wire decoder was checked with valid multipart records, a truncated record that must not count as complete, and a control frame that must not count as an application record. [wire-parser-check.json](wire-parser-check.json) records both acceptance and rejection checks. Trace perturbation is material: the straced Framework request source can accumulate many messages per write, demonstrating that the existing Core path can batch these same Framework records; those distorted rates are excluded from the main tables.
- Primary artifacts: [metrics.json](metrics.json), [CPU stages](perf-summary.json), [BPF counters](bpf-summary.json), [wire results](wire-summary.json), [savings arithmetic](savings-model.json), [build identities](builds.json), [initial repository state](initial-state.json), [collection and analysis tools](tools/), and per-run raw files under [out/](out/). Each capture retains before/after statistics, FD/TID inventories, commands, exit codes and native build identities.

Verification: **pass** ([verification.json](verification.json)). The lowest capture-end MemAvailable was 21.4 GiB; the largest raw trace was 89.7 MB. No raw trace exceeded 1 GB. All benchmark PIDs have terminated; the initial tracked-file and executable hashes are unchanged.

Repository/runtime rules before → after: unchanged; no state, queue, flag or lock added. Refactoring audit: not performed in this evidence-only task. No implementation, commit or push was performed.
