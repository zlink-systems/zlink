const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');

test('Session shutdown diagnostics pair boundaries, identify failures, and remain silent when disabled', async (t) => {
  for (const scenario of ['disabled', 'resolved', 'session-rejected', 'actor-rejected']) {
    await t.test(scenario, async () => {
      const previous = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
      const originalConsole = console.error;
      const records = [];
      const calls = [];
      const failure = new TypeError('disconnect callback failed');
      const sessionId = 'shutdown-diagnostic-session';
      const actorId = 'shutdown-diagnostic-actor';
      const socket = {
        sendTimeoutMs: 0,
        sendHighWaterMark: 0,
        maxMessageSize: 65536,
        async submit() {
          calls.push('closing');
        },
        disconnectPeer(id) {
          assert.equal(id, sessionId);
          calls.push('disconnectPeer');
        },
        async bindActor() {},
        async unbindActor() {},
        async dispose() {}
      };
      let connected;
      const connection = new Promise((resolve) => {
        connected = resolve;
      });
      const bindingRuntime = new framework.ZLinkStreamBindingRuntime({
        async notifyDisconnected(actor) {
          assert.equal(actor.actorId, actorId);
          calls.push('actorDisconnected');
          if (scenario === 'actor-rejected') throw failure;
        }
      });
      const runtime = new framework.ZLinkStreamSessionNodeRuntime({
        socket,
        bindingRuntime,
        readablePoller: { dispose() {}, wake() {} },
        sessionFactory(context) {
          return {
            context,
            onConnected() {
              connected(context);
            },
            onDisconnected() {
              calls.push('sessionDisconnected');
              if (scenario === 'session-rejected') throw failure;
            }
          };
        }
      });
      process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = scenario === 'disabled' ? '0' : '1';
      console.error = (...args) => records.push(args);
      try {
        runtime.markConnected(sessionId);
        const context = await connection;
        await context.actors.bind({
          nodeRid: 'actor-node',
          actorId,
          objectGeneration: 1n,
          meshName: 'actors'
        });
        // Ignore binding diagnostics so the assertion covers shutdown alone.
        records.length = 0;
        calls.length = 0;
        const [session] = runtime.shutdownSnapshot();
        await runtime.drainCloseSessions();
        assert.deepEqual(calls, [
          'closing',
          'disconnectPeer',
          'sessionDisconnected',
          'actorDisconnected'
        ]);
        assert.equal(await bindingRuntime.find(actorId), undefined);
        if (scenario === 'disabled') {
          assert.deepEqual(records, []);
          return;
        }
        const events = records.filter(([prefix]) => prefix === '[zlink.runtime.relocation]');
        assert.equal(session.sessionId, sessionId);
        for (const field of [
          'connected',
          'disconnected',
          'disconnectQueued',
          'disposed',
          'transportClosed',
          'serialClosed',
          'serialActive',
          'serialActiveLifecycle'
        ])
          assert.equal(typeof session[field], 'boolean', field);
        assert.equal(session.connected, true);
        assert.equal(session.disconnected, false);
        assert.equal(session.transportClosed, false);
        assert.deepEqual(session.boundActors, [actorId]);

        const pairs = [
          ['shutdown.sessions.enter', 'shutdown.sessions.exit'],
          ['session.drainClose.enter', 'session.drainClose.exit'],
          ['drainClose.final.submit', 'drainClose.final.terminal'],
          ['drainClose.final.enter', 'drainClose.final.exit'],
          ['closeForDrain.submit.start', 'closeForDrain.submit.terminal'],
          ['closeForDrain.disconnectPeer.start', 'closeForDrain.disconnectPeer.terminal'],
          ['complete.onDisconnected.enter', 'complete.onDisconnected.exit'],
          ['session.cleanup.enter', 'session.cleanup.exit'],
          ['cleanupBindings.enter', 'cleanupBindings.exit'],
          ['relay.notifyPhysicalDisconnect.enter', 'relay.notifyPhysicalDisconnect.exit'],
          ['relay.lifecycle.submit', 'relay.lifecycle.terminal'],
          ['relay.lifecycle.enter', 'relay.lifecycle.exit'],
          ['relay.remoteDisconnect.start', 'relay.remoteDisconnect.terminal']
        ];
        for (const [start, terminal] of pairs) {
          const selected = events.filter(([, marker]) => marker === start || marker === terminal);
          assert.deepEqual(
            selected.map(([, marker]) => marker),
            [start, terminal],
            start
          );
          for (const [, marker, detail] of selected) {
            if (!marker.startsWith('shutdown.sessions.'))
              assert.equal(detail.sessionId, sessionId, marker);
          }
          assert.ok(['resolved', 'rejected'].includes(selected[1][2].outcome), terminal);
        }
        const ordered = [
          'shutdown.sessions.enter',
          'session.drainClose.enter',
          'drainClose.final.submit',
          'drainClose.final.enter',
          'closeForDrain.submit.start',
          'closeForDrain.submit.terminal',
          'closeForDrain.disconnectPeer.start',
          'closeForDrain.disconnectPeer.terminal',
          'complete.onDisconnected.enter',
          'complete.onDisconnected.exit',
          'session.cleanup.enter',
          'cleanupBindings.enter',
          'relay.notifyPhysicalDisconnect.enter',
          'relay.lifecycle.submit',
          'relay.lifecycle.enter',
          'relay.lifecycle.exit',
          'relay.lifecycle.terminal',
          'relay.remoteDisconnect.start',
          'relay.remoteDisconnect.terminal',
          'relay.notifyPhysicalDisconnect.exit',
          'cleanupBindings.exit',
          'session.cleanup.exit',
          'drainClose.final.exit',
          'drainClose.final.terminal',
          'session.drainClose.exit',
          'shutdown.sessions.exit'
        ];
        assert.deepEqual(
          events.map(([, marker]) => marker).filter((marker) => ordered.includes(marker)),
          ordered
        );
        const rejected = events.filter(([, , detail]) => detail.outcome === 'rejected');
        if (scenario === 'resolved') assert.deepEqual(rejected, []);
        else {
          const marker =
            scenario === 'session-rejected'
              ? 'complete.onDisconnected.exit'
              : 'relay.remoteDisconnect.terminal';
          const record = rejected.find(([, event]) => event === marker);
          assert.ok(record, marker);
          assert.equal(record[2].errorType, failure.name);
          assert.equal(record[2].sessionId, sessionId);
        }
      } finally {
        await runtime.dispose();
        console.error = originalConsole;
        if (previous === undefined) delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
        else process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = previous;
      }
    });
  }
});
