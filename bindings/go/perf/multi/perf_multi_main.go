package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"time"

	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

type multiConfig struct {
	pattern   string
	transport string
	msgSize   int
	duration  time.Duration
	clients   int
}

var (
	multiPattern   = flag.String("pattern", "MULTI_PUBSUB", "")
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
	// The multi role hot loops are tight cgo round-trips with the core IO
	// threads. Pinning the role goroutine to a dedicated OS thread keeps the
	// Go scheduler from migrating its M across the many blocking poller waits
	// that backpressure produces at high pipe fan-in; without it, each wakeup
	// pays Go-runtime M-handoff latency and large many-client throughput
	// collapses (profiling showed both peers idle in futex/nanosleep, not CPU
	// bound). This is a measurement-neutral harness detail: it changes which OS
	// thread runs the loop, not the public API or wire semantics.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

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
	case "MULTI_PUBSUB":
		runMultiPubSubServer(cfg)
	case "MULTI_DEALER_DEALER":
		runMultiDealerDealerServer(cfg)
	case "MULTI_DEALER_ROUTER":
		runMultiDealerRouterServer(cfg)
	case "MULTI_ROUTER_ROUTER":
		runMultiRouterRouterServer(cfg)
	case "MULTI_STREAM":
		runMultiStreamServer(cfg)
	default:
		perfcommon.Must(&unsupportedMultiPatternError{pattern: cfg.pattern})
	}
}

func runMultiClientRole(cfg multiConfig, endpoint string) {
	switch cfg.pattern {
	case "MULTI_PUBSUB":
		result := runMultiPubSubClient(cfg, endpoint)
		printMultiResult(cfg, result)
	case "MULTI_DEALER_DEALER":
		runMultiDealerDealerClient(cfg, endpoint)
	case "MULTI_DEALER_ROUTER":
		result := runMultiDealerRouterClient(cfg, endpoint)
		printMultiResult(cfg, result)
	case "MULTI_ROUTER_ROUTER":
		result := runMultiRouterRouterClientRole(cfg, endpoint)
		printMultiResult(cfg, result)
	// MULTI_STREAM has no Go client role: the shared C
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
	result = perfcommon.FinalizeResult(cfg.pattern, cfg.msgSize, result)
	perfcommon.PrintResult(cfg.pattern, cfg.transport, cfg.msgSize, result)
}

func flushControlLine(format string, args ...interface{}) {
	fmt.Printf(format+"\n", args...)
	_ = os.Stdout.Sync()
}
