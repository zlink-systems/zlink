# progress: stream pull variants (asio_pull, cppserver_pull)

- 15:17 시작. 기존 asio/cppserver 스택·러너 구조 파악.
- 15:20 stacks/common/stream_pull_queue.hpp (mutex+condvar MPSC 큐), stacks/asio_pull/, stacks/cppserver_pull/ 작성.
- 15:22 러너(run_benchmarks.sh)·CMakeLists·README(ko/en)에 asio_pull, cppserver_pull 등록.
- 15:26 두 바이너리 빌드 성공(cppserver upstream entry: performance/stream_pull_server.cpp).
- 15:30 스모크(ccu20/1024B/3s): asio_pull 62.0 kops, cppserver_pull 72.0 kops, parse/send error 0.
- 다음: idle 창 대기 후 flock 하에 6개 스택 본 측정(--size all --ccu 1000 --runs 3 --reuse-build).
- 15:36~15:48 본 측정 1회차 완료(results/20260908_153737). 시작 load 0.77, 종료 시각 15:48.
  - 64B median kops: zlink 148.1 / asio 185.3 / asio_pull 148.9 / cppserver 177.0 / cppserver_pull 148.1 / zmq 164.3
  - run 간 산포가 큼(예: asio 64B 140.9/185.3/220.0) → 2회차 3런 추가 측정하여 6런 중앙값으로 판정 예정.
- 16:10 감독관 추가 요구 반영: (a) pull 변형에 worker-thread 카운터 추가(PULL_STATS: enqueued/processed_by_worker/processed_off_worker/echo_posts),
  (b) wake 방식 선택 가능(ZLINK_BENCH_PULL_WAKE=eventfd → zlink signaler와 동일, 기본 condvar).
  스모크 결과 off_worker=0 (모든 에코가 워커 스레드 경유 확인).
- 다음: idle 창에서 (1) 64B CCU 100/1000/4000 스윕, (2) 확정 3런(size all, ccu1000), (3) eventfd 1런.
- 16:20~16:24 CCU 스윕(64B, 1런): CCU 100/1000/4000. 스택별로 값이 뚜렷이 갈림(공유 병목 아님).
  CCU100 zlink 207 / asio 288 / asio_pull 235 / cppserver 289 / cppserver_pull 270 / zmq 215 kops
  CCU4000 zlink run_failed / asio 248 / asio_pull 212 / cppserver 286 / cppserver_pull 292 / zmq 251 kops
- 16:27 확정 측정(size all, ccu1000, 3런) + eventfd wake 1런 시작(flock).
- 16:42~16:53 확정 측정(results/20260908_164225, idle load 0.58) + eventfd 1런(20260908_165308) 완료.
- 17:05 보고서 작성 완료: doc/plan/c016-worklog/stream-pull-variants-summary.md. 커밋하지 않음(감독관이 커밋).
