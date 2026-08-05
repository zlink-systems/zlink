const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkHandlerGroup,
  ZLinkRequest,
  ZLinkSend,
  exposeZLinkHandlers,
  invokeZLinkHandlerFilters,
  ZLinkFrameworkErrorKind,
  ZLinkHandlerDispatchKind,
  scanZLinkHandlerTypes
} = require('../../packages/framework/dist/internal');

test('handler scan does not expose handlers unless policy selects them', () => {
  class PublicHandler {}
  class PrivateHandler {}

  ZLinkHandlerGroup('api')(PublicHandler);
  ZLinkRequest('GetProfile')(PublicHandler.prototype, 'handle', descriptor());
  ZLinkSend('InternalCommand')(PrivateHandler.prototype, 'handle', descriptor());

  assert.equal(scanZLinkHandlerTypes([PublicHandler, PrivateHandler]).length, 2);
  assert.deepEqual(
    exposeZLinkHandlers([PublicHandler, PrivateHandler], { handlerGroups: ['api'] }).map((descriptor) => descriptor.handlerType),
    [PublicHandler]
  );
  assert.deepEqual(exposeZLinkHandlers([PublicHandler, PrivateHandler], { handlerGroups: [] }), []);
  assert.deepEqual(
    exposeZLinkHandlers([PublicHandler, PrivateHandler], { explicitHandlers: [PrivateHandler] }).map((descriptor) => descriptor.handlerType),
    [PrivateHandler]
  );
});

test('handler filters run before and after in registration order', async () => {
  const events = [];
  const context = {
    channelName: 'api',
    packetName: 'Ping',
    metadata: new Map(),
    dispatchKind: ZLinkHandlerDispatchKind.ChannelRequest
  };
  const filters = [
    {
      async invoke(actual, next) {
        assert.equal(actual, context);
        events.push('a:before');
        await next();
        events.push('a:after');
      }
    },
    {
      async invoke(actual, next) {
        assert.equal(actual, context);
        events.push('b:before');
        await next();
        events.push('b:after');
      }
    }
  ];

  const handlerInvoked = await invokeZLinkHandlerFilters(filters, context, async () => {
    events.push('handler');
  });

  assert.equal(handlerInvoked, true);
  assert.deepEqual(events, ['a:before', 'b:before', 'handler', 'b:after', 'a:after']);
});

test('handler filter short circuit cannot substitute a request result', async () => {
  let handlerCalls = 0;
  const handlerInvoked = await invokeZLinkHandlerFilters(
    [{
      async invoke() {
        return 42;
      }
    }],
    filterContext(),
    async () => {
      handlerCalls += 1;
    }
  );

  assert.equal(handlerInvoked, false);
  assert.equal(handlerCalls, 0);
});

test('handler filter atomically rejects concurrent duplicate next', async () => {
  let handlerCalls = 0;
  await assert.rejects(
    () => invokeZLinkHandlerFilters(
      [{
        async invoke(_context, next) {
          await Promise.all([next(), next()]);
        }
      }],
      filterContext(),
      async () => {
        handlerCalls += 1;
      }
    ),
    error => error.kind === ZLinkFrameworkErrorKind.InvalidOperation
  );
  assert.equal(handlerCalls, 1);
});

function filterContext() {
  return {
    channelName: 'api',
    packetName: 'Ping',
    metadata: new Map(),
    dispatchKind: ZLinkHandlerDispatchKind.ChannelRequest
  };
}

function descriptor() {
  return {
    configurable: true,
    enumerable: false,
    value() {},
    writable: true
  };
}
