P1 준비 파일은 `/root/jobs/fx/cx/v21/s033/xp1/scripts/run-p1.sh`와 `analyze-p1.py`이다. P2 준비 파일은 `run-p2.sh`와 `analyze-p2.py`이며, bench 변경은 `bench.diff`에 저장했다.

P1은 각 캡처를 단 한 번 실행한다. `flock -o /root/jobs/fx/cx/measure.lock` 안에서 MemAvailable 12 GiB 이상을 확인하고, xio의 `run_local_capture.sh`를 `RUNS=1`, `WARMUP_SECONDS=2`, `DURATION_SECONDS=16`, `SKIP_BUILD=1`로 호출한다. 기존 runner가 source를 CPU 0–7, target을 CPU 8–15에 고정한다. warmup 후 active 단계에서 Core I/O TID, 이름, 현재 CPU와 affinity를 기록하고 active 시작 0.2초 뒤부터 14초 동안 TID별 계수를 수집한다. 캡처는 binding equal22k(rate 32000), binding request 10k, binding send 10k, Framework equal22k(rate 48000), Framework request 10k, Framework send 10k 순서다. 결과는 `xp1/raw/p1/<capture>/` 아래 보존한다.

P1의 syscall 계수 명령은 `perf stat --per-thread --tid="$tid" -e 'syscalls:sys_enter_epoll_wait,syscalls:sys_enter_read,syscalls:sys_enter_recvmsg,syscalls:sys_enter_write,syscalls:sys_enter_writev,syscalls:sys_enter_sendmsg,syscalls:sys_enter_futex,context-switches,<설치된 uprobe 이벤트>' -x, -o <role-TID.stat.csv>`이다. eventfd만 세는 추가 명령은 프로세스의 `anon_inode:[eventfd]` fd마다 `perf stat --per-thread --tid="$tid" -e syscalls:sys_enter_write --filter "fd == $fd" -x, -o <role-TID.eventfd-FD.stat.csv>`이다. cycle 표본 명령은 `perf record -e cycles -F 4999 --tid="$tid" -o <role-TID.perf.data>`이며 call graph를 켜지 않는다. 각 파일의 self-symbol 보고서는 `perf report --stdio --sort symbol -i <role-TID.perf.data>`로 만든다. uprobe는 `perf probe -x "$LIBZLINK" --add "zlink_p1:<이벤트번호>=<nm에서 찾은 mangled symbol>"`로 추가하고, EXIT/INT/TERM trap에서 `perf probe --del "zlink_p1:<이벤트번호>"`로 제거한다. 이벤트별 카운트도 같은 TID의 perf stat에 넣는다.

실행 전 읽기 전용 `nm`/`nm -C` 검사 결과는 다음과 같다. `zlink::signaler_t::send()`는 `_ZN5zlink10signaler_t4sendEv`로 발견됐다. `zlink::mailbox_t::send(zlink::command_t const&)`는 `_ZN5zlink9mailbox_t4sendERKNS_9command_tE`로 발견됐다. `zlink::pipe_t::process_activate_read()`는 `_ZN5zlink6pipe_t21process_activate_readEv`로 발견됐다. `zlink::socket_base_t::process_async_mailbox()`는 `_ZN5zlink13socket_base_t21process_async_mailboxEv`로 발견됐다. `zlink::asio_engine_t::on_read_complete(...)`는 `_ZN5zlink13asio_engine_t16on_read_completeERKN5boost6system10error_codeEm.part.0` clone 심볼로 발견됐다. `zlink::socket_base_t::set_local_receive_flow_state`는 현재 libzlink에서 발견되지 않아 missing/inlined로 기록된다. `consume_receive_flow_state_frame`은 `_ZN5zlink13socket_base_t32consume_receive_flow_state_frameEPNS_6pipe_tERKNS_5msg_tE`로 발견됐다. `zlink::socket_poller_t::rebuild()`는 `_ZN5zlink15socket_poller_t7rebuildEv`로 발견됐다. runner는 다른 실행 환경에서도 실제 libzlink의 nm 결과를 다시 확인하고, 없는 심볼은 추측해 probe하지 않고 missing/inlined로 기록한다.

P1 분석기는 각 TID별 이벤트와 uprobe의 메시지당 횟수, 상위 20개 self-cycle 심볼을 출력한다. verdict 표에는 epoll_wait와 on_read_complete의 Framework/binding 비율, 양쪽 process의 target/source eventfd write/msg, 세 Core 함수의 Framework/binding self-cycle 비율, flow-state 호출/s를 함께 표시한다. N1 기준은 두 비율 중 하나가 1.3 이상인지다. N2는 target Framework 약 2회/binding 약 0회와 source 양쪽의 일치 여부를 수치로 비교한다. N3 기준은 초당 200회 이상으로 표시한다. 현재 libzlink에는 `set_local_receive_flow_state` 심볼이 없어 실제 캡처의 N3는 0이 아니라 unavailable로 출력한다.

P2는 같은 lock과 MemAvailable 조건에서 각 변형을 한 번씩, request-backpressure 1024, target-rate 32000의 xio pacing으로 실행한다. 생성된 runner는 기존 `run_local_capture.sh`의 CPU affinity와 pacing을 재사용하고 server/client 실행 인자에 `--p2-variant S1|S2|S3|C1|C2`를 전달한다. 각 활성 구간의 Core I/O TID마다 task-clock과 P1 syscall 계수를 perf stat으로 모으며, task-clock을 완료 메시지 수로 나눠 µs/msg를 계산한다.

P2 변형은 server 파일에서 S1이 pollin|pollout|pollroute|pollconnect|pollcompletion poller와 DONTWAIT recv를 사용한다. S2는 S1에 reply queue와 condition-variable 기반 두 번째 reply thread를 더한다. S3는 S2 경로에 200건마다 receive-flow paused/running 토글을 추가한다. 구현 위치는 [bench_zlink_cpp_server.cpp](/root/worktree/zlink-v21-s033/xio/framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:40)이며 poller mask는 51–57행, queue/reply thread는 58–84행, DONTWAIT과 flow toggle은 87–123행이다. client 파일의 `--p2-variant` 기본값은 off이고, C1/C2 poller mask는 [bench_cpp_client.cpp](/root/worktree/zlink-v21-s033/xio/framework/bench/grpc/cpp/client/bench_cpp_client.cpp:67) 및 489–495행이다. C2는 두 번째 thread에서 poller wait/completion drain을 수행한 뒤 main thread가 다음 제출을 계속하며 구현은 602–611행이다. 옵션 parsing은 1432행이다.

가짜 입력은 `xp1/dry/p1/` 및 `xp1/dry/p2/`에 만들었다. dry-check P1 verdict 표 출력은 다음과 같다.

```text
capture  N1 epoll_wait/msg F/B  N1 on_read_complete/msg F/B  N2 target eventfd-write/msg F/B  N2 source eventfd-write/msg F/B  Core self cycles (%) F/B  N3 set_local_receive_flow_state/s  verdict rows
equal22k  0.75/0.5 (1.500)       0.025/0.025 (1.000)          0.125/0.125                     0.125/0.125                     process_async_mailbox=F25.00%/B25.00%, process_commands=F10.00%/B10.00%, mailbox_t::send=F5.00%/B5.00%  2.500  N1=yes; N2=compare target≈2/≈0 and source parity; Core profile=compare F/B; N3=no
```

P2 dry analyzer도 다섯 변형 각각 한 행을 출력했으며 예시는 `C1 target/456 1000 50.00000 µs/msg`이다. 전체 dry 출력은 `xp1/dry/p1-output.txt`와 `xp1/dry/p2-output.txt`에 있다. 두 runner에 `bash -n`, 두 analyzer에 `python3 -m py_compile`을 수행했다. `bash scripts/format/format.sh --check cpp`도 성공했다. 빌드, 테스트, perf 또는 benchmark는 실행하지 않았다.

실행 job은 Core gate verdict 파일이 생긴 뒤 다음 명령을 실행하면 된다. `VCPKG_ROOT`는 기존 빌드 환경에 설정되어 있어야 한다. P1은 기존 bench 바이너리를 사용하고, P2를 위해 그 뒤 bench만 다시 빌드한다.

```bash
test -f /root/jobs/fx/cx/v21/s033/core124b/verdict.md
bash /root/jobs/fx/cx/v21/s033/xp1/scripts/run-p1.sh
python3 /root/jobs/fx/cx/v21/s033/xp1/scripts/analyze-p1.py /root/jobs/fx/cx/v21/s033/xp1/raw/p1
(cd /root/worktree/zlink-v21-s033/xio/framework/bench/grpc/cpp && bash build.sh)
bash /root/jobs/fx/cx/v21/s033/xp1/scripts/run-p2.sh
python3 /root/jobs/fx/cx/v21/s033/xp1/scripts/analyze-p2.py /root/jobs/fx/cx/v21/s033/xp1/raw/p2
```

gate verdict 대기 시간은 제외하고 예상 실행 시간은 약 15–30분이다. bench 빌드 시간과 시스템 부하에 따라 달라질 수 있다.
