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

## 교차 언어 wire 수렴 라운드 (2026-08-18 후속)

언어 간 메시징이 지원 요구사항으로 확정되어 진행. 커밋: Node 15c36b3cde, .NET dcc0e4238d,
Java 26dcab2f09, 하네스 e19a79b482·9b0f2e7c95, wire tail 4973582867, C++ mesh f1784b39de,
Java accept c80e517d9a.

해소:
- **Java가 유일하게 raw-parts wire를 쓰던 문제** — 공유 2-part JSON envelope(formatMarker
  0xF2)로 이식. 이전에는 Java↔타언어 spot route가 정상 요청부터 상호 불가였다.
- **errorCode 표현 3원화**(C++ snake_case / Node 숫자 / .NET PascalCase) → snake_case 13종
  1:1로 통일, 4개 언어 golden 테스트.
- **.NET reply envelope의 metadata 필드 부재** → 추가, `zlink.origin=framework` 마커와
  stale 판정 협소화까지 C++와 동일 규칙으로 수렴(이전 잔존 항목 해소).
- **reply 헤더 tail 방언 분리** — 스키마 `request-specific-tail`은 `bodyLengthType`이 없는
  인라인 union(같은 파일 `actor-join-reply-tail`은 u16을 명시)이므로 C++/Java의 21바이트가
  정본. u16 길이를 쓰던 Node/.NET을 정정.
- **C++ `classify_node_direct_target`의 Location Store 단락** — route-only peer가 Store에
  없다고 무조건 `not_found`를 반환해 토폴로지 체크가 실행되지 않던 버그(C++↔C++에서도 재현).
- **Java listen-only 서버의 admission 전면 거부**(unconfigured-inbound fallback이
  security identity를 transport identity로 계산) 및 **핸들러 미등록 응답 kind**(INTERNAL_FAILURE
  → NOT_FOUND). 둘 다 Java↔Java에서는 우연히 성립해 자체 테스트로는 드러나지 않던 결함.
- 하네스: 4개 언어 모두 `framework/languages/<lang>/cross-language/`로 통일, Java peer host
  신설, spot route 7개 방향 전부 실단언 그린(정상 왕복 / framework not_found + 마커 /
  application rejected + 마커 없음).

## 잔존 (구조/설계 결정 필요 — 코드 강제 불가)

- **[교차] never-admitted 타깃의 오류 라벨 차이** — C++는 `not_connected`, Java는
  `TARGET_NOT_FOUND`. 각 언어의 판정 구조(Location Store 스냅샷 vs 라이브 peer 목록)상
  둘 다 타당하나 라벨이 갈린다. spec 32 kind 계약으로 통일할지 결정 필요.
- **[교차] C++/Java `decodeReplyHeader`가 tail이 붙은 reply를 거부** — 스키마상 tail은
  허용되므로 디코더를 넓혀야 하나, 두 언어가 클라이언트일 때만 노출되고 현재 경로에서
  미실행. `actor-create-terminal` 내부 u16도 .NET은 쓰고 C++은 쓰지 않는다(스키마상 .NET이
  맞음, 현재 미노출).
- **[Node] route surface가 `error.origin`을 채우지 않음** — 마커 없는 reply에 origin=none을
  보고(.NET은 같은 reply에 application). 하네스는 관측대로 단언 중.
- **[하네스] Java↔Node, Java↔.NET 방향 미구현** — Java peer host가 있어 확장 마찰은 낮음
  (Node peer host에 스테이지 1쌍 추가 수준). 반나절 규모로 추정.
- **[.NET] spot route error origin 마커** — 해소됨(위 참조). Node spot-direct 단일 JSON
  reply만 별도 구조라 미적용 유지.
- **[C++] core error frame 경로의 pre-suspension 예외 시 mutex self-deadlock** — 해소됨
  (커밋 647d214b51: 락 안은 상태 기록만, 완료·close는 lane 제출 순서로 시퀀싱, close 멱등화,
  fault 주입 회귀 테스트).
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
