package perfcommon

import (
	"fmt"
	"time"

	zlink "zlink.systems/zlink/v11"
)

type ReadyConfig struct {
	Monitor   *zlink.SocketMonitor
	MinEvents int
	Timeout   time.Duration
	Name      string
	Start     func() error
	Ready     <-chan struct{}
}

func WaitUntilReady(cfg ReadyConfig) error {
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = SingleReadyTimeout()
	}
	name := cfg.Name
	if name == "" {
		name = "perf endpoint"
	}

	if cfg.Monitor != nil {
		return waitMonitorReady(cfg.Monitor, maxReadyEvents(cfg.MinEvents), timeout, name)
	}
	if cfg.Start != nil {
		if err := cfg.Start(); err != nil {
			return err
		}
	}
	if cfg.Ready == nil {
		return fmt.Errorf("%s ready wait is not configured", name)
	}
	return waitProbeReady(cfg.Ready, timeout, name)
}

func WaitConnected(monitors ...*zlink.SocketMonitor) {
	WaitConnectedWithTimeout(SingleReadyTimeout(), monitors...)
}

func WaitConnectedWithTimeout(timeout time.Duration, monitors ...*zlink.SocketMonitor) {
	for _, monitor := range monitors {
		Must(WaitUntilReady(ReadyConfig{
			Monitor:   monitor,
			MinEvents: 1,
			Timeout:   timeout,
		}))
	}
}

func maxReadyEvents(minEvents int) int {
	if minEvents > 0 {
		return minEvents
	}
	return 1
}
