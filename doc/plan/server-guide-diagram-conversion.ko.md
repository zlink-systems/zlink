# server 가이드 다이어그램 변환 계획

> `framework/doc/framework/common/guide/server`의 가이드 문서에 남은 mermaid 다이어그램을
> **깔끔한 archify SVG**로 변환한다. 규칙은
> [다이어그램 작성·변환 가이드](../principal/documentation/diagram-authoring-guide.ko.md)를
> 단일 진실 원천으로 따른다. 변환 **실행은 codex sol 서브에이전트**가 맡고, **감독·리뷰는
> Claude가 단독**으로 한다.

## 재개 상태 (새 세션 핸드오프)

이 문서 하나로 새 세션이 이어받을 수 있게, 현재 상태를 여기 고정한다(진행하며 갱신할 것).

- **브랜치:** `docs/archify-diagrams-pilot` (main 아님). 아직 **미커밋** — 아래 산출물이 모두
  워킹트리에 있다. 새 세션은 시작 시 `git status`로 브랜치·워킹트리를 먼저 확인한다.
- **완료:** `01-overview.ko.md`(14개), `06-spot.ko.md`(1개). 다이어그램 에셋은
  `framework/doc/framework/common/diagrams/<name>.architecture.json`(+`.html`). 현재 15개.
- **남은 대상:** §3의 **8개 파일, 29개** mermaid.
- **도구:** `scripts/diagrams/`(`build-diagram.mjs`·`pad-viewbox.mjs`·`shot.mjs`·`shot-dark.mjs`·
  `shot-region.mjs` + README). `build-diagram.mjs`는 `ARCHIFY_DIR` 환경변수가 필요하다.
- **규칙 SSOT:** [`doc/principal/documentation/diagram-authoring-guide.ko.md`](../principal/documentation/diagram-authoring-guide.ko.md).
- **공통폭:** 현재 **1010**(가장 넓은 다이어그램 폭 이상). 새 다이어그램이 더 넓으면 전체를 새
  공통폭으로 다시 `pad-viewbox` 한다(§5).

## 환경 부트스트랩 (새 세션 필수)

렌더·크롭·스크린샷은 **archify**와 **playwright**에 의존한다. 이 둘은 이전 세션의 스크래치패드에만
설치돼 있어 **새 세션엔 없다.** 다음으로 재설치한다(chromium 바이너리는 `~/.cache/ms-playwright/`에
영속하므로 대개 재다운로드 불필요).

1. **archify** — `git clone https://github.com/tt-a1i/archify.git`. **v2.16.0 / commit `5de7275`**에서
   검증됨. 클론 안쪽의 `archify/archify`(= `bin/archify.mjs`·`schemas/`·`renderers/`가 있는 디렉터리)를
   `ARCHIFY_DIR`로 지정한다.
2. **playwright** — 임의 작업 디렉터리에서 `npm i playwright@1.62.1` 후 `npx playwright install chromium`
   (캐시가 있으면 no-op).
3. **실행 규칙** — `build-diagram.mjs`가 `import 'playwright'` 하므로 **playwright가 resolve되는
   디렉터리에서 실행**한다(위에서 npm 설치한 디렉터리, 또는 `NODE_PATH`로 지정). 출력 경로는 반드시
   **절대경로**.
   ```sh
   export ARCHIFY_DIR=<archify-clone>/archify
   REPO=/home/hep7/project/zlink
   D=$REPO/framework/doc/framework/common/diagrams
   node $REPO/scripts/diagrams/build-diagram.mjs architecture "$D/<name>.architecture.json" "$D/<name>.html"
   node $REPO/scripts/diagrams/shot.mjs      "file://$D/<name>.html" /tmp/l.png 1010 700
   node $REPO/scripts/diagrams/shot-dark.mjs "file://$D/<name>.html" /tmp/d.png 1010 700   # 라이트·다크 둘 다
   ```
4. **사이트로 확인** — `cd $REPO/doc/site && python3 scripts/generate_language_guides.py && mkdocs build
   -d <scratch-site>` 후 `python3 -m http.server`로 서빙, `/ko/cpp/guide/server/<name>/`에서 확인. **doc/site는
   수정·스테이징 금지**(빌드 산출은 scratch 디렉터리로).

## 0. 기준선과 진실 원천

- **기준선.** `01-overview.ko.md`는 이미 변환 완료(14개). 스타일·크기·×N 표기·임베드의 **레퍼런스
  구현**이다. 새 변환은 이것과 동일한 결과를 목표로 한다.
- **정본은 mermaid.** 각 다이어그램의 노드·라벨·연결·색 의미를 그대로 옮긴다. 편의로 노드를
  합치거나 라벨을 바꾸지 않는다(긴 라벨은 가이드 §4대로 카드/축약으로 보존). mermaid가 틀렸다고
  판단되면 **임의로 고치지 말고 Claude에게 에스컬레이션**한다.
- **규칙은 가이드.** `diagram-authoring-guide.ko.md`가 색 매핑·박스 제약·배치·×N·임베드·검증을
  규정한다. 서브에이전트는 이 문서를 정독한 뒤 작업한다.

## 1. 역할 분담

| 주체 | 책임 |
| --- | --- |
| **codex sol 서브에이전트** | 한 파일의 mermaid를 읽고, archify IR 작성 → `build-diagram.mjs` → `pad-viewbox.mjs` → 라이트/다크 스크린샷 자체검증 → markdown 임베드까지 수행. 산출물과 자체검증 결과를 보고. |
| **Claude(감독)** | 서브에이전트에 계약 전달, 산출물 리뷰(내용 대조 + 교차 눈검사 + 라이트/다크 + 크기 통일), 판정·에스컬레이션 처리, 공통폭 재계산, 커밋. 스펙/정본 판정은 Claude 단독. |

codex sol이 불능이면 **임시 폴백은 opus 서브에이전트**. Fable 직접 구현 금지.

> **현재 상태(2026-08-31).** 이 환경의 codex-rescue는 실제 작업 대신 분리된 codex 백그라운드
> 태스크만 띄우고 산출물 없이 반환한다(파일럿에서 확인). 따라서 **opus 서브에이전트 폴백을
> 적용 중**이다. opus는 자기 스크린샷을 Read로 1차 자체검증하고, 최종 시각 리뷰는 Claude가 한다.

## 2. 서브에이전트 계약(전달물·금지·산출)

한 서브에이전트에 **파일 하나**를 맡긴다. 전달물:

1. **가이드 문서 경로** `doc/principal/documentation/diagram-authoring-guide.ko.md` — 정독 지시.
2. **대상 파일 경로**와 그 안의 mermaid 블록(정본).
3. **레퍼런스** `framework/doc/framework/common/diagrams/01-*` (완료된 IR/HTML 예시).
4. **도구·환경**: `scripts/diagrams/*`, `ARCHIFY_DIR`, playwright.
5. **공통폭 값**(현재 계획: §5).

금지: mermaid 내용 변경, 노드 임의 합치기, 색 의미 바꾸기, 자동수정(autofix)식 side 회전으로
교차 덮기, 스펙/정본 판정. 모호하거나 mermaid가 틀린 것 같으면 **멈추고 보고**.

산출: 변환된 `<name>.architecture.json`(+ sequence면 해당 타입) / `.html`, 임베드된 markdown,
그리고 **자체검증 결과**(render 0 에러? 교차 몇 개? ×N 스택 삽입 개수와 라이트·다크 가시성?
누락 문구는 카드에?).

## 3. 대상 목록

`01-overview`(완료)를 제외한 9개 파일(30개) 중 **`06-spot`(1개) 완료** — 남은 것은 **8개 파일,
29개** mermaid. 다이어그램 타입은 서브에이전트가
블록마다 확인한다(architecture/flowchart는 가이드대로, sequenceDiagram은 archify sequence 타입으로).

| 파일 | mermaid | 비고 |
| --- | --- | --- |
| `02-getting-started.ko.md` | 1 | sequence 포함 |
| `03-concepts.ko.md` | 6 | 개념도 다수 |
| `04-backpressure.ko.md` | 1 | |
| `05-channel-messaging.ko.md` | 8 | 최다, sequence 포함 |
| `06-spot.ko.md` | 1 | **완료**(파일럿) — 2 region 세로 스택, 공통 gate fan-in |
| `10-location.ko.md` | 1 | flowchart |
| `12-operations.ko.md` | 2 | |
| `14-samples.ko.md` | 7 | 샘플 업무 흐름 |
| `17-alternative.ko.md` | 3 | sequence 포함 |

**작업 순서(권장).** 쉬운 것부터 신뢰를 쌓는다: 06(완료) → 04 → 10 → 12 → 02 → 03 → 17 → 14 → 05.
sequence가 섞인 파일(02·05·17)은 sequence 변환 품질을 Claude가 먼저 1개 샘플로 확인한 뒤 진행.

## 4. 워크플로 루프(파일 1개 기준)

1. **디스패치** — Claude가 §2 계약으로 서브에이전트 기동.
2. **변환** — 서브에이전트가 블록마다 타입 확인 → IR 작성 → `build-diagram.mjs`(절대경로 출력)
   → render 0 에러까지 수정 → 자체 스크린샷(라이트/다크).
3. **임베드** — mermaid 블록을 iframe으로 교체(root-absolute 경로, 언어탭 4칸 들여쓰기, 높이
   스크립트 페이지당 1개).
4. **보고** — 산출물 + 자체검증 결과.
5. **리뷰(Claude)** — §6 게이트. 통과면 다음 파일, 실패면 구체 수정 지시.

## 5. 공통폭 재계산

폭 통일 배율은 **전체 다이어그램 중 가장 넓은 것**이 정한다(가이드 §5·§7). 새 다이어그램이
현재 공통폭(1010)보다 넓으면, **전 다이어그램을 새 공통폭으로 다시 `pad-viewbox`** 한다.
따라서 폭 통일은 **모든 파일 변환이 끝난 뒤 한 번에** 재적용하고, 그 전에는 각자 crop 상태로
둔다. 지나치게 넓은 그림은 세로 스택으로 좁혀 공통폭을 낮춘다.

## 6. 리뷰 게이트(파일별)

Claude가 각 파일에 대해 확인한다. 하나라도 실패면 반려.

- [ ] **내용 대조** — mermaid의 모든 노드·라벨·연결·색 의미가 반영. 축약분은 카드에 있는가.
- [ ] **교차 0** — 스크린샷으로 눈으로 센다(validator 통과로 갈음 금지).
- [ ] **×N 스택** — 삽입 개수(`grep -c 'style="fill:none"'`)와 **라이트·다크 둘 다** 가시성.
- [ ] **색 의미** — 주황=오버헤드 / 초록=Spot / 파랑=app / 회색=infra / 보라=DB.
- [ ] **크롬·범례 없음**, root-absolute 임베드, 높이 스크립트 1개.
- [ ] **sequence** — sequence 다이어그램은 참여자·메시지·순서가 정확한가.

## 7. 전역 완료 체크리스트

- [ ] 9개 파일 30개 mermaid 전부 변환·임베드, 각 파일 §6 게이트 통과.
- [ ] 최종 **공통폭 재통일**(01-overview 포함 전 다이어그램 동일 배율).
- [ ] 사이트 빌드(`generate_language_guides.py` → `mkdocs build`) 무오류, 라이트·다크 확인.
- [ ] 파일별 mermaid 잔여 0(`grep -c '```mermaid'`).
- [ ] 커밋(도구·가이드·계획 + 변환 결과). doc/site는 **스테이징 금지**(커밋 전 cached 대조).
- [ ] 완료 후 **영문판(.en.md)** 변환으로 이관.

## 8. 리스크·주의

- **sequence ≠ architecture.** 가이드는 architecture 중심이다. sequence는 archify sequence 타입을
  쓰되 참여자·순서 보존을 우선하고, 변환 품질이 나쁘면 Claude 판단으로 해당 블록은 mermaid 유지도
  가능(에스컬레이션).
- **fresh 서브에이전트는 문맥이 없다.** 반드시 가이드·정본·레퍼런스를 명시적으로 전달한다.
- **doc/site 금지.** 변환은 `common/guide`(markdown)와 `common/diagrams`(에셋)만 건드린다.
- **커밋 시점.** 전 파일 완료·전역 체크리스트 통과 후 한 번에. 중간 산출은 워킹트리에 둔다.
