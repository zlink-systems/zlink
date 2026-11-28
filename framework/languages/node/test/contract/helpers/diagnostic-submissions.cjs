'use strict';

const {
  ZLinkMessageFlowDelivery
} = require('../../../packages/framework/dist/runtime/diagnostics/message-flow-delivery');

// Observe where a producer attempts submission. The delivery contract tests
// exercise the real process queue, lease checks and worker sink.
function observeDiagnosticSubmissions(visit, mock) {
  const prototype = ZLinkMessageFlowDelivery.prototype;
  const submit = prototype.submit;
  function observe(lease, record) {
    if ('marker' in record) {
      visit(record, lease);
      return;
    }
    submit.call(this, lease, record);
  }
  if (mock !== undefined) return mock.method(prototype, 'submit', observe);
  prototype.submit = observe;
  return () => { prototype.submit = submit; };
}

function debugArguments(record) {
  return [record.marker, record.message, { ...record.detail, ...record.error }];
}

module.exports = { observeDiagnosticSubmissions, debugArguments };
