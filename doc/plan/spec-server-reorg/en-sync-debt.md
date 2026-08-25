# en 동기화 부채 — ko 리뷰 반영이 en에 반영되지 않은 곳

> §5의 "en 47개 일괄 작성" 단계는 **리뷰 반영이 끝난 ko**를 기준으로 써야 한다. 이 문서는
> 그 기준을 어긋나게 만드는 항목을 모은다. 자동 검사(`check_doc_links.py`,
> `check_doc_tabs.py`, `mkdocs --strict`)는 en이 낡아도 전부 통과하므로 어떤 게이트도 이것을
> 잡지 못한다. 손으로 관리한다.

## 1. 구조가 갈라진 문서

`## `와 `### ` 개수를 ko/en 쌍으로 비교했다. 48쌍 중 1건.

**현재 0건이다.** 48쌍 전부 `## `·`### ` 개수가 같다.

| 문서 | 처리 |
|---|---|
| `00-foundation/04-interaction-model` | **해소 완료** — ko §7의 6개 소절(Spot 세 종류·실행 gate·Instance Spot 생성·Ref·Actor message 경로·handler 대기 중 진행)을 영어로 새로 썼다. 산문 불릿 5개였던 것을 같은 구조로 폈고, 게임·웹 서비스 비유와 "언제 쓰는가" 열도 함께 옮겼다. |

## 2. ko 본문만 바뀐 리뷰 반영

en에 같은 내용이 있는지 문서 단위로 확인해야 하는 항목이다.

| 반영 내용 | ko 위치 | en 확인 필요 |
|---|---|---|
| Spot 세 종류를 게임·웹 서비스 예로 설명 + "언제 쓰는가" 열 | `00-foundation/04-interaction-model` §7 | **반영 완료** |
| Frame head prefix byte 맵, flags 4열 표, multipart byte 맵 | `02-channel-transport/06-wire-protocol` §2 | **미반영** — 남은 유일한 항목 |
| D1·D2·D4·D5·D6·D7·D8 gap 결정 반영 | `04-session/01-stream-session`, `04-session/02-session-actor-binding` | **미반영** — ko만 고쳤다 |
| 용어 첫 등장 시 풀어쓰기(문서마다 다시) | 전 주제 | 분할 sweep이 en도 함께 편집했으므로 대체로 반영. 문서별 확인 필요 |
| `exact` 제거 | 전 주제 | 반영됨(en은 `per-language`로 치환) |

## 3. 확인 명령

```bash
cd framework/doc/framework/common/spec/server
for f in $(find 0*-*/ session -name "*.ko.md" | sort); do
  en="${f%.ko.md}.en.md"
  k2=$(grep -c '^## ' "$f");  e2=$(grep -c '^## ' "$en")
  k3=$(grep -c '^### ' "$f"); e3=$(grep -c '^### ' "$en")
  [ "$k2" != "$e2" ] || [ "$k3" != "$e3" ] && echo "$f  ##=$k2/$e2  ###=$k3/$e3"
done
```

이 목록이 비면 구조는 맞은 것이다. **본문 내용까지 같은지는 이 명령이 보증하지 않는다.**
