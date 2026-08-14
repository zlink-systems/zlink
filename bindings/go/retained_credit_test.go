package zlink_test

import (
	"bytes"
	"context"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

var (
	_ func(*zlink.PairSocket, *zlink.Received, zlink.RecvFlags) (bool, error)     = (*zlink.PairSocket).RecvRetained
	_ func(*zlink.DealerSocket, *zlink.Received, zlink.RecvFlags) (bool, error)   = (*zlink.DealerSocket).RecvRetained
	_ func(*zlink.RouterSocket, *zlink.Received, zlink.RecvFlags) (bool, error)   = (*zlink.RouterSocket).RecvRetained
	_ func(*zlink.StreamSocket, *zlink.Received, zlink.RecvFlags) (bool, error)   = (*zlink.StreamSocket).RecvRetained
	_ func(*zlink.SubSocket, *zlink.TopicMessage, zlink.RecvFlags) (bool, error)  = (*zlink.SubSocket).SubscribeRetained
	_ func(*zlink.XSubSocket, *zlink.TopicMessage, zlink.RecvFlags) (bool, error) = (*zlink.XSubSocket).SubscribeRetained
)

func waitForApplicationLeaseCount(
	t *testing.T,
	ctx *zlink.Context,
	want uint64,
) zlink.CoreHwmBudgetSnapshot {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		snapshot, err := ctx.CoreHwmBudgetSnapshot()
		if err != nil {
			t.Fatalf("CoreHwmBudgetSnapshot() error = %v", err)
		}
		if snapshot.OutstandingApplicationLeaseCount == want {
			return snapshot
		}
		if time.Now().After(deadline) {
			t.Fatalf(
				"OutstandingApplicationLeaseCount = %d, want %d (snapshot=%+v)",
				snapshot.OutstandingApplicationLeaseCount,
				want,
				snapshot,
			)
		}
		time.Sleep(time.Millisecond)
	}
}

func requireNoApplicationCredit(t *testing.T, ctx *zlink.Context) {
	t.Helper()
	snapshot := waitForApplicationLeaseCount(t, ctx, 0)
	if snapshot.ApplicationAccountedBytes != 0 {
		t.Fatalf("ApplicationAccountedBytes = %d, want 0", snapshot.ApplicationAccountedBytes)
	}
}

func requireQueuedCredit(t *testing.T, ctx *zlink.Context) zlink.CoreHwmBudgetSnapshot {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for {
		snapshot, err := ctx.CoreHwmBudgetSnapshot()
		if err != nil {
			t.Fatalf("CoreHwmBudgetSnapshot() error = %v", err)
		}
		if snapshot.CoreQueueAccountedBytes > 0 {
			return snapshot
		}
		if time.Now().After(deadline) {
			t.Fatalf("CoreQueueAccountedBytes = 0 (snapshot=%+v)", snapshot)
		}
		time.Sleep(time.Millisecond)
	}
}

func TestPairOrdinaryAndRetainedReceiveCreditLifetimes(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	endpoint := inprocEndpoint("pair-retained-credit")
	receiver, _ := ctx.PairSocket()
	sender, _ := ctx.PairSocket()
	defer receiver.Close()
	defer sender.Close()
	if err := receiver.SetReceiveHighWaterMark(128); err != nil {
		t.Fatalf("receiver SetReceiveHighWaterMark() error = %v", err)
	}
	if err := sender.SetSendHighWaterMark(128); err != nil {
		t.Fatalf("sender SetSendHighWaterMark() error = %v", err)
	}
	if err := receiver.Bind(endpoint); err != nil {
		t.Fatalf("receiver Bind() error = %v", err)
	}
	if err := sender.Connect(endpoint); err != nil {
		t.Fatalf("sender Connect() error = %v", err)
	}

	if ok, err := sender.Send().Bytes([]byte("ordinary-a")).Bytes([]byte("ordinary-b")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("ordinary Send() = (%v, %v), want (true, nil)", ok, err)
	}
	var ordinary zlink.Received
	if ok, err := receiver.Recv(&ordinary, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("ordinary Recv() = (%v, %v), want (true, nil)", ok, err)
	}
	if len(ordinary.Parts()) != 2 {
		t.Fatalf("ordinary parts = %d, want 2", len(ordinary.Parts()))
	}
	requireNoApplicationCredit(t, ctx)
	if err := ordinary.Close(); err != nil {
		t.Fatalf("ordinary Close() error = %v", err)
	}

	if ok, err := sender.Send().Bytes([]byte("retained-a")).Bytes([]byte("retained-b")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("retained Send() = (%v, %v), want (true, nil)", ok, err)
	}
	queued := requireQueuedCredit(t, ctx)
	var retained zlink.Received
	if ok, err := receiver.RecvRetained(&retained, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	parts := retained.Parts()
	if len(parts) != 2 || !bytes.Equal(parts[0].Data(), []byte("retained-a")) || !bytes.Equal(parts[1].Data(), []byte("retained-b")) {
		t.Fatalf("retained multipart payloads were not preserved")
	}
	held := waitForApplicationLeaseCount(t, ctx, 2)
	if held.ApplicationAccountedBytes == 0 {
		t.Fatalf("ApplicationAccountedBytes = 0 while two retained parts are live")
	}
	if held.CoreQueueAccountedBytes != 0 || held.CurrentAccountedBytes != queued.CurrentAccountedBytes {
		t.Fatalf(
			"retained credit transfer changed total accounting: queued=%+v held=%+v",
			queued,
			held,
		)
	}

	if ok, err := receiver.RecvRetained(&retained, zlink.RecvFlagsDontWait); err != nil || ok {
		t.Fatalf("empty reused RecvRetained() = (%v, %v), want (false, nil)", ok, err)
	}
	requireNoApplicationCredit(t, ctx)

	if ok, err := sender.Send().Bytes([]byte("cross-goroutine-close")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("final Send() = (%v, %v), want (true, nil)", ok, err)
	}
	if ok, err := receiver.RecvRetained(&retained, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("final RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	waitForApplicationLeaseCount(t, ctx, 1)
	closed := make(chan error, 1)
	go func() {
		closed <- retained.Close()
	}()
	select {
	case err := <-closed:
		if err != nil {
			t.Fatalf("cross-goroutine Close() error = %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("cross-goroutine Close() timed out")
	}
	requireNoApplicationCredit(t, ctx)
	if err := retained.Close(); err != nil {
		t.Fatalf("repeated retained Close() error = %v", err)
	}

	if ok, err := sender.Send().Bytes([]byte("detach-error-cleanup")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("detach Send() = (%v, %v), want (true, nil)", ok, err)
	}
	if ok, err := receiver.RecvRetained(&retained, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("detach RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	waitForApplicationLeaseCount(t, ctx, 1)
	if err := receiver.Close(); err != nil {
		t.Fatalf("receiver Close() with retained result error = %v", err)
	}
	if ok, err := receiver.RecvRetained(&retained, zlink.RecvFlagsDontWait); err == nil || ok {
		t.Fatalf("closed receiver RecvRetained() = (%v, %v), want (false, error)", ok, err)
	}
	requireNoApplicationCredit(t, ctx)
}

func TestRetainedRoutedReceivePreservesMultipartAndRequestMetadata(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	endpoint := inprocEndpoint("routed-retained-credit")
	router, _ := ctx.RouterSocket()
	dealer, _ := ctx.DealerSocket()
	defer router.Close()
	defer dealer.Close()
	dealerRID := zlink.NewRoutingID([]byte("retained-dealer"))
	if err := dealer.SetRoutingID(dealerRID); err != nil {
		t.Fatalf("dealer SetRoutingID() error = %v", err)
	}
	if err := router.Bind(endpoint); err != nil {
		t.Fatalf("router Bind() error = %v", err)
	}
	if err := dealer.Connect(endpoint); err != nil {
		t.Fatalf("dealer Connect() error = %v", err)
	}

	if err := awaitRoutedSend(t, dealer.Send().Bytes([]byte("raw-a")).Bytes([]byte("raw-b")).Submit(context.Background())); err != nil {
		t.Fatalf("dealer Send() error = %v", err)
	}
	var routed zlink.Received
	if ok, err := router.RecvRetained(&routed, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("router RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	if !routed.RoutingID().Equal(dealerRID) || routed.HasRequestSeq() || len(routed.Parts()) != 2 {
		t.Fatalf("raw routed metadata = rid %q, hasSeq %v, parts %d", routed.RoutingID(), routed.HasRequestSeq(), len(routed.Parts()))
	}
	waitForApplicationLeaseCount(t, ctx, 2)
	if err := routed.Close(); err != nil {
		t.Fatalf("raw routed Close() error = %v", err)
	}
	requireNoApplicationCredit(t, ctx)

	dealerCompletion := dealer.Request().
		Bytes([]byte("router-request")).
		Timeout(2 * time.Second).
		Submit(context.Background())
	if ok, err := router.RecvRetained(&routed, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("router request RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	if !routed.RoutingID().Equal(dealerRID) || !routed.HasRequestSeq() || routed.RequestSeq() == 0 || len(routed.Parts()) != 1 {
		t.Fatalf("router request metadata = rid %q, seq %d, parts %d", routed.RoutingID(), routed.RequestSeq(), len(routed.Parts()))
	}
	waitForApplicationLeaseCount(t, ctx, 1)
	if err := routed.Reply().Message(newMessage(t, "router-reply")).Submit(context.Background()); err != nil {
		t.Fatalf("router retained Reply() error = %v", err)
	}
	if err := routed.Close(); err != nil {
		t.Fatalf("router request Close() error = %v", err)
	}
	requireNoApplicationCredit(t, ctx)
	select {
	case completion := <-dealerCompletion:
		if completion.Err != nil {
			t.Fatalf("dealer request completion error = %v", completion.Err)
		}
		if len(completion.Parts) != 1 || string(completion.Parts[0].Data()) != "router-reply" {
			t.Fatalf("dealer request completion payload was not preserved")
		}
		zlink.MultipartClose(completion.Parts)
	case <-time.After(4 * time.Second):
		t.Fatal("dealer request completion timed out")
	}

	routerCompletion := router.Request(dealerRID).
		Bytes([]byte("dealer-request")).
		Timeout(2 * time.Second).
		Submit(context.Background())
	var dealerRequest zlink.Received
	if ok, err := dealer.RecvRetained(&dealerRequest, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("dealer request RecvRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	if dealerRequest.HasRoutingID() || !dealerRequest.HasRequestSeq() || dealerRequest.RequestSeq() == 0 || len(dealerRequest.Parts()) != 1 {
		t.Fatalf("dealer request metadata = hasRid %v, seq %d, parts %d", dealerRequest.HasRoutingID(), dealerRequest.RequestSeq(), len(dealerRequest.Parts()))
	}
	waitForApplicationLeaseCount(t, ctx, 1)
	if err := dealerRequest.Reply().Message(newMessage(t, "dealer-reply")).Submit(context.Background()); err != nil {
		t.Fatalf("dealer retained Reply() error = %v", err)
	}
	if err := dealerRequest.Close(); err != nil {
		t.Fatalf("dealer request Close() error = %v", err)
	}
	requireNoApplicationCredit(t, ctx)
	select {
	case completion := <-routerCompletion:
		if completion.Err != nil {
			t.Fatalf("router request completion error = %v", completion.Err)
		}
		if len(completion.Parts) != 1 || string(completion.Parts[0].Data()) != "dealer-reply" {
			t.Fatalf("router request completion payload was not preserved")
		}
		zlink.MultipartClose(completion.Parts)
	case <-time.After(4 * time.Second):
		t.Fatal("router request completion timed out")
	}
}

func TestRetainedSubscribePreservesTopicAndMultipart(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	if err := ctx.Options().SetAutoHwmEnabled(false); err != nil {
		t.Fatalf("SetAutoHwmEnabled(false) error = %v", err)
	}

	endpoint := inprocEndpoint("subscribe-retained-credit")
	publisher, _ := ctx.XPubSocket()
	subscriber, _ := ctx.SubSocket()
	defer publisher.Close()
	defer subscriber.Close()
	if err := publisher.Bind(endpoint); err != nil {
		t.Fatalf("publisher Bind() error = %v", err)
	}
	if err := subscriber.Connect(endpoint); err != nil {
		t.Fatalf("subscriber Connect() error = %v", err)
	}
	if err := subscriber.SetSubscription("credit."); err != nil {
		t.Fatalf("SetSubscription() error = %v", err)
	}
	var subscription zlink.SubscriptionEvent
	if ok, err := publisher.ReceiveSubscriptionEvent(&subscription, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("ReceiveSubscriptionEvent() = (%v, %v), want (true, nil)", ok, err)
	}
	if !subscription.Subscribed() || subscription.Topic() != "credit." {
		t.Fatalf("subscription event = subscribed %v, topic %q", subscription.Subscribed(), subscription.Topic())
	}

	if ok, err := publisher.Publish("credit.ordinary").Bytes([]byte("ordinary")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("ordinary Publish() = (%v, %v), want (true, nil)", ok, err)
	}
	var ordinary zlink.TopicMessage
	if ok, err := subscriber.Subscribe(&ordinary, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("ordinary Subscribe() = (%v, %v), want (true, nil)", ok, err)
	}
	if ordinary.Topic() != "credit.ordinary" || len(ordinary.Parts()) != 1 {
		t.Fatalf("ordinary topic result = topic %q, parts %d", ordinary.Topic(), len(ordinary.Parts()))
	}
	requireNoApplicationCredit(t, ctx)
	if err := ordinary.Close(); err != nil {
		t.Fatalf("ordinary TopicMessage.Close() error = %v", err)
	}

	if ok, err := publisher.Publish("credit.retained").Bytes([]byte("topic-a")).Bytes([]byte("topic-b")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("retained Publish() = (%v, %v), want (true, nil)", ok, err)
	}
	var retained zlink.TopicMessage
	if ok, err := subscriber.SubscribeRetained(&retained, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("SubscribeRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	if retained.Topic() != "credit.retained" || retained.HasRoutingID() || len(retained.Parts()) != 2 {
		t.Fatalf("retained topic result = topic %q, hasRid %v, parts %d", retained.Topic(), retained.HasRoutingID(), len(retained.Parts()))
	}
	parts := retained.Parts()
	if string(parts[0].Data()) != "topic-a" || string(parts[1].Data()) != "topic-b" {
		t.Fatalf("retained topic multipart payloads were not preserved")
	}
	waitForApplicationLeaseCount(t, ctx, 2)
	if ok, err := subscriber.SubscribeRetained(&retained, zlink.RecvFlagsDontWait); err != nil || ok {
		t.Fatalf("empty reused SubscribeRetained() = (%v, %v), want (false, nil)", ok, err)
	}
	requireNoApplicationCredit(t, ctx)

	if ok, err := publisher.Publish("credit.close").Bytes([]byte("close-owned")).Submit(context.Background()); err != nil || !ok {
		t.Fatalf("close Publish() = (%v, %v), want (true, nil)", ok, err)
	}
	if ok, err := subscriber.SubscribeRetained(&retained, zlink.RecvFlagsNone); err != nil || !ok {
		t.Fatalf("close SubscribeRetained() = (%v, %v), want (true, nil)", ok, err)
	}
	waitForApplicationLeaseCount(t, ctx, 1)
	if err := retained.Close(); err != nil {
		t.Fatalf("retained TopicMessage.Close() error = %v", err)
	}
	requireNoApplicationCredit(t, ctx)
}
