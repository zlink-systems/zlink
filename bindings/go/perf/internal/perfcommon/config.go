package perfcommon

import (
	"os"
	"strconv"
	"strings"
	"time"
)

type SingleConfig struct {
	Pattern   string
	Transport string
	MsgSize   int
	Duration  time.Duration
}

type MultiConfig struct {
	Pattern   string
	Transport string
	MsgSize   int
	Duration  time.Duration
	Clients   int
}

func LoadSingleConfig(pattern, transport string, msgSize, duration int) SingleConfig {
	resolvedDuration := duration
	if resolvedDuration <= 0 {
		resolvedDuration = resolvePositiveEnv("PERF_SINGLE_DURATION_SECONDS", 5)
	}
	cfg := SingleConfig{
		Pattern:   strings.ToUpper(pattern),
		Transport: strings.ToLower(transport),
		MsgSize:   msgSize,
		Duration:  time.Duration(resolvedDuration) * time.Second,
	}
	ValidateCommon(cfg.Transport, cfg.MsgSize)
	return cfg
}

func LoadMultiConfig(pattern, transport string, msgSize, duration int, clients int) MultiConfig {
	resolvedPattern := strings.ToUpper(pattern)
	resolvedDuration := duration
	if resolvedDuration <= 0 {
		resolvedDuration = resolvePositiveEnv("PERF_MULTI_DURATION_SECONDS", 5)
	}
	resolvedClients := clients
	if resolvedClients <= 0 {
		defaultClients := 100
		if resolvedPattern == "MULTI_STREAM" {
			defaultClients = 10000
		}
		resolvedClients = resolvePositiveEnv("PERF_MULTI_CLIENTS", defaultClients)
	}
	cfg := MultiConfig{
		Pattern:   resolvedPattern,
		Transport: strings.ToLower(transport),
		MsgSize:   msgSize,
		Duration:  time.Duration(resolvedDuration) * time.Second,
		Clients:   resolvedClients,
	}
	ValidateCommon(cfg.Transport, cfg.MsgSize)
	if cfg.Clients <= 0 {
		Must(&invalidClientCountError{Clients: cfg.Clients})
	}
	return cfg
}

type invalidClientCountError struct {
	Clients int
}

func (e *invalidClientCountError) Error() string {
	return "clients must be > 0"
}

func resolvePositiveEnv(name string, fallback int) int {
	raw := strings.TrimSpace(os.Getenv(name))
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return value
}
