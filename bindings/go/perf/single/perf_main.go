package main

import (
	"flag"
	"os"
	"time"

	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

type benchmarkConfig struct {
	pattern   string
	transport string
	msgSize   int
	duration  time.Duration
}

var (
	pattern   = flag.String("pattern", "PAIR", "")
	transport = flag.String("transport", "tcp", "")
	msgSize   = flag.Int("msg-size", 64, "")
	duration  = flag.Int("duration", 5, "")
)

func main() {
	flag.Parse()

	loaded := perfcommon.LoadSingleConfig(
		*pattern,
		*transport,
		*msgSize,
		*duration,
	)
	cfg := benchmarkConfig{
		pattern:   loaded.Pattern,
		transport: loaded.Transport,
		msgSize:   loaded.MsgSize,
		duration:  loaded.Duration,
	}

	var result perfcommon.Result
	switch cfg.pattern {
	case "PAIR":
		result = runPair(cfg)
	case "PUBSUB":
		result = runPubSub(cfg)
	case "DEALER_DEALER":
		result = runDealerDealer(cfg)
	case "DEALER_ROUTER":
		result = runDealerRouter(cfg)
	case "ROUTER_ROUTER":
		result = runRouterRouter(cfg)
	default:
		perfcommon.Must(
			&unsupportedPatternError{pattern: cfg.pattern},
		)
	}

	// perf_single_one_way.hpp run_active_phase: received==0 / latency
	// count==0 is a FAIL (no RESULT line, nonzero exit), not 0.000.
	if !result.Valid {
		perfcommon.PrintFail(cfg.pattern, cfg.transport, cfg.msgSize)
		os.Exit(1)
	}
	result = perfcommon.FinalizeResult(cfg.pattern, cfg.msgSize, result)
	perfcommon.PrintResult(cfg.pattern, cfg.transport, cfg.msgSize, result)
}

type unsupportedPatternError struct {
	pattern string
}

func (e *unsupportedPatternError) Error() string {
	return "unsupported single perf pattern: " + e.pattern
}
