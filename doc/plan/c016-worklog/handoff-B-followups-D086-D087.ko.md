# B 세션 작업 요청 — 캠페인 A(0.17.0 DONTWAIT framework completion)에서 넘기는 Core/bindings 후속 2건

너는 머신 B의 감독 세션이다. 규칙은 그대로다: 모든 조사·구현은 codex sub-agent(sol/high 기본, 어려우면 sol/xhigh,
기계적 게이트는 terra/medium)로 진행하고 너는 브리프 작성·게이트 실행·diff 리뷰·커밋/푸시만 한다. sub-agent는
절대 spec(`core/doc/spec/**`, `bindings/doc/spec/**`, `framework/doc/framework/**`, `doc/site/**`)을 수정하지 않는다.
리뷰 때 spec gap을 만들지 않았는지 확인한다. 버그로 확정되면 회귀 테스트 + 수정 + main 커밋·푸시까지 한다.
Core 테스트는 공개 C API만 사용하고 `ctest -j2`(CONTRIBUTING.ko.md §4/§5). 작업은 main에서 하고, A 세션도 main에
커밋하므로 push 전에 항상 `git fetch && git merge --no-edit origin/main`.

배경: A 캠페인은 완료됐다(`doc/plan/dontwait-0.17.0-framework-completion.ko.md` 전부 체크). Core/bindings 확정
버그는 0건이었고, 아래 2건은 조사 중 드러난 Core/bindings 영역의 후속이다. 상세 근거는
`doc/plan/c016-worklog/decisions.ko.md`의 D-086, D-087.

## 작업 1 — D-086: tcp에서 same-RID replacement DEALER admission 지연 (Core, 성능)

증상: ROUTER에 fixed RID로 admitted된 DEALER A의 pipe가 살아있는 상태에서 같은 RID의 DEALER B가 **tcp**로
connect하면(RID duplicate policy HANDOVER) B의 admission 완료까지 0.1~2.9 s, 간헐 5 s 이상. A의 reconnect
interval이 짧을수록 악화. **inproc은 즉시**. 측정은 공개 C API repro에서 나왔다:
`doc/plan/c016-worklog/evidence/test_ctx_term_fixed_rid_handover.cpp` (+ `…cmake.diff`, 원래 worktree
`zlink-core-term`용 `core/tests/CMakeLists.txt` 등록 diff). 전체 맥락은
`doc/plan/c016-worklog/core-ctx-term-teardown-hang-summary.md` §"Secondary finding".

영향: .NET framework 테스트의 handover 기대치 2 s가 이 분포 안에 있어
`CanonicalActorJoinIngressReplyTests.CanonicalActorJoinRequest_HandoverKeepsPriorReplyEpochUntilExactDisconnect`(:560)의
간헐 실패와 36분짜리 `ActorCreateCompletion_AfterHandoverHello_UsesCapturedReplyRoute`(D-068/D-072)의 후보 원인.

할 일:
1. sub-agent(sol/high)에게 repro를 `core/tests/integration/`에 등록시키고(labels `integration;serial`, TIMEOUT 30)
   admission 지연을 분포로 측정하게 한다(tcp vs inproc, reconnect interval 10/100/1000 ms, 20회씩).
2. 원인을 Core에서 찾는다. 후보: `router_admission.cpp` `identify_peer`의 `reciprocal_duplicate`/handover 경로가 이전
   pipe의 종료(또는 reconnect 타이머 만료)를 기다리는지, tcp에서만 생기는 이유(peer 종료 handshake·linger·reaper
   타이밍)와 spec(`core/doc/spec/core/socket/README.ko.md` §4 RID duplicate policy, `07-router`)이 정한 handover
   완료 조건을 대조한다. spec이 "이전 pipe 종료 확인 후 handover"를 요구하면 지연 자체는 계약일 수 있다 — 그 경우
   지연 상한과 원인을 문서화하고, 계약 안에서 줄일 수 있는 부분(예: 불필요한 타이머 대기)만 고친다.
3. 수정하면 회귀 테스트(지연 상한 assert, 예: tcp handover admission < 200 ms p95)를 같은 integration test에
   추가하고 CONTRIBUTING §5 게이트(ctest -j2 전체 + hotpath 관련 있으면 perf policy) 뒤 커밋·푸시.
4. 결과를 `doc/plan/c016-worklog/decisions.ko.md`에 D-B9x로 기록. A 쪽 후속 여부(2 s 기대치 조정 필요 여부)를
   명시.

## 작업 2 — D-087: Java 바인딩 네이티브 라이브러리 추출 임시 디렉터리 누수 (bindings/java, 운영 품질)

위치: `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java:60-64`.
JVM마다 `Files.createTempDirectory("zlink-native-")`에 `libzlink.so`(84 MB)를 복사하고 `deleteOnExit`에만 의존한다.
samples runner가 role JVM을 SIGKILL하거나 crash/hang-kill되면 삭제되지 않는다. 머신 A에서 하룻밤 java 게이트로
`/tmp/zlink-native-*` 약 95개(≈8 GB)가 남아 8 GB tmpfs `/tmp`가 100%가 됐고 세션 도구까지 ENOSPC로 실패했다.

할 일:
1. sub-agent(sol/high)에게 추출 위치를 콘텐츠 해시 기반 고정 경로로 바꾸게 한다:
   `${ZLINK_JAVA_NATIVE_CACHE:-~/.cache/zlink/native}/<sha256-of-resource>/<libFile>` — 이미 같은 해시의 파일이
   있으면 복사를 생략하고 그대로 `System.load`. 동시 JVM이 같이 추출할 때의 경합은 임시 파일 + atomic move로
   처리. 캐시 디렉터리를 만들 수 없으면 기존 temp 방식으로 fallback. Windows dep preload(`preloadWindowsDeps`)
   경로도 같은 디렉터리를 쓰게 유지.
2. 회귀 테스트(`bindings/java` 테스트): JVM(또는 classloader) 2회 연속 로드 시 새 temp 디렉터리 증가 0, 캐시
   경로에 파일 1개, 해시 불일치 시 재추출.
3. bindings/java 게이트(기존 테스트 전체 + 샘플 스모크 1개) 뒤 커밋·푸시. `bindings/doc/spec/**`에 로더 경로 계약이
   있으면 sub-agent가 아니라 네가 확인해서 spec gap이 없게 한다(계약 변경이 필요하면 사용자에게 보고).
4. decisions에 D-B9x로 기록.

## 산출물
- 각 작업마다 `doc/plan/c016-worklog/briefs/<job>.prompt`와 `…-summary.md`(원인 `file:line`, spec 조항, 수정,
  회귀 테스트, 게이트 수치, BLOCKERS).
- 커밋 메시지에 D-086/D-087 참조. 완료 후 A 세션이 로컬 패키지를 재빌드해 framework 게이트를 다시 돌릴 수 있게
  마지막 커밋 해시를 decisions에 남긴다.
