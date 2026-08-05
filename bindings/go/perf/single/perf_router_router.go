package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"syscall"
	"time"
	zlink "zlink.systems/zlink/v11"
	"zlink.systems/zlink/v11/perf/internal/perfcommon"
)

func runRouterRouter(cfg benchmarkConfig) perfcommon.Result {
	debug := os.Getenv("PERF_DEBUG") != ""
	debugf := func(format string, args ...any) {
		if debug {
			fmt.Fprintf(os.Stderr, format+"\n", args...)
		}
	}

	ctx, err := perfcommon.NewSingleContext()
	perfcommon.Must(err)
	defer ctx.Close()
	perfcommon.ApplySingleAutoHWMMsgUnit(ctx, cfg.msgSize)

	server, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer server.Close()
	client, err := ctx.RouterSocket()
	perfcommon.Must(err)
	defer client.Close()
	serverMon := perfcommon.OpenMonitor(server)
	defer serverMon.Close()
	clientMon := perfcommon.OpenMonitor(client)
	defer clientMon.Close()

	serverID := zlink.NewRoutingID([]byte("ROUTER1"))
	clientID := zlink.NewRoutingID([]byte("ROUTER2"))

	debugf("router/router tls setup server")
	perfcommon.Must(perfcommon.ConfigureTLSServer(server, cfg.transport))
	debugf("router/router tls setup client")
	perfcommon.Must(perfcommon.ConfigureTLSClient(client, cfg.transport))
	perfcommon.ApplySingleHWM(server)
	perfcommon.ApplySingleHWM(client)
	endpoint := perfcommon.BindAndResolveEndpoint(server, cfg.transport, "perf-router-router")
	debugf("router/router set server routing id")
	perfcommon.Must(server.SetRoutingID(serverID))
	debugf("router/router set client routing id")
	perfcommon.Must(client.SetRoutingID(clientID))
	perfcommon.Must(server.SetMandatory(true))
	perfcommon.Must(client.SetMandatory(true))
	debugf("router/router set connect routing id")
	perfcommon.Must(client.SetConnectRoutingID(serverID))
	debugf("router/router connect %s", endpoint)
	perfcommon.Must(client.Connect(endpoint))
	debugf("router/router apply benchmark socket options")
	perfcommon.ApplySingleBenchmarkSocketOptions(server, cfg.transport)
	perfcommon.ApplySingleBenchmarkSocketOptions(client, cfg.transport)
	debugf("router/router wait connected")
	perfcommon.WaitConnectedWithTimeout(perfcommon.SingleReadyTimeout(), serverMon, clientMon)
	debugf("router/router wait route ready")
	targetID := waitRouterRouterRouteReady(server, client, serverID)

	result := runSingleRoutedOneWayWithTransient(cfg, server, func(message *zlink.Message) (bool, error) {
		return client.SendTo(targetID).MoveMessage(message).Submit(context.Background())
	}, func(message *zlink.Message) error {
		_, err := client.SendTo(targetID).MoveMessage(message).Submit(context.Background())
		return err
	}, isRouterRouterSendTransient)
	perfcommon.PrintSingleAutoHWMDetail(serverMon, cfg.pattern, cfg.transport, "receiver", zlink.SocketTypeRouter, cfg.msgSize)
	perfcommon.PrintSingleAutoHWMDetail(clientMon, cfg.pattern, cfg.transport, "sender", zlink.SocketTypeRouter, cfg.msgSize)
	return result
}

func isRouterRouterSendTransient(err error) bool {
	if perfcommon.IsTransient(err) {
		return true
	}
	var zerr zlink.ZlinkError
	if !errors.As(err, &zerr) {
		return false
	}
	switch zerr.InternalErrno() {
	case int(syscall.EHOSTUNREACH), int(syscall.ENOTCONN):
		return true
	default:
		return false
	}
}

func waitRouterRouterRouteReady(
	server *zlink.RouterSocket,
	client *zlink.RouterSocket,
	serverID zlink.RoutingID,
) zlink.RoutingID {
	deadline := perfcommon.SingleReadyTimeout()
	ping := []byte("PING")
	pong := []byte("PONG")
	stopAt := time.Now().Add(deadline)
	serverPoller := perfcommon.NewSocketPoller(server, perfcommon.ZLinkPollIn)
	defer serverPoller.Close()
	clientPoller := perfcommon.NewSocketPoller(client, perfcommon.ZLinkPollIn)
	defer clientPoller.Close()
	serverEvents := make([]zlink.PollEvent, 1)
	clientEvents := make([]zlink.PollEvent, 1)

	for time.Now().Before(stopAt) {
		_, sendErr := perfcommon.SubmitMessage(perfcommon.NewMessage(ping), func(message *zlink.Message) (bool, error) {
			return client.SendTo(serverID).MoveMessage(message).Submit(context.Background())
		})
		if sendErr != nil && !perfcommon.IsTransient(sendErr) {
			perfcommon.Must(sendErr)
		}
		event, waitErr := perfcommon.WaitPollerOne(serverPoller, serverEvents, time.Until(stopAt))
		if waitErr != nil {
			if perfcommon.IsTransient(waitErr) {
				continue
			}
			perfcommon.Must(waitErr)
		}
		if event == nil || event.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		var received zlink.Received
		var clientID zlink.RoutingID
		for {
			ok, recvErr := server.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if perfcommon.IsTransient(recvErr) {
					break
				}
				perfcommon.Must(recvErr)
			}
			if !ok {
				break
			}
			part, partErr := received.SinglePartOrError()
			if partErr == nil && string(part.Data()) == string(ping) && received.HasRoutingID() {
				clientID = received.RoutingID()
			}
			_ = received.Close()
		}
		if clientID.Size() == 0 {
			continue
		}
		_, err := perfcommon.SubmitMessage(perfcommon.NewMessage(pong), func(message *zlink.Message) (bool, error) {
			return server.SendTo(clientID).MoveMessage(message).Submit(context.Background())
		})
		perfcommon.Must(err)
		clientEvent, clientWaitErr := perfcommon.WaitPollerOne(clientPoller, clientEvents, time.Until(stopAt))
		if clientWaitErr != nil {
			if perfcommon.IsTransient(clientWaitErr) {
				continue
			}
			perfcommon.Must(clientWaitErr)
		}
		if clientEvent == nil || clientEvent.Revents&perfcommon.ZLinkPollIn == 0 {
			continue
		}
		for {
			ok, recvErr := client.Recv(&received, zlink.RecvFlagsDontWait)
			if recvErr != nil {
				if perfcommon.IsTransient(recvErr) {
					break
				}
				perfcommon.Must(recvErr)
			}
			if !ok {
				break
			}
			part, partErr := received.SinglePartOrError()
			targetID := received.RoutingID()
			isPong := partErr == nil && string(part.Data()) == string(pong)
			_ = received.Close()
			if isPong && targetID.Size() > 0 {
				return targetID
			}
		}
	}
	perfcommon.Must(fmt.Errorf("router/router perf endpoint did not complete reverse route probe"))
	return serverID
}
