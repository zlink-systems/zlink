# #4244 Core 테스트 임시 포트 보고서

- 커밋: `4892683445da95a898de5480c82687a3d80cd24e` (`test: use ephemeral ports in core integration tests`), 로컬 커밋만 완료.
- 변경 파일: `core/tests/testutil.hpp`에서 PID offset, 환경변수 처리, 고정 endpoint와 port 매크로를 삭제; `core/tests/testutil.cpp`의 BSD 소켓 테스트 포트를 0으로 바꾸고 실제 bind port를 `getsockname`으로 읽음; `test_reconnect_options.cpp`, `test_connect_rid_alias_binding.cpp`, `test_bind_after_connect_tcp.cpp`, `test_last_endpoint.cpp`에서 wildcard bind와 관측 endpoint를 사용.
- helper/macro 규칙 수: 제거 6개 함수(`test_port_offset`, `test_port`, `endpoint_0..3`)와 5개 매크로(`ENDPOINT_0..3`, `PORT_6`); 추가 0개. 기존 `test_bind`를 재사용.

## 점유 재현

명령: 아래 Python driver가 TCP listener를 `127.0.0.1:5556`에 열어 둔 채 환경변수 `ZLINK_TEST_PORT_OFFSET=0`으로 테스트를 실행했습니다.

```sh
python3 -c 'import os,socket,subprocess; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(("127.0.0.1",5556)); s.listen(); e=os.environ.copy(); e["ZLINK_TEST_PORT_OFFSET"]="0"; p=subprocess.run(["build-p4244/bin/test_connect_rid_alias_binding"],env=e); s.close(); raise SystemExit(p.returncode)'
```


```sh
ZLINK_TEST_PORT_OFFSET=0 build-p4244/bin/test_connect_rid_alias_binding
```

변경 전 출력:

```text
EXIT: 2
...test_connect_rid_alias_binding.cpp:87:alias_survives_reconnect_directed_route:FAIL: zlink_bind (server, ENDPOINT_1) failed, bind = 502, errno = 98 (Address already in use)
...test_connect_rid_alias_binding.cpp:136:back_to_back_aliases_bind_distinct_routes:FAIL: zlink_bind (server_a, ENDPOINT_1) failed, bind = 502, errno = 98 (Address already in use)
2 Tests 2 Failures 0 Ignored
FAIL
```

변경 후 같은 listener·명령:

```text
EXIT: 0
...test_connect_rid_alias_binding.cpp:206:alias_survives_reconnect_directed_route:PASS
...test_connect_rid_alias_binding.cpp:207:back_to_back_aliases_bind_distinct_routes:PASS
2 Tests 0 Failures 0 Ignored
OK
```

## 검증

- Release 빌드: 전체 Core 빌드 완료 (`-j3`, `cpp-build-slot.sh` 사용).
- 반복: `test_connect_rid_alias_binding`, `test_reconnect_options`, `test_bind_after_connect_tcp`, `test_last_endpoint` 각각 20/20 통과.
- 전체 Core ctest: `ctest --test-dir build-p4244 -j4 --output-on-failure`; 232개 중 231 통과, 1 실패. `test_socket_disconnect_progress_without_app_poll`의 무대기 TCP disconnect 측정이 207ms로 200ms 상한을 넘어 timeout 처리됨. 포트 변경과 무관한 타이밍 실패로 원문 로그를 보존했고 재실행하지 않음.
- 형식: `bash scripts/format/format.sh --check cpp` 통과.
- 빌드 디렉터리 `build-p4244` 제거 완료. 원시 로그는 `/root/jobs/fx/cx/v21/s033/p4244/logs/`에 보존.
- 남은 문제: 위 ctest 타이밍 실패 1건.
