const { performance } = require('node:perf_hooks');
const deferred = require('../helpers/deferred.cjs');

function startupClaim(t, store) {
  let now = 0;
  t.mock.method(performance, 'now', () => now);
  const started = deferred();
  const cancelled = deferred();
  const commit = deferred();
  const committed = deferred();
  const calls = [];
  return {
    started: started.promise,
    cancelled: cancelled.promise,
    calls,
    commitAfterRenewTimeout() {
      now = 4000;
      commit.resolve();
      return committed.promise;
    },
    leaseStore: {
      async claimOwnerLease(ownerId, ttl, signal) {
        signal.addEventListener('abort', cancelled.resolve, { once: true });
        started.resolve();
        await commit.promise;
        try {
          return await store.claimOwnerLease(ownerId, ttl);
        } finally {
          committed.resolve();
        }
      },
      async readOwnerLease(ownerId, signal) {
        calls.push('read');
        return await store.readOwnerLease(ownerId, signal);
      },
      async releaseOwnerLease(token, signal) {
        calls.push('release');
        return await store.releaseOwnerLease(token, signal);
      },
      async renewOwnerLease(...args) {
        calls.push('renew');
        return await store.renewOwnerLease(...args);
      }
    }
  };
}

module.exports = { startupClaim };
