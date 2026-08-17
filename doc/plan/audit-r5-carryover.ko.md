# 감사 루프(R1~R5) 이월 목록 — 해소 결과

기준: 5라운드 감사(2026-08-17~18, tag `samples-audit-r5-converged`) 이월분을
2026-08-18 후속 라운드에서 해소. 해소 커밋: Node 09478d3553, Java 157ae609ca,
C++ df6fa9de6d, .NET 0aef8fd295, 문서(스펙 32 §13·언어별 커넥터 문서·가이드 09-stream §5.1).

## 해소됨

- **커넥터 diagnostics level 공개 API** — 4개 언어 전부 추가(Off/Errors/Normal/Detailed,
  기본 Errors). 계약은 stream-connector 공통 스펙 §13, 표면은 언어별 03-stream-connector 문서.
- **one-way Send correlation id** — 스펙 27 §2 명문("one-way에는 만들지 않는다")대로
  .NET/Java/C++의 고의 부착을 제거(Node는 기존 준수). 4개 언어 pin 테스트.
- **spot route error kind 평탄화(Java)** — 전면 보존으로 전환. framework 자체 생성 오류에만
  `zlink.origin=framework` metadata 마커(Java 4th part, C++ envelope metadata, Node channel
  envelope metadata), stale-route 판정을 `NOT_FOUND(+C++는 unavailable) && framework origin`으로
  협소화.
- **Java spot publish/direct-outbound flow 배선** — value-passing으로 구현(캡처한 flow state를
  encode 인자로 전달, publish turn 반환 stage는 bare admission 유지 — R4 회귀 사슬 차단을 테스트로 pin).
- **Node flow-context enterWith 영속 설치** — 전 outbound 진입점(25곳)을 call-scoped
  `runWithOutboundFlow`로 재구조화, enterWith 잔존 0.
- **.NET 채널 outbound sent/reply_received terminal** — Java/Node 시맨틱으로 구현
  (ClientServer/RouteMesh/node-direct, caller-facing builder 소유의 exactly-once).
- malformed channel envelope invalid_frame 기록(Node request는 protocol error reply로 통일,
  .NET 기록 추가), Node relay JSON flowOrigin 소문자 정정, Node shutdown drop 기록,
  reply-write reason `reply_path_missing` 정정, C++ 콜드 경로 flow-capture 스레딩(13곳),
  C++ Off 36B materialize 제거, C++ relayed_frames 1024 상한, C++ M6 이중 컴파일 제거,
  .NET spot 거절 경로 live 게이트, .NET ConfiguredLevel volatile, Java actor packet header
  flow 검증, C++ filter-chain/core-error-frame 값 전달 수명 수정.

## 잔존 (구조/설계 결정 필요 — 코드 강제 불가)

- **[.NET] spot route error origin 마커 미구현** — .NET reply envelope(JSON 단일 헤더)에
  metadata 필드가 없어 wire 계약 변경 필요. kind는 이미 전면 보존 중이며 stale 판정
  (NotFound|Unavailable) 협소화만 보류. 교차 언어 envelope 필드 추가 창구에서 처리.
  (Node spot-direct 단일 JSON reply도 동일 사유로 마커 미적용 — 별도 구조.)
- **[C++] core error frame 경로의 pre-suspension 예외 시 mutex self-deadlock 가능성** —
  `observe_task_completion` 인라인 완료가 `begin_core_session_close`와 같은 비재귀 mutex를
  재획득할 수 있음(stream_host_service.cpp 2621→1605→2519→2442). error/close 순서 재배치 필요.
- **[C++] request_erased 50ms 폴링** — awaitable delay/notification 프리미티브가 없어 최소
  수정 불가. 프레임워크 async 프리미티브 추가 시 전환.
- **[C++] authority-version 관측 헬퍼 unwired** — `observe_spot/actor_authority_version`의
  런타임 호출자가 없음(테스트 전용). 배선은 동작 변경이라 별도 결정.
- **[Node] sampleRate<1에서 flow 없는 inbound의 hop 일괄 선택 약화, begin() sampling과
  ambient 불일치 미세 창** — 관측 품질 한정, 샘플링 재설계 창구에서 처리.
- **[.NET] ZLinkTelemetry process-global level(last-writer-wins)** — submit-admission activity
  게이팅 한정. per-runtime화는 정적 계측 전면 재배선 필요(현행 유지 판정).
- 커넥터 diagnostics level의 실행 중 변경은 범위 외로 명시(스펙 §13 — 요구하지 않음).

## 환경/기타

- C++ `test_cpp_framework_m6a_runtime` 결정적 실패는 2026-08-17 재설치된 core 0.11.1 패키지
  기인(프레임워크 무관, stash 베이스라인으로 확정).
- 알려진 flake 목록과 known-broken 사용자 영역은 감사 대상에서 제외.
