package main

import (
	"context"
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"

	zlink "zlink.systems/zlink"
	"zlink.systems/zlink/perf/internal/perfcommon"
)

// PERF_MULTI_TEST_POLICY / perf_multi_stream_server.cpp: the measured
// surface for MULTI_STREAM is the Go STREAM *server* / packet handler.
// The client role is the shared C reference binary
// bindings/c/perf/common/streamclient/perf_stream_client, spawned by
// run_benchmarks_multi.sh (mirroring the dotnet runner). There is no
// Go STREAM client reimplementation.
func runMultiStreamServer(cfg multiConfig) {
	ctx, err := perfcommon.NewMultiServerContext()
	perfcommon.Must(err)
	defer ctx.Close()

	server, err := ctx.StreamSocket()
	perfcommon.Must(err)
	defer server.Close()

	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-stream")
	stopSender, senderErrors := startMultiStreamEchoServer(server)
	flushControlLine("READY,%s", endpoint)
	var activeErr error
	select {
	case <-waitForStopAsync():
	case activeErr = <-senderErrors:
	}
	stopErr := stopSender()
	if activeErr != nil {
		perfcommon.Must(activeErr)
	}
	perfcommon.Must(stopErr)
}

func startMultiStreamEchoServer(server *zlink.StreamSocket) (func() error, <-chan error) {
	dispatch := newMultiStreamRouteDispatch(server)

	perfcommon.Must(server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		packet := perfcommon.FrameStreamPacketMessage(header, body)
		_ = header.Close()
		_ = body.Close()
		dispatch.enqueue(source, packet)
	}))
	return dispatch.stop, dispatch.errors
}

type multiStreamPacketNode struct {
	packet *zlink.Message
	next   *multiStreamPacketNode
}

// A route owns exactly one blocking Submit goroutine. Its linked queue is
// intentionally unbounded: application code imposes no fixed in-flight window,
// while Core/HWM blocks only this route's sender when it cannot admit more data.
type multiStreamRouteSender struct {
	dispatch *multiStreamRouteDispatch
	source   zlink.RoutingID

	mu       sync.Mutex
	ready    *sync.Cond
	head     *multiStreamPacketNode
	tail     *multiStreamPacketNode
	stopping bool
	closed   bool
}

type multiStreamRouteDispatch struct {
	submit func(zlink.RoutingID, *zlink.Message) (bool, error)

	mu        sync.Mutex
	accepting bool
	routes    map[zlink.RoutingID]*multiStreamRouteSender
	firstErr  error
	senders   sync.WaitGroup
	errors    chan error
}

func newMultiStreamRouteDispatch(server *zlink.StreamSocket) *multiStreamRouteDispatch {
	return newMultiStreamRouteDispatchWithSubmit(
		func(source zlink.RoutingID, packet *zlink.Message) (bool, error) {
			return server.SendTo(source).MoveMessage(packet).Submit(context.Background())
		},
	)
}

func newMultiStreamRouteDispatchWithSubmit(
	submit func(zlink.RoutingID, *zlink.Message) (bool, error),
) *multiStreamRouteDispatch {
	return &multiStreamRouteDispatch{
		submit:    submit,
		accepting: true,
		routes:    make(map[zlink.RoutingID]*multiStreamRouteSender),
		errors:    make(chan error, 1),
	}
}

func (d *multiStreamRouteDispatch) enqueue(source zlink.RoutingID, packet *zlink.Message) {
	d.mu.Lock()
	if !d.accepting {
		d.mu.Unlock()
		_ = packet.Close()
		return
	}
	route := d.routes[source]
	if route == nil {
		route = &multiStreamRouteSender{dispatch: d, source: source}
		route.ready = sync.NewCond(&route.mu)
		d.routes[source] = route
		d.senders.Add(1)
		go route.run()
	}
	d.mu.Unlock()

	if !route.enqueue(packet) {
		_ = packet.Close()
	}
}

func (r *multiStreamRouteSender) enqueue(packet *zlink.Message) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.stopping || r.closed {
		return false
	}
	node := &multiStreamPacketNode{packet: packet}
	if r.tail == nil {
		r.head = node
	} else {
		r.tail.next = node
	}
	r.tail = node
	r.ready.Signal()
	return true
}

func (r *multiStreamRouteSender) next() (*zlink.Message, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for r.head == nil && !r.stopping && !r.closed {
		r.ready.Wait()
	}
	if r.closed || (r.head == nil && r.stopping) {
		return nil, false
	}
	node := r.head
	r.head = node.next
	if r.head == nil {
		r.tail = nil
	}
	node.next = nil
	return node.packet, true
}

func (r *multiStreamRouteSender) run() {
	defer r.dispatch.routeDone(r)
	for {
		packet, ok := r.next()
		if !ok {
			return
		}
		if !r.submit(packet) {
			return
		}
	}
}

func (r *multiStreamRouteSender) submit(packet *zlink.Message) bool {
	defer packet.Close()
	for {
		sent, sendErr := r.dispatch.submit(r.source, packet)
		if sendErr == nil {
			if sent {
				return true
			}
			// A normal blocking STREAM backpressure result restores MoveMessage
			// ownership. Retry on this same route while active. Once stop begins,
			// abandon this route so teardown cannot spin behind a disconnected or
			// permanently backpressured client.
			if r.dispatch.canContinue() {
				continue
			}
			r.abort()
			return false
		}
		if perfcommon.IsStaleRoute(sendErr) {
			// The connection vanished after callback admission. Drop this route's
			// queued echoes without affecting any other route sender.
			r.abort()
			return false
		}
		r.dispatch.recordError(fmt.Errorf("multi stream server send: %w", sendErr))
		r.abort()
		return false
	}
}

func (r *multiStreamRouteSender) stop() {
	r.mu.Lock()
	r.stopping = true
	r.ready.Broadcast()
	r.mu.Unlock()
}

func (r *multiStreamRouteSender) abort() {
	r.mu.Lock()
	if r.closed {
		r.mu.Unlock()
		return
	}
	r.closed = true
	r.stopping = true
	head := r.head
	r.head = nil
	r.tail = nil
	r.ready.Broadcast()
	r.mu.Unlock()

	closeMultiStreamPacketNodes(head)
}

func closeMultiStreamPacketNodes(head *multiStreamPacketNode) {
	for head != nil {
		next := head.next
		_ = head.packet.Close()
		head.packet = nil
		head.next = nil
		head = next
	}
}

func (d *multiStreamRouteDispatch) canContinue() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.accepting
}

func (d *multiStreamRouteDispatch) recordError(err error) {
	d.mu.Lock()
	if d.firstErr != nil {
		d.mu.Unlock()
		return
	}
	d.firstErr = err
	d.accepting = false
	d.mu.Unlock()
	d.errors <- err
}

func (d *multiStreamRouteDispatch) routeDone(route *multiStreamRouteSender) {
	route.abort()
	d.mu.Lock()
	if d.routes[route.source] == route {
		delete(d.routes, route.source)
	}
	d.mu.Unlock()
	d.senders.Done()
}

func (d *multiStreamRouteDispatch) stop() error {
	d.mu.Lock()
	d.accepting = false
	routes := make([]*multiStreamRouteSender, 0, len(d.routes))
	for _, route := range d.routes {
		routes = append(routes, route)
	}
	d.mu.Unlock()

	for _, route := range routes {
		route.stop()
	}

	done := make(chan struct{})
	go func() {
		d.senders.Wait()
		close(done)
	}()

	timer := time.NewTimer(multiStreamShutdownTimeout())
	defer timer.Stop()
	select {
	case <-done:
		d.mu.Lock()
		err := d.firstErr
		d.mu.Unlock()
		return err
	case <-timer.C:
		return fmt.Errorf("multi stream server send drain timed out")
	}
}

func multiStreamShutdownTimeout() time.Duration {
	const fallback = 5 * time.Second
	raw := os.Getenv("PERF_MULTI_SERVER_SHUTDOWN_TIMEOUT_MS")
	if raw == "" {
		raw = os.Getenv("PERF_SERVER_SHUTDOWN_TIMEOUT_MS")
	}
	if raw == "" {
		return fallback
	}
	milliseconds, err := strconv.Atoi(raw)
	if err != nil || milliseconds < 0 {
		return fallback
	}
	return time.Duration(milliseconds) * time.Millisecond
}
