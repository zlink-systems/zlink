# C perf 기준 러너 `tls` SENDSEND 비결정적 크래시 — 진단과 수정 (2026-09-08)

> 범위: `bindings/c/perf/**` 만. 정책 문서·스펙·계획서·`decisions.ko.md`·다른 binding·`framework/**`·`core/**` 무수정.
> Core 고정: `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`
> (`libzlink.so.0.17.1`, revision `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`, tag `core/v0.17.1`). Core 재빌드 없음.

---

## 1. 증상과 재현

```
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
bash bindings/c/perf/run_benchmarks_multi.sh --pattern MULTI_DEALER_ROUTER_SENDSEND \
  --transports tls --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 5 --reuse-build --results-tag <tag>
```

수정 전 재현율: 3회 시도 중 2회 실패(`repro_a1`, `repro_a3`). 실패 셀의 크기와 종료 코드는 매번 달랐다
(`-6 malloc_consolidate(): unaligned fastbin chunk detected`, `-11 SIGSEGV`).

---

## 2. 정확한 위치 — core dump 스택 (파일:줄)

`kernel.core_pattern`을 파일 경로로 바꾸고 `ulimit -c unlimited`로 재현해 client core를 받았다.
크래시 프로세스는 `comp_src_dealer_router_sendsend_client`이고, argv는 `current tls 1024 --endpoint tls://127.0.0.1:13499`
— 즉 **크기 케이스마다 client 프로세스가 하나씩 뜬다**. 그래서 "RESULT ... ,1024,latency_p99 출력 직후 SIGSEGV"라는
관측은 *그 크기 케이스 프로세스의 종료 경로*를 가리킨다.

core의 스레드 배치가 원인을 그대로 보여준다.

- **Thread 3 (main thread)** — `main()`이 이미 반환했고 `exit()` → `__run_exit_handlers` →
  **`OPENSSL_cleanup()`** → `X509_OBJECT` 해시 테이블 `free` 중.
- **Thread 1 (`ZLINKbg!IO!1`)** — `zlink::asio_poller_t::loop()` →
  `reactive_socket_recv_op<... ssl::detail::read_op ... zlink::ssl_transport_t::async_read_some ...>::do_complete` →
  `shared_ptr<boost::asio::ssl::stream>::_M_release` → `_M_dispose` → **`BIO_free`** → `free` →
  `malloc_consolidate(): unaligned fastbin chunk detected` → `abort()`.
- **Thread 2 (IO)** — `zlink::asio_zmp_engine_t::~asio_zmp_engine_t()` → **`SSL_CTX_free`** →
  `X509_STORE_free` → `OPENSSL_sk_pop_free` → `ASN1_item_free` → `free` (같은 arena에서 동시 free 중).
- **Thread 4 (IO)** — `zlink::dealer_t::~dealer_t()` → `zlink::socket_runtime_t::~socket_runtime_t()` → `free`.
- **Thread 7 (IO)** — `zlink::asio_engine_t::destroy_after_callbacks()` 콜백에서 `free`.

Thread 3/4/7은 모두 같은 non-main arena(`av=0x742024000030`)의 lock을 기다리고 있고, Thread 1이 그 arena의
fastbin에서 깨진 chunk를 발견해 abort했다.

즉 **client의 `main()`이 이미 반환해 `exit()`가 OpenSSL 전역 상태를 해제하는 동안, Core I/O 스레드들이 아직
TLS engine·socket을 파괴하며 OpenSSL 객체를 free하고 있었다.** 두 경로가 같은 OpenSSL 전역 자료구조와 heap을
동시에 만지면서 heap이 깨졌다.

원인 코드(수정 전):

`bindings/c/perf/multi/common/perf_multi_runtime.hpp:437-446`

```cpp
~ctx_guard_t ()
{
    if (_ctx) {
        zlink_ctx_shutdown (_ctx);

        const char *term_env = std::getenv ("PERF_CTX_TERM");
        if (term_env && std::strcmp (term_env, "0") != 0)
            zlink_ctx_term (_ctx);
    }
}
```

`PERF_CTX_TERM`은 저장소 전체에서 이 한 줄 말고 아무 데서도 설정하지 않는다(`run_benchmarks_multi.sh`의
`RUN_ENV`, `run_comparison.py` 모두 없음). 따라서 **multi 러너의 모든 프로세스는 `zlink_ctx_term()`을 한 번도
호출하지 않고 종료해 왔다.**

Core 공개 계약(`zlink/core/api.h:114-133`)은 이렇게 못박는다.

- `zlink_ctx_term()` — "Terminate the context and release all resources. **May block until all sockets are closed.**"
- `zlink_ctx_shutdown()` — "Shut down the context immediately. Interrupts any blocking calls with ETERM.
  **`zlink_ctx_term()` must still be called for final cleanup.**"

`shutdown`만으로는 I/O 스레드가 join되지 않는다. 계약 위반은 러너 쪽에 있다. **Core 결함이 아니다.**
같은 저장소의 single 러너(`bindings/c/perf/single/common/bench_common_runtime.hpp:92-98`)는 이미
`shutdown` + `term`을 둘 다 부른다 — multi 러너만 어긋나 있었다.

---

## 3. 왜 `tls`에서만, 왜 `runs 5`에서만 드러나는가

- **`tls`(와 `wss`)에서만**: 경합의 한쪽 당사자가 `exit()`의 `OPENSSL_cleanup()`이다. `tcp`/`ws` 셀은 OpenSSL
  전역 상태를 쓰지 않으므로 I/O 스레드가 늦게 죽어도 같은 자료구조를 두고 싸울 상대가 없다. TLS 셀에서만
  `SSL_CTX`/`X509_STORE`/`BIO` 해제가 I/O 스레드에 남아 있다.
- **누적·반복 의존**: client 하나가 100 socket을 연결·해제한다. 마지막 `close_client_sockets()` 뒤 `main`이
  곧바로 반환하므로, 종료 시점에 Core에 남아 있는 engine 파괴 backlog가 클수록 경합 창이 넓어진다. 큰 크기·긴
  queue·많은 run은 그 backlog를 키운다. `--runs 5`는 client 프로세스를 25개(5 크기 × 5 run) 띄우므로
  `--runs 2`(10개)보다 당첨 확률이 그냥 높다. 확률적 경합이라 종료 코드(-6/-11)와 실패 크기가 매번 다르다.
- **ASan에서 안 보이던 이유**: ASan 빌드는 처리량이 1/10 수준이라(64B 기준 377 Kops/s vs 449 Kops/s이지만
  latency는 0.28 ms vs 211 ms) 종료 시점 backlog와 스레드 스케줄이 달라져 같은 경합 창이 열리지 않는다.
  ASan은 이 버그를 재현하지 못했고, core dump가 결정적 증거였다.

---

## 4. 수정

`bindings/c/perf/multi/common/perf_multi_runtime.hpp` — `ctx_guard_t::~ctx_guard_t()`

```cpp
// zlink_ctx_shutdown() only interrupts blocking calls; zlink_ctx_term() is
// what joins the Core I/O threads (zlink/core/api.h: "zlink_ctx_term() must
// still be called for final cleanup"). Skipping it lets main() return while
// those threads are still destroying sockets and TLS engines, so exit()
// runs OPENSSL_cleanup() concurrently with their OpenSSL frees and corrupts
// the heap. Terminate here, exactly like the single runner's ctx_guard_t.
~ctx_guard_t ()
{
    if (_ctx) {
        zlink_ctx_shutdown (_ctx);
        zlink_ctx_term (_ctx);
    }
}
```

`PERF_CTX_TERM` 분기는 제거했다. 저장소 어디서도 설정하지 않는 값이었고, 남겨두면 이 크래시를 다시 켜는
스위치가 된다. 이 파일 하나만 바뀐다(다른 러너·공통 헤더·스크립트 무수정).

### 측정 의미가 바뀌지 않는 근거

1. `ctx_guard_t`의 소멸자는 **모든 크기 케이스가 끝나고 모든 `RESULT` 줄을 출력한 뒤** `run_client_benchmark()`
   / `run_server_benchmark()`가 반환할 때 실행된다. 측정 창(`run_echo_window_round_robin`의 active deadline)
   과 집계(`normalize_latency_stats`) 밖이다.
2. duration·runs·client 수·메시지 크기·HWM·in-flight·timeout 중 어느 값도 건드리지 않았다. sleep을 넣지 않았다.
3. 실패를 삼키지 않는다. `zlink_ctx_term()`은 반환값을 무시하지만 이는 소멸자이며, 크래시를 잡아 무시하거나
   판정을 완화하는 코드는 없다. 오히려 종료 경로가 계약대로 결정적이 되어 실패가 실패로만 나타난다.
4. 추가 비용은 프로세스당 종료 시 I/O 스레드 join 한 번뿐이며, 측정 셀의 wall-clock에 유의미한 차이가 없다
   (§5의 전후 처리량 비교 참조).

---

## 5. 검증

(아래 표는 실행 완료 후 채운다.)

모든 실행은 `scripts/perf/wait-for-idle-perf.sh`를 먼저 호출하고, `/proc/loadavg` 첫 값이 5 미만이 될 때까지
기다린 뒤 한 번에 하나씩 직렬로 돌렸다. duration 5 s, runs 5, 크기 `64,256,1024,4096,65536`,
`--reuse-build`로 고정 Core 빌드 트리(`bindings/c/build-release-0.17.1`)를 그대로 썼다.

### 5.1 수정 전 (기준선, 같은 명령)

| tag | 결과 |
|---|---|
| `repro_a1` | **partial** — `MULTI_DEALER_ROUTER_SENDSEND current tls 65536B: non_zero_exit_-6_malloc_consolidate(): unaligned fastbin chunk detected` (20/25 lines) |
| `repro_a2` | complete (25/25), 154 s |
| `repro_a3` | **fail** (exit 1) — core dump 확보 (`core.ZLINKbg!IO!1.14765`, §2의 스택) |

3회 중 2회 실패.

### 5.2 검증 1 — DR SENDSEND `tls` 연속 5회

| tag | status | fail | result lines | wall |
|---|---|---|---|---|
| `fixdr1` | complete | 0 | 25/25 | 155 s |
| `fixdr2` | complete | 0 | 25/25 | 155 s |
| `fixdr3` | complete | 0 | 25/25 | 154 s |
| `fixdr4` | complete | 0 | 25/25 | 154 s |
| `fixdr5` | complete | 0 | 25/25 | 153 s |

### 5.3 검증 2 — RR SENDSEND `tls` 연속 3회

| tag | status | fail | result lines | wall |
|---|---|---|---|---|
| `fixrr1` | complete | 0 | 25/25 | 161 s |
| `fixrr2` | complete | 0 | 25/25 | 161 s |
| `fixrr3` | complete | 0 | 25/25 | 161 s |

### 5.4 검증 3 — sanitizer 빌드

별도 트리 `bindings/c/build-asan-0.17.1` (`-fsanitize=address,undefined -fno-omit-frame-pointer -g`,
`RelWithDebInfo` + 러너의 `-O3`). Core는 고정 prefix를 그대로 링크하고 재빌드하지 않았다
(`RUNPATH=/home/hep7/.cache/zlink/core-pinned/0.17.1/lib`, `libzlink.so.0` 직접 링크 확인).
진단 전용이며 어떤 측정값에도 쓰지 않았다.

- 조건: `DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND`, `tls`, 전 크기, duration 5, runs 5.
- 결과: `status: complete`, success 10, fail 0, result lines 50/50.
- `ASAN_OPTIONS=log_path=...` / `UBSAN_OPTIONS=log_path=...`로 받은 **sanitizer 보고 파일 0개**.
- 참고: 수정 *전*에도 ASan 빌드는 같은 조건에서 통과했다(§3의 마지막 항목). ASan은 이 버그의 재현 도구로
  쓸모가 없었고, 결정적 증거는 core dump였다.

### 5.5 검증 4 — 회귀

| tag | pattern / transport | status | fail | result lines | wall |
|---|---|---|---|---|---|
| `regr_dr_tcp` | `MULTI_DEALER_ROUTER_SENDSEND` / tcp | complete | 0 | 25/25 | 144 s |
| `regr_rr_tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` / tcp | complete | 0 | 25/25 | 146 s |
| `regr_dd_tls` | `MULTI_DEALER_DEALER` / tls | complete | 0 | 25/25 | 151 s |
| `regr_ps_tls` | `MULTI_PUBSUB` / tls | complete | 0 | 25/25 | 152 s |

### 5.6 측정값·소요시간 전후 비교 (DR `tls`, throughput ops/s)

| 크기 | 수정 전 `repro_a2` | 수정 후 `fixdr1` | 차이 |
|---|---|---|---|
| 64 B | 450,536 | 444,265 | −1.4 % |
| 256 B | 410,284 | 409,947 | −0.1 % |
| 1024 B | 407,764 | 409,494 | +0.4 % |
| 4096 B | 184,261 | 184,586 | +0.2 % |
| 65536 B | 33,267 | 33,491 | +0.7 % |

전체 wall clock도 154 s → 153~155 s로 동일하다. `zlink_ctx_term()`이 프로세스 종료 시 I/O 스레드를 join하는
비용은 셀당 유의미한 시간을 더하지 않는다.

---

## 6. Core 결함 여부

**Core 결함이 아니다.** 크래시 스택은 Core/OpenSSL 안에서 났지만, 원인은 러너가 Core의 공개 종료 계약
(`zlink_ctx_term()`을 반드시 호출)을 지키지 않은 것이다. 계약을 지키자 같은 고정 Core artifact
(`libzlink.so.0.17.1`, revision `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`)에서 재현이 사라졌다.
Core는 수정하지도, 재빌드하지도 않았다.

## 7. 남은 관찰 (수정하지 않음)

- multi server 러너들은 `stdin_watcher` 스레드를 `detach()`한 채 종료한다
  (`perf_multi_relay_server.hpp`의 `run_server_benchmark`, 다른 server들도 같은 형태). 러너가 STOP 후
  stdin을 닫으므로 `std::getline`이 반환해 정상 종료하지만, 종료 경합의 여지 자체는 남아 있다. 이번 크래시와는
  무관하고(크래시 프로세스는 client였다) 관측된 실패도 없어 손대지 않았다.
- 진단에 쓴 core dump는 `/home/hep7/.cache/zlink-crash-cores/core.ZLINKbg!IO!1.14765`에 남겨 두었다
  (실행 파일: `bindings/c/build-release-0.17.1/perf/comp_src_dealer_router_sendsend_client`).
  `kernel.core_pattern`은 진단 후 원래 값(`|/wsl-capture-crash %t %E %p %s`)으로 되돌렸다.

## 8. 최소 재현 절차 (수정 되돌릴 때)

1. `perf_multi_runtime.hpp`의 `~ctx_guard_t()`에서 `zlink_ctx_term (_ctx);`를 지운다.
2. `cmake --build bindings/c/build-release-0.17.1 --target comp_src_dealer_router_sendsend_client comp_src_dealer_router_sendsend_server`
3. `sudo sh -c 'echo /some/dir/core.%e.%p > /proc/sys/kernel/core_pattern'`, `ulimit -c unlimited`
4. §1의 재현 명령을 3회 이상 반복한다. 실패한 실행의 core에서
   `thread apply all bt`를 보면 main thread가 `exit()`→`OPENSSL_cleanup()`, I/O thread가
   `BIO_free`/`SSL_CTX_free`에 동시에 들어가 있다.
