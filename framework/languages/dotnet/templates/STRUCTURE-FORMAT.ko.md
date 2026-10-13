# ZLink 프로젝트 구조 형식 v1

[English](STRUCTURE-FORMAT.md)

`zlink.structure.json`은 내려받는 서버 프로젝트와 구성도를 정의한다. Initializr와 구조 편집기는 같은 UTF-8 JSON 형식을 만든다. 이 파일은 application 구성을 정의하며, runtime 동작은 Framework 공개 계약을 따른다.

## 최상위 필드

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| `version` | integer | 형식 버전. 이 문서는 `1`을 정의한다. 알 수 없는 버전은 거부한다. |
| `id` | string | 랜딩 구성도의 topology ID. 예: `mmo-seamless`. |
| `name` | string | 프로젝트 이름. Initializr가 프로젝트 생성 시 `my-game`을 바꾼다. |
| `language` | string | 서버 언어. 이 템플릿은 `dotnet`이다. |
| `nodes` | array | 외부 client와 인프라를 포함한 구성도 참여자. |
| `channels` | array | application node가 소유하는 channel 등록. |
| `spots` | array | 고정 logical Spot과 요청에 따라 만드는 Spot 종류. |
| `edges` | array | 랜딩 구성도 순서의 방향 있는 연결. |

## Node

Node마다 `id`, `role`, `title`, `config`를 둔다. ID는 비어 있지 않은 고유 문자열이며 랜딩 구성도의 ID와 같다. `title`은 구성도 표시 이름이다. `role`은 application의 책임을 나타낸다. 이 템플릿은 `external`, `session`, `zone`, `instance`, `location-store`를 사용한다.

`config`는 `/`로 구분한 node 설정 파일의 상대 경로다. 외부 client는 `null`이며 게임 client 프로젝트를 만들지 않는다. 경로는 프로젝트 루트 안에 있어야 한다. Redis는 `docker-compose.yml`을 가리킨다. Application process의 JSON 설정은 해당 node ID와 같은 `Id`를 가져야 한다.

`instance`는 배포 역할이며 여러 process로 실행할 수 있다. 템플릿은 process 하나에서 Dungeon User Spot을 호스팅한다. 이 역할은 Actor가 없는 Framework Instance Spot 종류를 뜻하지 않는다.

## Channel

Channel마다 비어 있지 않은 `name`, 소유 node ID인 `owner`, `mesh` 이름을 둔다. `(name, owner)` 쌍은 고유하다. 여러 node가 같은 mesh에 같은 channel 이름을 등록할 수 있다. 선언은 소유 node의 server 등록을 나타내며 별도 transport 연결을 뜻하지 않는다. 심리스 템플릿의 두 Zone node는 `my-game-world`에 `border`를 등록한다.

## Spot

Spot마다 구성 안에서 비어 있지 않은 고유 `id`, `kind`(`entry`, `user`, `instance`), `type`, 소유 node ID인 `owner`를 둔다. User·Instance Spot의 `type`은 Framework stable type이다. Entry Spot은 runtime ID를 Framework가 발급하며 stable-type 등록이 없으므로 `type`을 `null`로 둔다. Owner는 내려받는 구성의 초기 host다. Runtime relocation으로 현재 owner가 바뀔 수 있다. 구조 파일은 Location Store의 authority를 대신하지 않는다.

고정 User·Instance Spot은 정확한 runtime logical ID인 `logicalId`가 필수다. `dynamic`이 `true`이면 runtime 요청이 logical ID를 선택하며 `logicalId`를 두지 않는다. 예를 들어 구성 ID `dungeon`은 party ID를 사용한 `GetOrCreate`로 만드는 `Dungeon` User Spot 종류를 나타낸다. Entry Spot은 `logicalId`와 `dynamic`을 모두 두지 않는다. Framework가 replacement node의 새 runtime ID를 발급해도 구성 ID는 유지된다.

고정 Zone의 `logicalId` 값은 소유 node 설정의 `ZoneIds`와 같아야 한다. `ZoneCapacity`는 배치 상한이며 초기 Zone 개수가 아니다. Zone B는 drain 때 A의 Zone 세 개를 받을 수 있도록 용량을 6으로 둔다.

## Edge

Edge마다 존재하는 node ID인 `from`, `to`와 비어 있지 않은 `label`을 둔다. 선택 필드 `semantic`은 랜딩 구성도의 의미 태그를 보존한다. 순간별 강조가 같은 edge index를 가리키도록 배열 순서를 구성도와 같게 둔다. Edge는 구성도의 주요 흐름을 나타내며 모든 mesh 연결이나 Redis client를 열거하지 않는다.

심리스 topology의 방향 있는 연결은 다음과 같다.

| Index | From | To | Label | Semantic |
| --- | --- | --- | --- | --- |
| 0 | players | session | STREAM | |
| 1 | session | zoneA | relay | |
| 2 | zoneA | zoneB | relocation | |
| 3 | zoneB | zoneA | border pub/sub | border-updates |
| 4 | zoneB | instance | GetOrCreate | |
| 5 | zoneB | location | Location / lease | location-authority |

## 일치 검증

템플릿의 구조 검사는 버전, 식별자 중복, owner와 edge 참조, 설정 경로의 범위, node 설정 ID, 초기 Zone 소유, 선언한 channel과 Spot type을 서버 계약과 대조한다. 잘못된 구조는 node 기동 전에 실패해야 한다. 검증에서는 의도적으로 불일치한 구조도 시험한다. JSON 파싱 통과만으로 판정하지 않는다.

참조 산출물: [MMORPG 심리스 월드](mmorpg-seamless/my-game/zlink.structure.json).
