import * as zlink from '@zlink-systems/zlink';

const ctx = zlink.createContext();
const routingId = zlink.RoutingId.from(Buffer.from('peer'));
const pair = zlink.createPairSocket(ctx);
const pub = zlink.createPubSocket(ctx);
const sub = zlink.createSubSocket(ctx);
const xsub = zlink.createXSubSocket(ctx);
const dealer = zlink.createDealerSocket(ctx);
const router = zlink.createRouterSocket(ctx);
const stream = zlink.createStreamSocket(ctx);
const monitor = pair.monitorOpen();
const monitorWithHwm = pair.monitorOpen(undefined, 12_345n);
const poller = zlink.createPoller();
const events = zlink.createPollEvents(8);
const timer = zlink.createTimer();

const pairSend: Promise<void> = pair.send().message('one').message(Buffer.from('two')).submit();
const pairSyncSend: void = pair.send().message('sync').submit(zlink.SendFlags.None);
const received = new zlink.Received();
pair.recv(received, zlink.RecvFlags.DontWait);
received.parts;
const receivedSendAsync: Promise<void> = received.send().message('async').submit();
const receivedSendSync: void = received.send().message('sync').submit(zlink.SendFlags.DontWait);
void receivedSendAsync;
void receivedSendSync;

const pubSend: void = pub.publish('topic').message('payload').submit();
sub.setSubscription('topic');
const topicMessage = new zlink.TopicMessage();
sub.subscribe(topicMessage, zlink.RecvFlags.DontWait);

dealer.setRoutingId(routingId);
const dealerSend: Promise<void> = dealer.send().message('dealer-send').submit();
const dealerSyncSend: void = dealer.send().message('dealer-sync').submit(zlink.SendFlags.DontWait);
const dealerRequest: Promise<zlink.Message[]> = dealer.request()
  .message('request')
  .timeout(1000)
  .submit();
dealer.setReceiveFlowState(zlink.ReceiveFlowState.RUNNING);
router.setReceiveFlowState(zlink.ReceiveFlowState.PAUSED);
const routerSend: Promise<void> = router.send(routingId).message('routed').submit();
const routerSyncSend: void = router.send(routingId).message('routed-sync').submit(zlink.SendFlags.None);
const routerRequest: Promise<zlink.Message[]> = router.request(routingId)
  .message('request')
  .timeout(1000)
  .submit();
const routerPairRequest: Promise<zlink.Message[]> = router.requestTransportPair(
  routingId,
  1n,
  1n
).message('request').submit();
const immediatePairSend: void = router.sendTransportPair(
  routingId,
  1n,
  1n,
  'immediate',
  zlink.SendFlags.DontWait
);
void pairSend;
void pairSyncSend;
void pubSend;
void dealerSend;
void dealerSyncSend;
void dealerRequest;
void routerSend;
void routerSyncSend;
void routerRequest;
void routerPairRequest;
void immediatePairSend;
// @ts-expect-error managed routed sends do not expose compatibility flags
dealer.send().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error managed send terminals accept only zero arguments or SendFlags
dealer.send().message('legacy').submit((_result: unknown) => {});
// @ts-expect-error managed requests do not expose compatibility flags
dealer.request().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error Promise submit is the sole managed request terminal
dealer.request().message('legacy').submit((_result, _parts) => {});
router.reply(routingId, 1n).message('reply').submit();

stream.setRoutingId(routingId);
const streamSend: Promise<void> = stream.send(routingId).message('packet').timeout(1).submit();
const streamSyncSend: void = stream.send(routingId).message('packet').submit(zlink.SendFlags.DontWait);
const streamTrySend: boolean = stream.trySend(routingId).message('packet').submit();
void streamTrySend;
void streamSend;
void streamSyncSend;
stream.setPacketHandler((_source, header, body) => {
  header.size();
  body.size();
});

monitor.recv(zlink.RecvFlags.DontWait);
const monitorStatus = monitor.status();
const flowPausedConnections: bigint = monitorStatus.flowPausedConnections;
const flowPauseAppliedTotal: bigint = monitorStatus.flowPauseAppliedTotal;
const flowResumeAppliedTotal: bigint = monitorStatus.flowResumeAppliedTotal;
const flowStateStaleTotal: bigint = monitorStatus.flowStateStaleTotal;
const flowPauseDurationMs: bigint = monitorStatus.flowPauseDurationMs;
void flowPausedConnections;
void flowPauseAppliedTotal;
void flowResumeAppliedTotal;
void flowStateStaleTotal;
void flowPauseDurationMs;
// @ts-expect-error Core-completion sends do not expose compatibility flags
pair.send().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error publish is synchronous and has no binding timeout stage
pub.publish('events').message('legacy').timeout(1);
// The flow events are part of the same MonitorEventType union as every
// other socket monitor lifecycle event, and MonitorEvent.value is a
// lossless uint64 bigint.
const flowEventTypes: zlink.MonitorEventType[] = [
  zlink.MonitorEventType.SendFlowPaused,
  zlink.MonitorEventType.SendFlowResumed,
  zlink.MonitorEventType.FlowStateStale
];
void flowEventTypes;
const flowEventFlags: zlink.MonitorEventFlag[] = [
  zlink.MonitorEventFlag.ConnectionReadyEdge,
  zlink.MonitorEventFlag.SendFlowWritable,
  zlink.MonitorEventFlag.FlowStateStaleGeneration,
  zlink.MonitorEventFlag.FlowStateStaleEpoch
];
void flowEventFlags;
const maybeEvent = monitor.recv(zlink.RecvFlags.DontWait);
if (maybeEvent) {
  const eventValue: bigint = maybeEvent.value;
  void eventValue;
}
poller.add(pair, [zlink.PollEventFlag.PollIn], 1);
poller.add(timer, 2);
poller.wait(events, 0);

ctx.shutdown();
timer.close();
events.close();
poller.close();
monitor.close();
monitorWithHwm.close();
stream.close();
router.close();
dealer.close();
xsub.close();
sub.close();
pub.close();
pair.close();
ctx.close();
