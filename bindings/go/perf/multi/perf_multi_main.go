package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"strconv"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

type multiConfig struct {
	pattern   string
	transport string
	msgSize   int
	duration  time.Duration
	clients   int
}

func multiSendDrainTimeout() time.Duration {
	value := 5000
	if parsed, err := strconv.Atoi(os.Getenv("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS")); err == nil && parsed > 0 {
		value = parsed
	}
	return time.Duration(value) * time.Millisecond
}

// multiSendTurnCoordinator keeps one admission owner per socket. An
// immediately admitted send makes that socket available for the next round;
// a backpressured send remains unavailable until its exact WRITABLE retry
// completes through the harness-owned public completion poller.
type multiSendTurnCoordinator struct {
	pending []zlink.SendSubmission
	count   int
	next    int
}

func newMultiSendTurnCoordinator(socketCount int) *multiSendTurnCoordinator {
	return &multiSendTurnCoordinator{pending: make([]zlink.SendSubmission, socketCount)}
}

func (c *multiSendTurnCoordinator) submitRound(
	stopAt time.Time,
	submit func(int) (zlink.SendSubmission, error),
) (int, error) {
	if len(c.pending) == 0 {
		return 0, nil
	}
	start := c.next
	c.next = (c.next + 1) % len(c.pending)
	submitted := 0
	for attempt := 0; attempt < len(c.pending); attempt++ {
		if !time.Now().Before(stopAt) {
			break
		}
		index := (start + attempt) % len(c.pending)
		if c.pending[index] != nil {
			continue
		}
		submission, err := submit(index)
		if err != nil {
			return submitted, err
		}
		submitted++
		if submission == nil {
			return submitted, fmt.Errorf("multi send returned no submission")
		}
		switch submission.Result() {
		case zlink.SubmitOK:
		case zlink.SubmitBackpressured:
			c.pending[index] = submission
			c.count++
		default:
			return submitted, fmt.Errorf("multi send returned submit result %d", submission.Result())
		}
	}
	return submitted, nil
}

// resumeReady observes admission only after the public completion poller has
// had a turn. A canceled context makes the check nonblocking without canceling
// the submission retained by the binding.
func (c *multiSendTurnCoordinator) resumeReady() (bool, error) {
	progressed := false
	for index, submission := range c.pending {
		if submission == nil {
			continue
		}
		ready, err := multiSendAdmissionReady(submission)
		if !ready {
			continue
		}
		c.pending[index] = nil
		c.count--
		progressed = true
		if err != nil {
			return progressed, err
		}
	}
	return progressed, nil
}

func multiSendAdmissionReady(submission zlink.SendSubmission) (bool, error) {
	readyContext, cancelReady := context.WithCancel(context.Background())
	cancelReady()
	err := submission.Admitted(readyContext)
	if errors.Is(err, context.Canceled) {
		return false, nil
	}
	return true, err
}

func waitMultiSendAdmission(
	poller *zlink.Poller,
	events []zlink.PollEvent,
	submission zlink.SendSubmission,
) error {
	if submission == nil {
		return fmt.Errorf("multi send returned no submission")
	}
	switch submission.Result() {
	case zlink.SubmitOK:
		return nil
	case zlink.SubmitBackpressured:
	default:
		return fmt.Errorf("multi send returned submit result %d", submission.Result())
	}
	for {
		ready, err := multiSendAdmissionReady(submission)
		if ready {
			return err
		}
		if _, err := poller.Wait(events, 50*time.Millisecond); err != nil &&
			!perfcommon.IsTransient(err) {
			return err
		}
	}
}

func runMultiSendTurns(
	socketCount int,
	window perfcommon.BenchmarkWindow,
	label string,
	submit func(int) (zlink.SendSubmission, error),
	progress func(time.Duration) error,
	onSubmitted func(int),
	hasDrainWork func() bool,
) error {
	if progress == nil {
		return fmt.Errorf("%s requires a public completion poller", label)
	}
	coordinator := newMultiSendTurnCoordinator(socketCount)
	for time.Now().Before(window.StopAt) {
		submitted, err := coordinator.submitRound(window.StopAt, submit)
		if err != nil {
			return err
		}
		if onSubmitted != nil {
			onSubmitted(submitted)
		}
		if err := progress(multiSendTurnWait(window.StopAt, submitted > 0)); err != nil {
			return err
		}
		if _, err := coordinator.resumeReady(); err != nil {
			return err
		}
	}

	drainWindow := multiSendDrainTimeout()
	if hasDrainWork != nil {
		// C echo client (perf_multi_client_helpers.hpp): the teardown window is
		// max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, 3 s per active second) because
		// small messages can fill every per-client Core queue.
		if active := window.StopAt.Sub(window.ActiveAt); active > 0 {
			if scaled := 3 * active; scaled > drainWindow {
				drainWindow = scaled
			}
		}
	}
	drainDeadline := time.Now().Add(drainWindow)
	for coordinator.count > 0 || (hasDrainWork != nil && hasDrainWork()) {
		if coordinator.count == 0 && (hasDrainWork == nil || !hasDrainWork()) {
			break
		}
		if !time.Now().Before(drainDeadline) {
			return fmt.Errorf("%s send drain timed out", label)
		}
		if err := progress(multiSendTurnWait(drainDeadline, false)); err != nil {
			return err
		}
		if _, err := coordinator.resumeReady(); err != nil {
			return err
		}
	}
	return nil
}

func multiSendTurnWait(deadline time.Time, progressed bool) time.Duration {
	if progressed {
		return 0
	}
	wait := time.Until(deadline)
	if wait > 50*time.Millisecond {
		return 50 * time.Millisecond
	}
	if wait < 0 {
		return 0
	}
	return wait
}

var (
	multiPattern   = flag.String("pattern", "PUBSUB", "")
	multiTransport = flag.String("transport", "tcp", "")
	multiSize      = flag.Int("msg-size", 64, "")
	multiDuration  = flag.Int("duration", 5, "")
	multiClients   = flag.Int("clients", 100, "")
	multiRole      = flag.String("role", "", "")
	multiEndpoint  = flag.String("endpoint", "", "")
)

func main() {
	flag.Parse()

	loaded := perfcommon.LoadMultiConfig(
		*multiPattern,
		*multiTransport,
		*multiSize,
		*multiDuration,
		*multiClients,
	)
	cfg := multiConfig{
		pattern:   loaded.Pattern,
		transport: loaded.Transport,
		msgSize:   loaded.MsgSize,
		duration:  loaded.Duration,
		clients:   loaded.Clients,
	}

	// PERF_MULTI_TEST_POLICY: multi benchmarks use the separate-process
	// (role-based) model exclusively, mirroring the C runner. There is
	// no in-process single-process variant.
	if *multiRole == "" {
		perfcommon.Must(fmt.Errorf("--role is required for multi perf (server|client)"))
	}
	runMultiRole(cfg, *multiRole, *multiEndpoint)
}

type unsupportedMultiPatternError struct {
	pattern string
}

func (e *unsupportedMultiPatternError) Error() string {
	return "unsupported multi perf pattern: " + e.pattern
}

func runMultiRole(cfg multiConfig, role, endpoint string) {
	switch role {
	case "server":
		runMultiServerRole(cfg)
	case "client":
		if endpoint == "" {
			perfcommon.Must(fmt.Errorf("--endpoint is required for multi client role"))
		}
		runMultiClientRole(cfg, endpoint)
	default:
		perfcommon.Must(fmt.Errorf("unsupported multi perf role: %s", role))
	}
}

func runMultiServerRole(cfg multiConfig) {
	switch cfg.pattern {
	case "PUBSUB":
		runMultiPubSubServer(cfg)
	case "DEALER_DEALER":
		runMultiDealerDealerServer(cfg)
	case "DEALER_ROUTER_SENDSEND":
		runMultiDealerRouterServer(cfg)
	case "DEALER_ROUTER_REQREP":
		runMultiDealerRouterReqRepServer(cfg)
	case "ROUTER_ROUTER_SENDSEND":
		runMultiRouterRouterServer(cfg)
	case "ROUTER_ROUTER_REQREP":
		runMultiRouterRouterReqRepServer(cfg)
	case "STREAM":
		runMultiStreamServer(cfg)
	default:
		perfcommon.Must(&unsupportedMultiPatternError{pattern: cfg.pattern})
	}
}

func runMultiClientRole(cfg multiConfig, endpoint string) {
	switch cfg.pattern {
	case "PUBSUB":
		result := runMultiPubSubClient(cfg, endpoint)
		printMultiResult(cfg, result)
	case "DEALER_DEALER":
		runMultiDealerDealerClient(cfg, endpoint)
	case "DEALER_ROUTER_SENDSEND":
		result := runMultiDealerRouterClient(cfg, endpoint)
		printMultiResult(cfg, result)
	case "DEALER_ROUTER_REQREP":
		runMultiDealerRouterReqRepClient(cfg, endpoint)
	case "ROUTER_ROUTER_SENDSEND":
		result := runMultiRouterRouterClientRole(cfg, endpoint)
		printMultiResult(cfg, result)
	case "ROUTER_ROUTER_REQREP":
		runMultiRouterRouterReqRepClient(cfg, endpoint)
	// STREAM has no Go client role: the shared C
	// perf_stream_client binary is the reference client (spawned by
	// run_benchmarks_multi.sh).
	default:
		perfcommon.Must(&unsupportedMultiPatternError{pattern: cfg.pattern})
	}
}

func printMultiResult(cfg multiConfig, result perfcommon.Result) {
	// perf_multi_client_helpers.hpp run_one_way_duration /
	// run_echo_duration / run_recv_duration: recv==0 or lat_count==0
	// returns false -> FAIL (no RESULT line, nonzero exit).
	if !result.Valid {
		perfcommon.PrintFail(cfg.pattern, cfg.transport, cfg.msgSize)
		os.Exit(1)
	}
	result = perfcommon.FinalizeResult(perfcommon.SuiteMulti, cfg.pattern, cfg.msgSize, result)
	perfcommon.PrintResult(cfg.pattern, cfg.transport, cfg.msgSize, result)
}

func flushControlLine(format string, args ...interface{}) {
	fmt.Printf(format+"\n", args...)
}
