# SupportChat Java 샘플

SupportChat은 고객과 상담원이 하나의 Session 서버 stream endpoint에 연결하고, API 서버와 Support
서버가 인증과 상담 상태를 나누어 처리하는 Java framework 샘플이다.

## 실행

```bash
./run_sample.sh
```

runner는 전용 Redis container와 세 서버 역할을 시작한 뒤 client self-check를 수행한다.
성공하면 client marker와 server evidence marker를 모두 출력한다.
