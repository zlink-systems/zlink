package perfcommon

import (
	"fmt"
	"time"

	zlink "zlink.systems/zlink"
)

func waitMonitorReady(
	monitor *zlink.SocketMonitor,
	minEvents int,
	timeout time.Duration,
	name string,
) error {
	readyEvents := 0
	for readyEvents < minEvents {
		type result struct {
			event *zlink.MonitorEvent
			err   error
		}
		ch := make(chan result, 1)
		go func() {
			event, err := monitor.Recv(0)
			ch <- result{event: event, err: err}
		}()
		select {
		case out := <-ch:
			if out.err != nil {
				if IsTransient(out.err) {
					continue
				}
				return out.err
			}
			if out.event != nil && out.event.IsConnectionReady() {
				readyEvents++
			}
		case <-time.After(timeout):
			return fmt.Errorf("%s did not become ready", name)
		}
	}
	return nil
}

func waitMonitorReadyWithActivity(
	activity zlink.SocketTarget,
	monitors []*zlink.SocketMonitor,
	timeout time.Duration,
) error {
	poller := NewSocketPoller(activity, zlink.PollIn)
	defer poller.Close()

	ready := make([]bool, len(monitors))
	remaining := len(monitors)
	events := make([]zlink.PollEvent, 1)
	deadline := time.Now().Add(timeout)
	for remaining > 0 && time.Now().Before(deadline) {
		for i, monitor := range monitors {
			if ready[i] {
				continue
			}
			event, err := monitor.Recv(zlink.RecvFlagsDontWait)
			if err != nil {
				if IsTransient(err) {
					continue
				}
				return err
			}
			if event != nil && event.IsConnectionReady() {
				ready[i] = true
				remaining--
			}
		}
		if remaining == 0 {
			return nil
		}

		wait := time.Until(deadline)
		if wait > 10*time.Millisecond {
			wait = 10 * time.Millisecond
		}
		if _, err := poller.Wait(events, wait); err != nil && !IsTransient(err) {
			return err
		}
	}
	return fmt.Errorf("connection did not become ready")
}
