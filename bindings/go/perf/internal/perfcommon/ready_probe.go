package perfcommon

import (
	"fmt"
	"time"
)

func waitProbeReady(ready <-chan struct{}, timeout time.Duration, name string) error {
	if ready == nil {
		return fmt.Errorf("%s ready wait is not configured", name)
	}

	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case <-ready:
		return nil
	case <-timer.C:
		return fmt.Errorf("%s did not become ready", name)
	}
}
