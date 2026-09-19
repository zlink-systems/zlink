[English](./framework-node-0.18.1.md) | [한국어](./framework-node-0.18.1.ko.md)

# ZLink Node.js Framework 0.18.1 릴리스 노트

Framework 0.18.1는 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- stream-connector(TypeScript): `waitFor`·`waitForSequence`의 timeout 오류 코드가 `RequestTimeout`에서 `ValidationFailed`로 바뀝니다(스펙 32 §10.1.1). `RequestTimeout`은 request의 reply 대기에만 씁니다. `expectNone`은 영향이 없습니다. timeout 코드를 검사하던 호출자는 `ValidationFailed`로 바꿉니다. (#667)

## 공통 변경

- 배포 zip(`zlink-tutorial-node.zip`, `zlink-samples-node.zip`)이 저장소 없이 빌드·실행됩니다. 각 zip 루트에 `README.ko.md`·`README.md`가 있고, 전제 조건·내려받기와 설치·빌드·실행·검증·문제 해결 절을 갖습니다. CI guard `standalone-zips`가 checkout 없는 job에서 그 README의 명령 블록을 그대로 실행합니다. (#655, #669)
- Framework GitHub Release마다 tutorial 4·samples 4, 여덟 zip을 자산으로 첨부합니다. core·binding release도 같은 자산을 싣습니다. (#639)
- Node tutorial에 .NET과 같은 Instance Spot 대기열(`MatchQueue`)을 더했습니다. (#666)
- 가이드에 일곱 샘플의 따라 읽기 장(50~56)을 두고, 01·03장 코드를 tutorial snippet 참조로 바꿨습니다. (#640, #641)

## 수정

- 대기자가 자기가 관찰한 연결이 끝나는 순간 `Disconnected`로 끝납니다. 이전에는 다음 연결이 성립할 때 끝나서, 끊긴 뒤 재연결이 없으면 timeout까지 매달렸습니다(스펙 32 §10.1.1). (#667)
- `@zlink-systems/stream-connector`의 배포 패키지에 `dist/package.json`이 빠져 ESM 해석이 실패하던 것을 고쳤습니다. (#655)
- 배포 samples zip의 `prepare-sample-dependencies.mjs`가 unzip한 디렉터리를 저장소로 오판해 일곱 샘플 전부 `prebuild`에서 `ENOENT`로 죽던 것을 고쳤습니다. 저장소 workspace root의 `package.json`을 이름으로 확인합니다. (#655)
- Bingo·DeliveryDispatch·GameQuest·ZoneWorld 샘플을 계약 정본에 맞췄습니다. Bingo room은 `PreserveStateWith`와 application-signaled readiness로 등록하고 완료 round 경계에서 relocation-ready를 미룹니다. DeliveryDispatch는 없는 offer의 courier decision을 로그 뒤 무시합니다. GameQuest sync는 store snapshot을 읽고 `PlayerQuestSpot`이 initialize에서 player ID를 묶습니다. ZoneWorld는 join completion을 OperationId로 중복 제거하고 relocation payload에 보존합니다. (#658, #662, #664, #665)
- binding 1.2.1이 npm 패키지에 win32-x64 prebuild를 실어, Windows에서 소스 빌드 없이 설치됩니다. (#656)

## 설치

```bash
npm install @zlink-systems/framework@0.18.1
```

릴리스 태그는 [`framework-node/v0.18.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.18.1)입니다.
