# Node ZoneWorld browser entry 복구 결과

## 변경 내용과 근거

`game.html`과 `ops.html`은 Vite가 각 Preact application을 시작하는 최소 HTML 문서다.

| 요소 | `game.html` | `ops.html` | 근거 |
|---|---|---|---|
| mount 요소 | `<div id="app"></div>` | `<div id="app"></div>` | `src/app/game.tsx:7`과 `src/app/ops.tsx:7`이 `document.getElementById('app')`에 page를 render한다. |
| module entry | `/src/app/game.tsx` | `/src/app/ops.tsx` | `vite.config.ts:9-10`이 HTML을 별도 entry로 선언하고, 각 app module이 해당 page와 공통 theme를 import한다. |
| title | `ZoneWorld` | `ZoneWorld Ops` | `src/pages/game/game-page.tsx:29`과 `src/pages/ops/ops-page.tsx:21`의 최상위 heading 및 `tests/e2e/shell.spec.ts:15,22`의 기대값과 일치한다. |
| 문서 언어 | `lang="en"` | `lang="en"` | 두 page의 heading, navigation, button과 안내문이 영어로 제공된다. |
| browser 기본 설정 | UTF-8 charset, responsive viewport | UTF-8 charset, responsive viewport | 영문 UI와 `src/shared/ui/theme.css:96-101`의 viewport 기반 반응형 layout을 browser가 표준 방식으로 처리한다. |
| runtime 설정 | HTML attribute 없음 | HTML attribute 없음 | `src/shared/config/runtime.ts:6-16`은 `/config.json`의 `gateway`와 `ops`만 읽는다. meta, query 또는 global 값은 조회하지 않는다. |

두 문서에는 inline application logic을 넣지 않았다. `.gitignore:118`에는 다음 negation을
추가했다.

```gitignore
!framework/languages/shared_sample/zoneworld/client/*.html
```

root에서 `git check-ignore -v`를 실행하면 두 파일 모두 위 negation 규칙을 표시한다.
`git status --short`는 두 파일을 `??`로 표시하므로 commit 대상이 될 수 있다.

## 검증 결과

| 검증 | 결과 |
|---|---|
| `npm exec vite build` | 통과. `dist/game.html`, `dist/ops.html`과 각 application bundle을 생성했다. |
| `npm exec playwright test -- tests/e2e/shell.spec.ts` | 통과, 2 tests. Playwright 1.61.1이 요구하는 Chromium revision 1228을 먼저 설치했다. |
| 지정된 Node ZoneWorld sample gate, 첫 번째 성공 실행 | 통과. `shared-browser=completed`, `zoneworld=completed`, `PASS ZoneWorld`을 확인했다. |
| 지정된 Node ZoneWorld sample gate, 두 번째 성공 실행 | 통과. `shared-browser=completed`, `zoneworld=completed`, `PASS ZoneWorld`을 확인했다. |

첫 Node gate 시도는 Chromium revision 1228이 없어 browser test가 시작되지 않았고 lifecycle
marker 대기에서 종료됐다. 필요한 browser를 설치한 뒤 shell spec과 Node gate 두 번이 모두
통과했다.

## 다른 언어 runner 확인

`framework/languages/dotnet/samples/ZoneWorld/run_sample.sh:687-719`도 같은 shared browser client를
Vite로 build하고 Playwright live spec을 실행한다. C++과 Java ZoneWorld runner에서는 `vite`,
`playwright` 또는 shared client 경로 참조를 찾지 못했다. 요청 범위에 따라 이 runner들은
실행하지 않았다.

## BLOCKERS

없음.
