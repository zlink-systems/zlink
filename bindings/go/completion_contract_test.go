package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestReplyTokenRejectsZeroAndDifferentRouterOwner(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	router, _ := ctx.RouterSocket()
	other, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer other.Close()
	defer dealer.Close()

	endpoint := inprocEndpoint("reply-token-owner")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	serverDone := make(chan error, 1)
	go func() {
		var received zlink.Received
		if _, err := router.Recv(&received, zlink.RecvFlagsNone); err != nil {
			serverDone <- err
			return
		}
		defer received.Close()
		token, ok := received.ReplyToken()
		if !ok {
			serverDone <- fmt.Errorf("request did not expose a reply token")
			return
		}
		if token == (zlink.ReplyToken{}) {
			serverDone <- fmt.Errorf("valid token equals its zero value")
			return
		}
		again, _ := received.ReplyToken()
		if token != again || map[zlink.ReplyToken]bool{token: true}[again] != true {
			serverDone <- fmt.Errorf("reply token equality or map hashing is unstable")
			return
		}
		var submitErr *zlink.SubmitError
		err := other.Reply(received.RoutingID(), token).Message(newMessage(t, "wrong-owner")).Submit(context.Background())
		if !errors.As(err, &submitErr) || submitErr.Result != zlink.SubmitInvalidArgument {
			serverDone <- fmt.Errorf("different-owner reply error = %v", err)
			return
		}
		err = router.Reply(received.RoutingID(), zlink.ReplyToken{}).Message(newMessage(t, "zero-token")).Submit(context.Background())
		if !errors.As(err, &submitErr) || submitErr.Result != zlink.SubmitInvalidArgument {
			serverDone <- fmt.Errorf("zero-token reply error = %v", err)
			return
		}
		serverDone <- router.Reply(received.RoutingID(), token).Message(newMessage(t, "ok")).Submit(context.Background())
	}()

	parts, err := dealer.Request().Bytes([]byte("request")).Timeout(2 * time.Second).Submit(context.Background())
	if err != nil {
		t.Fatalf("Request() error = %v", err)
	}
	defer zlink.MultipartClose(parts)
	if len(parts) != 1 || string(parts[0].Data()) != "ok" {
		t.Fatalf("reply parts = %d, want ok", len(parts))
	}
	if err := <-serverDone; err != nil {
		t.Fatalf("server error = %v", err)
	}
}

func TestCompletionCanJoinBeforeSubmitReturns(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	server, _ := ctx.PairSocket()
	client, _ := ctx.PairSocket()
	defer server.Close()
	defer client.Close()
	endpoint := inprocEndpoint("completion-join")
	if err := server.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := client.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	const count = 128
	recvDone := make(chan error, 1)
	go func() {
		for i := 0; i < count; i++ {
			var received zlink.Received
			if _, err := server.Recv(&received, zlink.RecvFlagsNone); err != nil {
				recvDone <- err
				return
			}
			_ = received.Close()
		}
		recvDone <- nil
	}()
	for i := 0; i < count; i++ {
		if err := client.Send().Bytes([]byte{byte(i)}).Submit(context.Background()); err != nil {
			t.Fatalf("Submit(%d) error = %v", i, err)
		}
	}
	if err := <-recvDone; err != nil {
		t.Fatalf("receive loop error = %v", err)
	}
}

func TestCanceledRequestLateResultIsCleanedAndOwnerContinues(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	endpoint := inprocEndpoint("late-request-cleanup")
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("Connect() error = %v", err)
	}

	serverDone := make(chan error, 1)
	go func() {
		for i := 0; i < 2; i++ {
			var received zlink.Received
			if _, err := router.Recv(&received, zlink.RecvFlagsNone); err != nil {
				serverDone <- err
				return
			}
			if i == 0 {
				time.Sleep(40 * time.Millisecond)
			}
			err := received.Reply().Message(newMessage(t, fmt.Sprintf("reply-%d", i))).Submit(context.Background())
			_ = received.Close()
			if err != nil {
				serverDone <- err
				return
			}
		}
		serverDone <- nil
	}()

	waitCtx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	parts, err := dealer.Request().Bytes([]byte("cancelled")).Timeout(time.Second).Submit(waitCtx)
	if parts != nil || !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("cancelled request = (%v, %v), want (nil, deadline exceeded)", parts, err)
	}
	parts, err = dealer.Request().Bytes([]byte("next")).Timeout(time.Second).Submit(context.Background())
	if err != nil {
		t.Fatalf("request after late result error = %v", err)
	}
	defer zlink.MultipartClose(parts)
	if len(parts) != 1 || string(parts[0].Data()) != "reply-1" {
		t.Fatalf("second reply = %v", parts)
	}
	if err := <-serverDone; err != nil {
		t.Fatalf("server error = %v", err)
	}
}
