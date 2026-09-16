// Drives the emscripten-linked Unity WebGL harness from inside the page.
//
// Every call here crosses the real boundary: `ccall`/`cwrap` into the wasm
// module compiled from harness.c, which in turn calls the jslib functions that
// emscripten linked with --js-library. Nothing in this file touches the
// connector or the jslib directly.
//
// `frame()` is one Unity Update(): keep a Dispatch in flight, pump the boundary,
// transfer the copies. Between frames the driver yields to the browser so the
// WebSocket and the connector's promises can run, exactly as a real player does.
/* eslint-env browser */
/* global zlinkHarnessFramework */

(function (scope) {
  'use strict';

  function bind(module) {
    const api = {
      reset: module.cwrap('zlh_reset', null, []),
      create: module.cwrap('zlh_create', 'number', ['string']),
      takeLastError: module.cwrap('zlh_take_last_error', 'string', []),
      destroy: module.cwrap('zlh_destroy', null, []),
      frame: module.cwrap('zlh_frame', 'number', []),
      pump: module.cwrap('zlh_pump', 'number', []),
      connect: module.cwrap('zlh_connect', 'number', []),
      close: module.cwrap('zlh_close', 'number', []),
      dispatchCall: module.cwrap('zlh_dispatch_call', 'number', []),
      advanceIdle: module.cwrap('zlh_advance_idle', 'number', []),
      send: module.cwrap('zlh_send', 'number', ['string', 'string']),
      request: module.cwrap('zlh_request', 'number', ['string', 'string']),
      callState: module.cwrap('zlh_call_state', 'number', ['number']),
      callText: module.cwrap('zlh_call_text', 'string', ['number']),
      callPayload: module.cwrap('zlh_call_payload', 'string', ['number']),
      releaseCall: module.cwrap('zlh_release_call', null, ['number']),
      observe: module.cwrap('zlh_observe', 'number', ['string']),
      unobserve: module.cwrap('zlh_unobserve', null, ['string']),
      registerHandler: module.cwrap('zlh_register_handler', null, ['string']),
      pendingDispatch: module.cwrap('zlh_pending_dispatch', 'number', []),
      jslibPendingDispatch: module.cwrap('zlh_jslib_pending_dispatch', 'number', []),
      runDispatchQueue: module.cwrap('zlh_run_dispatch_queue', 'number', []),
      takeUnread: module.cwrap('zlh_take_unread', 'string', ['string']),
      receivedCount: module.cwrap('zlh_received_count', 'number', ['string']),
      isConnected: module.cwrap('zlh_is_connected', 'number', []),
      state: module.cwrap('zlh_state', 'number', []),
      closeReason: module.cwrap('zlh_close_reason', 'number', []),
      diagnosticsLevel: module.cwrap('zlh_diagnostics_level', 'number', []),
      setNestedPump: module.cwrap('zlh_set_nested_pump', null, ['number']),
      nestedPumpCalls: module.cwrap('zlh_nested_pump_calls', 'number', []),
      nestedPumpNotRefused: module.cwrap('zlh_nested_pump_not_refused', 'number', []),
      sinkCalls: module.cwrap('zlh_sink_calls', 'number', []),
      handlerRuns: module.cwrap('zlh_handler_runs', 'number', []),
      pumpFailures: module.cwrap('zlh_pump_failures', 'number', []),
      pumpFailureText: module.cwrap('zlh_pump_failure_text', 'string', []),
      handlerLog: module.cwrap('zlh_handler_log', 'string', []),
      stateLog: module.cwrap('zlh_state_log', 'string', []),
      disconnectCount: module.cwrap('zlh_disconnect_count', 'number', []),
      errorCount: module.cwrap('zlh_error_count', 'number', []),
      stateChangeCount: module.cwrap('zlh_state_change_count', 'number', []),
      violations: module.cwrap('zlh_violations', 'string', []),
      heapInUse: module.cwrap('zlh_heap_in_use', 'number', []),
      heapBreak: module.cwrap('zlh_heap_break', 'number', []),
      liveAllocs: module.cwrap('zlh_live_allocs', 'number', [])
    };

    const yieldToBrowser = () => new Promise((resolve) => setTimeout(resolve, 1));

    async function untilCallCompletes(callId, timeoutMs) {
      const deadline = Date.now() + (timeoutMs ?? 15_000);
      for (;;) {
        api.frame();
        const state = api.callState(callId);
        if (state === 1) {
          const result = { text: api.callText(callId), payload: api.callPayload(callId) };
          api.releaseCall(callId);
          return result;
        }
        if (state === 2) {
          const text = api.callText(callId);
          api.releaseCall(callId);
          const failure = new Error(text);
          failure.detail = text;
          throw failure;
        }
        if (Date.now() > deadline) throw new Error(`boundary call ${callId} did not complete`);
        await yieldToBrowser();
      }
    }

    async function untilTrue(predicate, what, timeoutMs) {
      const deadline = Date.now() + (timeoutMs ?? 15_000);
      for (;;) {
        api.frame();
        if (predicate()) return;
        if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
        await yieldToBrowser();
      }
    }

    return {
      raw: api,

      // Every scenario starts from a torn-down managed side.
      create(options) {
        api.reset();
        const handle = api.create(JSON.stringify(options));
        if (handle !== 0) return handle;
        const error = api.takeLastError();
        const failure = new Error(error || 'connector creation failed');
        failure.detail = error;
        throw failure;
      },

      async connect(timeoutMs) {
        await untilCallCompletes(api.connect(), timeoutMs);
      },

      async close(timeoutMs) {
        await untilCallCompletes(api.close(), timeoutMs);
      },

      async request(payload, call, timeoutMs) {
        return untilCallCompletes(api.request(JSON.stringify(call), payload), timeoutMs);
      },

      async send(payload, call, timeoutMs) {
        await untilCallCompletes(api.send(JSON.stringify(call), payload), timeoutMs);
      },

      // The wait surface: pump until the unread history has the message, then
      // consume it without ever running a handler.
      async waitFor(name, timeoutMs) {
        api.observe(name);
        let taken = '';
        await untilTrue(() => {
          taken = api.takeUnread(name);
          return taken !== '';
        }, `a '${name}' message`, timeoutMs);
        const [messageName, codec, ...rest] = taken.split('|');
        return { name: messageName, codec: Number(codec), payload: rest.join('|') };
      },

      async frames(count) {
        for (let index = 0; index < count; index += 1) {
          api.frame();
          await yieldToBrowser();
        }
      },

      async untilPending(timeoutMs) {
        await untilTrue(() => api.pendingDispatch() > 0, 'a queued callback', timeoutMs);
      },

      // Unity's Update(): Dispatch reads the transport, then the registered
      // handlers run. The connector permits one pending stream read, so this
      // waits for the Dispatch the frame loop already has in flight instead of
      // starting a second one.
      async dispatch(timeoutMs) {
        api.frame();
        const deadline = Date.now() + (timeoutMs ?? 15_000);
        for (;;) {
          api.pump();
          if (api.advanceIdle() === 1) break;
          if (Date.now() > deadline) throw new Error('the pending Dispatch did not settle');
          await yieldToBrowser();
        }
        return api.runDispatchQueue();
      },

      handlerLog() {
        const log = api.handlerLog();
        return log === '' ? [] : log.split('\n');
      },

      stateLog() {
        const log = api.stateLog();
        return log === '' ? [] : log.split('\n').map((line) => JSON.parse(line));
      },

      snapshot() {
        return {
          state: api.state(),
          isConnected: api.isConnected() === 1,
          closeReason: api.closeReason(),
          diagnosticsLevel: api.diagnosticsLevel(),
          pendingDispatch: api.pendingDispatch(),
          jslibPendingDispatch: api.jslibPendingDispatch(),
          sinkCalls: api.sinkCalls(),
          handlerRuns: api.handlerRuns(),
          pumpFailures: api.pumpFailures(),
          pumpFailureText: api.pumpFailureText(),
          disconnects: api.disconnectCount(),
          errors: api.errorCount(),
          stateChanges: api.stateChangeCount(),
          nestedPumpCalls: api.nestedPumpCalls(),
          nestedPumpNotRefused: api.nestedPumpNotRefused(),
          violations: api.violations(),
          heapInUse: api.heapInUse(),
          heapBreak: api.heapBreak(),
          liveAllocs: api.liveAllocs()
        };
      }
    };
  }

  scope.startZlinkUnityHarness = async function startZlinkUnityHarness() {
    const module = await zlinkHarnessFramework();
    const harness = bind(module);
    // Proof that --pre-js ran: both plugin globals exist inside the module scope
    // the jslib shares. The package README's manual check looks for exactly these
    // two names in the built framework.js.
    harness.linkedPlugins = {
      runtime: typeof globalThis.ZlinkStreamWebGlRuntime !== 'undefined',
      bundle: typeof globalThis.ZlinkStreamConnectorBundle !== 'undefined'
    };
    return harness;
  };
})(typeof window === 'undefined' ? globalThis : window);
