// ZLink STREAM connector — Unity WebGL runtime state.
//
// Unity links this file with emscripten's --pre-js, so it runs inside the module
// body before the wasm runtime starts. It owns every connector instance, the
// JS -> C# event queue and the pump guard. It never touches the emscripten heap:
// all marshalling lives in ZlinkStreamConnector.jslib, which is the only place
// with HEAPU8, _malloc and the C# function pointer.
//
// Reentrancy rule for this whole adapter:
//
//   Nothing in this file ever calls into C#. WebSocket events, promise
//   continuations and the TypeScript connector's own handlers only append to
//   `events`. The C# sink is invoked from ZlinkStreamPump, which C# itself calls
//   from the Unity main thread, so a callback can never arrive on a browser
//   event stack while C# is somewhere else.
//
// The connector implementation comes from zlink-stream-connector.jspre, which is
// the IIFE build of the @zlink-systems/stream-connector package root. This file
// adds no wire behaviour of its own.
var ZlinkStreamWebGlRuntime = (function () {
  'use strict';

  var EVENT_CALL_COMPLETED = 1;
  var EVENT_MESSAGE = 2;
  var EVENT_ERROR_RECEIVED = 3;
  var EVENT_DISCONNECTED = 4;
  var EVENT_STATE_CHANGED = 5;
  var EVENT_ACTOR_BOUND = 6;
  var EVENT_ACTOR_UNBOUND = 7;
  var EVENT_REPLY_RECEIVED = 8;

  var instances = {};
  var nextHandle = 1;
  var lastError = null;

  function bundle() {
    if (typeof ZlinkStreamConnectorBundle !== 'undefined' && ZlinkStreamConnectorBundle) {
      return ZlinkStreamConnectorBundle;
    }
    if (typeof globalThis !== 'undefined' && globalThis.ZlinkStreamConnectorBundle) {
      return globalThis.ZlinkStreamConnectorBundle;
    }
    throw new Error(
      'zlink-stream-connector.jspre was not linked into this build. Check that ' +
      'Plugins/WebGL/*.jspre is included in the WebGL player build.'
    );
  }

  function describeError(cause) {
    var error = cause && cause.error ? cause.error : undefined;
    if (error && typeof error.code === 'string') {
      return { code: error.code, message: String(error.message || '') };
    }
    return {
      code: 'sendFailed',
      message: cause && cause.message ? String(cause.message) : String(cause)
    };
  }

  function instance(handle) {
    var found = instances[handle];
    if (!found) {
      throw new Error('Unknown ZLink stream connector handle ' + handle + '.');
    }
    return found;
  }

  function push(state, event) {
    // The only way an event reaches C#. Producers never invoke the sink.
    state.events.push(event);
  }

  function metadataToObject(metadata) {
    var values = {};
    if (metadata && metadata.values && typeof metadata.values.forEach === 'function') {
      metadata.values.forEach(function (value, key) { values[key] = value; });
    }
    return values;
  }

  function completeOk(state, callId, text, bytes) {
    push(state, {
      type: EVENT_CALL_COMPLETED,
      id: callId,
      value: 1,
      text: text === undefined ? null : text,
      bytes: bytes === undefined ? null : bytes
    });
  }

  function completeFailed(state, callId, cause) {
    push(state, {
      type: EVENT_CALL_COMPLETED,
      id: callId,
      value: 0,
      text: JSON.stringify(describeError(cause)),
      bytes: null
    });
  }

  function start(state, callId, run) {
    var controller = typeof AbortController === 'function' ? new AbortController() : undefined;
    state.calls[callId] = controller;
    var promise;
    try {
      promise = Promise.resolve(run(controller ? controller.signal : undefined));
    } catch (cause) {
      delete state.calls[callId];
      completeFailed(state, callId, cause);
      return;
    }
    promise.then(
      function (value) {
        delete state.calls[callId];
        if (value && value.payload instanceof Uint8Array) {
          completeOk(state, callId, JSON.stringify({ codec: value.codec }), value.payload);
        } else {
          completeOk(state, callId);
        }
      },
      function (cause) {
        delete state.calls[callId];
        completeFailed(state, callId, cause);
      }
    );
  }

  return {
    eventTypes: {
      callCompleted: EVENT_CALL_COMPLETED,
      message: EVENT_MESSAGE,
      errorReceived: EVENT_ERROR_RECEIVED,
      disconnected: EVENT_DISCONNECTED,
      stateChanged: EVENT_STATE_CHANGED,
      actorBound: EVENT_ACTOR_BOUND,
      actorUnbound: EVENT_ACTOR_UNBOUND,
      replyReceived: EVENT_REPLY_RECEIVED
    },

    takeLastError: function () {
      var error = lastError;
      lastError = null;
      return error;
    },

    // Records why ZlinkStreamPump failed, so the -2 it returns can be explained.
    // The error taxonomy in spec 32 section 9 describes stream errors; a pump
    // that throws is the adapter's own boundary breaking, so this is reported as
    // a message rather than dressed up as one of those codes.
    reportPumpFailure: function (cause) {
      lastError = JSON.stringify({
        code: 'pumpFailed',
        message: cause && cause.message ? String(cause.message) : String(cause)
      });
    },

    create: function (optionsJson) {
      var options;
      try {
        options = JSON.parse(optionsJson);
      } catch (cause) {
        lastError = JSON.stringify({ code: 'configurationError', message: 'Connector options are not valid JSON.' });
        return 0;
      }
      var connector;
      try {
        // ws:// and wss:// only. tcp:// and tls:// fail here with
        // ConfigurationError, which is the platform rule in spec 32 section 3.2;
        // the adapter does not re-check it, so the two stay in step.
        connector = bundle().zlinkStreamConnectorFactory.create(options);
      } catch (cause) {
        lastError = JSON.stringify(describeError(cause));
        return 0;
      }
      var handle = nextHandle++;
      var state = {
        connector: connector,
        events: [],
        calls: {},
        observers: {},
        nextObserverId: 1,
        actorHandles: {},
        actorHandleIds: new WeakMap(),
        currentActorHandles: {},
        nextActorHandle: 1,
        sink: null,
        pumping: false,
        subscriptions: []
      };
      state.subscriptions.push(connector.onErrorReceived(function (error) {
        push(state, {
          type: EVENT_ERROR_RECEIVED,
          id: 0,
          value: 0,
          text: JSON.stringify({ code: error.code, message: error.message || '' }),
          bytes: null
        });
      }));
      state.subscriptions.push(connector.onReplyReceived(function (context) {
        var reply = context.reply;
        push(state, {
          type: EVENT_REPLY_RECEIVED,
          id: 0,
          value: context.succeeded ? reply.payload.codec : -1,
          text: JSON.stringify({
            requestPacketName: context.requestPacketName,
            actorId: context.actorId || null,
            succeeded: context.succeeded,
            name: reply ? reply.name : null,
            metadata: reply ? metadataToObject(reply.metadata) : null,
            error: context.error ? describeError({ error: context.error }) : null,
            elapsed: context.elapsed
          }),
          bytes: reply ? reply.payload.payload : null
        });
      }));
      state.subscriptions.push(connector.onDisconnected(function () {
        push(state, {
          type: EVENT_DISCONNECTED,
          id: 0,
          value: 0,
          text: JSON.stringify({ closeReason: connector.closeReason || null }),
          bytes: null
        });
      }));
      state.subscriptions.push(connector.onConnectionStateChanged(function (change) {
        push(state, {
          type: EVENT_STATE_CHANGED,
          id: 0,
          value: 0,
          text: JSON.stringify({
            previous: change.previous,
            current: change.current,
            error: change.error ? { code: change.error.code, message: change.error.message || '' } : null
          }),
          bytes: null
        });
      }));
      state.subscriptions.push(connector.onActorBound(function (actor) {
        var actorHandle = state.nextActorHandle++;
        state.actorHandles[actorHandle] = actor;
        state.actorHandleIds.set(actor, actorHandle);
        state.currentActorHandles[actor.actorId] = actorHandle;
        push(state, {
          type: EVENT_ACTOR_BOUND,
          id: 0,
          value: 0,
          text: JSON.stringify({ actorId: actor.actorId, actorHandle: actorHandle }),
          bytes: null
        });
      }));
      state.subscriptions.push(connector.onActorUnbound(function (actor) {
        var actorHandle = state.actorHandleIds.get(actor);
        if (state.currentActorHandles[actor.actorId] === actorHandle) {
          delete state.currentActorHandles[actor.actorId];
        }
        push(state, {
          type: EVENT_ACTOR_UNBOUND,
          id: 0,
          value: 0,
          text: JSON.stringify({ actorId: actor.actorId, actorHandle: actorHandle }),
          bytes: null
        });
      }));
      instances[handle] = state;
      return handle;
    },

    destroy: function (handle) {
      var state = instances[handle];
      if (!state) return;
      delete instances[handle];
      for (var index = 0; index < state.subscriptions.length; index += 1) {
        try { state.subscriptions[index].dispose(); } catch (ignored) { /* teardown is best effort */ }
      }
      for (var name in state.observers) {
        if (Object.prototype.hasOwnProperty.call(state.observers, name)) {
          try { state.observers[name].subscription.dispose(); } catch (ignored) { /* teardown is best effort */ }
        }
      }
      state.events.length = 0;
      state.sink = null;
      try {
        var closing = state.connector.close();
        if (closing && typeof closing.catch === 'function') closing.catch(function () { /* already closed */ });
      } catch (ignored) { /* the connector may already be closed */ }
    },

    setEventSink: function (handle, sink) {
      instance(handle).sink = sink;
    },

    beginPump: function (handle) {
      var state = instances[handle];
      if (!state || state.pumping) return false;
      state.pumping = true;
      return true;
    },

    endPump: function (handle) {
      var state = instances[handle];
      if (state) state.pumping = false;
    },

    sink: function (handle) {
      return instance(handle).sink;
    },

    takeEvent: function (handle) {
      var state = instances[handle];
      if (!state || state.events.length === 0) return null;
      return state.events.shift();
    },

    connect: function (handle, callId) {
      var state = instance(handle);
      start(state, callId, function (signal) { return state.connector.connect(signal); });
    },

    close: function (handle, callId) {
      var state = instance(handle);
      start(state, callId, function (signal) { return state.connector.close(signal); });
    },

    dispatch: function (handle, callId) {
      var state = instance(handle);
      start(state, callId, function (signal) {
        // The connector queues inbound messages and hands them to the observer
        // callbacks through microtasks. Draining those microtasks before the
        // call completes keeps one Unity frame's Dispatch + Pump pair in step;
        // without it every push would arrive one frame late.
        return state.connector.dispatch(signal)
          .then(function () { return Promise.resolve(); })
          .then(function () { return Promise.resolve(); })
          .then(function () { return Promise.resolve(); });
      });
    },

    send: function (handle, callId, callJson, payload) {
      var state = instance(handle);
      var call = JSON.parse(callJson);
      start(state, callId, function () {
        var target = call.actorHandle == null ? state.connector : state.actorHandles[call.actorHandle];
        if (!target) throw actorNotBound(call.actorId);
        var builder = target.send({ codec: call.codec, payload: payload });
        if (call.packetName) builder = builder.packetName(call.packetName);
        builder = applyMetadata(builder, call.metadata);
        if (call.compress) builder = builder.compress();
        return builder.submit();
      });
    },

    request: function (handle, callId, callJson, payload) {
      var state = instance(handle);
      var call = JSON.parse(callJson);
      start(state, callId, function (signal) {
        var target = call.actorHandle == null ? state.connector : state.actorHandles[call.actorHandle];
        if (!target) throw actorNotBound(call.actorId);
        var builder = target.request({ codec: call.codec, payload: payload });
        if (call.packetName) builder = builder.packetName(call.packetName);
        builder = applyMetadata(builder, call.metadata);
        if (typeof call.timeoutMs === 'number') builder = builder.timeout(call.timeoutMs);
        if (call.compress) builder = builder.compress();
        return builder.submitEncoded(signal);
      });
    },

    cancel: function (handle, callId) {
      var state = instances[handle];
      if (!state) return;
      var controller = state.calls[callId];
      if (controller) controller.abort();
    },

    observe: function (handle, name) {
      var state = instance(handle);
      var existing = state.observers[name];
      if (existing) {
        existing.refCount += 1;
        return existing.id;
      }
      var id = state.nextObserverId++;
      // One TypeScript `on(name)` subscription per packet name. Every message
      // it sees is mirrored into the C# unread history, which is where the C#
      // On/WaitFor/ExpectNone/WaitForSequence/ReceivedCount surface reads from.
      // Predicates stay in C# because a JS-side predicate would have to call
      // back into C# from a promise continuation.
      var subscription = state.connector.on(name, function (message) {
        push(state, {
          type: EVENT_MESSAGE,
          id: id,
          value: message.payload.codec,
          text: JSON.stringify({
            name: message.name,
            metadata: metadataToObject(message.metadata),
            actorId: message.actorId || null,
            actorHandle: message.actorId ? state.currentActorHandles[message.actorId] || null : null
          }),
          bytes: message.payload.payload
        });
      });
      state.observers[name] = { id: id, refCount: 1, subscription: subscription };
      return id;
    },

    unobserve: function (handle, name) {
      var state = instances[handle];
      if (!state) return;
      var existing = state.observers[name];
      if (!existing) return;
      existing.refCount -= 1;
      if (existing.refCount > 0) return;
      delete state.observers[name];
      try { existing.subscription.dispose(); } catch (ignored) { /* teardown is best effort */ }
    },

    isConnected: function (handle) {
      return instance(handle).connector.isConnected ? 1 : 0;
    },

    state: function (handle) {
      return stateOrdinal(instance(handle).connector.state);
    },

    closeReason: function (handle) {
      return closeReasonOrdinal(instance(handle).connector.closeReason);
    },

    pendingDispatchCount: function (handle) {
      return instance(handle).connector.pendingDispatchCount | 0;
    },

    stateOrdinal: stateOrdinal,
    closeReasonOrdinal: closeReasonOrdinal
  };

  function applyMetadata(builder, metadata) {
    if (!metadata) return builder;
    for (var key in metadata) {
      if (Object.prototype.hasOwnProperty.call(metadata, key)) {
        builder = builder.metadata(key, metadata[key]);
      }
    }
    return builder;
  }

  function actorNotBound(actorId) {
    return {
      error: {
        code: 'validationFailed',
        message: "Actor '" + actorId + "' is no longer bound."
      }
    };
  }

  function stateOrdinal(value) {
    switch (value) {
      case 'created': return 0;
      case 'connecting': return 1;
      case 'connected': return 2;
      case 'reconnecting': return 3;
      case 'disconnected': return 4;
      case 'closed': return 5;
      default: return 0;
    }
  }

  function closeReasonOrdinal(value) {
    switch (value) {
      case 'ClientClose': return 0;
      case 'IdleTimeout': return 1;
      case 'HeartbeatTimeout': return 2;
      case 'ServerDrain': return 3;
      case 'ProtocolError': return 4;
      case 'TransportError': return 5;
      default: return -1;
    }
  }

})();

if (typeof globalThis !== 'undefined') { globalThis.ZlinkStreamWebGlRuntime = ZlinkStreamWebGlRuntime; }
