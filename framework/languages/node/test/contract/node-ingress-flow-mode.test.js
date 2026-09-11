const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const {
  ZLinkRuntimeRouteTransport
} = require('../../packages/framework/dist/runtime/channels/channel-transports');
const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');

function capturingRouteTransport(flowCreationEnabled) {
  const sent = [];
  const node = {
    status() { return { routingId: 'source-node' }; },
    peers() { return [{ routingId: 'target-node' }]; },
    sendToNode(_target, parts) {
      sent.push(parts);
      return Promise.resolve(zlink.SubmitResult.Ok);
    }
  };
  return {
    sent,
    transport: new ZLinkRuntimeRouteTransport(
      () => undefined,
      undefined,
      () => ({ meshNode: () => node, meshCompletionTable: () => undefined }),
      undefined,
      undefined,
      undefined,
      undefined,
      flowCreationEnabled
    )
  };
}

function header(parts) {
  return JSON.parse(Buffer.from(parts[0]).toString('utf8'));
}

test('Node route transport reads the host-owned flow mode for each outbound envelope', async () => {
  let enabled = false;
  const { transport, sent } = capturingRouteTransport(() => enabled);
  try {
    await transport.submit('mesh', 'target-node', 'FlowProbe', { value: 1 });
    const off = header(sent[0]);
    assert.equal(off.flowId, undefined);
    assert.equal(off.flowOrigin, undefined);

    enabled = true;
    await transport.submit('mesh', 'target-node', 'FlowProbe', { value: 2 });
    const on = header(sent[1]);
    assert.match(
      on.flowId,
      /^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i
    );
    assert.equal(on.flowOrigin, 3);

    enabled = false;
    await transport.submit('mesh', 'target-node', 'FlowProbe', { value: 3 });
    const disabledAgain = header(sent[2]);
    assert.equal(disabledAgain.flowId, undefined);
    assert.equal(disabledAgain.flowOrigin, undefined);
  } finally {
    for (const parts of sent) envelope.closeMessages(parts);
  }
});
