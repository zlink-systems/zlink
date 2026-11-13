# Spec revision E1–E8 적용 보고

작업 브랜치: `spec/1639-owner-multi-drain`. 변경은 서버 스펙 파일 여덟 개에 한정했고 commit/push는 하지 않았다.

## E1 — §02 gate 없는 owner의 ordering

- ko: `01-execution/02-handler-turn-and-execution-gate.ko.md:97–105`
- en: `01-execution/02-handler-turn-and-execution-gate.en.md:101–108`
- §2 정의 paragraph의 draft 병렬 실행 추가 문장은 제거하고 main 문장으로 되돌렸다. 구체적인 queue take 순서와 pipeline 진입 시점을 E1 bullet에서 정의하므로 중복 설명은 두지 않았다.

## E2 — §08 §4.3 record 실행 소유권

- ko: `01-execution/08-messaging-hot-path.ko.md:220–241`
- en: `01-execution/08-messaging-hot-path.en.md:243–265`
- W1 행과 gate 없는 owner의 여러 worker 처리 규칙을 교체했다. ko/en이 record 선택, dispatch 책임, ready-state 소유자, suspend 후 continuation 및 gated owner 규칙을 동일하게 설명한다.

## E3 — §04 ready set과 대기 protocol

- ko: `01-execution/04-application-job-queue-and-backpressure.ko.md:189–224`
- en: `01-execution/04-application-job-queue-and-backpressure.en.md:210–245`
- Membership, enqueue/last-take ordering, worker register/recheck/wait, 추가 worker 가용 조건, notification 병합과 내부 확인 조건을 반영했다.

## E4 — §04 enqueue notification

- ko: `01-execution/04-application-job-queue-and-backpressure.ko.md:241–242`
- en: `01-execution/04-application-job-queue-and-backpressure.en.md:266–267`
- 빈 queue 게시 시 ready-owner 규칙에 따라 scheduling을 요청하고 commit 구간 뒤 실제 알림을 전달하도록 바꿨다.

## E5 — §04 permit lifetime

- ko: `01-execution/04-application-job-queue-and-backpressure.ko.md:120–125`
- en: `01-execution/04-application-job-queue-and-backpressure.en.md:134–139`
- Queue에서 물리적으로 꺼낸 시점과 permit/count accounting 종료를 분리하고 pre-callback terminal의 정확히 한 번 반환을 명시했다.

## E6 — worker별 drain 예산과 scheduler fairness

- ko: `01-execution/02-handler-turn-and-execution-gate.ko.md:409–417`; `00-foundation/06-framework-api.ko.md:547–562`
- en: `01-execution/02-handler-turn-and-execution-gate.en.md:470–478`; `00-foundation/06-framework-api.en.md:621–638`
- §02 문안은 §9의 기존 예산 paragraph 뒤, §10 앞에 배치했다. Foundation의 점유 상한 문단은 gated owner 설명과 gate 없는 worker별 drain/fairness 설명으로 교체했다.

## E7 — §08 I4 switch counting unit

- ko: `01-execution/08-messaging-hot-path.ko.md:187`
- en: `01-execution/08-messaging-hot-path.en.md:204`
- I4 표의 마지막 cell만 owner 단위에서 record claim부터 handler 첫 instruction까지의 단위로 바꿨다.

## E8 — §08 internal check와 verification item (f)

- ko: `01-execution/08-messaging-hot-path.ko.md:294–298`, `409–418`
- en: `01-execution/08-messaging-hot-path.en.md:325–329`, `454–463`
- ready membership/wait의 원자성, worker별 budget 재선택, I4–W3 switch count 확인을 추가하고 §7의 (f)를 넣었다.

## 링크 검사

실행: `/tmp/zlink-doc-links-venv/bin/python doc/site/scripts/check_doc_links.py framework`

결과: `검사[framework]: 문서 1203개, 상대 링크 22120개` / `OK — 상대 링크가 모두 저장소의 실제 파일을 가리킨다`. checker가 한글 heading slug를 포함한 anchor 존재 여부도 검사했다. checker는 framework 트리 전체를 받으므로 변경 파일 여덟 개에 한정된 입력 옵션은 제공하지 않는다.

`git diff --check`도 통과했다. 지정 문안에서 적용하지 못한 항목은 없다.
