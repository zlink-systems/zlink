const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');

test('ZLinkChannelClient rejects an invalid reply kind', async () => {
  await withMalformedChannelReply(
    'inproc://channel-invalid-reply-kind',
    encodeEnvelope({
      kind: 'response',
      channelName: 'api',
      messageName: 'Ping',
      contentType: 'application/json',
      correlationId: null,
      deadline: null,
      topic: null,
      errorCode: null,
      errorMessage: null
    }, { value: 'pong' }),
    /kind is not supported/
  );
});

test('ZLinkChannelClient rejects an unsafe reply body', async () => {
  await withMalformedChannelReply(
    'inproc://channel-unsafe-reply-body',
    [
      Buffer.from(JSON.stringify({
        formatMarker: 0xf2,
        flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
        flowOrigin: 1,
        kind: 2,
        channelName: 'api',
        messageName: 'Ping',
        contentType: 'application/json',
        correlationId: 'malformed-reply',
        deadline: null,
        topic: null,
        errorCode: null,
        errorMessage: null
      })),
      Buffer.from('{"__proto__":{"polluted":true}}')
    ],
    /not allowed/
  );
  assert.equal({}.polluted, undefined);
});

async function withMalformedChannelReply(endpoint, replyParts, expectedError) {
  const context = zlink.createContext();
  const dealer = zlink.createDealerSocket(context);
  const router = zlink.createRouterSocket(context);
  try {
    router.bind(endpoint);
    dealer.connect(endpoint);
    const registration = framework.createFrameworkRegistration({
      channels: { api: { client: { manualConnections: [endpoint] } } }
    });
    const client = new framework.DefaultZLinkChannelClient(
      registration,
      new framework.ZLinkDealerChannelClientTransport(dealer)
    );
    const reply = client.requestToChannel('api', ping()).timeout(1000).submit();
    const request = await receive(router);
    submitMultipart(router.reply(request.routingId, request.requestSeq), replyParts);
    await assert.rejects(() => withTimeout(reply, 1000), expectedError);
    request.close();
  } finally {
    dealer.close();
    router.close();
    context.close();
  }
}

function ping() {
  const Ping = { Ping: class {} }.Ping;
  return Object.assign(new Ping(), { value: 'ping' });
}

async function receive(router) {
  const deadline = Date.now() + 1000;
  while (Date.now() < deadline) {
    const received = new zlink.Received();
    if (router.recv(received, zlink.RecvFlags.DontWait)) return received;
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail('router did not receive request');
}

function encodeEnvelope(header, body) {
  return [
    Buffer.from(JSON.stringify({
      formatMarker: 0xf2,
      flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
      flowOrigin: 1,
      ...header
    })),
    Buffer.from(JSON.stringify(body))
  ];
}

function submitMultipart(operation, parts) {
  let current = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index += 1) {
    current = current.message(parts[index]);
  }
  current.submit();
}

function withTimeout(promise, timeoutMs) {
  let timeout;
  const guard = new Promise((_, reject) => {
    timeout = setTimeout(() => reject(new Error('malformed reply timed out')), timeoutMs);
  });
  return Promise.race([promise, guard]).finally(() => clearTimeout(timeout));
}
