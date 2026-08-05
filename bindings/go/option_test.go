package zlink_test

import (
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

func TestCommonTypedOptions(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	opts := ctx.Options()

	socket, _ := ctx.RouterSocket()
	defer socket.Close()

	if err := socket.SetSendHighWaterMark(1000); err != nil {
		t.Fatalf("SetSendHighWaterMark() error = %v", err)
	}
	if got, err := socket.SendHighWaterMark(); err != nil || got != 1000 {
		t.Fatalf("SendHighWaterMark() = (%d, %v), want (1000, nil)", got, err)
	}
	if err := socket.SetReceiveHighWaterMark(2000); err != nil {
		t.Fatalf("SetReceiveHighWaterMark() error = %v", err)
	}
	if got, err := socket.ReceiveHighWaterMark(); err != nil || got != 2000 {
		t.Fatalf("ReceiveHighWaterMark() = (%d, %v), want (2000, nil)", got, err)
	}
	if err := socket.SetLinger(50 * time.Millisecond); err != nil {
		t.Fatalf("SetLinger() error = %v", err)
	}
	if got, err := socket.SubmitRetryMode(); err != nil || got != zlink.SubmitRetryOff {
		t.Fatalf("SubmitRetryMode() = (%v, %v), want (off, nil)", got, err)
	}
	if got, err := socket.SubmitRetryTimeout(); err != nil || got != 0 {
		t.Fatalf("SubmitRetryTimeout() = (%v, %v), want (0, nil)", got, err)
	}
	if got, err := socket.SubmitRetryAttempts(); err != nil || got != 0 {
		t.Fatalf("SubmitRetryAttempts() = (%d, %v), want (0, nil)", got, err)
	}
	if err := socket.SetSubmitRetryMode(zlink.SubmitRetryLocalFailure); err != nil {
		t.Fatalf("SetSubmitRetryMode() error = %v", err)
	}
	if err := socket.SetSubmitRetryTimeout(42 * time.Millisecond); err != nil {
		t.Fatalf("SetSubmitRetryTimeout() error = %v", err)
	}
	if err := socket.SetSubmitRetryAttempts(2); err != nil {
		t.Fatalf("SetSubmitRetryAttempts() error = %v", err)
	}
	if got, err := socket.SubmitRetryMode(); err != nil || got != zlink.SubmitRetryLocalFailure {
		t.Fatalf("SubmitRetryMode() = (%v, %v), want (local failure, nil)", got, err)
	}
	if got, err := socket.SubmitRetryTimeout(); err != nil || got != 42*time.Millisecond {
		t.Fatalf("SubmitRetryTimeout() = (%v, %v), want (42ms, nil)", got, err)
	}
	if got, err := socket.SubmitRetryAttempts(); err != nil || got != 2 {
		t.Fatalf("SubmitRetryAttempts() = (%d, %v), want (2, nil)", got, err)
	}
	if err := socket.SetTCPKeepalive(true); err != nil {
		t.Fatalf("SetTCPKeepalive() error = %v", err)
	}
	if err := socket.SetTCPNoDelay(true); err != nil {
		t.Fatalf("SetTCPNoDelay() error = %v", err)
	}
	if err := socket.SetIPv6(false); err != nil {
		t.Fatalf("SetIPv6() error = %v", err)
	}
	if err := opts.SetMaxSockets(1024); err != nil {
		t.Fatalf("SetMaxSockets() error = %v", err)
	}
	if got, err := opts.MaxSockets(); err != nil || got != 1024 {
		t.Fatalf("MaxSockets() = (%d, %v), want (1024, nil)", got, err)
	}
	if err := opts.SetBlocky(false); err != nil {
		t.Fatalf("SetBlocky() error = %v", err)
	}
	if got, err := opts.Blocky(); err != nil || got {
		t.Fatalf("Blocky() = (%v, %v), want (false, nil)", got, err)
	}
	if err := opts.SetAutoHwmProfile(zlink.AutoHwmProfileCompact); err != nil {
		t.Fatalf("SetAutoHwmProfile() error = %v", err)
	}
	if got, err := opts.AutoHwmProfile(); err != nil || got != zlink.AutoHwmProfileCompact {
		t.Fatalf("AutoHwmProfile() = (%v, %v), want (compact, nil)", got, err)
	}
	if got, err := opts.AutoHwmMsgUnitBytes(); err != nil || got != 0 {
		t.Fatalf("AutoHwmMsgUnitBytes() = (%d, %v), want (0, nil)", got, err)
	}
	if err := opts.SetAutoHwmMsgUnitBytes(64); err != nil {
		t.Fatalf("SetAutoHwmMsgUnitBytes(64) error = %v", err)
	}
	if got, err := opts.AutoHwmMsgUnitBytes(); err != nil || got != 64 {
		t.Fatalf("AutoHwmMsgUnitBytes() = (%d, %v), want (64, nil)", got, err)
	}
	if err := opts.SetAutoHwmMsgUnitBytes(0); err != nil {
		t.Fatalf("SetAutoHwmMsgUnitBytes(0) error = %v", err)
	}
	if got, err := opts.AutoHwmMsgUnitBytes(); err != nil || got != 0 {
		t.Fatalf("AutoHwmMsgUnitBytes() after reset = (%d, %v), want (0, nil)", got, err)
	}
	if err := opts.SetAutoHwmMsgUnitBytes(-1); err == nil {
		t.Fatalf("SetAutoHwmMsgUnitBytes(-1) succeeded, want error")
	}
}

func TestSocketSpecificOptions(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	router, _ := ctx.RouterSocket()
	defer router.Close()
	if err := router.SetMandatory(true); err != nil {
		t.Fatalf("SetMandatory() error = %v", err)
	}
	if err := router.SetRidDuplicatePolicy(int(zlink.RidDuplicateHandover)); err != nil {
		t.Fatalf("SetRidDuplicatePolicy() error = %v", err)
	}
	if err := router.SetProbe(true); err != nil {
		t.Fatalf("SetProbe() error = %v", err)
	}
	rid := zlink.NewRoutingID([]byte("router-typed-options"))
	if err := router.SetRoutingID(rid); err != nil {
		t.Fatalf("SetRoutingID() error = %v", err)
	}
	if got, err := router.RoutingID(); err != nil || !got.Equal(rid) {
		t.Fatalf("RoutingID() = (%v, %v), want (%v, nil)", got, err, rid)
	}
	if err := router.SetRequestTimeout(123 * time.Millisecond); err != nil {
		t.Fatalf("SetRequestTimeout() error = %v", err)
	}
	if got, err := router.RequestTimeout(); err != nil || got != 123*time.Millisecond {
		t.Fatalf("RequestTimeout() = (%v, %v), want (123ms, nil)", got, err)
	}

	dealer, _ := ctx.DealerSocket()
	defer dealer.Close()
	if err := dealer.SetProbe(true); err != nil {
		t.Fatalf("Dealer.SetProbe() error = %v", err)
	}
	if err := dealer.SetRequestTimeout(123 * time.Millisecond); err != nil {
		t.Fatalf("Dealer.SetRequestTimeout() error = %v", err)
	}

	xpub, _ := ctx.XPubSocket()
	defer xpub.Close()
	if err := xpub.SetVerbose(true); err != nil {
		t.Fatalf("XPub.SetVerbose() error = %v", err)
	}
	if err := xpub.SetVerboser(true); err != nil {
		t.Fatalf("XPub.SetVerboser() error = %v", err)
	}
	if err := xpub.SetManual(true); err != nil {
		t.Fatalf("XPub.SetManual() error = %v", err)
	}
	if err := xpub.SetNoDrop(true); err != nil {
		t.Fatalf("XPub.SetNoDrop() error = %v", err)
	}
	pubOpts := xpub.PubOptions()
	if err := pubOpts.SetManualLastValue(true); err != nil {
		t.Fatalf("PubOptions.SetManualLastValue() error = %v", err)
	}
	welcome, err := zlink.NewMessage([]byte("welcome"))
	if err != nil {
		t.Fatalf("NewMessage() error = %v", err)
	}
	defer welcome.Close()
	if err := pubOpts.SetWelcomeMessage(welcome); err != nil {
		t.Fatalf("PubOptions.SetWelcomeMessage() error = %v", err)
	}

	stream, _ := ctx.StreamSocket()
	defer stream.Close()
	if err := stream.SetNotify(true); err != nil {
		t.Fatalf("SetNotify() error = %v", err)
	}
	if got, err := stream.Notify(); err != nil || !got {
		t.Fatalf("Notify() = (%v, %v), want (true, nil)", got, err)
	}

	sub, _ := ctx.SubSocket()
	defer sub.Close()
	if err := sub.SetSubscription("alpha"); err != nil {
		t.Fatalf("SetSubscription() error = %v", err)
	}
	filter, isPattern, err := sub.SubscriptionAt(0)
	if err != nil {
		t.Fatalf("SubscriptionAt() error = %v", err)
	}
	if filter != "alpha" {
		t.Fatalf("SubscriptionAt() filter = %q, want %q", filter, "alpha")
	}
	if isPattern {
		t.Fatalf("SubscriptionAt() isPattern = true, want false")
	}
	if got, err := sub.TopicsCount(); err != nil || got < 1 {
		t.Fatalf("TopicsCount() = (%d, %v), want (>=1, nil)", got, err)
	}
}
