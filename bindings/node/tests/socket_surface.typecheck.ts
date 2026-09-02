import * as zlink from '@zlink-systems/zlink';

const ctx = zlink.createContext();
const routingId = zlink.RoutingId.from(Buffer.from('peer'));
const pair = zlink.createPairSocket(ctx);
const pub = zlink.createPubSocket(ctx);
const dealer = zlink.createDealerSocket(ctx);
const router = zlink.createRouterSocket(ctx);
const stream = zlink.createStreamSocket(ctx);
const monitor = pair.monitorOpen();
const poller = zlink.createPoller();
const events = zlink.createPollEvents(8);
const timer = zlink.createTimer();

const pairSend: Promise<void> = pair.send().message('one').message('two').submit();
const pairSync: void = pair.send().message('sync').submit_sync();
const dealerSend: Promise<void> = dealer.send().message('dealer').submit();
const dealerSync: void = dealer.send().message('dealer').submit_sync();
const routerSend: Promise<void> = router.send(routingId).message('router').submit();
const routerSync: void = router.send(routingId).message('router').submit_sync();
const streamSend: Promise<void> = stream.send(routingId).message('stream').submit();
const streamSync: void = stream.send(routingId).message('stream').submit_sync();
void pairSend; void pairSync; void dealerSend; void dealerSync;
void routerSend; void routerSync; void streamSend; void streamSync;

const dealerRequest: Promise<zlink.Message[]> = dealer.request()
  .message('request').timeout(1000).submit();
const dealerRequestSync: zlink.Message[] = dealer.request()
  .message('request').submit_sync();
const routerRequest: Promise<zlink.Message[]> = router.request(routingId)
  .message('request').timeout(1000).submit();
void dealerRequest; void dealerRequestSync; void routerRequest;

const received = new zlink.Received();
router.recv(received, zlink.RecvFlags.DontWait);
if (received.routingId && received.replyToken) {
  const reply: void = router.reply(received.routingId, received.replyToken)
    .message('reply').submit();
  const same: boolean = received.replyToken.equals(received.replyToken);
  const hash: number = received.replyToken.hashCode();
  void reply; void same; void hash;
}

stream.options.recvMode = zlink.StreamRecvMode.Packet;
const packet = new zlink.StreamPacket();
const packetReceived: boolean = stream.recvPacket(packet, zlink.RecvFlags.DontWait);
packet.routingId;
packet.header;
packet.body;
packet.close();
void packetReceived;

monitor.recv(zlink.RecvFlags.DontWait);
monitor.status();
poller.add(pair, [zlink.PollEventFlag.PollCompletion], 1);
poller.add(timer, 2);
poller.wait(events, 0);

// @ts-expect-error send has no timeout stage
pair.send().message('legacy').timeout(1);
// @ts-expect-error send terminal is flag-free
pair.send().message('legacy').submit_sync(zlink.SendFlags.DontWait);
// @ts-expect-error request has no flags stage
dealer.request().message('legacy').flags(zlink.SendFlags.DontWait);
// @ts-expect-error request submit accepts no callback
dealer.request().message('legacy').submit(() => {});
// @ts-expect-error request sync terminal is flag-free and callback-free
dealer.request().message('legacy').submit_sync(zlink.SendFlags.None, () => {});
// @ts-expect-error pair/generation request selector was removed
router.requestTransportPair(routingId, 1n, 1n);
// @ts-expect-error pair/generation send selector was removed
router.sendTransportPair(routingId, 1n, 1n, 'legacy');
// @ts-expect-error immediate STREAM send family was removed
stream.trySend(routingId);
// @ts-expect-error STREAM callback receive was removed
stream.setPacketHandler(() => {});
// @ts-expect-error monitor is pull-only
monitor.onEvent(() => {});
// @ts-expect-error timer is pull-only
timer.onFire(() => {});
// @ts-expect-error reply tokens have no public constructor
new zlink.ReplyToken();
// @ts-expect-error reply requires a ReplyToken, not a raw integer
router.reply(routingId, 1n);
// @ts-expect-error request sequence is no longer public
received.requestSeq;
// @ts-expect-error pair identity is no longer public
received.transportPairId;

ctx.shutdown();
packet.close(); timer.close(); events.close(); poller.close(); monitor.close();
stream.close(); router.close(); dealer.close(); pub.close(); pair.close(); ctx.close();
