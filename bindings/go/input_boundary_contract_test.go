// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"context"
	"errors"
	"strings"
	"syscall"
	"testing"

	zlink "zlink.systems/zlink"
)

func TestPublishRejectsEmbeddedNULWithoutConsumingMessage(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	pub, err := ctx.PubSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer pub.Close()
	xpub, err := ctx.XPubSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer xpub.Close()
	for _, publish := range []interface{ Publish(string) zlink.PublishOp }{pub, xpub} {
		msg := newMessage(t, "body")
		defer msg.Close()
		ok, err := publish.Publish("bad\x00topic").Message(msg).Submit(context.Background())
		var config *zlink.ConfigError
		if ok || !errors.As(err, &config) || config.Result != zlink.ConfigInvalidArgument || !errors.Is(err, syscall.EINVAL) {
			t.Errorf("%T Publish = %t, %v, want invalid argument (EINVAL)", publish, ok, err)
		}
		if msg.String() != "body" {
			t.Errorf("%T consumed payload after topic rejection", publish)
		}
	}
}

func TestEndpointLengthLimitIncludesTransportPrefix(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()
	s, err := ctx.PairSocket()
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	const prefix = "inproc://"
	max := prefix + strings.Repeat("x", 255-len(prefix))
	if err := s.Bind(max); err != nil {
		t.Fatalf("Bind(255 bytes): %v", err)
	}
	if err := s.Unbind(max); err != nil {
		t.Fatalf("Unbind(255 bytes): %v", err)
	}
	for name, call := range map[string]func(string) error{
		"Bind": s.Bind, "Unbind": s.Unbind, "Connect": s.Connect, "Disconnect": s.Disconnect,
	} {
		err := call(max + "x")
		var config *zlink.ConfigError
		if !errors.As(err, &config) || config.Result != zlink.ConfigInvalidArgument || !errors.Is(err, syscall.EINVAL) {
			t.Errorf("%s(256 bytes) = %v, want invalid argument (EINVAL)", name, err)
		}
	}
}
