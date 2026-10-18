const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');

for (const applied of [false, true]) {
  for (const lostState of ['preparing', 'serving']) {
    test(`startup confirms a later descriptor after a lost ${lostState} write result, applied ${applied}`, async (t) => {
      const inner = new framework.ZLinkInMemoryProviderLocationStore();
      const repository = new framework.ZLinkLocationStoreRepository(inner);
      let lostWrite = false;
      let confirmationKey;
      const submissions = [];
      const provider = {
        read(key, signal) {
          if (key.value === confirmationKey) {
            confirmationKey = undefined;
            return Promise.reject(new Error('confirmation reply lost'));
          }
          return inner.read(key, signal);
        },
        scan: (request, signal) => inner.scan(request, signal),
        async write(request, signal) {
          const descriptor = request.mutations.find(
            (m) => m.kind === 'put' && m.key.value.startsWith('mesh-node')
          );
          const record = descriptor && JSON.parse(Buffer.from(descriptor.bytes).toString());
          if (record)
            submissions.push({
              state: record.descriptor.state,
              revision: record.descriptorRevision
            });
          if (!lostWrite && record?.descriptor.state === lostState) {
            lostWrite = true;
            if (applied) await inner.write(request, signal);
            confirmationKey = descriptor.key.value;
            throw new Error('descriptor write reply lost');
          }
          return await inner.write(request, signal);
        }
      };
      const host = new framework.ZLinkFrameworkRuntimeHost({
        registration: framework.createFrameworkRegistration({
          locations: { storeInstance: provider },
          spotNodes: { play: { router: { bind: 'tcp://127.0.0.1:0' }, objectRole: 'server' } }
        })
      });
      const observation = new AbortController();
      try {
        await host.start();
        assert.equal(lostWrite, true);
        if (lostState === 'preparing') {
          assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Serving);
          assert.equal(
            (await repository.listMeshNodes('play')).items[0].state,
            framework.ZLinkFrameworkRuntimeState.Serving
          );
          return;
        }
        assert.equal(host.status.state, framework.ZLinkFrameworkRuntimeState.Preparing);
        const iterator = host.routeMeshRuntime
          .observe('play', 64, observation.signal)
          [Symbol.asyncIterator]();
        const initial = (await iterator.next()).value.status;
        assert.equal(initial.state, framework.ZLinkTopologyState.Starting);
        assert.equal(initial.isReady, false);
        assert.equal(initial.sequence, 1n);
        assert.equal(
          (await repository.listMeshNodes('play')).items[0].state,
          applied
            ? framework.ZLinkFrameworkRuntimeState.Serving
            : framework.ZLinkFrameworkRuntimeState.Preparing
        );

        let recoveryFinished;
        const recovered = new Promise((resolve) => {
          recoveryFinished = resolve;
        });
        const resume = host.resumeOwnerLeaseOwnedWork.bind(host);
        host.resumeOwnerLeaseOwnedWork = async (...args) => {
          try {
            return await resume(...args);
          } finally {
            recoveryFinished();
          }
        };
        // Drive the real heartbeat renewal and its installed recovery handler;
        // its completion barrier fixes the order without elapsed-time assertions.
        await host.locationOwner.currentRuntime.renewOwnerLeaseOnce();
        await recovered;
        t.diagnostic(
          JSON.stringify({ applied, lostState, afterRecovery: host.status.state, submissions })
        );
        assert.equal(
          host.status.state,
          framework.ZLinkFrameworkRuntimeState.Serving,
          JSON.stringify(submissions)
        );
        const ready = (await iterator.next()).value.status;
        assert.equal(ready.isReady, true);
        assert.equal(ready.placement.isAvailable, true);
        assert.ok(ready.sequence > initial.sequence);
        assert.equal(
          (await repository.listMeshNodes('play')).items[0].state,
          framework.ZLinkFrameworkRuntimeState.Serving
        );
      } finally {
        observation.abort();
        await host.stop();
      }
    });
  }
}
