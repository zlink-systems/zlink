# C++ SupportChat 샘플

이 샘플은 공통 SupportChat 시나리오의 고객 상담 흐름을 C++ 샘플 디렉터리에 둔다.
고객과 상담원이 같은 conversation 상태를 공유하고, 상담원 배정, 메시지 순서, typing,
reconnect 뒤 상태 유지, idle 이후 재활성화, close 전이를 확인한다.

이 샘플 디렉터리에서 실행한다.

```bash
./run_sample.sh
```

별도 빌드 디렉터리를 사용할 때만 `ZLINK_CPP_BUILD_DIR`로 configure가 끝난 디렉터리를 지정한다.
runner는 Redis location store endpoint를 준비하고 `Api`, `Session`, `Support`, `Client`
target을 빌드한 뒤 client self-check marker와 role별 message-flow 로그를 검증한다.
