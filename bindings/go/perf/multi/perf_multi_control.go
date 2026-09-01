package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"zlink.systems/zlink/perf/internal/perfcommon"
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

type multiStreamControl struct {
	start <-chan error
	stop  <-chan struct{}
}

func newMultiStreamControl(reader io.Reader, msgSize int) *multiStreamControl {
	start := make(chan error, 1)
	stop := make(chan struct{})
	go func() {
		defer close(stop)
		scanner := bufio.NewScanner(reader)
		started := false
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if line == "" {
				continue
			}
			if !started {
				expected := fmt.Sprintf("START,%d", msgSize)
				if line != expected {
					start <- fmt.Errorf(
						"multi stream control token mismatch: got %q, expected %q",
						line,
						expected,
					)
					return
				}
				start <- nil
				started = true
				continue
			}
			if line == "STOP" || line == "QUIT" {
				return
			}
		}
		if !started {
			if err := scanner.Err(); err != nil {
				start <- fmt.Errorf("multi stream control read: %w", err)
			} else {
				start <- fmt.Errorf("multi stream control closed before START,%d", msgSize)
			}
		}
	}()
	return &multiStreamControl{start: start, stop: stop}
}

func (control *multiStreamControl) waitForStart(timeout time.Duration) error {
	if timeout <= 0 {
		return fmt.Errorf("multi stream START timeout must be positive")
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case err := <-control.start:
		return err
	case <-timer.C:
		return fmt.Errorf("multi stream server timed out waiting for START")
	}
}

func activeDeadline(duration time.Duration) perfcommon.BenchmarkWindow {
	return perfcommon.NewBenchmarkWindow(duration)
}
