// SPDX-License-Identifier: MPL-2.0

package native

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"runtime"
	"syscall"
	"testing"
	"time"
)

const requestWritableRegressionRuns = 5

func TestPublicRequestRetriesExactPacketAfterWritable(t *testing.T) {
	for run := 0; run < requestWritableRegressionRuns; run++ {
		t.Run(fmt.Sprintf("run-%d", run), func(t *testing.T) {
			testPublicRequestRetriesExactPacketAfterWritable(t, run)
		})
	}
}

func testPublicRequestRetriesExactPacketAfterWritable(t *testing.T, run int) {
	ctx, err := NewContext()
	if err != nil {
		t.Fatalf("NewContext() error = %v", err)
	}
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	if err := dealer.SetSendHighWaterMark(1); err != nil {
		t.Fatalf("dealer SetSendHighWaterMark(1) error = %v", err)
	}
	if err := router.SetReceiveHighWaterMark(1); err != nil {
		t.Fatalf("router SetReceiveHighWaterMark(1) error = %v", err)
	}

	endpoint := fmt.Sprintf("inproc://go-request-writable-%p-%d", ctx, run)
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}
	// The receive side drives the connect-before-ready WRITABLE if the inproc
	// pipe has not attached yet; the completed exchange is the readiness barrier.
	primeDone := make(chan error, 1)
	go func() { primeDone <- dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background()) }()
	var prime Received
	if ok, err := router.Recv(&prime, RecvFlagsNone); err != nil || !ok {
		t.Fatalf("prime Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	_ = prime.Close()
	if err := <-primeDone; err != nil {
		t.Fatalf("prime Submit() error = %v", err)
	}

	serverPoller, err := NewPoller()
	if err != nil {
		t.Fatalf("server NewPoller() error = %v", err)
	}
	defer serverPoller.Close()
	if err := serverPoller.AddSocket(router, PollIn, 1); err != nil {
		t.Fatalf("server AddSocket(PollIn) error = %v", err)
	}
	clientPoller, err := NewPoller()
	if err != nil {
		t.Fatalf("client NewPoller() error = %v", err)
	}
	defer clientPoller.Close()
	if err := clientPoller.AddSocket(dealer, PollOut|PollCompletion, 2); err != nil {
		t.Fatalf("client AddSocket(PollOut|PollCompletion) error = %v", err)
	}

	knownEntries := make(map[*completionEntry]bool)
	requestPayloads := []string{"request-0"}
	requestResults := []chan requestTestResult{make(chan requestTestResult, 1)}
	go submitTestRequest(dealer, []byte(requestPayloads[0]), requestResults[0])
	firstEntry, firstBackpressured := waitForNewRequestSubmission(t, dealer.socketCore.completion, knownEntries)
	knownEntries[firstEntry] = true
	if firstBackpressured {
		t.Fatal("first request was backpressured before the HWM queue contained a request")
	}
	serverEvents := make([]PollEvent, 1)
	if n, err := serverPoller.Wait(serverEvents, 5*time.Second); err != nil || n != 1 || serverEvents[0].Revents&PollIn == 0 {
		t.Fatalf("first request readiness = (%d, %+v, %v)", n, serverEvents[0], err)
	}

	var entry *completionEntry
	for requestIndex := 1; requestIndex < 32; requestIndex++ {
		payload := fmt.Sprintf("request-%d", requestIndex)
		done := make(chan requestTestResult, 1)
		requestPayloads = append(requestPayloads, payload)
		requestResults = append(requestResults, done)
		go submitTestRequest(dealer, []byte(payload), done)
		candidate, backpressured := waitForNewRequestSubmission(t, dealer.socketCore.completion, knownEntries)
		knownEntries[candidate] = true
		if backpressured {
			entry = candidate
			break
		}
	}
	if entry == nil {
		t.Fatal("request HWM did not produce backpressure within 32 submissions")
	}
	entry.mu.Lock()
	waitToken := entry.completion
	entry.mu.Unlock()
	if waitToken == 0 || entry.request == nil || entry.request.payload == nil {
		t.Fatalf("backpressured request state = (token %d, retry %v)", waitToken, entry.request)
	}
	lastPayload := []byte(requestPayloads[len(requestPayloads)-1])
	if len(entry.request.payload.owned) != 1 || !bytes.Equal(entry.request.payload.owned[0].Data(), lastPayload) {
		t.Fatal("managed request did not retain the exact logical packet")
	}

	clientEvents := make([]PollEvent, 1)
	writableSeen := false
	for requestIndex := 0; requestIndex+1 < len(requestPayloads); requestIndex++ {
		reply := fmt.Sprintf("reply-%d", requestIndex)
		serverReply(t, router, requestPayloads[requestIndex], reply)
		if n, err := clientPoller.Wait(clientEvents, 5*time.Second); err != nil || n != 1 {
			t.Fatalf("reply %d Wait() = (%d, %v)", requestIndex, n, err)
		}
		writableSeen = writableSeen || clientEvents[0].Revents&PollOut != 0
		assertRequestTestResult(t, <-requestResults[requestIndex], reply)
	}
	if !writableSeen {
		t.Fatal("peer drain did not surface the request WRITABLE as POLLOUT")
	}
	lastIndex := len(requestPayloads) - 1
	lastReply := fmt.Sprintf("reply-%d", lastIndex)
	serverReply(t, router, requestPayloads[lastIndex], lastReply)
	if n, err := clientPoller.Wait(clientEvents, 5*time.Second); err != nil || n != 1 || clientEvents[0].Revents&PollCompletion == 0 {
		t.Fatalf("retried reply Wait() = (%d, %+v, %v)", n, clientEvents[0], err)
	}
	assertRequestTestResult(t, <-requestResults[lastIndex], lastReply)
}

func TestPublicRequestConnectBeforeBindUsesWritable(t *testing.T) {
	for run := 0; run < requestWritableRegressionRuns; run++ {
		ctx, _ := NewContext()
		router, _ := ctx.RouterSocket()
		dealer, _ := ctx.DealerSocket()
		endpoint := fmt.Sprintf("inproc://go-request-connect-before-bind-%p-%d", ctx, run)
		if err := dealer.Connect(endpoint); err != nil {
			t.Fatalf("Connect() error = %v", err)
		}
		done := make(chan requestTestResult, 1)
		go submitTestRequest(dealer, []byte("before-bind"), done)
		waitForManagedRequestToken(t, dealer.socketCore.completion)
		if err := router.Bind(endpoint); err != nil {
			t.Fatalf("Bind() error = %v", err)
		}
		serverReply(t, router, "before-bind", "after-bind")
		assertRequestTestResult(t, <-done, "after-bind")
		_ = dealer.Close()
		_ = router.Close()
		_ = ctx.Close()
	}
}

func TestPublicRequestCloseCleansWritableToken(t *testing.T) {
	for run := 0; run < requestWritableRegressionRuns; run++ {
		ctx, _ := NewContext()
		dealer, _ := ctx.DealerSocket()
		endpoint := fmt.Sprintf("inproc://go-request-close-token-%p-%d", ctx, run)
		if err := dealer.Connect(endpoint); err != nil {
			t.Fatalf("Connect() error = %v", err)
		}
		done := make(chan requestTestResult, 1)
		go submitTestRequest(dealer, []byte("close-token"), done)
		waitForManagedRequestToken(t, dealer.socketCore.completion)
		if err := dealer.Close(); err != nil {
			t.Fatalf("Close() error = %v", err)
		}
		result := <-done
		var requestErr *RequestError
		if result.parts != nil || !errors.As(result.err, &requestErr) || requestErr.Result != RequestTerminated {
			t.Fatalf("closed request = (%v, %v), want RequestTerminated", result.parts, result.err)
		}
		if !errors.Is(result.err, syscall.ESHUTDOWN) {
			t.Fatalf("closed request error = %v, want ESHUTDOWN", result.err)
		}
		dealer.socketCore.completion.mu.Lock()
		remaining := len(dealer.socketCore.completion.entries)
		dealer.socketCore.completion.mu.Unlock()
		if remaining != 0 {
			t.Fatalf("completion entries after close = %d, want 0", remaining)
		}
		_ = ctx.Close()
	}
}

func TestPublicRequestAndSendWritableTokensCoexist(t *testing.T) {
	for run := 0; run < requestWritableRegressionRuns; run++ {
		ctx, _ := NewContext()
		router, _ := ctx.RouterSocket()
		dealer, _ := ctx.DealerSocket()
		endpoint := fmt.Sprintf("inproc://go-request-send-mixed-%p-%d", ctx, run)
		if err := dealer.Connect(endpoint); err != nil {
			t.Fatalf("Connect() error = %v", err)
		}

		requestDone := make(chan requestTestResult, 1)
		go submitTestRequest(dealer, []byte("mixed-request"), requestDone)
		sendDone := make(chan error, 1)
		go func() { sendDone <- dealer.Send().Bytes([]byte("mixed-send")).Submit(context.Background()) }()
		requestEntry, requestToken := waitForManagedRequestToken(t, dealer.socketCore.completion)
		sendEntry, sendToken := waitForManagedSendToken(t, dealer.socketCore.completion)
		if requestEntry.handleKey == sendEntry.handleKey || requestToken == sendToken {
			t.Fatalf("mixed tokens alias: request=(%d,%d) send=(%d,%d)", requestEntry.handleKey, requestToken, sendEntry.handleKey, sendToken)
		}

		if err := router.Bind(endpoint); err != nil {
			t.Fatalf("Bind() error = %v", err)
		}
		seenRequest := false
		seenSend := false
		for receivedCount := 0; receivedCount < 2; receivedCount++ {
			var received Received
			if ok, err := router.Recv(&received, RecvFlagsNone); err != nil || !ok {
				t.Fatalf("mixed Recv() = (%v, %v)", ok, err)
			}
			part, err := received.SinglePartOrError()
			if err != nil {
				t.Fatalf("mixed SinglePartOrError() = %v", err)
			}
			switch string(part.Data()) {
			case "mixed-request":
				seenRequest = true
				if err := received.Reply().Message(testMessage(t, "mixed-reply")).Submit(context.Background()); err != nil {
					t.Fatalf("mixed Reply() error = %v", err)
				}
			case "mixed-send":
				seenSend = true
			default:
				t.Fatalf("unexpected mixed payload %q", part.Data())
			}
			_ = received.Close()
		}
		if !seenRequest || !seenSend {
			t.Fatalf("mixed delivery = request:%v send:%v", seenRequest, seenSend)
		}
		assertRequestTestResult(t, <-requestDone, "mixed-reply")
		if err := <-sendDone; err != nil {
			t.Fatalf("mixed send error = %v", err)
		}
		_ = dealer.Close()
		_ = router.Close()
		_ = ctx.Close()
	}
}

type requestTestResult struct {
	parts []*Message
	err   error
}

func submitTestRequest(dealer *DealerSocket, payload []byte, done chan<- requestTestResult) {
	parts, err := dealer.Request().Bytes(payload).Timeout(5 * time.Second).Submit(context.Background())
	done <- requestTestResult{parts: parts, err: err}
}

func waitForManagedRequestToken(t testing.TB, owner *completionOwner) (*completionEntry, uint64) {
	t.Helper()
	for attempt := 0; attempt < 100_000; attempt++ {
		var waiting *completionEntry
		owner.mu.Lock()
		for _, entry := range owner.entries {
			if entry.kind == completionRequest && entry.writableWaiting && entry.request != nil {
				waiting = entry
				break
			}
		}
		if waiting != nil {
			waiting.mu.Lock()
			published := waiting.published
			token := waiting.completion
			waiting.mu.Unlock()
			owner.mu.Unlock()
			if published && token != 0 {
				return waiting, token
			}
			runtime.Gosched()
			continue
		}
		owner.mu.Unlock()
		runtime.Gosched()
	}
	t.Fatal("managed request did not reach a backpressured WRITABLE wait")
	return nil, 0
}

func waitForNewRequestSubmission(
	t testing.TB,
	owner *completionOwner,
	known map[*completionEntry]bool,
) (*completionEntry, bool) {
	t.Helper()
	for attempt := 0; attempt < 100_000; attempt++ {
		var candidate *completionEntry
		owner.mu.Lock()
		for _, entry := range owner.entries {
			if entry.kind != completionRequest || known[entry] {
				continue
			}
			candidate = entry
			break
		}
		owner.mu.Unlock()
		if candidate != nil {
			candidate.mu.Lock()
			published := candidate.published
			backpressured := false
			if published {
				backpressured = candidate.request != nil
			}
			candidate.mu.Unlock()
			if published {
				return candidate, backpressured
			}
		}
		runtime.Gosched()
	}
	t.Fatal("request submission did not publish an admission ID or wait token")
	return nil, false
}

func serverReply(t testing.TB, router *RouterSocket, wantRequest, reply string) {
	t.Helper()
	var received Received
	if ok, err := router.Recv(&received, RecvFlagsNone); err != nil || !ok {
		t.Fatalf("server Recv() = (%v, %v)", ok, err)
	}
	defer received.Close()
	part, err := received.SinglePartOrError()
	if err != nil || string(part.Data()) != wantRequest {
		t.Fatalf("server request = (%q, %v), want %q", partData(part), err, wantRequest)
	}
	if err := received.Reply().Message(testMessage(t, reply)).Submit(context.Background()); err != nil {
		t.Fatalf("server Reply() error = %v", err)
	}
}

func assertRequestTestResult(t testing.TB, result requestTestResult, want string) {
	t.Helper()
	if result.err != nil {
		t.Fatalf("Request() error = %v", result.err)
	}
	defer MultipartClose(result.parts)
	if len(result.parts) != 1 || string(result.parts[0].Data()) != want {
		t.Fatalf("reply = %v, want %q", result.parts, want)
	}
}

func testMessage(t testing.TB, value string) *Message {
	t.Helper()
	message, err := NewMessage([]byte(value))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	return message
}
