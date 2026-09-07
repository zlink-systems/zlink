# Core: 같은 socket에 동시 multipart 제출이 실패한다

> 보고일 2026-09-07 · 보고자 머신 A(bindings perf 캠페인) · 결정 `D-BP12`
> 대상 Core 0.17.1 (태그 `core/v0.17.1`, 커밋 `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`)
> Release+LTO, Build ID `101bdb2411495d6b33aa1a295142d1c780571375`

## 요약

서로 다른 thread가 **각자의 독립된 multipart 메시지**를 **같은 socket**에 동시에 보내면
한쪽이 실패한다. 단일 part이거나 순차 제출이면 문제가 없다.

Core가 record 원자성을 어기지는 않는다 — 조각이 섞여 잘못된 메시지 하나로 나가지 않고
감지해서 실패시킨다. 문제는 원자성이 아니라 **동시 조립을 지원하지 않는다는 것**이며,
그 상황이 계약에 정의돼 있지 않다.

## 재현

DEALER requester 1개, ROUTER replier 1개, 조건만 바꿔 3회 실행했다.

| parts | 동시 호출자 | 결과 |
|-------|-------------|------|
| 1 | 4 | 성공 4, 실패 0 |
| **2** | **4** | 성공 3, **실패 1** — `submit_error result=1 errno=11 eagain=true einval=false` |
| 2 | 1 | 성공 1, 실패 0 |

요청이 4건뿐인 새 socket이라 HWM backpressure일 수 없다. 이 `EAGAIN`은 혼잡이 아니라
충돌의 결과이며, 아래 계약의 "중간 실패는 staging한 prefix와 실패한 part를 모두 폐기한다"가
그대로 나타난 것이다. 반복 실행에서 `EINVAL`(`ZLINK_SUBMIT_INVALID_ARGUMENT`)도 관측됐다.

실행 방법은 §재현 프로그램 참조.

## 원인

`core/doc/spec/core/socket/README.ko.md:944`

> PAIR·DEALER·ROUTER에서 `MORE`는 **socket-local sequence**에 part를 staging하고 `FINAL`이
> 성공해야 record 하나로 admission한다. 같은 sequence의 함수 family, target과 flags는 같아야
> 한다. 중간 실패는 staging한 prefix와 실패한 part를 모두 폐기한다.

- **단일 part**: `FINAL` 한 번으로 즉시 admission된다. 소켓에 남는 중간 상태가 없어 동시
  호출이 서로 볼 것이 없다.
- **multipart**: `MORE`와 `FINAL` 사이에 소켓이 조립 상태를 들고 있다. 조립 슬롯이
  **소켓당 하나**이므로 두 번째 동시 호출자가 그 하나를 침범한다.

## 계약 공백

두 조항이 multipart에서 양립하지 않는다.

| 위치 | 내용 |
|------|------|
| `core/doc/spec/core/socket/README.ko.md:49` | "`send`는 **여러 thread에서 동시 호출을 허용**하는 hot path다" |
| 같은 문서 `:944` | 조립 슬롯이 **socket-local**이다 |

스펙은 "한 메시지의 part를 여러 thread가 나눠 보내는 것"만 금지하고, **"서로 다른 thread가
각자의 독립된 multipart 메시지를 같은 socket에 동시에 보내는 경우"를 정의하지 않았다.**

binding 쪽에서 막을 수도 없다 — `bindings/doc/spec/README.ko.md:1345`:

> Part 단위 Core API를 사용하는 binding은 **송신 경로에 자체 lock이나 gate를 두지 않는다.**
> Multipart 원자성·part 소비·**동시 제출 결과는 Core part send가 소유한다.**

## 영향

**Go에서 특히 필연적이다.** Go의 공개 request terminal은 `Submit(context.Context)` 하나뿐이고
reply까지 블로킹한다(`bindings/doc/spec/async-coroutine-policy.ko.md` §6). Go 관례(블로킹 함수 +
goroutine + `context.Context`)대로 동시성을 얻으려면 goroutine을 여러 개 띄울 수밖에 없고,
그 순간 제출이 동시가 된다. 즉 **관례를 따르면 반드시 이 상황에 들어간다.**

다른 언어는 awaitable terminal이 즉시 반환해 한 thread가 제출을 연달아 하고 대기만 겹치므로
조립이 자연히 직렬화된다. 잠복해 있을 뿐이며, **사용자가 여러 thread에서 multipart를 보내면
같은 문제를 겪는다.**

perf 영향: 공식 wire shape가 2-part로 고정돼 있고(`doc/perf/PERF_POLICY.md:339-341`,
`PERF_PART_COUNT=1`은 진단 전용이며 2-part baseline과 섞어 비교 금지), in-flight 1 직렬화는
정책 위반이므로 **Go는 REQREP에서 유효한 공식 측정을 낼 수 없다.** 해당 셀은 스펙 결정 전까지
보류한다.

## 요청

"원자성을 지켜라"가 아니라 **동시 조립을 지원하거나, 미지원임을 계약으로 명시하고 그 경우
binding의 대응을 정의해 달라**는 것이다.

1. **지원한다면** — 조립 슬롯을 제출자 단위로 분리하거나 `MORE`~`FINAL` 구간만 Core 내부에서
   직렬화한다. 후자는 뒤에 온 호출자가 끼어들지 않고 기다리게 된다.
2. **미지원으로 확정한다면** — `core/.../socket/README.ko.md:49`의 "동시 호출 허용"에 multipart
   예외를 명시하고, `bindings/doc/spec/README.ko.md:1345`의 송신 경로 lock 금지를 함께 완화해야
   binding이 직렬화할 수 있다. Go는 그 경우에도 관례를 유지할 수 있는지 별도 검토가 필요하다.

## 부차 항목 (Go binding)

Go binding이 이 `BACKPRESSURED`를 terminal 오류로 caller에게 노출한다. 계약상
(`bindings/go/contracts/sockets.go:43-45`) 대기 토큰의 `WRITABLE`에서 내부 재개해야 한다.
Core가 해결되면 대부분 사라지므로 후속으로 둔다.

## 재현 프로그램

`/tmp` 밖에 보존하기 위해 전문을 싣는다. 임의의 빈 디렉터리에 `main.go`로 저장하고 아래
`go.mod`와 함께 실행한다.

```text
# go.mod
module zlrepro

go 1.22

require zlink.systems/zlink v0.0.0
replace zlink.systems/zlink => /home/hep7/project/zlink/bindings/go
```

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
export LD_LIBRARY_PATH="$ZLINK_CORE_PACKAGE_PREFIX/lib"
go run . -parts 1 -callers 4   # 성공 4 / 실패 0
go run . -parts 2 -callers 4   # 실패 발생
go run . -parts 2 -callers 1   # 성공 1 / 실패 0
```

```go
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"net"
	"runtime"
	"sync"
	"syscall"
	"time"

	zlink "zlink.systems/zlink"
)

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func endpoint() string {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	must(err)
	address := listener.Addr().String()
	must(listener.Close())
	return "tcp://" + address
}

func waitReady(monitor *zlink.SocketMonitor) {
	ready := make(chan error, 1)
	go func() {
		for {
			event, err := monitor.Recv(zlink.RecvFlagsNone)
			if err != nil {
				ready <- err
				return
			}
			if event.IsConnectionReady() {
				ready <- nil
				return
			}
		}
	}()
	select {
	case err := <-ready:
		must(err)
	case <-time.After(5 * time.Second):
		panic("connection-ready timeout")
	}
}

func main() {
	partCount := flag.Int("parts", 2, "request part count (1 or 2)")
	callers := flag.Int("callers", 2, "concurrent Submit callers")
	rounds := flag.Int("rounds", 1, "requests per caller")
	flag.Parse()
	if *partCount != 1 && *partCount != 2 {
		panic("parts must be 1 or 2")
	}

	ctx, err := zlink.NewContext()
	must(err)
	defer ctx.Close()
	router, err := ctx.RouterSocket()
	must(err)
	defer router.Close()
	dealer, err := ctx.DealerSocket()
	must(err)
	defer dealer.Close()
	monitor, err := zlink.OpenSocketMonitor(dealer, zlink.MonitorEventConnectionReady)
	must(err)
	defer monitor.Close()

	address := endpoint()
	must(router.Bind(address))
	must(dealer.Connect(address))
	waitReady(monitor)

	serverDone := make(chan error, 1)
	go func() {
		for i := 0; i < *callers**rounds; i++ {
			var received zlink.Received
			if _, err := router.Recv(&received, zlink.RecvFlagsNone); err != nil {
				serverDone <- err
				return
			}
			reply, err := zlink.NewMessage([]byte("ok"))
			if err == nil {
				err = received.Reply().Message(reply).Submit(context.Background())
			}
			_ = received.Close()
			_ = reply.Close()
			if err != nil {
				serverDone <- err
				return
			}
		}
		serverDone <- nil
	}()

	start := make(chan struct{})
	results := make(chan error, *callers**rounds)
	var workers sync.WaitGroup
	workers.Add(*callers)
	for i := 0; i < *callers; i++ {
		go func(index int) {
			defer workers.Done()
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()
			<-start
			for round := 0; round < *rounds; round++ {
				submit := dealer.Request().Bytes([]byte(fmt.Sprintf("request-%d-%d", index, round)))
				if *partCount == 2 {
					submit = submit.Bytes(nil)
				}
				parts, err := submit.Timeout(2 * time.Second).Submit(context.Background())
				zlink.MultipartClose(parts)
				results <- err
			}
		}(i)
	}
	close(start)
	workers.Wait()
	close(results)

	succeeded := 0
	for err := range results {
		if err == nil {
			succeeded++
			continue
		}
		var submitErr *zlink.SubmitError
		if errors.As(err, &submitErr) {
			fmt.Printf("submit_error result=%v errno=%v eagain=%t einval=%t\n",
				submitErr.Result, submitErr.InternalErrno(),
				errors.Is(err, syscall.EAGAIN), errors.Is(err, syscall.EINVAL))
			continue
		}
		fmt.Printf("request_error=%T %v\n", err, err)
	}
	total := *callers * *rounds
	fmt.Printf("summary parts=%d callers=%d rounds=%d succeeded=%d failed=%d\n",
		*partCount, *callers, *rounds, succeeded, total-succeeded)

	if succeeded == total {
		must(<-serverDone)
	}
}
```
