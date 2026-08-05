package perfcommon

import (
	"fmt"
	"time"

	zlink "zlink.systems/zlink/v11"
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
