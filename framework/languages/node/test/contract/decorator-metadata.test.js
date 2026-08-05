const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkHandlerGroup,
  ZLinkPacket,
  ZLinkRequest,
  ZLinkSend,
  readZLinkDecoratorMetadata
} = require('../../packages/framework/dist');

test('handler decorators work without exposing scanner metadata', () => {
  class Handler {}

  ZLinkHandlerGroup('api')(Handler);
  ZLinkPacket('profile.changed')(Handler);
  ZLinkRequest('getProfile')(Handler.prototype, 'getProfile', descriptor());
  ZLinkSend('updateProfile')(Handler.prototype, 'updateProfile', descriptor());

  assert.equal(readZLinkDecoratorMetadata, undefined);
});

function descriptor() {
  return {
    configurable: true,
    enumerable: false,
    value() {},
    writable: true
  };
}
