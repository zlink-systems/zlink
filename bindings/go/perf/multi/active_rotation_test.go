package main

import (
	"reflect"
	"testing"
	"time"

	zlink "zlink.systems/zlink"
)

func TestActiveSenderRotation(t *testing.T) {
	coordinator := newMultiSendTurnCoordinator(2)
	var senders []int
	_, err := coordinator.submitRound(time.Now().Add(time.Second), func(index int) (zlink.SendSubmission, error) {
		senders = append(senders, index)
		return testSendSubmission{result: zlink.SubmitOK}, nil
	})
	want := make([]int, 128)
	for i := 64; i < len(want); i++ {
		want[i] = 1
	}
	if err != nil || !reflect.DeepEqual(senders, want) {
		t.Fatalf("sender visits: %v, error: %v", senders, err)
	}
}
