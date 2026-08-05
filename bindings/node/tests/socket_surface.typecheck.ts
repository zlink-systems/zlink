import * as zlink from '@zlink-systems/zlink';

const ctx = zlink.createContext();
const routingId = zlink.RoutingId.from(Buffer.from('peer'));
const pair = zlink.createPairSocket(ctx);
const pub = zlink.createPubSocket(ctx);
const sub = zlink.createSubSocket(ctx);
const dealer = zlink.createDealerSocket(ctx);
const router = zlink.createRouterSocket(ctx);
const stream = zlink.createStreamSocket(ctx);
const monitor = pair.monitorOpen();
const poller = zlink.createPoller();
const events = zlink.createPollEvents(8);
const timer = zlink.createTimer();

pair.send().message('one').message(Buffer.from('two')).submit();
const received = new zlink.Received();
pair.recv(received, zlink.RecvFlags.DontWait);
received.parts;

pub.publish('topic').message('payload').submit();
sub.setSubscription('topic');
const topicMessage = new zlink.TopicMessage();
sub.subscribe(topicMessage, zlink.RecvFlags.DontWait);

dealer.setRoutingId(routingId);
dealer.request().message('request').timeout(1000).submit();
router.send(routingId).message('routed').submit();
router.request(routingId).message('request').timeout(1000).submit();
router.reply(routingId, 1n).message('reply').submit();

stream.setRoutingId(routingId);
stream.send(routingId).message('packet').submit();
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
stream.close();
router.close();
dealer.close();
sub.close();
pub.close();
pair.close();
ctx.close();
