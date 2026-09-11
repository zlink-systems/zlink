package zlink_test

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

const submissionContractRuns = 5

func TestRequestSubmissionImmediateAdmissionContract(t *testing.T) {
	for run := 0; run < submissionContractRuns; run++ {
		t.Run(fmt.Sprintf("run-%d", run), testRequestSubmissionImmediateAdmission)
	}
}

func testRequestSubmissionImmediateAdmission(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	endpoint := inprocEndpoint("request-submission-ok")
	if err := router.Bind(endpoint); err != nil {
		t.Fatal(err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatal(err)
	}
	primeSubmission, err := dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var prime zlink.Received
	if ok, err := router.Recv(&prime, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("prime Recv() = (%v, %v)", ok, err)
	}
	_ = prime.Close()
	if err := primeSubmission.Admitted(context.Background()); err != nil {
		t.Fatal(err)
	}

	received := make(chan *zlink.Received, 1)
	serverDone := make(chan error, 1)
	allowReply := make(chan struct{})
	go func() {
		request := &zlink.Received{}
		if ok, err := router.Recv(request, zlink.RecvFlagsNone); err != nil || !ok {
			serverDone <- fmt.Errorf("request Recv() = (%v, %v)", ok, err)
			return
		}
		received <- request
		<-allowReply
		reply := newMessage(t, "ok-reply")
		err := request.Reply().Message(reply).Submit(context.Background())
		_ = request.Close()
		serverDone <- err
	}()

	submission, err := dealer.Request().Bytes([]byte("ok-request")).Timeout(5 * time.Second).Submit(context.Background())
	if err != nil {
		t.Fatalf("Submit() error = %v", err)
	}
	if submission.Result() != zlink.SubmitOK {
		t.Fatalf("Result() = %v, want SubmitOK", submission.Result())
	}
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	if err := submission.Admitted(canceled); err != nil {
		t.Fatalf("completed Admitted() error = %v", err)
	}
	select {
	case <-received:
	case err := <-serverDone:
		t.Fatal(err)
	case <-time.After(5 * time.Second):
		t.Fatal("server did not receive immediate request")
	}
	if parts, err := submission.Reply(canceled); parts != nil || !errors.Is(err, context.Canceled) {
		t.Fatalf("Reply() before server reply = (%v, %v)", parts, err)
	}
	close(allowReply)
	parts, err := submission.Reply(context.Background())
	if err != nil {
		t.Fatalf("Reply() error = %v", err)
	}
	defer zlink.MultipartClose(parts)
	if len(parts) != 1 || string(parts[0].Data()) != "ok-reply" {
		t.Fatalf("Reply() parts = %v", parts)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}

func TestRequestSubmissionBackpressuredAdmissionContract(t *testing.T) {
	for run := 0; run < submissionContractRuns; run++ {
		t.Run(fmt.Sprintf("run-%d", run), testRequestSubmissionBackpressuredAdmission)
	}
}

func testRequestSubmissionBackpressuredAdmission(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatal(err)
	}
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	if err := dealer.SetSendHighWaterMark(1); err != nil {
		t.Fatal(err)
	}
	if err := router.SetReceiveHighWaterMark(1); err != nil {
		t.Fatal(err)
	}
	endpoint := inprocEndpoint("request-submission-backpressured")
	if err := router.Bind(endpoint); err != nil {
		t.Fatal(err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatal(err)
	}
	primeSubmission, err := dealer.Send().Bytes([]byte("route-prime")).Submit(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var prime zlink.Received
	if ok, err := router.Recv(&prime, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("prime Recv() = (%v, %v)", ok, err)
	}
	_ = prime.Close()
	if err := primeSubmission.Admitted(context.Background()); err != nil {
		t.Fatal(err)
	}

	var submissions []zlink.RequestSubmission
	var backpressured zlink.RequestSubmission
	for index := 0; index < 32; index++ {
		submission, err := dealer.Request().Bytes([]byte(fmt.Sprintf("request-%d", index))).Timeout(5 * time.Second).Submit(context.Background())
		if err != nil {
			t.Fatalf("Submit(%d) error = %v", index, err)
		}
		submissions = append(submissions, submission)
		if submission.Result() == zlink.SubmitBackpressured {
			backpressured = submission
			break
		}
		if submission.Result() != zlink.SubmitOK {
			t.Fatalf("Submit(%d) result = %v", index, submission.Result())
		}
	}
	if backpressured == nil {
		t.Fatal("HWM did not produce SubmitBackpressured")
	}
	canceled, cancel := context.WithCancel(context.Background())
	cancel()
	if err := backpressured.Admitted(canceled); !errors.Is(err, context.Canceled) {
		t.Fatalf("Admitted() before WRITABLE = %v, want context.Canceled", err)
	}

	allReceived := make(chan struct{})
	allowReplies := make(chan struct{})
	serverDone := make(chan error, 1)
	go func() {
		received := make([]*zlink.Received, 0, len(submissions))
		for range submissions {
			request := &zlink.Received{}
			if ok, err := router.Recv(request, zlink.RecvFlagsNone); err != nil || !ok {
				serverDone <- fmt.Errorf("request Recv() = (%v, %v)", ok, err)
				return
			}
			received = append(received, request)
		}
		close(allReceived)
		<-allowReplies
		for index, request := range received {
			reply := newMessage(t, fmt.Sprintf("reply-%d", index))
			if err := request.Reply().Message(reply).Submit(context.Background()); err != nil {
				serverDone <- err
				return
			}
			_ = request.Close()
		}
		serverDone <- nil
	}()
	select {
	case <-allReceived:
	case err := <-serverDone:
		t.Fatal(err)
	case <-time.After(5 * time.Second):
		t.Fatal("server did not receive requests through WRITABLE admission")
	}
	waitCtx, cancelWait := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancelWait()
	if err := backpressured.Admitted(waitCtx); err != nil {
		t.Fatalf("Admitted() after WRITABLE error = %v", err)
	}
	if parts, err := backpressured.Reply(canceled); parts != nil || !errors.Is(err, context.Canceled) {
		t.Fatalf("Reply() before replies = (%v, %v)", parts, err)
	}
	close(allowReplies)
	for index, submission := range submissions {
		parts, err := submission.Reply(waitCtx)
		if err != nil {
			t.Fatalf("Reply(%d) error = %v", index, err)
		}
		if len(parts) != 1 || string(parts[0].Data()) != fmt.Sprintf("reply-%d", index) {
			t.Fatalf("Reply(%d) parts = %v", index, parts)
		}
		zlink.MultipartClose(parts)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}
