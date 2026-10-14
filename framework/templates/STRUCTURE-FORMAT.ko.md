# ZLink 프로젝트 구조 형식 v1

[English](STRUCTURE-FORMAT.md)

`zlink.structure.json`은 내려받는 서버 프로젝트와 구성도를 정의한다. Initializr와 구조 편집기는 같은 UTF-8 JSON 형식을 만든다. 이 파일은 application 구성을 정의하며, runtime 동작은 Framework 공개 계약을 따른다. 모든 서버 언어의 모든 장르 템플릿이 이 형식을 사용한다.

## 최상위 필드

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| `version` | integer | 형식 버전. 이 문서는 `1`을 정의한다. 알 수 없는 버전은 거부한다. |
| `id` | string | 랜딩 구성도의 topology ID. 예: `mmo-seamless`, `board-tables`. |
| `name` | string | 프로젝트 이름. Initializr가 프로젝트 생성 시 `my-game`을 바꾼다. |
| `language` | string | 서버 언어. `dotnet`, `java`, `kotlin`, `node`, `cpp` 중 하나다. |
| `run` | object | 형식 v1의 필수 필드. 빌드·e2e 명령과 순서가 있는 e2e 시나리오 ID를 둔다. |
| `nodes` | array | 외부 client와 인프라를 포함한 구성도 참여자. |
| `channels` | array | application node가 소유하는 channel 등록. |
| `spots` | array | 고정 logical Spot과 요청에 따라 만드는 Spot 종류. |
| `edges` | array | 표시 순서의 방향 있는 연결. |

## Run

형식 v1은 `run.commands`와 `run.scenarios`를 필수로 둔다. `commands`에는 `sh`와 `powershell` 키만 둔다. 각 값은 비어 있지 않은 문자열 배열이어야 한다. 구조 검사는 이 형식만 확인하며 명령을 실행하거나 문자열 내용을 대조하지 않는다. 배열에는 프로젝트 README의 Build, 기본 Local e2e, 시나리오 선택 명령을 둔다.

`scenarios`는 실행 순서의 e2e 시나리오 ID 배열이다. 프로젝트 e2e client에 정의된 시나리오 목록과 원소 및 순서가 같아야 한다.

## Node

Node마다 `id`, `role`, `title`, `config`를 둔다. ID는 비어 있지 않은 고유 문자열이며 구성 안에서 참여자를 식별한다. Node 개수와 ID를 프리셋 값으로 고정하지 않는다. `title`은 구성도 표시 이름이다. `role`은 application의 책임을 나타낸다. 예: `external`, `session`, `zone`, `table`, `instance`, `location-store`. Role을 이 목록으로 제한하지 않는다.

`config`는 `/`로 구분한 node 설정 파일의 상대 경로다. 외부 client는 `null`이며 게임 client 프로젝트를 만들지 않는다. 경로는 프로젝트 루트 안에 있어야 한다. `location-store` node는 인프라 설정 파일을 가리킨다. 예를 들어 Redis는 `docker-compose.yml`을 사용한다. Application process의 JSON 설정은 해당 node ID와 같은 `Id`를 가져야 한다.

Role은 배포 책임이다. 구성도에 ×N으로 표시한 `instance`, `table` 같은 role은 여러 process로 실행할 수 있다. Role은 Framework Spot 종류를 정하지 않는다. 예를 들어 `instance`는 Dungeon User Spot을 호스팅하며, Actor가 없는 Framework Instance Spot 종류를 뜻하지 않는다.

## Channel

Channel마다 비어 있지 않은 `name`, 소유 node ID인 `owner`, `mesh` 이름을 둔다. `(name, owner)` 쌍은 고유하다. 여러 node가 같은 mesh에 같은 channel 이름을 등록할 수 있다. 선언은 소유 node의 server 등록을 나타내며 별도 transport 연결을 뜻하지 않는다.

## Spot

Spot마다 구성 안에서 비어 있지 않은 고유 `id`, `kind`(`entry`, `user`, `instance`), `type`, 소유 node ID인 `owner`를 둔다. User·Instance Spot의 `type`은 Framework stable type이다. Entry Spot은 runtime ID를 Framework가 발급하며 stable-type 등록이 없으므로 `type`을 `null`로 둔다. Owner는 내려받는 구성의 초기 host다. Runtime relocation으로 현재 owner가 바뀔 수 있다. 구조 파일은 Location Store의 authority를 대신하지 않는다.

고정 User·Instance Spot은 정확한 runtime logical ID인 `logicalId`가 필수다. 고정 logical ID는 구성 안에서 stable type과 무관하게 고유해야 한다. `dynamic`이 `true`이면 runtime 요청이 logical ID를 선택하며 `logicalId`를 두지 않는다. 예를 들어 구성 ID `dungeon`은 party ID를 사용한 `GetOrCreate`로 만드는 `Dungeon` User Spot 종류를 나타낸다. Entry Spot은 `logicalId`와 `dynamic`을 모두 두지 않는다. Framework가 replacement node의 새 runtime ID를 발급해도 구성 ID는 유지된다.

## Edge

Edge마다 존재하는 node ID인 `from`, `to`와 비어 있지 않은 `label`을 둔다. 선택 필드 `semantic`은 랜딩 구성도의 의미 태그를 보존한다. 배열 순서는 구성도의 표시 순서를 정한다. 일치 검사는 추가하거나 순서를 바꾼 edge를 허용하며 배열 index로 동작을 판정하지 않는다. Edge는 구성도의 주요 흐름을 나타내며 모든 mesh 연결이나 Redis client를 열거하지 않는다.

## 일치 검증

모든 e2e 실행은 node 기동 전에 구조를 검증한다. 검사는 버전, 비어 있지 않은 최상위 문자열, `run`의 형식과 시나리오 목록, node·구성 Spot ID의 고유성, channel 등록·logical Spot ID의 고유성, owner와 edge 참조, 프로젝트 안의 상대 설정 경로, application 설정 ID를 확인한다. 선언한 channel 이름·mesh·Spot type은 서버 상수와 대조한다. 템플릿은 자기 설정과 선언한 Spot을 대조하는 검사를 더할 수 있다. 심리스 템플릿은 각 node의 고정 Zone `logicalId` 집합을 설정의 `ZoneIds`와 대조한다.

검사는 프리셋 ID, 고정 node·Spot 개수, 고정 배포 ID, edge 목록, 배열 위치를 요구하지 않는다. Node·Spot·edge를 추가해도 형식 규칙과 application 계약을 만족하면 허용한다. JSON 파싱 통과만으로 판정하지 않는다.

참조 산출물:

- [MMO 심리스 월드, .NET](../languages/dotnet/templates/mmo-seamless/my-game/zlink.structure.json)
