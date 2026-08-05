package main

import (
	"context"

	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
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
	perfcommon.ApplyMultiAutoHWMMsgUnit(ctx, cfg.msgSize)

	server, err := ctx.StreamSocket()
	perfcommon.Must(err)
	defer server.Close()

	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	perfcommon.ApplyMultiHWM(server, cfg.pattern)
	perfcommon.ApplyMultiBenchmarkSocketOptions(server, cfg.transport)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-multi-stream")
	startMultiStreamEchoServer(server)
	flushControlLine("READY,%s", endpoint)
	waitForStopToken()
}

func startMultiStreamEchoServer(server *zlink.StreamSocket) {
	perfcommon.Must(server.OnPacket(func(source zlink.RoutingID, header, body *zlink.Message) {
		packet := perfcommon.FrameStreamPacketMessage(header, body)
		_ = header.Close()
		_ = body.Close()
		_, sendErr := server.SendTo(source).Message(packet).Submit(context.Background())
		perfcommon.Must(sendErr)
	}))
}
