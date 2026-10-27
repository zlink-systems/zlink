# 템플릿 제작 규칙

이 문서는 저장소에서 장르 템플릿을 제작·검증·배포하는 개발자와 에이전트에게 적용한다.
내려받은 프로젝트의 기능 개발 지침은 각 프로젝트의 `AGENTS.md`가 소유한다.
템플릿 제작 절차와 저장소 운영 조건을 사용자 프로젝트의 지침에 복사하지 않는다.

## 산출물과 의존성

- 템플릿 디렉터리만 복사해 빌드하고 실행할 수 있게 한다. 저장소 Framework 소스,
  로컬 package feed, 개발자 머신의 절대 경로에 의존하지 않는다.
- 배포한 공개 Framework·connector package를 사용하고 버전을 언어별 중앙 설정에 고정한다.
  프로젝트 참조는 생성되는 프로젝트 자체의 모듈 사이에서만 사용한다.
- Initializr가 치환하는 `my-game`·`MyGame` 토큰을 유지한다.
- node 역할별 진입점을 분리하고 공통 hosting·설정·제어 protocol은 한 곳에 둔다.
- AGENTS는 영어 `AGENTS.md`와 한국어 `AGENTS.ko.md`를 같은 구성으로 작성한다.
  다운로드 zip에는 사이트 언어에 맞는 판 하나가 `AGENTS.md`로 들어간다.
  README는 영어로 작성하고 마지막에 짧은 한국어 실행 안내를 둔다.
  AGENTS 두 판은 [문서 작성 원칙](../../doc/principal/documentation/documentation-principles.ko.md)을 따른다.
  코드 주석은 영어로 작성한다. 제작 job의 보고서는 격식 있는 기술 문체 한국어로 쓴다.
- 로그, 빌드 결과, package cache, 검증용 사본, 자격 증명과 license 파일을 커밋하지 않는다.
- 포매터·lint·설치 명령은 대상 템플릿 디렉터리 안에서 실행한다. 파일 목록에서 로그와
  검증·빌드 결과를 제외하고, 커밋 전에 템플릿 밖 변경이 없는지 확인한다.

## 구성과 검사기

- `zlink.structure.json`, 실제 node 등록, 설정 ID, shared contract·상수, scenario 목록을
  함께 맞춘다. README 명령과 descriptor의 `run.commands`도 일치시킨다.
- 장르가 요구하는 기본 scenario와 순서, reconnect·update의 관찰과 assertion을 유지한다.
  새 기능의 scenario는 기본 검증을 대체하지 않고 확장한다.
- 구조 검사기는 실제 프로젝트 파일을 읽고 참조·설정·타입의 불일치를 거부하게 한다.
  Redis와 node를 시작하기 전에 구조 검사를 실행한다.
- 유효한 확장 구성과 의도적으로 잘못 만든 구성을 모두 검사한다. checker 자체 테스트나
  입력 변이 코드를 배포되는 검사기에 추가하지 않는다. 검증용 입력은 개발용 사본에서 만든다.
- 초기 구성의 node 수·배열 순서·ID를 그대로 요구하는 검사로 유효한 확장을 막지 않는다.

## 검증과 자원 정리

- 관련 domain test와 구조 검사부터 실행하고 Windows와 Linux/WSL에서 빌드·e2e를 검증한다.
  제작 완료 검증에서 성공한 전체 e2e는 환경별 세 번 실행한다. 개별 job의 명시적 검증 범위와
  실행 횟수가 있으면 그 지시를 따른다.
- reply·event·lifecycle observation을 실제로 받아 판정한다. 명령 수락만으로 relocation 완료나
  safe shutdown을 판정하지 않는다. 고정 sleep·retry·deadline 확대·assertion 완화로 통과시키지 않는다.
- 간헐 실패는 한 번이라도 결함으로 기록한다. 재실행 통과를 근거로 첫 실패를 제외하지 않는다.
- message-flow 기록은 dispatch diagnostics `Normal` 로거가 node stderr에 남긴다. 별도 listener를 더하지 않는다.
- 첫 재현부터 node stdout·stderr와 message-flow 기록을 보존한다. 임시 로깅이나 재실행 전에
  이미 있는 로그의 첫 실패를 읽고, 통과 실행과 transition을 대조한다.
- 실행마다 고유 Redis key prefix와 Compose project 이름, OS가 배정하는 포트를 사용한다.
  실행별 설정과 로그는 `.logs/run-<id>/`에 분리한다.
- runner가 만든 child process와 Compose project만 정리한다. 정리 오류도 실행 실패로 반환한다.
  공용 Docker daemon과 다른 작업의 process·container를 중단하지 않는다.
- Framework 결함은 해당 job의 BLOCKERS에 공개 API 재현, package 버전, 첫 node 오류,
  message-flow 기록과 관련 Framework `file:line`을 남긴다. 기존 이슈가 있으면 번호를 적는다.
  Framework 수정과 이슈·보드 운영은 담당 작업의 범위·승인 절차를 따른다.
- 완료 전에 변경 파일 범위에서 성능, POSDDD 소유 관계, 불필요 코드를 점검한다.
  수정 사항·남은 실패·검증 환경과 실행 횟수를 보고하고 임시 자원을 정리한다.

## 사용자 가이드의 범위

사용자 AGENTS에는 프로젝트 구조, 복사 가능한 개발 명령, 실제 확장 지점, 공개 문서 링크,
상태 소유·비동기 실행의 주의점, 진단 설정과 결함 보고 방법을 적는다. 공개 API 경계,
첫 실패 로그 보존, 간헐 실패 판정과 자원 소유는 사용자 개발에도 필요하므로 유지한다.
반면 제작 완료를 위한 반복 횟수, 검사기 구현 정책, Initializr 토큰, 저장소 배포 의존성,
문서 언어와 검증 산출물 정책은 이 문서에서 관리한다. README가 소유하는 실행·배포·알려진
문제는 사용자 AGENTS에서 링크한다.
