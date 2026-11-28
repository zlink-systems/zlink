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
	pending  []zlink.SendSubmission
	count    int
	next     int
	burst    int
	schedule *multiSchedule
	due      []time.Time
	accepted uint64
	stopAt   time.Time
}

func newMultiSendTurnCoordinator(socketCount int) *multiSendTurnCoordinator {
	return &multiSendTurnCoordinator{pending: make([]zlink.SendSubmission, socketCount), due: make([]time.Time, socketCount)}
}

func (c *multiSendTurnCoordinator) submitRound(
	stopAt time.Time,
	submit func(int) (zlink.SendSubmission, error),
) (int, error) {
	if len(c.pending) == 0 {
		return 0, nil
	}
	c.stopAt = stopAt
	submitted := 0
	for visited := 0; visited < len(c.pending); {
		now := time.Now()
		if !now.Before(stopAt) || (c.schedule != nil && !c.schedule.due(now)) {
			break
		}
		index := c.next
		if c.pending[index] != nil || c.burst >= maxBurstPerSender {
			c.next = (index + 1) % len(c.pending)
			c.burst = 0
			visited++
			continue
		}
		submission, err := submit(index)
		if err != nil {
			return submitted, err
		}
		submitted++
		c.burst++
		if submission == nil {
			return submitted, fmt.Errorf("multi send returned no submission")
		}
		if c.schedule != nil {
			c.due[index] = c.schedule.reserve()
		}
		switch submission.Result() {
		case zlink.SubmitOK:
			c.recordAdmission(index)
		case zlink.SubmitBackpressured:
			c.pending[index] = submission
			c.count++
		default:
			return submitted, fmt.Errorf("multi send returned submit result %d", submission.Result())
		}
	}
	return submitted, nil
}

func (c *multiSendTurnCoordinator) recordAdmission(index int) {
	now := time.Now()
	if c.schedule != nil {
		c.schedule.admit(c.due[index], now)
	} else if now.Before(c.stopAt) {
		c.accepted++
	}
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
		c.recordAdmission(index)
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
	schedule *multiSchedule,
	label string,
	submit func(int) (zlink.SendSubmission, error),
	progress func(time.Duration, func() error) error,
	onSubmitted func(int),
	hasDrainWork func() bool,
) (uint64, error) {
	coordinator := newMultiSendTurnCoordinator(socketCount)
	coordinator.schedule = schedule
	submitDue := func() (bool, error) {
		resumed, err := coordinator.resumeReady()
		if err != nil {
			return false, err
		}
		n, err := coordinator.submitRound(window.StopAt, submit)
		if onSubmitted != nil {
			onSubmitted(n)
		}
		return resumed || n > 0, err
	}
	interleave := func() error {
		_, err := submitDue()
		return err
	}
	for time.Now().Before(window.StopAt) {
		progressed, err := submitDue()
		if err != nil {
			return 0, err
		}
		now := time.Now()
		until := window.StopAt
		if schedule != nil && !schedule.due(now) && schedule.next().Before(until) {
			until = schedule.next()
		}
		wait := multiWait(now, until)
		if progressed {
			wait = 0
		}
		if err := progress(wait, interleave); err != nil {
			return 0, err
		}
	}
	drainWindow := multiSendDrainTimeout()
	if hasDrainWork != nil {
		drainWindow = max(drainWindow, 3*window.StopAt.Sub(window.ActiveAt))
	}
	deadline := time.Now().Add(drainWindow)
	for coordinator.count > 0 || (hasDrainWork != nil && hasDrainWork()) {
		now := time.Now()
		if !now.Before(deadline) {
			return 0, fmt.Errorf("%s send drain timed out", label)
		}
		if err := progress(multiWait(now, deadline), func() error { return nil }); err != nil {
			return 0, err
		}
		if _, err := coordinator.resumeReady(); err != nil {
			return 0, err
		}
	}
	return coordinator.accepted, nil
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
	if !result.Valid && result.LatencyInvalid == "" {
		perfcommon.PrintFail(cfg.pattern, cfg.transport, cfg.msgSize)
		os.Exit(1)
	}
	result = perfcommon.FinalizeResult(perfcommon.SuiteMulti, cfg.pattern, cfg.msgSize, result)
	perfcommon.PrintResult(cfg.pattern, cfg.transport, cfg.msgSize, result)
}

func flushControlLine(format string, args ...interface{}) {
	fmt.Printf(format+"\n", args...)
}
