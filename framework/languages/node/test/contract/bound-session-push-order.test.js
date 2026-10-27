const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const {
  ZLinkBoundSessionService
} = require('../../packages/framework/dist/runtime/streams/bound-session-service');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
const {
  ZLinkStreamFrameMessageFactory
} = require('../../packages/framework/dist/runtime/streams/stream-frame-factory');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  decodeStreamFrame,
  decodeStreamHeader,
  encodeActorBoundFrame,
  encodeActorUnboundFrame
} = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');

const operations = {
  submit: (service) => service.submitLocalBoundSession('actor', {}, 'push', new Map()),
  send: (service) => service.sendLocalBoundSession('actor', {}, 'push', new Map()),
  response: (service) =>
    service.sendLocalBoundSessionResponse('actor', 'push', 1n, {}, new Map(), false),
  error: (service) =>
    service.sendLocalBoundSessionError('actor', 'push', 1n, new Error('failure'), new Map())
};

async function fixture(onSubmit = () => Promise.resolve()) {
  const frames = [];
  const registry = new ZLinkActorSessionBindingRegistry();
  const stream = new ZLinkManagedStream(
    {
      submit(_routingId, frame) {
        const header = decodeStreamHeader(decodeStreamFrame(frame.data()).header);
        frames.push(header);
        return onSubmit(header);
      }
    },
    'session'
  );
  const context = {
    routingId: 'session',
    stream,
    bindLocal() {},
    unbindLocal() {},
    actorSlotControls: {
      enqueueBound: (slot, actorId) => control(encodeActorBoundFrame(slot, actorId)),
      enqueueUnbound: (slot) => control(encodeActorUnboundFrame(slot))
    }
  };
  async function control(bytes) {
    const frame = ZLinkBufferMessage.from(bytes);
    try {
      await stream.submitRaw(frame);
    } finally {
      frame.close();
    }
  }
  const factory = new ZLinkStreamFrameMessageFactory({ flowCreationEnabled: () => false });
  const service = new ZLinkBoundSessionService(registry, factory, {});
  await registry.bind(
    context,
    { actorId: 'actor', ref: { actorId: 'actor', bindingGeneration: 1n } },
    'token'
  );
  const slot = (await registry.route('actor')).actorSlot;
  frames.length = 0;
  return { registry, context, factory, service, frames, slot };
}

for (const [name, send] of Object.entries(operations)) {
  test(`${name}: current check and slotted submission precede concurrent unbound`, async () => {
    const { registry, context, factory, service, frames, slot } = await fixture();
    let unbind;
    const create = factory.createJsonFrameMessage.bind(factory);
    factory.createJsonFrameMessage = (...args) => {
      const frame = create(...args);
      // Queue unbind after the final admission turn starts, before its caller resumes.
      queueMicrotask(() => {
        unbind = registry.unbind('actor', context, 'token');
      });
      return frame;
    };
    const result = await send(service);
    await unbind;
    assert.equal(
      name === 'submit' ? result.status : result,
      name === 'submit' ? ZLinkSubmitStatus.Submitted : true
    );
    assert.equal(frames.length, 2);
    assert.equal(frames[0].actorSlot, slot);
    assert.equal(frames[1].name, '$zlink.actor.unbound');
  });

  test(`${name}: unbind before admission rejects the old slot`, async () => {
    const { registry, context, factory, service, frames } = await fixture();
    let unbind;
    const create = factory.createJsonFrameMessage.bind(factory);
    factory.createJsonFrameMessage = (...args) => {
      const frame = create(...args);
      unbind = registry.unbind('actor', context, 'token');
      return frame;
    };
    const result = await send(service);
    await unbind;
    assert.equal(
      name === 'submit' ? result.status : result,
      name === 'submit' ? ZLinkSubmitStatus.TargetNotFound : false
    );
    assert.deepEqual(
      frames.map((header) => header.name),
      ['$zlink.actor.unbound']
    );
  });

  test(`${name}: pending transport completion releases the lane and retains the frame`, async () => {
    let submitted;
    let complete;
    let closed = false;
    const entered = new Promise((resolve) => {
      submitted = resolve;
    });
    const completion = new Promise((resolve) => {
      complete = resolve;
    });
    const { registry, context, factory, service, frames, slot } = await fixture((header) => {
      if (header.actorSlot === undefined) return Promise.resolve();
      submitted();
      return completion;
    });
    const create = factory.createJsonFrameMessage.bind(factory);
    factory.createJsonFrameMessage = (...args) => {
      const frame = create(...args);
      const close = frame.close.bind(frame);
      frame.close = () => {
        closed = true;
        close();
      };
      return frame;
    };
    const sending = send(service);
    try {
      await entered;
      await registry.unbind('actor', context, 'token');
      assert.equal(closed, false);
      assert.equal(frames[0].actorSlot, slot);
      assert.equal(frames[1].name, '$zlink.actor.unbound');
    } finally {
      complete();
      await sending;
    }
    assert.equal(closed, true);
  });
}
