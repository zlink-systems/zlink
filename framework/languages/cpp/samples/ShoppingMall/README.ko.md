# ShoppingMall C++ 샘플

`ShoppingMall`은 주문 생성, 멱등성, 실패 보상, projection rebuild, scale-out owner
선택을 C++ framework 표면으로 보여주는 샘플이다. 서버는 구 registry 프로세스를 쓰지
않고 Redis location store에 현재 프로세스 위치를 등록한다. 주문 이벤트 스트림, 조회 모델,
재고, 결제, 멱등 상태도 같은 Redis 안에서 실행별 key prefix로 격리한다.

## 실행

```bash
ZLINK_CPP_BUILD_DIR=build-redis-vcpkg ./framework/languages/cpp/samples/ShoppingMall/run_sample.sh
```

## 구성

- `Server/CommerceApi/`는 HTTP 주문 요청을 받고 workflow 역할로 주문 처리를 위임한다. API
  프로세스도 같은 MeshName의 MeshNode에 참여해 workflow owner의 order Spot으로 direct request를 보낸다.
- `Server/OrderWorkflow/`는 order별 `OrderWorkflowSpot`을 만들고, 주문 상태 전이, 실패 처리,
  projection rebuild를 spot handler에서 실행한다.
- `Client/`는 성공 주문, 멱등성, pending 복구, 재고 실패, 결제 실패, projection rebuild,
  지연 읽기 일관성, scale-out 증거를 검증한다.
- `run_sample.sh`는 실행마다 전용 Redis 컨테이너를 만들고, 모든 Redis key와 log를 실행별
  prefix와 디렉터리 아래로 격리한다.

성공하면 client log에 `shoppingmall=completed`가 남고 workflow log에 order spot 실행 증거가 남는다.
runner는 두 조건을 확인한 뒤 `PASS ShoppingMall.Cpp`를 출력한다.
