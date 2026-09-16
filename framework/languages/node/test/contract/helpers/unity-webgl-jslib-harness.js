// Loads the Unity WebGL UPM package's jslib boundary outside Unity.
//
// Unity is not available in this repository's CI, and the C# side and the actual
// WebGL player build are verified by hand (see the package README checklist).
// What is verified automatically is the boundary itself: this harness supplies
// the emscripten runtime symbols the jslib uses - mergeInto, UTF8ToString,
// stringToUTF8, lengthBytesUTF8, _malloc, _free, HEAPU8 and a wasm function
// table - and then drives the exported functions exactly the way the generated
// C# P/Invoke stubs do.
//
// The heap is a real Uint8Array with a bump allocator that zero-fills on free,
// so a use-after-free at the boundary shows up as zeroed bytes rather than as a
// test that happens to pass.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const packageRoot = path.resolve(
  __dirname,
  '../../../../unity/com.zlink.stream-connector.webgl'
);

const HEAP_SIZE = 8 * 1024 * 1024;

function createHarness() {
  const heap = new Uint8Array(HEAP_SIZE);
  const blocks = new Map();
  let bump = 16;

  function malloc(size) {
    const length = Math.max(1, size | 0);
    if (bump + length >= HEAP_SIZE) throw new Error('harness heap exhausted');
    const pointer = bump;
    bump += length + 8 - ((bump + length) % 8);
    blocks.set(pointer, length);
    return pointer;
  }

  function free(pointer) {
    const length = blocks.get(pointer);
    if (length === undefined) throw new Error(`double free or bad pointer ${pointer}`);
    heap.fill(0, pointer, pointer + length);
    blocks.delete(pointer);
  }

  function utf8ToString(pointer) {
    if (!pointer) return '';
    let end = pointer;
    while (end < HEAP_SIZE && heap[end] !== 0) end += 1;
    return Buffer.from(heap.subarray(pointer, end)).toString('utf8');
  }

  function stringToUTF8(text, pointer, maxBytes) {
    const bytes = Buffer.from(text, 'utf8');
    const length = Math.min(bytes.length, maxBytes - 1);
    heap.set(bytes.subarray(0, length), pointer);
    heap[pointer + length] = 0;
  }

  const functions = [null];
  const wasmTable = {
    get(index) {
      const entry = functions[index];
      if (!entry) throw new Error(`no wasm table entry at ${index}`);
      return entry;
    }
  };

  const library = {};
  const context = {
    console,
    Promise,
    JSON,
    Math,
    Date,
    Error,
    Object,
    Array,
    Uint8Array,
    ArrayBuffer,
    String,
    Number,
    Boolean,
    Map,
    Set,
    TextEncoder,
    TextDecoder,
    URL,
    URLSearchParams,
    BigInt,
    Symbol,
    Function,
    RangeError,
    TypeError,
    AggregateError,
    Reflect,
    WeakMap,
    WeakSet,
    DataView,
    AbortController,
    AbortSignal,
    WebSocket: globalThis.WebSocket,
    crypto: globalThis.crypto,
    performance: globalThis.performance,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    queueMicrotask,
    HEAPU8: heap,
    _malloc: malloc,
    _free: free,
    UTF8ToString: utf8ToString,
    stringToUTF8,
    lengthBytesUTF8: (text) => Buffer.byteLength(text, 'utf8'),
    wasmTable,
    mergeInto(target, members) {
      Object.assign(target, members);
    },
    LibraryManager: { library }
  };
  context.globalThis = context;
  vm.createContext(context);

  for (const file of ['zlink-stream-connector.jspre', 'ZlinkStreamRuntime.jspre', 'ZlinkStreamConnector.jslib']) {
    vm.runInContext(
      fs.readFileSync(path.join(packageRoot, 'Plugins/WebGL', file), 'utf8'),
      context,
      { filename: file }
    );
  }

  return {
    heap,
    library,
    context,
    allocatedBlocks: () => blocks.size,
    /** Registers the C# side's static sink and returns its function-pointer value. */
    registerFunction(fn) {
      functions.push(fn);
      return functions.length - 1;
    },
    /** Mirrors IL2CPP's UTF-8 marshalling of a `string` parameter. */
    withString(text, run) {
      const size = Buffer.byteLength(text, 'utf8') + 1;
      const pointer = malloc(size);
      try {
        stringToUTF8(text, pointer, size);
        return run(pointer);
      } finally {
        free(pointer);
      }
    },
    /** Mirrors the C# side pinning a payload buffer for the duration of one call. */
    withBytes(bytes, run) {
      if (bytes.length === 0) return run(0, 0);
      const pointer = malloc(bytes.length);
      try {
        heap.set(bytes, pointer);
        return run(pointer, bytes.length);
      } finally {
        free(pointer);
      }
    },
    readBytes(pointer, length) {
      if (!pointer || length === 0) return new Uint8Array(0);
      return Uint8Array.from(heap.subarray(pointer, pointer + length));
    },
    readString: utf8ToString
  };
}

module.exports = { createHarness, packageRoot };
