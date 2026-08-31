# 다이어그램 작성·변환 가이드

> 이 문서는 가이드 문서의 다이어그램을 **mermaid에서 깔끔한 SVG로 변환**하거나 새로
> 그릴 때 따르는 규칙과 절차를 담는다. 주 독자는 다이어그램 작업을 수행하는 AI다.
> 목표는 하나다 — **원본 mermaid에 충실하되, 선이 꼬이지 않고, 크기가 통일된, 담백한
> 그림**을 문서 사이트에 임베드한다. 도구는 [archify](https://github.com/tt-a1i/archify)를
> 렌더러로 쓰고, 후처리·통일·임베드는 저장소 스크립트가 맡는다.

## 0. 왜 이렇게 하는가

mermaid는 빠르게 그리기엔 좋지만, 문서 사이트에서 두 가지가 약하다 — **레이아웃 제어**(선
교차·박스 배치를 강제할 수 없다)와 **톤 일관성**(사이트 테마와 겉도는 격자·색). 그래서
mermaid를 **정본(source of truth)**으로 두고, 그것을 archify IR로 옮겨 배치를 손으로 잡고,
뷰어 크롬·범례를 걷어내고, 폭을 통일해 임베드한다.

**정본은 mermaid다.** 변환은 mermaid가 담은 노드·라벨·연결·색 의미를 **그대로** 옮기는
일이다. 편의를 위해 노드를 합치거나 라벨을 바꾸지 않는다(긴 라벨은 §4의 방법으로 보존한다).
mermaid 자체가 틀렸다면 mermaid를 먼저 고치고, 그 다음 변환한다.

## 1. 결과물의 형식

한 다이어그램은 `common/diagrams/<name>.html` 하나로 만든다. 그 안에는 archify가 렌더한
`<svg>` 하나가 들어가고, 다음을 만족한다.

| 항목 | 규칙 |
| --- | --- |
| 뷰어 크롬 | 없음(Present/Export/PATH·MAP·LENS 툴바 제거) |
| 범례(Legend) | 없음(`meta.legend.mode: "hidden"`) |
| 배경 | 사이트 테마를 따른다(라이트/다크 모두에서 읽혀야 한다) |
| 폭 | 모든 다이어그램이 **같은 viewBox 폭**(내용은 가운데 정렬) → 박스 크기 통일 |
| 선 | 교차 0을 목표로 한다 |
| 긴 설명 | 박스에 안 들어가는 문구는 aside **카드**에 보존 |
| ×N 표기 | 다중 인스턴스 박스는 뒤에 겹친 사각형(§6) |

## 2. 도구와 파이프라인

도구는 `scripts/diagrams/`에 두고 저장소에 커밋한다(현재 스크립트가 없다면 이
문서의 스니펫으로 재생성한다). archify CLI는 별도 설치가 필요하다.

| 도구 | 역할 |
| --- | --- |
| `archify … render architecture <ir.json> <out.html>` | IR을 렌더. `validate`는 엄격, `render`는 관대 |
| `build-diagram.mjs` | render → **×N 스택 삽입** → **뷰어 크롬 제거** → **viewBox를 내용에 맞게 crop** |
| `pad-viewbox.mjs` | 모든 다이어그램 viewBox 폭을 공통값으로 **통일**(내용 가운데 정렬) |
| `shot.mjs` / `shot-dark.mjs` | 라이트/다크 스크린샷으로 **눈으로** 검증 |
| `generate_language_guides.py` → `mkdocs build` | 사이트 빌드 |

한 다이어그램을 만드는 순서.

1. **IR 작성** — mermaid를 보고 archify architecture IR(`<name>.architecture.json`)을 쓴다(§3~§6).
2. **렌더+후처리** — `node build-diagram.mjs architecture <ir> <out.html>`. 검증 에러가 나면
   §7로 고친다. **출력 경로는 절대경로**로 준다(상대경로면 crop 단계의 `file://`가 깨진다).
3. **폭 통일** — 모든 다이어그램을 다 만든 뒤 `node pad-viewbox.mjs <공통폭> common/diagrams/01-*.html`.
   공통폭은 **가장 넓은 다이어그램의 폭 이상**으로 잡는다(§5).
4. **검증** — `shot.mjs`와 `shot-dark.mjs`로 라이트·다크를 뜨고, **선 교차를 눈으로 센다**(§8).
5. **임베드** — markdown의 mermaid 블록을 iframe으로 교체(§9).
6. **사이트 빌드** — `generate_language_guides.py` 후 `mkdocs build`.

> **검증 통과 ≠ 깔끔함.** archify `validate`가 0 에러여도 선은 꼬여 있을 수 있다. 유일한
> 진짜 검사는 스크린샷으로 교차를 세는 것이다.

## 3. 색 의미 — mermaid classDef → archify type

mermaid의 색은 의미를 담는다. 그 의미를 archify type으로 **매핑해서 보존**한다.

| mermaid(의미) | 색 | archify `type` |
| --- | --- | --- |
| app(업무 앱·API 서버) | 파랑 | `frontend` |
| infra(LB·location store 등) | 회색 | `external` |
| extra(실시간·순서 때문에 붙는 조각) | 주황 | `messagebus` |
| spot(ZLink Spot/actor) | 초록(두꺼움) | `backend` |
| DB(`[( )]`) | 보라 | `database` |
| stream node | 보라 | `database` |
| actor/relay | 노랑·주황 | `messagebus` |

핵심 대비 — **주황 = "붙는 오버헤드"**, **초록 = "ZLink가 대신하는 것"**. 이 대비가 기존/ZLink
비교 그림의 요점이므로 색을 임의로 바꾸지 않는다.

## 4. 박스 제약과 라벨

**archify 박스는 120×60 고정이다.** `cellW`는 열 간격만 넓히고 박스 자체는 안 커진다. 텍스트는
`label` 1줄 + `sublabel` 1줄이며 자동 줄바꿈이 없다(넘치면 축약되거나 검증 에러).

- **라벨은 짧게**, 핵심 명사만. 예: `"OrderWorkflow 서버들 ×N / OrderWorkflowSpot / (OrderId
  owner · 직렬 실행 · hot state)"`(mermaid 4줄) → label `"OrderWorkflow ×N"` + sublabel
  `"OrderId owner · 직렬 실행"`.
- **잃은 문구는 카드로 보존.** 위 예의 `hot state`, `OrderWorkflowSpot`, `채널 직접 호출` 같은
  세부는 `cards`에 문장으로 남긴다. 카드는 fidelity의 안전장치다 — 함부로 지우지 않는다.
- **엣지 라벨도 짧게.** 긴 mermaid 엣지 라벨(예: `"고객이 어느 WS인지 몰라 배달 상태 방송"`)은
  그대로 두면 겹친다. 그림엔 `"배달 상태 방송"`만, 전체 문구는 카드에.
- sublabel이 ~120px를 넘으면 `Sublabel … needs ~Npx` 에러가 난다 → 더 줄인다.

## 5. 배치 — 선을 꼬지 않는 법

선 교차의 대부분은 **노드 배치**에서 결정된다. archify는 그리드 셀 사이를 자동 라우팅하므로,
배치가 대체로 planar이면 라우팅도 깨끗하다.

- **side 오버라이드를 남발하지 않는다.** `fromSide`/`toSide`로 에러만 침묵시키면 짧은 선이 L자
  우회선으로 바뀌어 오히려 더 꼬인다. **먼저 그리드를 다시 놓고**, 정말 필요한 엣지에만 side를
  준다.
- **허브 노드**(연결 5개 이상)는 중앙에 두고 이웃을 상하좌우로 분산한다.
- **discovery/저장 대상**(location store, DB)은 소비자 **아래**에 모아 짧은 수직선으로 잇는다.
  여러 노드가 한 store로 모이면 하단 중앙에 두고 fan으로 수렴시킨다.
- **가장 지배적인 선**(예: 서버 간 channel messaging)이 **가장 짧아야** 한다. 이를 위해 두 노드를
  인접 셀에 둔다.
- **중간 노드를 건너뛰는 엣지**(예: client가 matchmaker를 건너 room spot에 직접)는 관통하니,
  `fromSide:"bottom", toSide:"bottom"`으로 **아래로 우회**시킨다.
- **수직 스택 배치**로 폭을 줄인다. 4분면 같은 넓은 그림은 2×2(폭↑) 대신 **1열 4행**으로 세로
  스택하면 폭이 절반이 되고, §2-3의 공통폭을 낮춰 **전체 박스가 커진다**. 세로로 인접한 region은
  겹침 없이 렌더된다(좌우 인접은 frame overlap 위험이 크니 피한다).

**폭 통일과 크기의 관계.** 모든 다이어그램을 공통폭으로 pad하면 박스 크기가 같아진다. 이때
배율은 **가장 넓은 다이어그램**이 정한다. 따라서 넓은 그림을 세로 스택으로 좁혀 공통폭을 낮추면
전체가 커진다. 반대로 박스 많은 그림(예: 11박스 "기존" 그림)이 하한을 만든다.

### region(subgraph) 프레임

mermaid의 `subgraph`는 archify `boundaries`로 옮긴다.

```json
"boundaries": [
  { "kind": "region", "label": "진입 서버 (예: Api)", "wraps": ["http", "apic"] }
]
```

region 라벨은 좌상단에 작게 렌더된다. 4분면 비교처럼 서로 독립인 그림은 **분면마다 region 하나**로
묶으면 분면 간 엣지가 없어 교차가 원천 차단된다.

## 6. ×N — 다중 인스턴스 표기

`×N`, `×길드 수`, `서버들`, `서비스들`이 라벨에 있으면 **여러 인스턴스**라는 뜻이다. 이를 박스
뒤에 **겹친 사각형**으로 표시한다. archify엔 이 기능이 없으므로 `build-diagram.mjs`가 렌더된 SVG를
후처리해서 각 노드의 `<g>` 안에 오프셋 사각형 2개를 넣는다.

두 가지 함정이 있다(둘 다 겪었다).

- **`<title>` 건너뛰기.** 노드는 `<g id="node-…"><title>…</title><rect class="c-mask"/>…`
  구조다. `<g>` 바로 뒤가 `<rect>`가 아니라 `<title>`이므로, 정규식이 `<title>`을 건너뛰지
  않으면 **하나도 안 들어간다**(겉보기엔 단일 박스). 정규식은 `<g …>` 다음을 `[\s\S]*?<rect …
  c-mask>`로 느슨하게 잡아야 한다.
- **테마 무관 색.** 겹친 사각형에 `fill:#ffffff` 같은 하드코딩 색을 쓰면 **다크에서 사라진다**.
  `fill:none` + archify의 `class`(테두리 색이 CSS 변수라 테마 적응)를 써서 색 **테두리만** 그린다.
  오프셋과 굵기는 라이트·다크 모두에서 보이도록 넉넉히(예: 오프셋 11/22px, `stroke-width:2.2`).

```js
// build-diagram.mjs — 렌더 직후, crop 전에 실행
function insertStacks(html) {
  const re = /(<g id="node-\w+"([^>]*)>)([\s\S]*?<rect x="([\d.-]+)" y="([\d.-]+)" width="([\d.-]+)" height="([\d.-]+)" rx="6" class="c-mask"\/>\s*<rect x="[\d.-]+" y="[\d.-]+" width="[\d.-]+" height="[\d.-]+" rx="6" class="(c-[a-z]+)"[^>]*\/>)/g;
  return html.replace(re, (m, gopen, attrs, rects, x, y, w, hh, cls) => {
    if (!/×|서버들|서비스들/.test(attrs)) return m;          // 다중 인스턴스만
    const X=+x, Y=+y, W=+w, H=+hh;
    const copy = (o) => `<rect x="${X+o}" y="${Y+o}" width="${W}" height="${H}" rx="6" class="${cls}" style="fill:none" stroke-width="2.2"/>`;
    return gopen + copy(22) + copy(11) + rects;               // 뒤(먼저 그려짐)부터
  });
}
```

특정 인스턴스를 가리키는 단수 박스(예: `앱 서버 A`, `앱 서버 B`)는 ×가 없으므로 스택을 넣지
않는다 — mermaid가 단수로 쓴 것을 존중한다.

## 7. 뷰어 크롬 제거 · viewBox crop

`build-diagram.mjs`가 렌더 후처리로 함께 수행한다.

- **크롬 제거.** archify 렌더 출력엔 상단 툴바(`.toolbar`)와 하단 PATH·MAP·LENS 바
  (`.diagram-nav`, `.no-print`)가 있다. `<head>`에 `<style>.toolbar,.no-print{display:none!important}</style>`를
  주입해 상시 숨긴다(archify의 `@media print`가 쓰는 규칙과 같다).
- **crop.** 렌더는 원본 좌표계에 넓은 여백을 둔다. playwright로 내용
  (`rect[class*="c-"], rect[data-graph-role], text, path[class*="m-"|"a-"]`)의 bbox를 재서
  `viewBox`를 그 bbox(+28px 여백)로 좁힌다. 이러면 내용이 프레임을 꽉 채운다.
- **공통폭.** 이후 `pad-viewbox.mjs`가 crop된 viewBox 폭을 공통값으로 늘려(`x -= (CW-w)/2`)
  가운데 정렬한다. svg는 컨테이너 폭 100%로 렌더되므로, viewBox 폭이 같으면 배율이 같아져 박스
  크기가 통일된다.

## 8. 검증 체크리스트

배포 전 각 다이어그램에 대해 확인한다.

- [ ] `render`가 0 에러(라벨 겹침·sublabel 초과·edge-through-node 없음).
- [ ] **선 교차를 눈으로 세어 0.** (validator 통과로 갈음하지 않는다.)
- [ ] mermaid의 노드·라벨·연결이 **빠짐없이** 반영(짧아진 문구는 카드에 있는가).
- [ ] 색 의미가 mermaid와 일치(주황=오버헤드, 초록=Spot …).
- [ ] ×N/서버들 박스에 **겹친 사각형이 실제로 보인다** — 삽입 개수는
      `grep -o 'style="fill:none" stroke-width="2.2"' <file> | wc -l`로 센다(한 줄에 여러 사각형이
      들어가므로 `grep -c`는 과소집계한다). **라이트와 다크 둘 다** 스크린샷으로 눈으로 확인.
- [ ] 폭이 공통값. 쌍(기존/ZLink)이 같은 스타일·방향·박스 크기.
- [ ] 크롬·범례 없음.

## 9. markdown 임베드

에셋은 `common/diagrams/`에 둔다(`common/guide/`는 사이트 빌드에서 제외되므로 안 된다). mermaid
블록을 다음으로 교체한다.

```html
<iframe class="zlink-diagram" src="/common/diagrams/01-delivery-existing.html"
        title="기존 방식 — 배달 주문 앱" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-delivery-existing.html" target="_blank">↗ 크게 보기</a></p>
```

- 경로는 **root-absolute**(`/common/diagrams/…`)로 쓴다. mkdocs는 markdown 링크·이미지 경로는
  재작성하지만 **raw iframe의 `src`는 건드리지 않기** 때문이다.
- 언어 탭(`=== "…"`) 안에 넣을 땐 iframe 줄을 **4칸 들여쓴다**(탭 콘텐츠 유지).
- 페이지 끝에 **iframe 높이 자동 맞춤 스크립트를 페이지당 하나** 둔다. iframe은 내부 문서 높이를
  모르므로, `load` 후 `contentDocument`의 `scrollHeight`를 재서 iframe 높이에 반영한다.

## 10. 자주 나는 실패와 처리

| 증상 | 원인·처리 |
| --- | --- |
| `Label "…" overlaps component` | 수직 엣지 라벨이 박스에 겹침 → `labelDy`(위/아래로) 또는 라벨 축약. 폭 넓은 라벨은 옮겨도 겹치니 **축약**이 근본. |
| 수직 엣지 라벨이 안 비켜남 | `labelDx`로 옆으로. 그래도 안 되면 라벨을 빼고 카드로. |
| `Label … wider than component` | 엣지/박스 라벨이 120px 초과 → 축약. |
| `Sublabel … needs ~Npx` | sublabel 초과 → 축약, 세부는 카드로. |
| `edge-through-node crosses` | 엣지가 중간 노드를 관통 → **배치 재조정**이 우선. 불가피하면 `fromSide/toSide`로 아래/옆 우회. autofix식 side 회전으로 덮지 말 것. |
| `must NOT have additional properties {"bidir"}` | archify엔 양방향 속성이 없다. 양방향은 엣지 2개로 그리거나(강조 시), 그림에선 단방향+카드 설명으로 갈음. |
| region이 서로 겹친다는 에러 | 좌우로 인접한 region은 frame이 닿는다 → **세로 스택**으로 바꾼다. |
| 겹친 사각형이 안 보임 | ① 삽입 정규식이 `<title>`을 안 건너뜀(§6) ② 하드코딩 색이 테마에 묻힘 → `fill:none`+class(§6). |
| 단수 노드에 ×N 스택이 잘못 들어감 | `build-diagram.mjs`가 **region 라벨의 `×`** 를 멤버 노드 `data-node-context`로 전파해 단수 노드까지 스택함. 우회: region 라벨에서 `×N`을 빼고(예 `ZoneNode ×2`→`ZoneNode 2대`) 원문은 카드에. 근본 수정은 build-diagram의 ×N 판정을 노드 label/sublabel로 한정. |
| sequence의 `note`가 안 보임 | archify sequence 렌더러가 `messages[].note`/`Note over`를 **렌더하지 않는다**(validate는 통과). suspend/resume·부연은 메시지 **label**로 옮기거나 **카드**에 문장으로 보존. |
| 다이어그램마다 크기가 다름 | viewBox 폭이 제각각 → `pad-viewbox.mjs`로 공통폭 통일(§7). |

## 완료 점검

- [ ] 정본(mermaid) 대비 내용 보존 — 노드·라벨·연결·색 의미, 축약분은 카드에.
- [ ] 선 교차 0(스크린샷으로 확인).
- [ ] ×N 스택이 라이트·다크 모두에서 보임.
- [ ] 폭 통일, 쌍은 동일 스타일·크기.
- [ ] 크롬·범례 없음, root-absolute 경로로 임베드, 높이 스크립트 1개.
- [ ] 도구 스크립트가 저장소에 있고, 이 문서와 버전이 맞는다.
