package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

func waitForStartToken(msgSize int) bool {
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		switch {
		case line == "STOP" || line == "QUIT":
			return false
		case line == fmt.Sprintf("START,%d", msgSize):
			return true
		case strings.HasPrefix(line, "START,"):
			parts := strings.Split(line, ",")
			if len(parts) == 2 {
				size, err := strconv.Atoi(parts[1])
				if err == nil && size == msgSize {
					return true
				}
			}
		}
	}
	return false
}

func waitForStopToken() {
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "STOP" || line == "QUIT" {
			return
		}
	}
}

func waitForStopAsync() <-chan struct{} {
	done := make(chan struct{})
	go func() {
		defer close(done)
		waitForStopToken()
	}()
	return done
}

func activeDeadline(duration time.Duration) perfcommon.BenchmarkWindow {
	return perfcommon.NewBenchmarkWindow(duration)
}
