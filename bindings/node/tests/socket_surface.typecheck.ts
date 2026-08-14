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

const pairSend: boolean = pair.send().message('one').message(Buffer.from('two')).submit();
const received = new zlink.Received();
pair.recv(received, zlink.RecvFlags.DontWait);
pair.recvRetained(received, zlink.RecvFlags.DontWait);
dealer.recvRetained(received, zlink.RecvFlags.DontWait);
router.recvRetained(received, zlink.RecvFlags.DontWait);
stream.recvRetained(received, zlink.RecvFlags.DontWait);
received.parts;

const pubSend: boolean = pub.publish('topic').message('payload').submit();
sub.setSubscription('topic');
const topicMessage = new zlink.TopicMessage();
sub.subscribe(topicMessage, zlink.RecvFlags.DontWait);
sub.subscribeRetained(topicMessage, zlink.RecvFlags.DontWait);
xsub.subscribeRetained(topicMessage, zlink.RecvFlags.DontWait);

dealer.setRoutingId(routingId);
const dealerSend: Promise<void> = dealer.send().message('dealer-send').submit();
const dealerRequest: Promise<zlink.Message[]> = dealer.request()
  .message('request')
  .timeout(1000)
  .submit();
const routerSend: Promise<void> = router.send(routingId).message('routed').submit();
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
void pubSend;
void dealerSend;
void dealerRequest;
void routerSend;
void routerRequest;
void routerPairRequest;
void immediatePairSend;
// @ts-expect-error managed routed sends do not expose compatibility flags
dealer.send().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error Promise submit is the sole managed send terminal
dealer.send().message('legacy').submit((_result: unknown) => {});
// @ts-expect-error managed requests do not expose compatibility flags
dealer.request().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error Promise submit is the sole managed request terminal
dealer.request().message('legacy').submit((_result, _parts) => {});
router.reply(routingId, 1n).message('reply').submit();

stream.setRoutingId(routingId);
const streamSend: Promise<void> = stream.send(routingId).message('packet').timeout(1).submit();
const streamTrySend: boolean = stream.trySend(routingId).message('packet').submit();
const publisherAsync: Promise<void> = pub.publishAsync('events').message('packet').submit();
void streamTrySend;
void publisherAsync;
void streamSend;
stream.setPacketHandler((_source, header, body) => {
  header.size();
  body.size();
});

monitor.recv(zlink.RecvFlags.DontWait);
monitor.status();
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
