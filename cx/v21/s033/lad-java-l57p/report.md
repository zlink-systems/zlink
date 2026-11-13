# Java ladder L5~L7 구현 보고

- **변경:** `FrameworkStack.java:268-270`에서 L5~L7 protobuf codec 등록을 건너뜀. `ZLinkFrameworkBenchServer.java:448-450`도 동일하게 처리하고, protobuf `BenchPayload` route/channel handler는 `:475-493`에서 L5~L7일 때 등록하지 않음. L5~L7의 `BenchTypedPayload`는 기본 JSON serializer를 사용하며 server info는 `:94-98`에서 `framework-json-v1`로 표시함.
- **커밋:** `ee7e5c3343bda3bf2069395b66fbe39fc7eb5507` — `bench(java): no protobuf codec registration in ladder L5_7 (#4354)` (실험 branch 로컬 commit, push 안 함).
- **빌드·검사:** `bash framework/bench/grpc/java/build.sh` 성공. `./gradlew --no-daemon --max-workers=1 check` 성공. ladder typed payload, client/shared bench checks 포함.
- **무부하 dry-start:** `ZLINK_BENCH_LADDER_L5_7=1`로 server 시작, `/ready`가 `ready`, `/bench/info`가 `framework-json-v1` 반환. stdin 종료 후 drain 완료 및 exit 0. client/load/benchmark 미실행.
- **측정용 설치 디렉터리:** client `/root/worktree/zlink-v21-s033/lad-java-l57p/framework/bench/grpc/java/client/build/install/bench-client`; server `/root/worktree/zlink-v21-s033/lad-java-l57p/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server`.
