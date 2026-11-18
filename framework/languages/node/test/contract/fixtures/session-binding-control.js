const assert = require('node:assert/strict');
const {
  ApplicationIngressRecordOwner
} = require('../../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ReceiveKind
} = require('../../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  RequestResult
} = require('../../../packages/framework/dist/runtime/backend/runtime-values');

// These transport tests bind once without a current Session or user callback.
// Model the Host's infrastructure consumer, including its reply and job release.
function acceptSessionBindingControl(record) {
  try {
    assert.equal(record.domain, 'infrastructure');
    assert.equal(record.stateful.receiveKind, ReceiveKind.ActorBinding);
    assert.equal(record.stateful.kindData.transition, 'active');
    assert.equal(record.stateful.reply(RequestResult.Ok, 0), true);
  } finally {
    record.applicationJob.close();
  }
}

function sessionBindingControlPorts() {
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration());
  return {
    async reserveLocalIngress() {
      const permit = await queue.acquire(undefined, 'local');
      return ApplicationIngressRecordOwner.create(queue, permit, { close() {} });
    },
    mailbox: {
      tryEnqueue(record) {
        queueMicrotask(() => acceptSessionBindingControl(record));
        return true;
      }
    }
  };
}

module.exports = { acceptSessionBindingControl, sessionBindingControlPorts };
