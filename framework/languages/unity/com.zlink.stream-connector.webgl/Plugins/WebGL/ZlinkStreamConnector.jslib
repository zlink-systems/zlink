// ZLink STREAM connector — Unity WebGL jslib boundary.
//
// Unity links this file with emscripten's --js-library. Every function here is
// callable from C# through [DllImport("__Internal")]; see
// Runtime/Interop/ZlinkStreamInterop.cs for the matching declarations.
//
// This file is the only place that touches the emscripten heap. The connector
// state machine is in ZlinkStreamRuntime.jspre and the connector itself is the
// package-root bundle in zlink-stream-connector.jspre.
//
// ---------------------------------------------------------------------------
// Buffer ownership
// ---------------------------------------------------------------------------
// C# -> JS (ZlinkStreamSend, ZlinkStreamRequest):
//   `payload` points into HEAPU8 and is owned by C#. This file copies the bytes
//   out synchronously, before returning, and never keeps the pointer. C# frees
//   its buffer as soon as the call returns.
//
// JS -> C# (ZlinkStreamPump):
//   This file allocates the text and payload buffers with _malloc, hands the
//   pointers to the C# sink, and frees them in a finally block as soon as the
//   sink returns. The pointers are valid only for the duration of that one call,
//   so C# must copy what it needs before returning. Nothing in the adapter frees
//   these buffers from the C# side.
//
// JS -> C# (ZlinkStreamTakeLastError):
//   The returned char* is allocated here and ownership passes to C#, which frees
//   it with ZlinkStreamFreeBuffer. This is the one buffer C# owns.
//
// ---------------------------------------------------------------------------
// Reentrancy
// ---------------------------------------------------------------------------
// ZlinkStreamPump is the only function that calls into C#, and C# is the only
// caller of ZlinkStreamPump. WebSocket events, promise continuations and the
// TypeScript connector's own handlers never reach C# directly; they append to a
// JS queue that this function drains. ZlinkStreamWebGlRuntime.beginPump refuses
// a nested pump (returns -1 to C#), so C# code running inside the sink cannot
// re-enter the drain loop.
mergeInto(LibraryManager.library, {

  ZlinkStreamCreate: function (optionsPtr) {
    try {
      return ZlinkStreamWebGlRuntime.create(UTF8ToString(optionsPtr));
    } catch (error) {
      console.error('ZlinkStreamCreate failed', error);
      return 0;
    }
  },

  ZlinkStreamDestroy: function (handle) {
    try {
      ZlinkStreamWebGlRuntime.destroy(handle);
    } catch (error) {
      console.error('ZlinkStreamDestroy failed', error);
    }
  },

  // Returns a malloc'd UTF-8 string that C# owns and frees with
  // ZlinkStreamFreeBuffer, or 0 when no error is pending.
  ZlinkStreamTakeLastError: function () {
    var text = ZlinkStreamWebGlRuntime.takeLastError();
    if (!text) return 0;
    var size = lengthBytesUTF8(text) + 1;
    var buffer = _malloc(size);
    stringToUTF8(text, buffer, size);
    return buffer;
  },

  ZlinkStreamFreeBuffer: function (buffer) {
    if (buffer) _free(buffer);
  },

  // `callbackPtr` is a C# static method marshalled as a function pointer. The
  // signature is (int,int,int,int,int,int,int) -> void.
  //
  // Unity's documented form is {{{ makeDynCall('viiiiiii', 'callbackPtr') }}},
  // but that macro is expanded by the emscripten link step and cannot run in the
  // Node harness that tests this boundary. Resolving the callable once, here,
  // works with every emscripten link mode the Unity versions in range emit:
  // wasmTable.get for the default build, dynCall for -sDYNCALLS builds.
  ZlinkStreamSetEventSink: function (handle, callbackPtr) {
    var invoke = null;
    if (callbackPtr) {
      var table = (typeof wasmTable !== 'undefined' && wasmTable)
        ? wasmTable
        : ((typeof Module !== 'undefined' && Module.wasmTable) ? Module.wasmTable : null);
      if (table && typeof table.get === 'function') {
        var callable = table.get(callbackPtr);
        invoke = function (a, b, c, d, e, f, g) { callable(a, b, c, d, e, f, g); };
      } else if (typeof dynCall === 'function') {
        invoke = function (a, b, c, d, e, f, g) {
          dynCall('viiiiiii', callbackPtr, [a, b, c, d, e, f, g]);
        };
      } else if (typeof Module !== 'undefined' && typeof Module.dynCall === 'function') {
        invoke = function (a, b, c, d, e, f, g) {
          Module.dynCall('viiiiiii', callbackPtr, [a, b, c, d, e, f, g]);
        };
      } else {
        console.error('ZlinkStreamSetEventSink: no way to call a wasm function pointer.');
        return 0;
      }
    }
    try {
      ZlinkStreamWebGlRuntime.setEventSink(handle, invoke);
      return 1;
    } catch (error) {
      console.error('ZlinkStreamSetEventSink failed', error);
      return 0;
    }
  },

  // Drains at most `maxEvents` queued events into the C# sink and returns how
  // many were delivered, or -1 when a pump is already running on this stack.
  ZlinkStreamPump: function (handle, maxEvents) {
    if (!ZlinkStreamWebGlRuntime.beginPump(handle)) return -1;
    var dispatched = 0;
    try {
      var sink = ZlinkStreamWebGlRuntime.sink(handle);
      if (!sink) return 0;
      while (dispatched < maxEvents) {
        var event = ZlinkStreamWebGlRuntime.takeEvent(handle);
        if (!event) break;
        var textPtr = 0;
        var bytesPtr = 0;
        var bytesLength = 0;
        try {
          if (event.text) {
            var textSize = lengthBytesUTF8(event.text) + 1;
            textPtr = _malloc(textSize);
            stringToUTF8(event.text, textPtr, textSize);
          }
          if (event.bytes && event.bytes.length > 0) {
            bytesLength = event.bytes.length;
            bytesPtr = _malloc(bytesLength);
            // HEAPU8 is read here and not cached: a managed allocation inside
            // the sink can grow the wasm memory and replace the view object.
            HEAPU8.set(event.bytes, bytesPtr);
          }
          sink(handle, event.type, event.id, event.value, textPtr, bytesPtr, bytesLength);
        } finally {
          if (textPtr) _free(textPtr);
          if (bytesPtr) _free(bytesPtr);
        }
        dispatched += 1;
      }
    } catch (error) {
      console.error('ZlinkStreamPump failed', error);
    } finally {
      ZlinkStreamWebGlRuntime.endPump(handle);
    }
    return dispatched;
  },

  ZlinkStreamConnect: function (handle, callId) {
    ZlinkStreamWebGlRuntime.connect(handle, callId);
  },

  ZlinkStreamClose: function (handle, callId) {
    ZlinkStreamWebGlRuntime.close(handle, callId);
  },

  ZlinkStreamDispatch: function (handle, callId) {
    ZlinkStreamWebGlRuntime.dispatch(handle, callId);
  },

  ZlinkStreamCancel: function (handle, callId) {
    ZlinkStreamWebGlRuntime.cancel(handle, callId);
  },

  // `payload` is owned by C# and is copied out before this function returns.
  ZlinkStreamSend: function (handle, callId, callJsonPtr, payload, payloadLength) {
    ZlinkStreamWebGlRuntime.send(
      handle,
      callId,
      UTF8ToString(callJsonPtr),
      HEAPU8.slice(payload, payload + payloadLength)
    );
  },

  // `payload` is owned by C# and is copied out before this function returns.
  ZlinkStreamRequest: function (handle, callId, callJsonPtr, payload, payloadLength) {
    ZlinkStreamWebGlRuntime.request(
      handle,
      callId,
      UTF8ToString(callJsonPtr),
      HEAPU8.slice(payload, payload + payloadLength)
    );
  },

  ZlinkStreamObserve: function (handle, namePtr) {
    return ZlinkStreamWebGlRuntime.observe(handle, UTF8ToString(namePtr));
  },

  ZlinkStreamUnobserve: function (handle, namePtr) {
    ZlinkStreamWebGlRuntime.unobserve(handle, UTF8ToString(namePtr));
  },

  ZlinkStreamIsConnected: function (handle) {
    return ZlinkStreamWebGlRuntime.isConnected(handle);
  },

  ZlinkStreamGetState: function (handle) {
    return ZlinkStreamWebGlRuntime.state(handle);
  },

  ZlinkStreamGetCloseReason: function (handle) {
    return ZlinkStreamWebGlRuntime.closeReason(handle);
  },

  ZlinkStreamGetPendingDispatchCount: function (handle) {
    return ZlinkStreamWebGlRuntime.pendingDispatchCount(handle);
  },

  ZlinkStreamGetDiagnosticsLevel: function (handle) {
    return ZlinkStreamWebGlRuntime.diagnosticsLevel(handle);
  },

  ZlinkStreamSetDiagnosticsLevel: function (handle, level) {
    return ZlinkStreamWebGlRuntime.setDiagnosticsLevel(handle, level);
  }
});
