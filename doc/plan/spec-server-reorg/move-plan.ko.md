# 이동 계획 — 옛 경로 → 새 경로

> 캠페인 §5의 이동 단계를 실행하기 위한 표다. 리허설과 실제 이동 모두 이 표를 쓴다.
> 47개 옛 문서 중 39개는 1:1, 8개는 1:N(분할·병합)이다.

## 1. 1:1 이동 — `git mv`

ko/en을 함께 옮긴다.

| 옛 문서 | 새 경로 |
|---|---|
| `00-public-contract-governance` | `00-foundation/01-public-contract-governance` |
| `01-glossary` | `00-foundation/02-glossary` |
| `02-overview` | `00-foundation/03-overview` |
| `03-interaction-model` | `00-foundation/04-interaction-model` |
| `04-message-model` | `00-foundation/05-message-model` |
| `06-framework-api` | `00-foundation/06-framework-api` |
| `32-framework-error-model` | `00-foundation/07-framework-error-model` |
| `40-internal-layering` | `00-foundation/08-layering` |
| `50-internal-message-ownership` | `01-execution/05-payload-ownership-and-codec` |
| `07-channel-topology` | `02-channel-transport/01-channel-topology` |
| `08-channel-messaging` | `02-channel-transport/02-channel-messaging` |
| `09-client-server-channel` | `02-channel-transport/03-client-server-channel` |
| `10-network-listener-identity` | `02-channel-transport/04-network-listener-identity` |
| `51-internal-service-wire-protocol` | `02-channel-transport/06-wire-protocol` |
| `11-spot-model` | `03-spot-actor/01-spot-model` |
| `12-spot-messaging` | `03-spot-actor/02-spot-messaging` |
| `13-mesh-node` | `03-spot-actor/03-mesh-node` |
| `14-actor-model` | `03-spot-actor/04-actor-model` |
| `15-spot-actor` | `03-spot-actor/05-spot-actor-membership` |
| `16-spot-address-messaging` | `03-spot-actor/06-spot-address-messaging` |
| `17-stage-wrapper-on-spot` | `03-spot-actor/07-stage-wrapper-on-spot` |
| `18-object-routing` | `03-spot-actor/08-routing` |
| `45-internal-routing-and-cache` | `03-spot-actor/08-routing` (§1·§1.1·§2만) + `02-channel-transport/02-channel-messaging` (§3~§7) |
| `47-internal-object-lifecycle` | `03-spot-actor/09-object-lifecycle` |
| `19-stream-session` | `04-session/01-stream-session` |
| `20-session-actor-dispatch` | `04-session/02-session-actor-binding` |
| `48-internal-session-binding` | `04-session/02-session-actor-binding` |
| `21-location-runtime` | `05-location-relocation/01-location-runtime` |
| `22-location-store-redis` | `05-location-relocation/02-location-store-redis` |
| `23-relocation-store-redis` | `05-location-relocation/03-relocation-store-redis` |
| `28-relocation-flow` | `05-location-relocation/04-relocation-flow` |
| `44-internal-relocation-continuity` | `05-location-relocation/04-relocation-flow` |
| `52-internal-relocation-handoff` | `05-location-relocation/04-relocation-flow` |
| `30-host-relocation-flow` | `05-location-relocation/05-host-relocation-flow` |
| `31-failure-failover-policy` | `05-location-relocation/06-failure-failover-policy` |
| `24-runtime-monitoring` | `06-observability/01-runtime-monitoring` |
| `25-runtime-metrics` | `06-observability/02-runtime-metrics` |
| `26-message-flow-tracing` | `06-observability/03-message-flow-tracing` |
| `27-flow-correlation` | `06-observability/04-flow-correlation` |

새 문서는 이미 새 경로에 있으므로 **`git mv`가 아니라 옛 파일 삭제 + 새 파일 add**가 된다.
옛 파일은 이동 커밋에서 지운다.

## 2. 1:N — 링크가 여러 곳으로 갈라지는 옛 문서

이 8개는 옛 문서 하나가 여러 새 문서로 흩어졌다. 옛 경로를 가리키는 **외부 링크는 절 anchor를
보고 목적지를 골라야 한다.** 기본 목적지(anchor가 없거나 판단이 어려울 때)를 함께 적는다.

| 옛 문서 | 절 → 새 문서 | 기본 목적지 |
|---|---|---|
| `05-async-execution-policy` | §1.1~§1.4·§2·§6 → `01-execution/01-submit-and-completion` · §1.1(Yield)·§3·§3.1 → `02-handler-turn-and-execution-gate` · §4 → `03-cancellation-and-shutdown` · §5 → `03-spot-actor/10-spot-timer` · §10 → `04-application-job-queue-and-backpressure` | `01-execution/README` |
| `33-core-hwm-application-job-flow` | 전체 → `01-execution/04-application-job-queue-and-backpressure` | 같음 |
| `41-internal-serialization` | 전체 → `01-execution/02-handler-turn-and-execution-gate` | 같음 |
| `42-internal-progress-isolation` | §1~§4·§7 → `01-execution/02-handler-turn-and-execution-gate` · §5·§6 → `04-application-job-queue-and-backpressure` | `01-execution/README` |
| `43-internal-completion` | 전체 → `01-execution/01-submit-and-completion` | 같음 |
| `46-internal-dispatch-loop` | §1·§2·§6·§8 → `01-execution/04-application-job-queue-and-backpressure` · §7 → `03-spot-actor/10-spot-timer` · §3·§4 → `02-handler-turn-and-execution-gate` | `01-execution/README` |
| `29-transport-liveness` | 전체 → `02-channel-transport/05-transport-liveness` | 같음 |
| `49-internal-liveness-and-state` | §1 → `02-channel-transport/05-transport-liveness` · §2 → `03-spot-actor/03-mesh-node` · §3~§5 → `06-observability` | `02-channel-transport/05-transport-liveness` |

## 3. 절 anchor 치환

절 제목이 바뀐 문서가 많다. 외부 문서 266개, anchor 링크 505개가 영향받는다. 치환표는 각
주제의 `topics/NN-주제/ledger-*.md`의 `| 규칙 ID | 새 위치 |` 열과 각 문서의 실제 제목에서
만든다. 자동 생성 뒤 `check_doc_links.py`로 검증한다.

## 4. 코드·스크립트 갱신

| 대상 | 할 일 |
|---|---|
| `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp` | 3개 경로를 새 경로로. needle 19개는 그대로 유지되므로 문장 수정 불필요 |
| `scripts/verify-framework-submit-api.sh` | 경로 갱신(`05-async-execution-policy`→`01-execution/01-submit-and-completion` 등). needle은 유지 |
| `scripts/verify-framework-instance-spot-contracts.sh` | 경로 갱신 + **needle 6개 문구 갱신** — [needle-repair.md](needle-repair.md) 참조 |
| node `channel-socket-registry.ts:629`, java `ZLinkChannelRuntime.java:1100` | 주석의 `08-channel-messaging.ko.md §3.2` → `02-channel-transport/02-channel-messaging.ko.md` §3(고정 anchor `#weighted-round-robin-selection-order`) |
| `testdata/location/redis/*.json` 5개 | 경로 문자열 갱신 |
| `framework/runtime/conformance/relocation-behavior-v1.json` | `15-spot-actor.ko.md` 경로·절 번호 갱신 |

## 5. site

| 대상 | 할 일 |
|---|---|
| `doc/site/mkdocs.yml` | Spec nav를 7개 주제 그룹으로 재구성. "Internals" 그룹 해체 |
| redirect | 옛 URL 47개 → 새 URL. 1:N은 §2의 기본 목적지로 |
| `doc/site/scripts/generate_language_guides.py` | 공통 guide 재생성(언어별 guide는 생성물) |
| 최상위 `README.ko.md`/`.en.md` | [target-readme.ko.md](target-readme.ko.md)로 교체. "검증 runner 격리"는 e2e README로, "디버깅 원칙"·"Trace 비용 규칙"은 이미 observability로 이관됨 |

## 6. 검증

```bash
python3 doc/site/scripts/check_doc_links.py framework   # 기준선: zoneworld 4건(이동 시 해소 대상)
python3 doc/site/scripts/check_doc_tabs.py framework
cd doc/site && mkdocs build --strict
git diff --check
bash scripts/verify-framework-submit-api.sh
bash scripts/verify-framework-instance-spot-contracts.sh
# cpp layout contract test
```

zoneworld README의 깨진 anchor 4건은 `15-spot-actor` → `03-spot-actor/05-spot-actor-membership`
치환과 함께 해소한다.

## 7. 리허설

실제 커밋 전에 한 번 돌려 보고 되돌린다.

```bash
git status --short > /tmp/before.txt
# 치환 스크립트 실행
python3 doc/site/scripts/check_doc_links.py framework
cd doc/site && mkdocs build --strict
git checkout -- .          # 되돌리기 (새 파일은 untracked이므로 남는다)
```
