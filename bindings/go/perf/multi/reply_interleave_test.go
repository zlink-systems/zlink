package main

import (
	"reflect"
	"testing"
)

func TestPacedRepliesAdvanceSchedule(t *testing.T) {
	replies := 0
	var rechecks []int
	for i := 1; i <= 130; i++ {
		if replyTurn(&replies) {
			rechecks = append(rechecks, i)
		}
	}
	if want := []int{64, 128}; !reflect.DeepEqual(rechecks, want) {
		t.Fatalf("rechecks = %v, want %v", rechecks, want)
	}
}
