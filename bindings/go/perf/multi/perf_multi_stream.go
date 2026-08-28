package main

import (
	"context"
	"fmt"
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
	stopSender := startMultiStreamEchoServer(server)
	flushControlLine("READY,%s", endpoint)
	waitForStopToken()
	stopSender()
}

type pendingStreamPacket struct {
	source zlink.RoutingID
	packet *zlink.Message
}

func startMultiStreamEchoServer(server *zlink.StreamSocket) func() {
	pending := make(chan pendingStreamPacket, 65536)
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			select {
			case <-stop:
				return
			case item := <-pending:
				for {
					_, sendErr := server.SendTo(item.source).Message(item.packet).Submit(context.Background())
					if sendErr == nil {
						break
					}
					if perfcommon.IsSubmitNotConnected(sendErr) {
						_ = item.packet.Close()
						break
					}
					if !perfcommon.IsTransient(sendErr) {
						perfcommon.Must(fmt.Errorf("multi stream server send: %w", sendErr))
					}
					perfcommon.PollIdle(time.Millisecond)
				}
			}
		}
	}()

	perfcommon.Must(server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		packet := perfcommon.FrameStreamPacketMessage(header, body)
		_ = header.Close()
		_ = body.Close()
		select {
		case pending <- pendingStreamPacket{source: source, packet: packet}:
		case <-stop:
			_ = packet.Close()
		}
	}))
	return func() {
		close(stop)
		<-done
	}
}
