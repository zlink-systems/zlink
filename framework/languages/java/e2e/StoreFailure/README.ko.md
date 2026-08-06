# Java StoreFailure E2E

이 E2E는 공통 Config 6 계약에 따라 Redis location store의 장애와 복구 동작을 Java framework
공개 API로 검증한다. 정식 시나리오 정의는
`framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`를 따른다.

## 구성

- `Shared`: client와 server가 함께 쓰는 메시지, HTTP 요청과 대기 helper를 둔다.
- `Server/Provider`: channel provider와 provider evidence endpoint를 제공한다.
- `Server/Consumer`: location store 자동 연결로 request를 보내고, public MeshNode runtime snapshot을
  HTTP endpoint로 노출한다.
- `Client`: scenario 이름을 받아 검증을 실행한다.

Runner는 실행별 role 설정 파일을 만들어 server에 전달한다. Client의 scenario selector는 시작할 때
검증하는 CLI 입력으로 전달한다.

## 실행

```bash
./run_e2e.sh SF-A1
./run_e2e.sh SF-A2
./run_e2e.sh SF-B1
./run_e2e.sh SF-B2
./run_e2e.sh SF-B3
./run_e2e.sh SF-C1
./run_e2e.sh SF-C2
./run_e2e.sh SF-C3
./run_e2e.sh SF-D1
./run_e2e.sh SF-D2
./run_e2e.sh SF-D3
./run_e2e.sh SF-E1
./run_e2e.sh SF-F9
./run_e2e.sh all
```

인자를 생략하면 baseline인 `SF-A1`을 실행한다. 실패하면 `logs/<run-id>/` 아래 role
stdout, stderr, flow log를 출력한다.

## 현재 검증 범위

시나리오별 완료 여부는 `feature-map.ko.md`가 소유한다. `SF-B2`, `SF-B3`, `SF-D2`는
공통 계약의 단언을 실제로 실행한 뒤 현재 runtime blocker를 기록하므로 runner 성공으로 처리하지
않는다. `SF-C1`과 `SF-E1`은 public topology state와 실제 Redis response delay를 사용한다.

Config 6 범위는 public HTTP harness, runtime query, 실제 messaging 결과를 함께 사용해 검증한다.
