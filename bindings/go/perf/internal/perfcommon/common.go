package perfcommon

import (
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	zlink "zlink.systems/zlink/v11"
)

var endpointCounter uint64

type Stats struct {
	count        uint64
	latencyCount uint64
	mu           sync.Mutex
	latNs        []float64
	sumNs        float64
}

type Result struct {
	Throughput   float64
	Bandwidth    float64
	LatencyNs    float64
	LatencyP95Ns float64
	LatencyP99Ns float64
	// Valid is false when no valid messages / latency samples were
	// recorded. perf_single_one_way.hpp run_active_phase returns false
	// when received==0 or latency count==0; that is a FAIL (no RESULT
	// line), not a 0.000 result.
	Valid bool
}

func NewStats() *Stats {
	return &Stats{
		latNs: make([]float64, 0),
	}
}

func (s *Stats) Add(sentAt time.Time) {
	s.AddLatencyNs(float64(time.Since(sentAt).Nanoseconds()))
}

func (s *Stats) AddLatencyNs(latencyNs float64) {
	atomic.AddUint64(&s.count, 1)
	atomic.AddUint64(&s.latencyCount, 1)
	s.mu.Lock()
	s.sumNs += latencyNs
	// perf_single_latency.hpp latency_stats_builder_t::add: unbounded
	// push_back, exact percentiles, no reservoir cap.
	s.latNs = append(s.latNs, latencyNs)
	s.mu.Unlock()
}

func (s *Stats) AddCount() uint64 {
	return atomic.AddUint64(&s.count, 1)
}

func (s *Stats) AddCountBy(delta uint64) {
	if delta == 0 {
		return
	}
	atomic.AddUint64(&s.count, delta)
}

func (s *Stats) AddLatencySampleNs(latencyNs float64) {
	atomic.AddUint64(&s.latencyCount, 1)
	s.mu.Lock()
	s.sumNs += latencyNs
	// perf_single_latency.hpp latency_stats_builder_t::add: unbounded
	// push_back, exact percentiles, no reservoir cap.
	s.latNs = append(s.latNs, latencyNs)
	s.mu.Unlock()
}

func (s *Stats) Snapshot(duration time.Duration, msgSize int) Result {
	s.mu.Lock()
	defer s.mu.Unlock()
	sort.Float64s(s.latNs)
	count := atomic.LoadUint64(&s.count)
	latencyCount := atomic.LoadUint64(&s.latencyCount)
	latencyMean := 0.0
	if latencyCount > 0 {
		latencyMean = s.sumNs / float64(latencyCount)
	}
	return Result{
		Throughput:   float64(count) / duration.Seconds(),
		Bandwidth:    float64(count*uint64(msgSize)) / duration.Seconds() / 1_000_000.0,
		LatencyNs:    latencyMean,
		LatencyP95Ns: percentile(s.latNs, 95),
		LatencyP99Ns: percentile(s.latNs, 99),
		Valid:        count > 0 && latencyCount > 0,
	}
}

// PrintFail mirrors perf_single_monitor.hpp print_failure_diagnostics:
// emit a FAIL line to stderr (no RESULT lines) and signal nonzero exit.
func PrintFail(pattern, transport string, msgSize int) {
	fmt.Fprintf(os.Stderr, "FAIL,current,%s,%s,%d\n", pattern, transport, msgSize)
}

func PrintResult(pattern, transport string, msgSize int, result Result) {
	fmt.Printf("RESULT,current,%s,%s,%d,throughput,%.3f\n", pattern, transport, msgSize, result.Throughput)
	fmt.Printf("RESULT,current,%s,%s,%d,bandwidth,%.3f\n", pattern, transport, msgSize, result.Bandwidth)
	fmt.Printf("RESULT,current,%s,%s,%d,latency,%.3f\n", pattern, transport, msgSize, result.LatencyNs/1_000_000.0)
	fmt.Printf("RESULT,current,%s,%s,%d,latency_p95,%.3f\n", pattern, transport, msgSize, result.LatencyP95Ns/1_000_000.0)
	fmt.Printf("RESULT,current,%s,%s,%d,latency_p99,%.3f\n", pattern, transport, msgSize, result.LatencyP99Ns/1_000_000.0)
}

func PrintSingleAutoHWMDetail(
	monitor *zlink.SocketMonitor,
	pattern string,
	transport string,
	component string,
	socketType zlink.SocketType,
	msgSize int,
) {
	if monitor == nil || pattern == "" || component == "" {
		return
	}
	snapshot, err := monitor.Status()
	if err != nil || !autoHWMStatusVisible(snapshot) {
		return
	}
	printAutoHWMDetail(pattern, transport, component, msgSize, "socket", 0, component, socketType, snapshot)
}

func autoHWMStatusVisible(snapshot *zlink.MonitorStatus) bool {
	return snapshot != nil &&
		(snapshot.AutoHwmAppliedSndHwmBytes > 0 ||
			snapshot.AutoHwmAppliedRcvHwmBytes > 0 ||
			snapshot.AutoHwmEffectiveMessageBytes > 0 ||
			snapshot.AutoHwmSocketMessageSlots > 0)
}

func printAutoHWMDetail(
	pattern string,
	transport string,
	component string,
	msgSize int,
	owner string,
	ownerID uint64,
	socketName string,
	socketType zlink.SocketType,
	snapshot *zlink.MonitorStatus,
) {
	if socketName == "" {
		socketName = component
	}
	fmt.Printf(
		"AUTO_HWM_DETAIL,pattern=%s,transport=%s,component=%s,msg_size=%d,owner=%s,owner_id=%d,socket=%s,socket_type=%s,role=%s,sndhwm=%d,rcvhwm=%d,effective_message_bytes=%d,effective_sndbuf=%d,effective_rcvbuf=%d,socket_message_slots=%d\n",
		pattern,
		transport,
		component,
		msgSize,
		owner,
		ownerID,
		socketName,
		socketTypeName(socketType),
		autoHWMRoleName(snapshot.AutoHwmRole),
		snapshot.AutoHwmAppliedSndHwmBytes,
		snapshot.AutoHwmAppliedRcvHwmBytes,
		snapshot.AutoHwmEffectiveMessageBytes,
		snapshot.AutoHwmEffectiveSndBuf,
		snapshot.AutoHwmEffectiveRcvBuf,
		snapshot.AutoHwmSocketMessageSlots,
	)
}

func autoHWMRoleName(role uint32) string {
	switch role {
	case 1:
		return "control"
	case 2:
		return "routed"
	case 3:
		return "fanout"
	case 4:
		return "recv_ingress"
	case 6:
		return "peer_queue"
	case 7:
		return "stream"
	default:
		return "none"
	}
}

func socketTypeName(socketType zlink.SocketType) string {
	switch socketType {
	case zlink.SocketTypePair:
		return "PAIR"
	case zlink.SocketTypePub:
		return "PUB"
	case zlink.SocketTypeSub:
		return "SUB"
	case zlink.SocketTypeDealer:
		return "DEALER"
	case zlink.SocketTypeRouter:
		return "ROUTER"
	case zlink.SocketTypeXPub:
		return "XPUB"
	case zlink.SocketTypeXSub:
		return "XSUB"
	case zlink.SocketTypeStream:
		return "STREAM"
	default:
		return "UNKNOWN"
	}
}

func Must(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func ValidateCommon(transport string, msgSize int) {
	if msgSize < MetricHeaderSize {
		Must(fmt.Errorf("msg-size must be >= %d, got %d", MetricHeaderSize, msgSize))
	}
	if strings.TrimSpace(transport) == "" {
		Must(fmt.Errorf("transport must not be empty"))
	}
}

const BenchmarkSocketTimeout = 200 * time.Millisecond

func singleSocketTimeout(send bool) time.Duration {
	if send {
		return durationFromEnv("PERF_SINGLE_SNDTIMEO_MS", BenchmarkSocketTimeout)
	}
	return durationFromEnv("PERF_SINGLE_RCVTIMEO_MS", BenchmarkSocketTimeout)
}

func multiSocketTimeout(send bool) time.Duration {
	if send {
		return durationFromEnv("PERF_MULTI_SNDTIMEO_MS", BenchmarkSocketTimeout)
	}
	return durationFromEnv("PERF_MULTI_RCVTIMEO_MS", BenchmarkSocketTimeout)
}

func SingleSendTimeout() time.Duration {
	return singleSocketTimeout(true)
}

func SingleReceiveTimeout() time.Duration {
	return singleSocketTimeout(false)
}

func MultiSendTimeout() time.Duration {
	return multiSocketTimeout(true)
}

func MultiReceiveTimeout() time.Duration {
	return multiSocketTimeout(false)
}

// bench_common_runtime.hpp bench_single_manual_socket_overrides_allowed /
// perf_multi_runtime.hpp bench_manual_socket_overrides_allowed: numeric
// HWM only applies when the manual-override env flag is "1"; otherwise
// context auto-HWM stays in effect.
func manualSocketOverridesAllowed(scopedEnv string) bool {
	value := os.Getenv(scopedEnv)
	if value == "" {
		value = os.Getenv("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES")
	}
	return value == "1"
}

// resolveManualSocketHWM mirrors resolve_single_socket_hwm /
// bench_hwm_from_env: base default is 0 (auto-HWM); send/recv envs
// override. Returns 0 when no numeric HWM should be set.
func resolveManualSocketHWM(baseEnv, sendEnv, recvEnv string, send bool) int {
	base := resolveIntEnv(baseEnv, 0)
	if send {
		return resolveIntEnv(sendEnv, base)
	}
	return resolveIntEnv(recvEnv, base)
}

func resolveIntEnv(name string, fallback int) int {
	raw := os.Getenv(name)
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 {
		return fallback
	}
	return value
}

type hwmSocket interface {
	SetSendHighWaterMark(int) error
	SetReceiveHighWaterMark(int) error
}

type benchmarkSocket interface {
	SetLinger(time.Duration) error
	SetSendTimeout(time.Duration) error
	SetReceiveTimeout(time.Duration) error
}

// ApplySingleHWM mirrors bench_common_runtime.hpp apply_single_hwm:
// numeric SNDHWM/RCVHWM are only set when the manual-override flag is
// enabled; otherwise context auto-HWM stays in effect.
func ApplySingleHWM(socket hwmSocket) {
	if socket == nil {
		return
	}
	if !manualSocketOverridesAllowed("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES") {
		return
	}
	if sndhwm := resolveManualSocketHWM("PERF_SINGLE_HWM", "PERF_SINGLE_SNDHWM", "PERF_SINGLE_RCVHWM", true); sndhwm > 0 {
		Must(socket.SetSendHighWaterMark(sndhwm))
	}
	if rcvhwm := resolveManualSocketHWM("PERF_SINGLE_HWM", "PERF_SINGLE_SNDHWM", "PERF_SINGLE_RCVHWM", false); rcvhwm > 0 {
		Must(socket.SetReceiveHighWaterMark(rcvhwm))
	}
}

// ApplySingleAutoHWMMsgUnit sets the context message-unit for the current
// payload size; numeric socket HWM remains behind the manual-override gate.
func ApplySingleAutoHWMMsgUnit(ctx *zlink.Context, msgSize int) {
	if ctx == nil || msgSize <= 0 {
		return
	}
	Must(ctx.Options().SetAutoHwmMsgUnitBytes(msgSize))
	Must(ctx.RecalculateAutoHwm())
}

// ApplyMultiHWM mirrors perf_multi_runtime.hpp apply_benchmark_hwm:
// numeric HWM only with manual override; otherwise auto-HWM stays.
func ApplyMultiHWM(socket hwmSocket, pattern string) {
	if socket == nil {
		return
	}
	if !manualSocketOverridesAllowed("PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES") {
		return
	}
	if sndhwm := resolveManualSocketHWM("PERF_MULTI_HWM", "PERF_MULTI_SNDHWM", "PERF_MULTI_RCVHWM", true); sndhwm > 0 {
		Must(socket.SetSendHighWaterMark(sndhwm))
	}
	if rcvhwm := resolveManualSocketHWM("PERF_MULTI_HWM", "PERF_MULTI_SNDHWM", "PERF_MULTI_RCVHWM", false); rcvhwm > 0 {
		Must(socket.SetReceiveHighWaterMark(rcvhwm))
	}
}

// ApplyMultiAutoHWMMsgUnit sets the context message-unit for the current
// payload size; numeric socket HWM remains behind the manual-override gate.
func ApplyMultiAutoHWMMsgUnit(ctx *zlink.Context, msgSize int) {
	if ctx == nil || msgSize <= 0 {
		return
	}
	Must(ctx.Options().SetAutoHwmMsgUnitBytes(msgSize))
	Must(ctx.RecalculateAutoHwm())
}

func ApplySingleBenchmarkSocketOptions(socket benchmarkSocket, _ string) {
	if socket == nil {
		return
	}
	Must(socket.SetLinger(0))
	Must(socket.SetSendTimeout(singleSocketTimeout(true)))
	Must(socket.SetReceiveTimeout(singleSocketTimeout(false)))
}

func ApplyMultiBenchmarkSocketOptions(socket benchmarkSocket, _ string) {
	if socket == nil {
		return
	}
	Must(socket.SetLinger(0))
	Must(socket.SetSendTimeout(multiSocketTimeout(true)))
	Must(socket.SetReceiveTimeout(multiSocketTimeout(false)))
}

type boundEndpointSocket interface {
	Bind(string) error
	LastEndpoint() (string, error)
}

func BindEndpoint(transport, prefix string) string {
	switch transport {
	case "tcp", "tls", "ws", "wss":
		return UniqueEndpoint(transport, prefix)
	case "inproc":
		id := atomic.AddUint64(&endpointCounter, 1)
		return fmt.Sprintf("inproc://%s-%d-%d", prefix, os.Getpid(), id)
	case "ipc":
		return UniqueEndpoint(transport, prefix)
	default:
		id := atomic.AddUint64(&endpointCounter, 1)
		return fmt.Sprintf("%s://%s-%d-%d", transport, prefix, os.Getpid(), id)
	}
}

func FinalizeResult(pattern string, msgSize int, result Result) Result {
	factor := 1.0
	if isEchoPattern(pattern) {
		factor = 2.0
	}
	result.Bandwidth = result.Throughput * float64(msgSize) * factor / 1_000_000.0
	return result
}

func isEchoPattern(pattern string) bool {
	switch pattern {
	case "MULTI_DEALER_ROUTER", "MULTI_ROUTER_ROUTER", "MULTI_STREAM":
		return true
	default:
		return false
	}
}

func BindAndResolveEndpoint(sock boundEndpointSocket, transport, prefix string) string {
	if sock == nil {
		Must(fmt.Errorf("bind socket must not be nil"))
	}
	endpoint := BindEndpoint(transport, prefix)
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "bind start transport=%s endpoint=%q\n", transport, endpoint)
	}
	Must(sock.Bind(endpoint))
	if transport == "inproc" {
		return endpoint
	}
	resolved, err := sock.LastEndpoint()
	Must(err)
	resolved = strings.TrimRight(resolved, "\x00")
	if transport == "ws" || transport == "wss" {
		resolved = strings.TrimRight(resolved, "/")
	}
	if transport == "tls" || transport == "wss" {
		resolved = strings.Replace(resolved, "://127.0.0.1:", "://localhost:", 1)
	}
	if os.Getenv("PERF_DEBUG") != "" {
		fmt.Fprintf(os.Stderr, "bind resolved transport=%s endpoint=%q resolved=%q\n", transport, endpoint, resolved)
	}
	return resolved
}

func UniqueEndpoint(transport, prefix string) string {
	id := atomic.AddUint64(&endpointCounter, 1)
	switch transport {
	case "tcp", "ws":
		listener, err := net.Listen("tcp", "127.0.0.1:0")
		Must(err)
		addr := listener.Addr().(*net.TCPAddr)
		_ = listener.Close()
		return fmt.Sprintf("%s://127.0.0.1:%d", transport, addr.Port)
	case "tls", "wss":
		listener, err := net.Listen("tcp", "127.0.0.1:0")
		Must(err)
		addr := listener.Addr().(*net.TCPAddr)
		_ = listener.Close()
		return fmt.Sprintf("%s://127.0.0.1:%d", transport, addr.Port)
	case "inproc":
		return fmt.Sprintf("inproc://%s-%d-%d", prefix, os.Getpid(), id)
	case "ipc":
		return fmt.Sprintf("ipc://%s", filepath.Join(os.TempDir(), fmt.Sprintf("%s-%d-%d.sock", prefix, os.Getpid(), id)))
	default:
		return fmt.Sprintf("%s://%s-%d-%d", transport, prefix, os.Getpid(), id)
	}
}

func OpenMonitor(socket zlink.SocketTarget) *zlink.SocketMonitor {
	mon, err := zlink.OpenSocketMonitor(socket, zlink.MonitorEventConnectionReady)
	Must(err)
	return mon
}

func PreparePayload(size int) []byte {
	return make([]byte, size)
}

func NewMessage(payload []byte) *zlink.Message {
	msg, err := zlink.NewMessage(payload)
	Must(err)
	return msg
}

func NewMessageWithSize(size int) *zlink.Message {
	msg, err := zlink.NewMessageWithSize(size)
	Must(err)
	return msg
}

func NewWindowMessage(size int, activeAt time.Time) *zlink.Message {
	msg := NewMessageWithSize(size)
	StampWindowPayload(msg.Data(), activeAt)
	return msg
}

func SubmitMessage(
	message *zlink.Message,
	submit func(*zlink.Message) (bool, error),
) (bool, error) {
	if message == nil {
		return false, fmt.Errorf("nil perf message")
	}
	sent, err := submit(message)
	if err != nil || !sent {
		_ = message.Close()
	}
	return sent, err
}

func SubmitPayload(
	payload []byte,
	submit func(*zlink.Message) (bool, error),
) (bool, error) {
	return SubmitMessage(NewMessage(payload), submit)
}

func SubmitWindowPayload(
	size int,
	activeAt time.Time,
	submit func(*zlink.Message) (bool, error),
) (bool, error) {
	return SubmitMessage(NewWindowMessage(size, activeAt), submit)
}

func NewSingleContext() (*zlink.Context, error) {
	return newContextWithIOThreads(1, "PERF_IO_THREADS")
}

func NewMultiContext() (*zlink.Context, error) {
	return newContextWithIOThreads(4, "PERF_IO_THREADS", "PERF_MULTI_DEFAULT_IO_THREADS", "PERF_DEFAULT_IO_THREADS")
}

func NewMultiServerContext() (*zlink.Context, error) {
	return newContextWithIOThreads(4, "PERF_MULTI_SERVER_IO_THREADS", "PERF_IO_THREADS", "PERF_MULTI_DEFAULT_IO_THREADS", "PERF_DEFAULT_IO_THREADS")
}

func NewMultiClientContext() (*zlink.Context, error) {
	return newContextWithIOThreads(4, "PERF_MULTI_CLIENT_IO_THREADS", "PERF_IO_THREADS", "PERF_MULTI_DEFAULT_IO_THREADS", "PERF_DEFAULT_IO_THREADS")
}

func newContextWithIOThreads(defaultThreads int, envNames ...string) (*zlink.Context, error) {
	ctx, err := zlink.NewContext()
	if err != nil {
		return nil, err
	}
	ioThreads := defaultThreads
	for _, name := range envNames {
		if value := resolveIntEnv(name, 0); value > 0 {
			ioThreads = value
			break
		}
	}
	if ioThreads > 0 {
		if err := ctx.Options().SetIOThreads(ioThreads); err != nil {
			_ = ctx.Close()
			return nil, err
		}
	}
	return ctx, nil
}

// IsTransient mirrors the C data-path retry classification: only
// EAGAIN/EWOULDBLOCK/EINTR are transient. Real connection errors
// (ENOTCONN/EHOSTUNREACH/ECONNREFUSED/ENETUNREACH/ENOENT) must fail,
// not silently retry. (perf_single_one_way.hpp send_socket_active_message
// / recv_single_part_header_flags; perf_multi_client_helpers.hpp
// classify_send_result.)
func IsTransient(err error) bool {
	var zerr zlink.ZlinkError
	if !errors.As(err, &zerr) {
		return false
	}
	switch zerr.InternalErrno() {
	// EWOULDBLOCK == EAGAIN on Linux; listing EAGAIN covers both.
	case int(syscall.EAGAIN), int(syscall.EINTR):
		return true
	default:
		return false
	}
}

// IsReadyProbeTransient is the ready-probe-only transient set:
// the ready-probe path allows
// ENOTCONN/EHOSTUNREACH/ENETUNREACH on the DONTWAIT ready-probe path
// (in addition to the data-path transients) while the peer is still
// connecting. It must NOT be used on the data path.
func IsReadyProbeTransient(err error) bool {
	if IsTransient(err) {
		return true
	}
	var zerr zlink.ZlinkError
	if !errors.As(err, &zerr) {
		return false
	}
	switch zerr.InternalErrno() {
	case int(syscall.ENOTCONN), int(syscall.EHOSTUNREACH), int(syscall.ENETUNREACH):
		return true
	default:
		return false
	}
}

func IsSubmitNotConnected(err error) bool {
	var submitErr *zlink.SubmitError
	return errors.As(err, &submitErr) && submitErr.Result == zlink.SubmitNotConnected
}
