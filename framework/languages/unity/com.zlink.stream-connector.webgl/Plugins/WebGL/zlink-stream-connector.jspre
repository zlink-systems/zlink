// GENERATED FILE - DO NOT EDIT.
// Produced by framework/languages/node/scripts/sync-unity-webgl-package.mjs from
// @zlink-systems/stream-connector (package root, IIFE build).
// Package version: 0.14.0
//
// This is the same TypeScript connector the npm package root ships. The UPM
// adapter adds no wire runtime of its own (stream-connector spec 32 section 11).

"use strict";
var ZlinkStreamConnectorBundle = (() => {
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __hasOwnProp = Object.prototype.hasOwnProperty;
  var __export = (target, all) => {
    for (var name in all)
      __defProp(target, name, { get: all[name], enumerable: true });
  };
  var __copyProps = (to, from, except, desc) => {
    if (from && typeof from === "object" || typeof from === "function") {
      for (let key of __getOwnPropNames(from))
        if (!__hasOwnProp.call(to, key) && key !== except)
          __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
    }
    return to;
  };
  var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

  // packages/stream-connector/src/index.ts
  var index_exports = {};
  __export(index_exports, {
    DefaultZlinkStreamConnector: () => DefaultZlinkStreamConnector,
    ZlinkStreamCodec: () => ZlinkStreamCodec,
    ZlinkStreamCompression: () => ZlinkStreamCompression,
    ZlinkStreamConnectionState: () => ZlinkStreamConnectionState,
    ZlinkStreamDiagnosticsLevel: () => ZlinkStreamDiagnosticsLevel,
    ZlinkStreamDispatchMode: () => ZlinkStreamDispatchMode,
    ZlinkStreamErrorCode: () => ZlinkStreamErrorCode,
    ZlinkStreamException: () => ZlinkStreamException,
    ZlinkStreamHeaderFlags: () => ZlinkStreamHeaderFlags,
    ZlinkStreamMessageKind: () => ZlinkStreamMessageKind,
    ZlinkStreamMetadataMap: () => ZlinkStreamMetadataMap,
    ZlinkStreamTransport: () => ZlinkStreamTransport,
    fromJson: () => fromJson,
    toJson: () => toJson,
    validateMetadataKey: () => validateMetadataKey,
    zlinkStreamAssert: () => zlinkStreamAssert,
    zlinkStreamConnectorFactory: () => zlinkStreamConnectorFactory,
    zlinkStreamJsonCodec: () => zlinkStreamJsonCodec,
    zlinkStreamJsonCodecName: () => zlinkStreamJsonCodecName
  });

  // packages/stream-wire/src/lz4-pickle.ts
  var defaultMaxDecompressedPayloadSize = 64 * 1024;
  function lz4PickleUncompressed(payload) {
    if (payload.length === 0) {
      return new Uint8Array();
    }
    const pickled = new Uint8Array(payload.length + 1);
    pickled[0] = 0;
    pickled.set(payload, 1);
    return pickled;
  }
  function lz4UnpicklePayload(payload, maxDecompressedSize = defaultMaxDecompressedPayloadSize) {
    if (payload.length === 0) {
      return new Uint8Array();
    }
    const header = payload[0];
    if ((header & 7) !== 0) {
      throw new Error("Unexpected LZ4 pickle version.");
    }
    const sizeOfDiff = decodeDiffSize(header >>> 6 & 3);
    const dataOffset = 1 + sizeOfDiff;
    if (payload.length < dataOffset) {
      throw new Error("LZ4 pickle header is incomplete.");
    }
    const data = payload.subarray(dataOffset);
    const resultDiff = sizeOfDiff === 0 ? 0 : readLittleEndian(payload, 1, sizeOfDiff);
    const resultLength = data.length + resultDiff;
    if (resultLength > maxDecompressedSize) {
      throw new Error("LZ4 decoded payload exceeds maximum stream payload size.");
    }
    if (resultDiff === 0) {
      return data.slice();
    }
    return decodeLz4Block(data, resultLength);
  }
  function decodeDiffSize(encoded) {
    return encoded === 3 ? 4 : encoded;
  }
  function readLittleEndian(source, offset, size) {
    if (size === 1) {
      return source[offset];
    }
    if (size === 2) {
      return source[offset] | source[offset + 1] << 8;
    }
    if (size === 4) {
      return source[offset] | source[offset + 1] << 8 | source[offset + 2] << 16 | source[offset + 3] * 16777216;
    }
    throw new Error(`Unexpected LZ4 pickle field size: ${size}`);
  }
  function decodeLz4Block(source, resultLength) {
    const target = new Uint8Array(resultLength);
    let sourceOffset = 0;
    let targetOffset = 0;
    while (sourceOffset < source.length) {
      const token = source[sourceOffset++];
      const literalLength = readLz4Length(source, token >>> 4, () => sourceOffset++);
      if (source.length - sourceOffset < literalLength) {
        throw new Error("LZ4 literal run is incomplete.");
      }
      if (target.length - targetOffset < literalLength) {
        throw new Error("LZ4 literal run exceeds output size.");
      }
      target.set(source.subarray(sourceOffset, sourceOffset + literalLength), targetOffset);
      sourceOffset += literalLength;
      targetOffset += literalLength;
      if (sourceOffset >= source.length) {
        break;
      }
      if (source.length - sourceOffset < 2) {
        throw new Error("LZ4 match offset is incomplete.");
      }
      const matchOffset = source[sourceOffset] | source[sourceOffset + 1] << 8;
      sourceOffset += 2;
      if (matchOffset === 0 || matchOffset > targetOffset) {
        throw new Error("LZ4 match offset is invalid.");
      }
      const matchLength = readLz4Length(source, token & 15, () => sourceOffset++) + 4;
      if (target.length - targetOffset < matchLength) {
        throw new Error("LZ4 match run exceeds output size.");
      }
      for (let index = 0; index < matchLength; index += 1) {
        target[targetOffset + index] = target[targetOffset - matchOffset + index];
      }
      targetOffset += matchLength;
    }
    if (targetOffset !== resultLength) {
      throw new Error("LZ4 decoded length does not match pickle header.");
    }
    return target;
  }
  function readLz4Length(source, nibble, nextOffset) {
    let length = nibble;
    if (length !== 15) {
      return length;
    }
    for (; ; ) {
      const offset = nextOffset();
      if (offset >= source.length) {
        throw new Error("LZ4 extended length is incomplete.");
      }
      const value = source[offset];
      length += value;
      if (value !== 255) {
        return length;
      }
    }
  }

  // packages/stream-wire/src/index.ts
  var ZlinkStreamCodec = /* @__PURE__ */ ((ZlinkStreamCodec2) => {
    ZlinkStreamCodec2[ZlinkStreamCodec2["Raw"] = 0] = "Raw";
    ZlinkStreamCodec2[ZlinkStreamCodec2["Json"] = 1] = "Json";
    ZlinkStreamCodec2[ZlinkStreamCodec2["MessagePack"] = 2] = "MessagePack";
    ZlinkStreamCodec2[ZlinkStreamCodec2["Protobuf"] = 3] = "Protobuf";
    return ZlinkStreamCodec2;
  })(ZlinkStreamCodec || {});
  var defaultHeaderFlags = {
    hasRequestSeq: 1,
    hasMetadata: 2,
    hasCorrelationId: 8,
    hasFlowId: 16
  };
  var ZLINK_STREAM_FORMAT_MARKER = 242;
  var ZLINK_STREAM_RESPONSE_KIND = 3;
  var ZLINK_STREAM_ERROR_KIND = 4;
  function encodeStreamWireFrame(header, payload) {
    if (header.length > 65535) {
      throw new Error("Stream header is too large.");
    }
    if (payload.length > 4294967295) {
      throw new Error("Stream payload is too large.");
    }
    const frame = new Uint8Array(6 + header.length + payload.length);
    writeUInt16BE(frame, 0, header.length);
    writeUInt32BE(frame, 2, payload.length);
    frame.set(header, 6);
    frame.set(payload, 6 + header.length);
    return frame;
  }
  function decodeStreamWireFrame(frame) {
    if (frame.length < 6) {
      throw new Error("Stream frame prefix is incomplete.");
    }
    const headerLength = readUInt16BE(frame, 0);
    const payloadLength = readUInt32BE(frame, 2);
    if (frame.length !== 6 + headerLength + payloadLength) {
      throw new Error("Stream frame length does not match prefix.");
    }
    return {
      header: frame.slice(6, 6 + headerLength),
      payload: frame.slice(6 + headerLength)
    };
  }
  function encodeStreamWireHeader(header, flags = defaultHeaderFlags) {
    const reply = isReplyKind(header.kind);
    const packetName = reply ? "" : header.name;
    if (!reply) validateStreamWirePacketName(packetName);
    const nameBytes = utf8Encode(packetName);
    const hasRequestSeq = header.requestSeq !== void 0;
    const hasMetadata = header.metadata.size > 0;
    const correlationBytes = header.correlationId !== void 0 && header.correlationId.length > 0 ? utf8Encode(header.correlationId) : void 0;
    if (correlationBytes !== void 0 && correlationBytes.length > 255) {
      throw new Error("Stream correlation id is too large.");
    }
    const hasCorrelation = correlationBytes !== void 0;
    const hasFlow = header.flowId !== void 0 || header.flowOrigin !== void 0;
    if (hasFlow && (header.flowId === void 0 || header.flowOrigin === void 0)) {
      throw new Error("Stream flow id and origin must be provided together.");
    }
    if (header.flowId !== void 0) validateFlowId(header.flowId);
    if (header.flowOrigin !== void 0 && ![1, 2, 3, 4].includes(header.flowOrigin)) {
      throw new Error("Stream flow origin is invalid.");
    }
    let headerFlags = header.flags;
    headerFlags = hasRequestSeq ? headerFlags | flags.hasRequestSeq : headerFlags & ~flags.hasRequestSeq;
    headerFlags = hasMetadata ? headerFlags | flags.hasMetadata : headerFlags & ~flags.hasMetadata;
    headerFlags = hasCorrelation ? headerFlags | flags.hasCorrelationId : headerFlags & ~flags.hasCorrelationId;
    headerFlags = hasFlow ? headerFlags | flags.hasFlowId : headerFlags & ~flags.hasFlowId;
    const metadataBytes = hasMetadata ? encodeStreamWireMetadata(header.metadata) : new Uint8Array();
    const size = 4 + (hasRequestSeq ? 8 : 0) + 1 + nameBytes.length + (hasMetadata ? 2 + metadataBytes.length : 0) + (hasCorrelation ? 1 + correlationBytes.length : 0) + (hasFlow ? 37 : 0);
    const buffer = new Uint8Array(size);
    let offset = 0;
    buffer[offset++] = ZLINK_STREAM_FORMAT_MARKER;
    buffer[offset++] = header.kind;
    buffer[offset++] = header.codec;
    buffer[offset++] = headerFlags;
    if (hasRequestSeq) {
      if (header.requestSeq === 0n) {
        throw new Error("Request sequence must not be zero.");
      }
      writeBigUInt64BE(buffer, offset, header.requestSeq);
      offset += 8;
    }
    buffer[offset++] = nameBytes.length;
    buffer.set(nameBytes, offset);
    offset += nameBytes.length;
    if (hasMetadata) {
      writeUInt16BE(buffer, offset, metadataBytes.length);
      offset += 2;
      buffer.set(metadataBytes, offset);
      offset += metadataBytes.length;
    }
    if (hasCorrelation) {
      buffer[offset++] = correlationBytes.length;
      buffer.set(correlationBytes, offset);
      offset += correlationBytes.length;
    }
    if (hasFlow) {
      buffer.set(asciiEncode(header.flowId), offset);
      offset += 36;
      buffer[offset++] = header.flowOrigin;
    }
    return buffer;
  }
  function decodeStreamWireHeader(header, flags = defaultHeaderFlags, includeFlow = true) {
    let offset = 0;
    if (header.length < 5) {
      throw new Error("Stream header is incomplete.");
    }
    if (header[offset++] !== ZLINK_STREAM_FORMAT_MARKER) {
      throw new Error("Stream header format marker is invalid.");
    }
    const kind = header[offset++];
    const codec = header[offset++];
    const headerFlags = header[offset++];
    const hasRequestSeq = (headerFlags & flags.hasRequestSeq) !== 0;
    const hasMetadata = (headerFlags & flags.hasMetadata) !== 0;
    const hasCorrelation = (headerFlags & flags.hasCorrelationId) !== 0;
    const hasFlow = (headerFlags & flags.hasFlowId) !== 0;
    if ((headerFlags & ~31) !== 0) {
      throw new Error("Unknown mandatory stream header flag.");
    }
    let requestSeq;
    if (hasRequestSeq) {
      if (header.length - offset < 8) {
        throw new Error("Stream request sequence is incomplete.");
      }
      requestSeq = readBigUInt64BE(header, offset);
      if (requestSeq === 0n) {
        throw new Error("Request sequence must not be zero.");
      }
      offset += 8;
    }
    if (header.length - offset < 1) {
      throw new Error("Stream packet name length is missing.");
    }
    const nameLength = header[offset++];
    if (!isReplyKind(kind) && nameLength === 0 || header.length - offset < nameLength) {
      throw new Error("Stream packet name is invalid.");
    }
    const decodedName = utf8Decode(header.subarray(offset, offset + nameLength));
    const name = isReplyKind(kind) ? "" : decodedName;
    offset += nameLength;
    const decodedMetadata = hasMetadata ? decodeStreamWireHeaderMetadata(header, offset) : { metadata: /* @__PURE__ */ new Map(), offset };
    offset = decodedMetadata.offset;
    let correlationId;
    if (hasCorrelation) {
      if (header.length - offset < 1) {
        throw new Error("Stream correlation id is incomplete.");
      }
      const correlationLength = header[offset++];
      if (header.length - offset < correlationLength) {
        throw new Error("Stream correlation id is incomplete.");
      }
      correlationId = utf8Decode(header.subarray(offset, offset + correlationLength));
      offset += correlationLength;
    }
    let flowId;
    let flowOrigin;
    if (hasFlow) {
      if (header.length - offset < 37) {
        throw new Error("Stream flow fields are incomplete.");
      }
      if (includeFlow) {
        flowId = asciiDecode(header.subarray(offset, offset + 36));
        validateFlowId(flowId);
      }
      offset += 36;
      const decodedFlowOrigin = header[offset++];
      if (includeFlow && ![1, 2, 3, 4].includes(decodedFlowOrigin)) {
        throw new Error("Stream flow origin is invalid.");
      }
      flowOrigin = includeFlow ? decodedFlowOrigin : void 0;
    }
    if (offset !== header.length) {
      throw new Error("Stream header has trailing bytes.");
    }
    return {
      kind,
      codec,
      flags: headerFlags,
      requestSeq,
      name,
      metadata: decodedMetadata.metadata,
      correlationId,
      flowId,
      flowOrigin
    };
  }
  function validateFlowId(flowId) {
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(flowId)) {
      throw new Error("Stream flow id must be a lowercase UUIDv7.");
    }
  }
  function asciiEncode(value) {
    return Uint8Array.from(value, (character) => character.charCodeAt(0));
  }
  function asciiDecode(value) {
    if (value.some((byte) => byte > 127)) throw new Error("Stream flow id must be ASCII.");
    return String.fromCharCode(...value);
  }
  function encodeStreamWireMetadata(metadata) {
    if (metadata.size > 255) {
      throw new Error("Metadata entry count must not exceed 255.");
    }
    let size = 1;
    const encoded = [...metadata].map(([key, value]) => {
      const keyBytes = utf8Encode(key);
      const valueBytes = utf8Encode(value);
      if (keyBytes.length === 0 || keyBytes.length > 255) {
        throw new Error("Metadata key length is invalid.");
      }
      if (valueBytes.length > 65535) {
        throw new Error("Metadata value is too large.");
      }
      size += 1 + keyBytes.length + 2 + valueBytes.length;
      return { keyBytes, valueBytes };
    });
    const buffer = new Uint8Array(size);
    let offset = 0;
    buffer[offset++] = metadata.size;
    for (const { keyBytes, valueBytes } of encoded) {
      buffer[offset++] = keyBytes.length;
      buffer.set(keyBytes, offset);
      offset += keyBytes.length;
      writeUInt16BE(buffer, offset, valueBytes.length);
      offset += 2;
      buffer.set(valueBytes, offset);
      offset += valueBytes.length;
    }
    return buffer;
  }
  function decodeStreamWireMetadata(metadata) {
    const decoded = decodeStreamWireMetadataAt(metadata, 0, metadata.length);
    if (decoded.offset !== metadata.length) {
      throw new Error("Stream metadata payload has trailing bytes.");
    }
    return decoded.metadata;
  }
  function lz4PickleUncompressed2(payload) {
    return lz4PickleUncompressed(payload);
  }
  function lz4UnpicklePayload2(payload, maxDecompressedSize = defaultMaxDecompressedPayloadSize) {
    return lz4UnpicklePayload(payload, maxDecompressedSize);
  }
  function utf8Encode(value) {
    return new TextEncoder().encode(value);
  }
  function utf8Decode(value) {
    return new TextDecoder().decode(value);
  }
  function decodeStreamWireHeaderMetadata(header, offset) {
    if (header.length - offset < 2) {
      throw new Error("Stream metadata section is incomplete.");
    }
    const metadataLength = readUInt16BE(header, offset);
    offset += 2;
    if (header.length - offset < metadataLength) {
      throw new Error("Stream metadata payload is incomplete.");
    }
    return decodeStreamWireMetadataAt(header, offset, offset + metadataLength);
  }
  function decodeStreamWireMetadataAt(source, offset, end) {
    if (offset >= end) {
      throw new Error("Stream metadata entry count is missing.");
    }
    const count = source[offset++];
    const metadata = /* @__PURE__ */ new Map();
    for (let index = 0; index < count; index += 1) {
      if (offset >= end) {
        throw new Error("Stream metadata key length is missing.");
      }
      const keyLength = source[offset++];
      if (keyLength === 0 || end - offset < keyLength) {
        throw new Error("Stream metadata key is invalid.");
      }
      const key = utf8Decode(source.subarray(offset, offset + keyLength));
      offset += keyLength;
      if (end - offset < 2) {
        throw new Error("Stream metadata value length is missing.");
      }
      const valueLength = readUInt16BE(source, offset);
      offset += 2;
      if (end - offset < valueLength) {
        throw new Error("Stream metadata value is incomplete.");
      }
      if (metadata.has(key)) {
        throw new Error("Duplicate metadata key is duplicated.");
      }
      metadata.set(key, utf8Decode(source.subarray(offset, offset + valueLength)));
      offset += valueLength;
    }
    if (offset !== end) {
      throw new Error("Stream metadata payload has trailing bytes.");
    }
    return { metadata, offset };
  }
  function validateStreamWirePacketName(name) {
    const nameBytes = utf8Encode(name);
    if (name.trim().length === 0 || nameBytes.length > 255) {
      throw new Error("Stream packet name is invalid.");
    }
  }
  function isReplyKind(kind) {
    return kind === ZLINK_STREAM_RESPONSE_KIND || kind === ZLINK_STREAM_ERROR_KIND;
  }
  function writeUInt16BE(buffer, offset, value) {
    buffer[offset] = value >>> 8 & 255;
    buffer[offset + 1] = value & 255;
  }
  function readUInt16BE(buffer, offset) {
    return buffer[offset] << 8 | buffer[offset + 1];
  }
  function readUInt32BE(buffer, offset) {
    return buffer[offset] * 16777216 + (buffer[offset + 1] << 16 | buffer[offset + 2] << 8 | buffer[offset + 3]);
  }
  function writeUInt32BE(buffer, offset, value) {
    buffer[offset] = value >>> 24 & 255;
    buffer[offset + 1] = value >>> 16 & 255;
    buffer[offset + 2] = value >>> 8 & 255;
    buffer[offset + 3] = value & 255;
  }
  function writeBigUInt64BE(buffer, offset, value) {
    for (let index = 7; index >= 0; index -= 1) {
      buffer[offset + index] = Number(value & 0xffn);
      value >>= 8n;
    }
  }
  function readBigUInt64BE(buffer, offset) {
    let value = 0n;
    for (let index = 0; index < 8; index += 1) {
      value = value << 8n | BigInt(buffer[offset + index]);
    }
    return value;
  }

  // packages/stream-connector/src/Contracts/ZlinkStreamEnums.ts
  var ZlinkStreamTransport = /* @__PURE__ */ ((ZlinkStreamTransport2) => {
    ZlinkStreamTransport2["WebSocket"] = "webSocket";
    ZlinkStreamTransport2["WebSocketSecure"] = "webSocketSecure";
    return ZlinkStreamTransport2;
  })(ZlinkStreamTransport || {});
  var ZlinkStreamCompression = /* @__PURE__ */ ((ZlinkStreamCompression2) => {
    ZlinkStreamCompression2["None"] = "none";
    ZlinkStreamCompression2["Lz4"] = "lz4";
    return ZlinkStreamCompression2;
  })(ZlinkStreamCompression || {});
  var ZlinkStreamDispatchMode = /* @__PURE__ */ ((ZlinkStreamDispatchMode2) => {
    ZlinkStreamDispatchMode2["Manual"] = "manual";
    ZlinkStreamDispatchMode2["Immediate"] = "immediate";
    return ZlinkStreamDispatchMode2;
  })(ZlinkStreamDispatchMode || {});
  var ZlinkStreamDiagnosticsLevel = /* @__PURE__ */ ((ZlinkStreamDiagnosticsLevel4) => {
    ZlinkStreamDiagnosticsLevel4["Off"] = "off";
    ZlinkStreamDiagnosticsLevel4["Errors"] = "errors";
    ZlinkStreamDiagnosticsLevel4["Normal"] = "normal";
    ZlinkStreamDiagnosticsLevel4["Detailed"] = "detailed";
    return ZlinkStreamDiagnosticsLevel4;
  })(ZlinkStreamDiagnosticsLevel || {});
  var ZlinkStreamMessageKind = /* @__PURE__ */ ((ZlinkStreamMessageKind2) => {
    ZlinkStreamMessageKind2[ZlinkStreamMessageKind2["Send"] = 1] = "Send";
    ZlinkStreamMessageKind2[ZlinkStreamMessageKind2["Request"] = 2] = "Request";
    ZlinkStreamMessageKind2[ZlinkStreamMessageKind2["Response"] = 3] = "Response";
    ZlinkStreamMessageKind2[ZlinkStreamMessageKind2["Error"] = 4] = "Error";
    ZlinkStreamMessageKind2[ZlinkStreamMessageKind2["Control"] = 5] = "Control";
    return ZlinkStreamMessageKind2;
  })(ZlinkStreamMessageKind || {});
  var ZlinkStreamHeaderFlags = /* @__PURE__ */ ((ZlinkStreamHeaderFlags2) => {
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["None"] = 0] = "None";
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["HasRequestSeq"] = 1] = "HasRequestSeq";
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["HasMetadata"] = 2] = "HasMetadata";
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["PayloadCompressed"] = 4] = "PayloadCompressed";
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["HasCorrelationId"] = 8] = "HasCorrelationId";
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["HasFlowId"] = 16] = "HasFlowId";
    return ZlinkStreamHeaderFlags2;
  })(ZlinkStreamHeaderFlags || {});
  var ZlinkStreamErrorCode = /* @__PURE__ */ ((ZlinkStreamErrorCode2) => {
    ZlinkStreamErrorCode2["Disconnected"] = "disconnected";
    ZlinkStreamErrorCode2["ConfigurationError"] = "configurationError";
    ZlinkStreamErrorCode2["ValidationFailed"] = "validationFailed";
    ZlinkStreamErrorCode2["RequestTimeout"] = "requestTimeout";
    ZlinkStreamErrorCode2["ConnectTimeout"] = "connectTimeout";
    ZlinkStreamErrorCode2["FrameDecodeFailed"] = "frameDecodeFailed";
    ZlinkStreamErrorCode2["FrameTooLarge"] = "frameTooLarge";
    ZlinkStreamErrorCode2["SendFailed"] = "sendFailed";
    ZlinkStreamErrorCode2["CompressionFailed"] = "compressionFailed";
    ZlinkStreamErrorCode2["DecompressionFailed"] = "decompressionFailed";
    ZlinkStreamErrorCode2["UserCallbackFailed"] = "userCallbackFailed";
    ZlinkStreamErrorCode2["RemoteError"] = "remoteError";
    return ZlinkStreamErrorCode2;
  })(ZlinkStreamErrorCode || {});
  var ZlinkStreamConnectionState = /* @__PURE__ */ ((ZlinkStreamConnectionState3) => {
    ZlinkStreamConnectionState3["Created"] = "created";
    ZlinkStreamConnectionState3["Connecting"] = "connecting";
    ZlinkStreamConnectionState3["Connected"] = "connected";
    ZlinkStreamConnectionState3["Reconnecting"] = "reconnecting";
    ZlinkStreamConnectionState3["Disconnected"] = "disconnected";
    ZlinkStreamConnectionState3["Closed"] = "closed";
    return ZlinkStreamConnectionState3;
  })(ZlinkStreamConnectionState || {});

  // packages/stream-connector/src/Contracts/ZlinkStreamMetadata.ts
  var ZlinkStreamMetadataMap = class _ZlinkStreamMetadataMap {
    constructor(values) {
      this.values = values;
    }
    static empty = new _ZlinkStreamMetadataMap(/* @__PURE__ */ new Map());
    get count() {
      return this.values.size;
    }
    get(key) {
      return this.values.get(key);
    }
    with(key, value) {
      validateMetadataKey(key);
      const next = new Map(this.values);
      next.set(key, value);
      return new _ZlinkStreamMetadataMap(next);
    }
    withMany(values) {
      const next = new Map(this.values);
      for (const [key, value] of values) {
        validateMetadataKey(key);
        next.set(key, value);
      }
      return new _ZlinkStreamMetadataMap(next);
    }
    static from(values) {
      return _ZlinkStreamMetadataMap.empty.withMany(values);
    }
  };
  function validateMetadataKey(key) {
    if (key.length === 0) {
      throw new Error("Metadata key must not be empty.");
    }
    for (let index = 0; index < key.length; index++) {
      const code = key.charCodeAt(index);
      if (code < 32 || code > 126 || key[index] === "=") {
        throw new Error('Metadata key must contain printable ASCII characters except "=".');
      }
    }
  }

  // packages/stream-connector/src/Contracts/ZlinkStreamModels.ts
  var ZlinkStreamException = class extends Error {
    constructor(error) {
      super(error.message);
      this.error = error;
      this.name = "ZlinkStreamException";
    }
  };

  // packages/stream-connector/src/Contracts/ZlinkStreamJsonCodec.ts
  var zlinkStreamJsonCodecName = "json";
  var codecOptions = {};
  var zlinkStreamJsonCodec = {
    configure(options) {
      codecOptions = options;
    },
    encode(payload, messageType) {
      return toJson(payload, messageType);
    },
    decode(payload) {
      return fromJson(payload);
    }
  };
  function toJson(value, messageType) {
    return {
      codec: 1 /* Json */,
      payload: new TextEncoder().encode(JSON.stringify(value, codecOptions.replacer)),
      messageType: messageType ?? inferMessageType(value)
    };
  }
  function fromJson(payload) {
    ensureJson(payload);
    return JSON.parse(new TextDecoder().decode(payload.payload), safeJsonReviver);
  }
  function ensureJson(payload) {
    if (payload.codec !== 1 /* Json */) {
      throw new Error(`Stream payload codec is ${payload.codec}, not Json.`);
    }
  }
  function inferMessageType(value) {
    if (value === null || value === void 0) {
      return void 0;
    }
    const constructor = Object.getPrototypeOf(value)?.constructor;
    return constructor === Object ? void 0 : constructor;
  }
  function safeJsonReviver(key, value) {
    if (isPrototypeKey(key)) {
      throw new Error(`JSON payload key '${key}' is not allowed.`);
    }
    if (codecOptions.reviver !== void 0) {
      return codecOptions.reviver.call(this, key, value);
    }
    return value;
  }
  function isPrototypeKey(key) {
    return key === "__proto__" || key === "constructor" || key === "prototype";
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamSupport.ts
  function connectorError(code, message, cause) {
    return new ZlinkStreamException({ code, message, cause });
  }
  function toStreamError(cause, code, message) {
    if (cause instanceof ZlinkStreamException) {
      return cause.error;
    }
    return { code, message, cause };
  }
  function unwrapStreamError(error) {
    if (error instanceof ZlinkStreamException) {
      return error.error;
    }
    return { code: "remoteError" /* RemoteError */, message: error instanceof Error ? error.message : String(error), cause: error };
  }
  function subscription(dispose) {
    return { dispose };
  }
  function throwIfAborted(signal) {
    if (signal?.aborted === true) {
      throw connectorError("disconnected" /* Disconnected */, "Operation canceled.");
    }
  }
  function delay(delayMs, signal) {
    throwIfAborted(signal);
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        signal?.removeEventListener("abort", onAbort);
        resolve();
      }, delayMs);
      const onAbort = () => {
        clearTimeout(timeout);
        reject(connectorError("disconnected" /* Disconnected */, "Operation canceled."));
      };
      signal?.addEventListener("abort", onAbort, { once: true });
    });
  }
  function utf8Encode2(value) {
    return new TextEncoder().encode(value);
  }
  function utf8Decode2(value) {
    return new TextDecoder().decode(value);
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamPacketNameValidator.ts
  function validateName(name, allowReserved = false) {
    if (name.length === 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Message name must not be empty.");
    }
    if (!allowReserved && name.startsWith("$zlink.")) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Message name uses a reserved zlink prefix.");
    }
    if (utf8Encode2(name).length > 255) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Message name must not exceed 255 UTF-8 bytes.");
    }
  }

  // packages/stream-connector/src/Runtime/Calls/ZlinkStreamCallBuilders.ts
  var ZlinkStreamCallBuilderState = class {
    executed = false;
    name;
    metadata = ZlinkStreamMetadataMap.empty;
    timeoutMs;
    compress = false;
    flow;
    constructor(name) {
      this.name = name;
    }
    ensureNotExecuted() {
      if (this.executed) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Builder instances can be executed only once.");
      }
      this.executed = true;
    }
    resolveMessageName() {
      if (this.name === void 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Message name is required when the encoded stream payload has no message type.");
      }
      return this.name;
    }
  };
  var ZlinkStreamSendBuilder = class {
    constructor(connector, name, payload) {
      this.connector = connector;
      this.payload = payload;
      this.state = new ZlinkStreamCallBuilderState(name);
    }
    state;
    packetName(name) {
      validateName(name);
      this.state.name = name;
      return this;
    }
    metadata(keyOrMetadata, value) {
      this.state.metadata = typeof keyOrMetadata === "string" ? this.state.metadata.with(keyOrMetadata, value ?? "") : keyOrMetadata;
      return this;
    }
    compress() {
      this.state.compress = true;
      return this;
    }
    flowFrom(flow) {
      this.state.flow = flow;
      return this;
    }
    async submit(signal) {
      throwIfAborted(signal);
      this.state.ensureNotExecuted();
      await this.connector.sendEncoded(
        1 /* Send */,
        this.state.resolveMessageName(),
        this.payload,
        this.state.metadata,
        this.state.compress,
        void 0,
        signal,
        this.state.flow
      );
    }
  };
  var ZlinkStreamRequestBuilder = class {
    constructor(connector, name, payload) {
      this.connector = connector;
      this.payload = payload;
      this.state = new ZlinkStreamCallBuilderState(name);
    }
    state;
    packetName(name) {
      validateName(name);
      this.state.name = name;
      return this;
    }
    metadata(keyOrMetadata, value) {
      this.state.metadata = typeof keyOrMetadata === "string" ? this.state.metadata.with(keyOrMetadata, value ?? "") : keyOrMetadata;
      return this;
    }
    timeout(timeoutMs) {
      this.state.timeoutMs = timeoutMs;
      return this;
    }
    compress() {
      this.state.compress = true;
      return this;
    }
    flowFrom(flow) {
      this.state.flow = flow;
      return this;
    }
    submit(signalOrCallback) {
      this.state.ensureNotExecuted();
      const operation = this.connector.requestEncoded(
        this.state.resolveMessageName(),
        this.payload,
        this.state.metadata,
        this.state.compress,
        this.state.timeoutMs ?? this.connector.options.requestTimeoutMs,
        typeof signalOrCallback === "function" ? void 0 : signalOrCallback,
        this.state.flow
      );
      if (typeof signalOrCallback === "function") {
        operation.then(
          (value) => signalOrCallback({ isSuccess: true, value }),
          (error) => signalOrCallback({ isSuccess: false, error: unwrapStreamError(error) })
        );
        return;
      }
      return operation.then((value) => (this.connector.options.codec ?? zlinkStreamJsonCodec).decode(value));
    }
    submitEncoded(signal) {
      this.state.ensureNotExecuted();
      return this.connector.requestEncoded(
        this.state.resolveMessageName(),
        this.payload,
        this.state.metadata,
        this.state.compress,
        this.state.timeoutMs ?? this.connector.options.requestTimeoutMs,
        signal,
        this.state.flow
      );
    }
  };
  var ZlinkStreamWaitBuilder = class {
    constructor(connector, name) {
      this.connector = connector;
      this.name = name;
    }
    executed = false;
    timeoutMs;
    predicate = () => true;
    where(predicate) {
      this.ensureConfigurable();
      this.predicate = predicate;
      return this;
    }
    timeout(timeoutMs) {
      this.ensureConfigurable();
      this.timeoutMs = timeoutMs;
      return this;
    }
    submit(signal) {
      this.markExecuted();
      return this.connector.waitForMessage(
        this.name,
        this.timeoutMs ?? this.connector.options.waitTimeoutMs,
        this.predicate,
        signal
      );
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Builder instances can be executed only once.");
      }
    }
    markExecuted() {
      this.ensureConfigurable();
      this.executed = true;
    }
  };

  // packages/stream-connector/src/Runtime/Calls/ZlinkStreamObservationBuilders.ts
  var ZlinkStreamExpectNoneBuilder = class {
    constructor(connector, name) {
      this.connector = connector;
      this.name = name;
    }
    windowMs;
    executed = false;
    within(windowMs) {
      this.ensureConfigurable();
      validateTimeout(windowMs);
      this.windowMs = windowMs;
      return this;
    }
    async run(signal) {
      this.markExecuted();
      if (this.windowMs === void 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "expectNone requires within(windowMs).");
      }
      try {
        await this.connector.waitForMessage(this.name, this.windowMs, () => true, signal);
      } catch (error) {
        if (unwrapStreamError(error).code === "requestTimeout" /* RequestTimeout */) {
          return;
        }
        throw error;
      }
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        `Expected no '${this.name}' message within ${this.windowMs}ms.`
      );
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Builder instances can be executed only once.");
      }
    }
    markExecuted() {
      this.ensureConfigurable();
      this.executed = true;
    }
  };
  var ZlinkStreamSequenceBuilder = class {
    constructor(connector, name) {
      this.connector = connector;
      this.name = name;
    }
    predicates = [];
    timeoutMs;
    executed = false;
    expect(predicate) {
      this.ensureConfigurable();
      this.predicates.push(predicate);
      return this;
    }
    timeout(timeoutMs) {
      this.ensureConfigurable();
      validateTimeout(timeoutMs);
      this.timeoutMs = timeoutMs;
      return this;
    }
    async run(signal) {
      this.markExecuted();
      if (this.predicates.length === 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "waitForSequence requires at least one expectation.");
      }
      const timeoutMs = this.timeoutMs ?? this.connector.options.waitTimeoutMs;
      const deadline = Date.now() + timeoutMs;
      const payloads = [];
      for (const predicate of this.predicates) {
        const message = await this.connector.waitForMessage(
          this.name,
          Math.max(0, deadline - Date.now()),
          (candidate) => {
            if (!predicate(candidate.payload)) {
              throw connectorError(
                "validationFailed" /* ValidationFailed */,
                `Message '${this.name}' arrived out of the expected sequence.`
              );
            }
            return true;
          },
          signal
        );
        payloads.push(message.payload);
      }
      return payloads;
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Builder instances can be executed only once.");
      }
    }
    markExecuted() {
      this.ensureConfigurable();
      this.executed = true;
    }
  };
  function validateTimeout(timeoutMs) {
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Timeout must be a non-negative finite number.");
    }
  }

  // packages/stream-connector/src/Runtime/Protocol/Compression/ZlinkStreamCompressionCodec.ts
  var zlinkStreamLz4CompressionCodec = {
    compress(payload) {
      return lz4PickleUncompressed2(payload);
    },
    decompress(payload, maxDecompressedSize) {
      return lz4UnpicklePayload2(payload, maxDecompressedSize);
    }
  };
  function compressPayload(payload, compression, compressionCodec) {
    const codec = resolveCompressionCodec(compression, compressionCodec);
    if (codec === void 0) {
      throw connectorError("compressionFailed" /* CompressionFailed */, "Compression codec is not configured.");
    }
    return codec.compress(payload);
  }
  function decompressIfNeeded(header, payload, compression, compressionCodec, maxDecompressedSize) {
    if ((header.flags & 4 /* PayloadCompressed */) === 0) {
      return payload;
    }
    const codec = resolveCompressionCodec(compression, compressionCodec);
    if (codec === void 0) {
      throw connectorError("decompressionFailed" /* DecompressionFailed */, "Compression codec is not configured.");
    }
    try {
      const decompressed = codec.decompress(payload, maxDecompressedSize);
      if (decompressed.length > maxDecompressedSize) {
        throw new Error("Decoded payload exceeds MaxReceivePayloadSize.");
      }
      return decompressed;
    } catch (cause) {
      throw connectorError("decompressionFailed" /* DecompressionFailed */, "Decompression failed.", cause);
    }
  }
  function resolveCompressionCodec(compression, compressionCodec) {
    if (compression === "none" /* None */) {
      return void 0;
    }
    return compressionCodec ?? zlinkStreamLz4CompressionCodec;
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamFrameCodec.ts
  var ZlinkStreamFrameCodec = class {
    static encode(header, payload, maxPayloadSize = 64 * 1024) {
      validatePayload(payload.length, maxPayloadSize);
      try {
        return encodeStreamWireFrame(header, payload);
      } catch (cause) {
        throw connectorError("frameTooLarge" /* FrameTooLarge */, "Frame is too large.", cause);
      }
    }
    static decode(frame) {
      try {
        return decodeStreamWireFrame(frame);
      } catch (cause) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Frame length does not match prefix.", cause);
      }
    }
  };
  function splitZlinkStreamFrames(chunk) {
    if (chunk.length === 0) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Stream frame prefix is incomplete.");
    }
    const frames = [];
    let offset = 0;
    while (offset < chunk.length) {
      const remaining = chunk.length - offset;
      if (remaining < 6) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Stream frame prefix is incomplete.");
      }
      const headerLength = chunk[offset] << 8 | chunk[offset + 1];
      const payloadLength = chunk[offset + 2] * 16777216 + (chunk[offset + 3] << 16) + (chunk[offset + 4] << 8) + chunk[offset + 5];
      const frameLength = 6 + headerLength + payloadLength;
      if (frameLength > remaining) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Frame length does not match prefix.");
      }
      frames.push(chunk.subarray(offset, offset + frameLength));
      offset += frameLength;
    }
    return frames;
  }
  function validatePayload(payloadLength, maxPayloadSize) {
    if (payloadLength > maxPayloadSize) {
      throw connectorError("frameTooLarge" /* FrameTooLarge */, "Payload exceeds MaxSendPayloadSize.");
    }
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamMetadataCodec.ts
  var ZLINK_STREAM_MAX_METADATA_BYTES = 1024;
  var ZlinkStreamMetadataCodec = class {
    static size(metadata) {
      try {
        const size = metadata.count === 0 ? 0 : encodeStreamWireMetadata(metadata.values).length;
        if (size > ZLINK_STREAM_MAX_METADATA_BYTES) {
          throw connectorError(
            "validationFailed" /* ValidationFailed */,
            `Metadata must not exceed ${ZLINK_STREAM_MAX_METADATA_BYTES} bytes.`
          );
        }
        return size;
      } catch (cause) {
        throw connectorError("validationFailed" /* ValidationFailed */, streamWireErrorMessage(cause), cause);
      }
    }
    static write(metadata, destination) {
      try {
        destination.set(encodeStreamWireMetadata(metadata.values));
      } catch (cause) {
        throw connectorError("validationFailed" /* ValidationFailed */, streamWireErrorMessage(cause), cause);
      }
    }
    static decode(metadata) {
      try {
        const values = decodeStreamWireMetadata(metadata);
        return values.size === 0 ? ZlinkStreamMetadataMap.empty : ZlinkStreamMetadataMap.from(values);
      } catch (cause) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, streamWireErrorMessage(cause), cause);
      }
    }
  };
  function streamWireErrorMessage(cause) {
    return cause instanceof Error ? cause.message : "Metadata is invalid.";
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamHeaderCodec.ts
  var ZlinkStreamHeaderCodec = class {
    static encode(header) {
      const reply = isReplyKind2(header.kind);
      if (!reply) validateName(header.name, header.kind === 5 /* Control */);
      validateHeaderSemantics(header);
      ZlinkStreamMetadataCodec.size(header.metadata);
      try {
        return encodeStreamWireHeader({
          kind: header.kind,
          codec: header.codec,
          flags: header.flags,
          requestSeq: header.requestSeq,
          name: header.name,
          metadata: header.metadata.values,
          correlationId: header.correlationId,
          flowId: header.flowId,
          flowOrigin: encodeFlowOrigin(header.flowOrigin)
        });
      } catch (cause) {
        throw connectorError("validationFailed" /* ValidationFailed */, streamWireErrorMessage2(cause), cause);
      }
    }
    /**
     * `includeFlow=false` (diagnostics Off, spec 27 §4) skips reading and
     * validating the inbound flow fields while the structural length checks in
     * the wire decoder are preserved.
     */
    static decode(header, includeFlow = true) {
      let decoded;
      try {
        const wire = decodeStreamWireHeader(header, void 0, includeFlow);
        const metadata = wire.metadata.size === 0 ? ZlinkStreamMetadataMap.empty : ZlinkStreamMetadataMap.from(wire.metadata);
        decoded = {
          kind: wire.kind,
          codec: wire.codec,
          flags: wire.flags,
          requestSeq: wire.requestSeq,
          name: wire.name,
          metadata,
          correlationId: wire.correlationId,
          flowId: wire.flowId,
          flowOrigin: decodeFlowOrigin(wire.flowOrigin)
        };
      } catch (cause) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, streamWireErrorMessage2(cause), cause);
      }
      validateEnum(decoded.kind, decoded.codec, decoded.flags);
      if (!isReplyKind2(decoded.kind)) {
        validateName(decoded.name, decoded.kind === 5 /* Control */);
      }
      validateHeaderSemantics(decoded);
      return decoded;
    }
  };
  function isReplyKind2(kind) {
    return kind === 3 /* Response */ || kind === 4 /* Error */;
  }
  function streamWireErrorMessage2(cause) {
    return cause instanceof Error ? cause.message : "Stream header is invalid.";
  }
  function buildHeader(kind, name, codec, metadata, compress, requestSeq, correlationId, flowId, flowOrigin) {
    let flags = 0 /* None */;
    if (requestSeq !== void 0) {
      flags |= 1 /* HasRequestSeq */;
    }
    if (metadata.count > 0) {
      flags |= 2 /* HasMetadata */;
    }
    if (compress) {
      flags |= 4 /* PayloadCompressed */;
    }
    if (correlationId !== void 0 && correlationId.length > 0) {
      flags |= 8 /* HasCorrelationId */;
    }
    if (flowId !== void 0) flags |= 16 /* HasFlowId */;
    return { kind, codec, flags, requestSeq, name, metadata, correlationId, flowId, flowOrigin };
  }
  function validateHeaderSemantics(header) {
    validateEnum(header.kind, header.codec, header.flags);
    const hasRequestSeq = header.requestSeq !== void 0 || (header.flags & 1 /* HasRequestSeq */) !== 0;
    const hasMetadata = header.metadata.count > 0 || (header.flags & 2 /* HasMetadata */) !== 0;
    if (header.kind === 1 /* Send */ && hasRequestSeq) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Send packet must not contain a request sequence.");
    }
    if ((header.kind === 2 /* Request */ || header.kind === 3 /* Response */) && !hasRequestSeq) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Request and response packets must contain a request sequence.");
    }
    if (header.kind === 4 /* Error */ && header.codec !== 1 /* Json */) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Error packet must use the JSON codec.");
    }
    if (header.kind === 5 /* Control */) {
      const hasCorrelation = header.correlationId !== void 0 && header.correlationId.length > 0 || (header.flags & 8 /* HasCorrelationId */) !== 0;
      const hasFlow = header.flowId !== void 0 || header.flowOrigin !== void 0 || (header.flags & 16 /* HasFlowId */) !== 0;
      if (header.flags !== 0 /* None */ || hasRequestSeq || hasMetadata || hasCorrelation || hasFlow || header.codec !== 0 /* Raw */) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Control packet must use raw codec and must not contain flags.");
      }
    }
  }
  function validateEnum(kind, codec, flags) {
    if (![1, 2, 3, 4, 5].includes(kind)) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown stream message kind.");
    }
    if (![0, 1, 2, 3].includes(codec)) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown stream codec.");
    }
    const known = 1 /* HasRequestSeq */ | 2 /* HasMetadata */ | 4 /* PayloadCompressed */ | 8 /* HasCorrelationId */ | 16 /* HasFlowId */;
    if ((flags & ~known) !== 0) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown stream header flag.");
    }
  }
  function encodeFlowOrigin(origin) {
    return origin === void 0 ? void 0 : { Inbound: 1, Timer: 2, Application: 3, Lifecycle: 4 }[origin];
  }
  function decodeFlowOrigin(origin) {
    return origin === void 0 ? void 0 : { 1: "Inbound", 2: "Timer", 3: "Application", 4: "Lifecycle" }[origin];
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamFrameProtocol.ts
  var ZLINK_STREAM_HEARTBEAT_PING = "$zlink.heartbeat.ping";
  var ZLINK_STREAM_HEARTBEAT_PONG = "$zlink.heartbeat.pong";
  var ZlinkStreamFrameProtocol = class {
    constructor(options) {
      this.options = options;
    }
    encode(kind, name, payload, metadata, compress, requestSeq, correlationId, flowId, flowOrigin) {
      const payloadBytes = compress ? compressPayload(payload.payload, this.options.compression, this.options.compressionCodec) : payload.payload;
      const header = buildHeader(kind, name, payload.codec, metadata, compress, requestSeq, correlationId, flowId, flowOrigin);
      return this.encodeFrame(header, payloadBytes);
    }
    encodeControl(name, payload = new Uint8Array()) {
      return this.encodeFrame({
        kind: 5 /* Control */,
        codec: 0 /* Raw */,
        flags: 0 /* None */,
        name,
        metadata: ZlinkStreamMetadataMap.empty
      }, payload);
    }
    decode(frameBytes, flowEnabled = this.flowEnabled()) {
      const frame = ZlinkStreamFrameCodec.decode(frameBytes);
      return {
        // Spec 27 §4: with diagnostics Off the inbound flow fields are neither
        // read nor validated (structural length checks are preserved).
        header: ZlinkStreamHeaderCodec.decode(frame.header, flowEnabled),
        payload: frame.payload
      };
    }
    flowEnabled() {
      return this.options.diagnosticsLevel !== "off" /* Off */;
    }
    decodeFrames(chunk, flowEnabled = this.flowEnabled()) {
      return splitZlinkStreamFrames(chunk).map((frame) => {
        const decoded = this.decode(frame, flowEnabled);
        if (decoded.payload.length > this.options.maxReceivePayloadSize) {
          throw connectorError("frameTooLarge" /* FrameTooLarge */, "Payload exceeds MaxReceivePayloadSize.");
        }
        return decoded;
      });
    }
    decodePayload(header, payload) {
      return decompressIfNeeded(
        header,
        payload,
        this.options.compression,
        this.options.compressionCodec,
        this.options.maxReceivePayloadSize
      );
    }
    encodeFrame(header, payload) {
      return ZlinkStreamFrameCodec.encode(
        ZlinkStreamHeaderCodec.encode(header),
        payload,
        this.options.maxSendPayloadSize
      );
    }
  };

  // packages/stream-connector/src/Runtime/Transport/ZlinkStreamEndpoint.ts
  function inferTransport(endpoint) {
    const url = parseEndpoint(endpoint);
    switch (url.protocol) {
      case "ws:":
        return "webSocket" /* WebSocket */;
      case "wss:":
        return "webSocketSecure" /* WebSocketSecure */;
      default:
        throw connectorError(
          "configurationError" /* ConfigurationError */,
          "The TypeScript Stream Connector supports only ws:// and wss:// endpoints."
        );
    }
  }
  function parseEndpoint(endpoint) {
    try {
      return new URL(endpoint);
    } catch (cause) {
      throw connectorError("configurationError" /* ConfigurationError */, "Endpoint is invalid.", cause);
    }
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamConnectorOptions.ts
  var validDiagnosticsLevels = /* @__PURE__ */ new Set([
    "off" /* Off */,
    "errors" /* Errors */,
    "normal" /* Normal */,
    "detailed" /* Detailed */
  ]);
  function normalizeOptions(options, defaultTransportFactory) {
    const endpoint = options.endpoint;
    if (endpoint.trim().length === 0) {
      throw connectorError("configurationError" /* ConfigurationError */, "Endpoint must not be empty.");
    }
    const inferredTransport = inferTransport(endpoint);
    if (options.transport !== void 0 && options.transport !== inferredTransport) {
      throw connectorError("configurationError" /* ConfigurationError */, "Configured transport conflicts with endpoint scheme.");
    }
    validatePositive(options.connectTimeoutMs ?? 5e3, "ConnectTimeout");
    validatePositive(options.requestTimeoutMs ?? 3e4, "RequestTimeout");
    validatePositive(options.waitTimeoutMs ?? 5e3, "WaitTimeout");
    validatePositive(options.maxSendPayloadSize ?? 64 * 1024, "MaxSendPayloadSize");
    validatePositive(options.maxReceivePayloadSize ?? 64 * 1024, "MaxReceivePayloadSize");
    validateHeartbeat(options.heartbeat);
    validateReconnect(options.reconnect);
    validateDiagnosticsLevel(options.diagnosticsLevel);
    return {
      endpoint,
      transport: inferredTransport,
      connectTimeoutMs: options.connectTimeoutMs ?? 5e3,
      requestTimeoutMs: options.requestTimeoutMs ?? 3e4,
      waitTimeoutMs: options.waitTimeoutMs ?? 5e3,
      heartbeat: {
        enabled: options.heartbeat?.enabled ?? true,
        intervalMs: options.heartbeat?.intervalMs ?? 1e3,
        timeoutMs: options.heartbeat?.timeoutMs ?? 5e3
      },
      reconnect: {
        enabled: options.reconnect?.enabled ?? true,
        initialDelayMs: options.reconnect?.initialDelayMs ?? 250,
        maxDelayMs: options.reconnect?.maxDelayMs ?? 5e3,
        backoffFactor: options.reconnect?.backoffFactor ?? 2,
        maxAttempts: options.reconnect?.maxAttempts ?? 3
      },
      maxSendPayloadSize: options.maxSendPayloadSize ?? 64 * 1024,
      maxReceivePayloadSize: options.maxReceivePayloadSize ?? 64 * 1024,
      dispatchMode: options.dispatchMode ?? "manual" /* Manual */,
      compression: options.compression ?? "lz4" /* Lz4 */,
      compressionCodec: resolveCompressionCodec2(options),
      nameResolver: options.nameResolver ?? { resolve: (type) => type.name },
      transportFactory: options.transportFactory ?? defaultTransportFactory,
      codec: options.codec,
      meterProvider: options.meterProvider,
      // Spec 26 §4: the default diagnostics level is Errors, which preserves
      // the connector's established wire behavior.
      diagnosticsLevel: options.diagnosticsLevel ?? "errors" /* Errors */
    };
  }
  function resolveCompressionCodec2(options) {
    const compression = options.compression ?? "lz4" /* Lz4 */;
    if (compression === "none" /* None */) {
      if (options.compressionCodec !== void 0) {
        throw connectorError("configurationError" /* ConfigurationError */, "compressionCodec cannot be set when compression is none.");
      }
      return void 0;
    }
    return options.compressionCodec;
  }
  function validateDiagnosticsLevel(level) {
    if (level !== void 0 && !validDiagnosticsLevels.has(level)) {
      throw connectorError("configurationError" /* ConfigurationError */, "DiagnosticsLevel is invalid.");
    }
  }
  function validatePositive(value, name) {
    if (value <= 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, `${name} must be positive.`);
    }
  }
  function validateHeartbeat(options) {
    const enabled = options?.enabled ?? true;
    const intervalMs = options?.intervalMs ?? 1e3;
    const timeoutMs = options?.timeoutMs ?? 5e3;
    if (!enabled) {
      return;
    }
    validatePositive(intervalMs, "Heartbeat interval");
    validatePositive(timeoutMs, "Heartbeat timeout");
    if (timeoutMs <= intervalMs) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Heartbeat timeout must be greater than the heartbeat interval.");
    }
  }
  function validateReconnect(options) {
    const enabled = options?.enabled ?? true;
    if (!enabled) {
      return;
    }
    validatePositive(options?.initialDelayMs ?? 250, "Reconnect InitialDelay");
    validatePositive(options?.maxDelayMs ?? 5e3, "Reconnect MaxDelay");
    if ((options?.backoffFactor ?? 2) < 1) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Reconnect BackoffFactor must be at least 1.0.");
    }
    if ((options?.maxAttempts ?? 3) <= 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Reconnect MaxAttempts must be null or positive.");
    }
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamDiagnosticsLevelCell.ts
  var ZlinkStreamDiagnosticsLevelCell = class {
    current;
    constructor(initial) {
      this.current = initial;
    }
    get level() {
      return this.current;
    }
    set(level) {
      const candidate = level;
      if (candidate === void 0 || candidate === null) {
        throw connectorError("configurationError" /* ConfigurationError */, "DiagnosticsLevel is invalid.");
      }
      validateDiagnosticsLevel(level);
      this.current = level;
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamPendingRequests.ts
  var ZlinkStreamPendingRequests = class {
    nextRequestSeq = 1n;
    active = /* @__PURE__ */ new Map();
    get count() {
      return this.active.size;
    }
    create(packetName, timeoutMs) {
      const requestSeq = this.nextRequestSeq++;
      let timeout;
      let resolvePending;
      let rejectPending;
      const promise = new Promise((resolve, reject) => {
        timeout = setTimeout(() => {
          this.active.delete(requestSeq);
          reject(connectorError("requestTimeout" /* RequestTimeout */, `Request '${packetName}' timed out.`));
        }, timeoutMs);
        resolvePending = resolve;
        rejectPending = (error) => reject(connectorError(error.code, error.message, error.cause));
      });
      this.active.set(requestSeq, {
        packetName,
        promise,
        resolve: (value) => {
          if (timeout !== void 0) {
            clearTimeout(timeout);
          }
          resolvePending(value);
        },
        reject: (error) => {
          if (timeout !== void 0) {
            clearTimeout(timeout);
          }
          rejectPending(error);
        },
        cancel: () => {
          if (timeout !== void 0) {
            clearTimeout(timeout);
          }
        }
      });
      return { requestSeq, promise };
    }
    /* stream connector spec §5.2: a pending request is matched by request_seq alone. Diagnostics
     * use the original request name retained by this registry, never a legacy reply name. */
    resolve(requestSeq, value) {
      const pending = this.active.get(requestSeq);
      if (pending === void 0) {
        return false;
      }
      this.active.delete(requestSeq);
      pending.resolve(value);
      return true;
    }
    reject(requestSeq, error) {
      const pending = this.active.get(requestSeq);
      if (pending === void 0) {
        return false;
      }
      this.active.delete(requestSeq);
      pending.reject(error);
      return true;
    }
    cancel(requestSeq) {
      const pending = this.active.get(requestSeq);
      if (pending === void 0) {
        return;
      }
      this.active.delete(requestSeq);
      pending.cancel();
    }
    failAll(error) {
      for (const [requestSeq, pending] of this.active) {
        this.active.delete(requestSeq);
        pending.reject(error);
      }
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamReceivedMessages.ts
  var ZlinkStreamReceivedMessages = class {
    constructor(events) {
      this.events = events;
    }
    handlers = /* @__PURE__ */ new Map();
    // A handler can be registered after messages for another name arrive, so the
    // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
    // without shifting every later message on the hot receive path.
    queue = [];
    queueHead = 0;
    queuedCount = 0;
    drainTask;
    on(name, handler) {
      validateName(name);
      let set = this.handlers.get(name);
      if (set === void 0) {
        set = /* @__PURE__ */ new Set();
        this.handlers.set(name, set);
      }
      set.add(handler);
      if (this.hasQueuedMessage(name)) {
        queueMicrotask(() => this.scheduleDrain());
      }
      return subscription(() => {
        set.delete(handler);
        if (set.size === 0 && this.handlers.get(name) === set) {
          this.handlers.delete(name);
        }
      });
    }
    enqueue(message, signal) {
      this.queue.push({ message, signal });
      this.queuedCount += 1;
      this.scheduleDrain();
    }
    scheduleDrain() {
      if (this.drainTask !== void 0) {
        return;
      }
      this.drainTask = this.drain().finally(() => {
        this.drainTask = void 0;
        if (this.findDeliverableIndex() >= 0) {
          this.scheduleDrain();
        }
      });
    }
    async drain() {
      for (let index = this.findDeliverableIndex(); index >= 0; index = this.findDeliverableIndex()) {
        const queued = this.queue[index];
        if (queued === void 0) continue;
        this.queue[index] = void 0;
        this.queuedCount -= 1;
        this.advanceHead();
        this.compactQueue();
        const { message, signal } = queued;
        const handlers = [...this.handlers.get(message.name)];
        for (const handler of handlers) {
          try {
            await handler(message, signal);
          } catch (cause) {
            await this.events.publishError({
              code: "userCallbackFailed" /* UserCallbackFailed */,
              message: "Typed message handler failed.",
              cause
            }, signal);
          }
        }
      }
    }
    findDeliverableIndex() {
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued !== void 0 && (this.handlers.get(queued.message.name)?.size ?? 0) > 0) {
          return index;
        }
      }
      return -1;
    }
    hasQueuedMessage(name) {
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        if (this.queue[index]?.message.name === name) return true;
      }
      return false;
    }
    advanceHead() {
      while (this.queueHead < this.queue.length && this.queue[this.queueHead] === void 0) {
        this.queueHead += 1;
      }
    }
    compactQueue() {
      if (this.queuedCount === 0) {
        this.queue.length = 0;
        this.queueHead = 0;
        return;
      }
      if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
        this.queue.splice(0, this.queueHead);
        this.queueHead = 0;
      }
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts
  var ZlinkStreamFrameSender = class {
    constructor(protocol, flowContext, metrics) {
      this.protocol = protocol;
      this.flowContext = flowContext;
      this.metrics = metrics;
    }
    pendingWrites = /* @__PURE__ */ new Set();
    async send(connection, kind, name, payload, metadata, compress, requestSeq, signal, correlationId, explicitFlow) {
      throwIfAborted(signal);
      const flow = this.protocol.flowEnabled() ? this.flowContext.currentOrCreate(explicitFlow) : void 0;
      await this.write(
        connection,
        this.protocol.encode(kind, name, payload, metadata, compress, requestSeq, correlationId, flow?.flowId, flow?.flowOrigin),
        signal
      );
    }
    async sendControl(connection, name, signal) {
      await this.write(connection, this.protocol.encodeControl(name), signal);
    }
    async drain(signal) {
      while (this.pendingWrites.size > 0) {
        throwIfAborted(signal);
        await Promise.allSettled([...this.pendingWrites]);
      }
    }
    async write(connection, frame, signal) {
      const write = connection.write(frame, signal);
      this.pendingWrites.add(write);
      try {
        await write;
        this.metrics.outbound(frame.byteLength);
      } finally {
        this.pendingWrites.delete(write);
      }
    }
  };

  // packages/stream-connector/src/Runtime/Protocol/ZlinkSessionClosing.ts
  var ZLINK_SESSION_CLOSING = "session-closing";
  var reasons = {
    1: "ClientClose",
    2: "IdleTimeout",
    3: "HeartbeatTimeout",
    4: "ServerDrain",
    5: "ProtocolError",
    6: "TransportError"
  };
  function decodeSessionClosing(payload) {
    if (payload.length < 4 || payload[0] !== 1) throw new Error("Unsupported session-closing version.");
    const closeReason = reasons[payload[1]];
    if (closeReason === void 0) throw new Error("Unknown session-closing reason.");
    const length = payload[2] << 8 | payload[3];
    if (length > 512 || payload.length !== 4 + length) throw new Error("Invalid session-closing diagnostic length.");
    const diagnostic = length === 0 ? void 0 : new TextDecoder("utf-8", { fatal: true }).decode(payload.subarray(4));
    return { closeReason, diagnostic };
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamReceiveDispatcher.ts
  var ZlinkStreamReceiveDispatcher = class {
    constructor(protocol, pendingRequests, receivedMessages, frameSender, events, flowContext, metrics, serverClosing) {
      this.protocol = protocol;
      this.pendingRequests = pendingRequests;
      this.receivedMessages = receivedMessages;
      this.frameSender = frameSender;
      this.events = events;
      this.flowContext = flowContext;
      this.metrics = metrics;
      this.serverClosing = serverClosing;
    }
    async readAndDispatch(connection, signal, isCurrent) {
      if (connection?.read === void 0) {
        return { available: false, inbound: false };
      }
      const frameBytes = await connection.read(signal);
      if (isCurrent !== void 0 && !isCurrent()) {
        return { available: false, inbound: false };
      }
      if (frameBytes === void 0) {
        return { available: false, inbound: false };
      }
      this.metrics.inbound(frameBytes.byteLength);
      const flowEnabled = this.protocol.flowEnabled();
      let frames;
      try {
        frames = this.protocol.decodeFrames(frameBytes, flowEnabled);
      } catch (cause) {
        await this.events.publishError(
          toStreamError(cause, "frameDecodeFailed" /* FrameDecodeFailed */, "Frame decode failed."),
          signal
        );
        return { available: true, inbound: false };
      }
      for (const frame of frames) {
        try {
          await this.dispatch(connection, frame.header, frame.payload, signal, flowEnabled);
        } catch (cause) {
          if (frame.header.kind === 5 /* Control */ && frame.header.name === ZLINK_STREAM_HEARTBEAT_PING) {
            throw cause;
          }
          await this.events.publishError(
            toStreamError(cause, "frameDecodeFailed" /* FrameDecodeFailed */, "Frame dispatch failed."),
            signal
          );
        }
      }
      return { available: true, inbound: true };
    }
    async dispatch(connection, header, payload, signal, flowEnabled) {
      if (header.kind === 3 /* Response */ && header.requestSeq !== void 0) {
        try {
          if (!this.pendingRequests.resolve(header.requestSeq, {
            codec: header.codec,
            payload: this.protocol.decodePayload(header, payload)
          })) {
            await this.events.publishError({
              code: "frameDecodeFailed" /* FrameDecodeFailed */,
              message: `Response request sequence '${header.requestSeq}' has no pending request.`
            }, signal);
          }
        } catch (cause) {
          const decodeError = toStreamError(
            cause,
            "decompressionFailed" /* DecompressionFailed */,
            "Decompression failed."
          );
          if (!this.pendingRequests.reject(
            header.requestSeq,
            decodeError
          )) {
            await this.events.publishError(decodeError, signal);
          }
        }
        return;
      }
      if (header.kind === 4 /* Error */ && header.requestSeq !== void 0) {
        try {
          const remoteError = decodeRemoteError(this.protocol, header, payload);
          if (!this.pendingRequests.reject(header.requestSeq, remoteError)) {
            await this.events.publishError(remoteError, signal);
          }
        } catch (cause) {
          const decodeError = toStreamError(
            cause,
            "frameDecodeFailed" /* FrameDecodeFailed */,
            "Remote error payload is invalid."
          );
          if (!this.pendingRequests.reject(
            header.requestSeq,
            decodeError
          )) {
            await this.events.publishError(decodeError, signal);
          }
        }
        return;
      }
      if (header.kind === 4 /* Error */) {
        await this.events.publishError(decodeRemoteError(this.protocol, header, payload), signal);
        return;
      }
      if (header.kind === 5 /* Control */) {
        await this.dispatchControl(connection, header, payload, signal);
        return;
      }
      if (header.kind === 1 /* Send */) {
        const flow = flowEnabled ? this.flowContext.createInbound(header.flowId, header.flowOrigin) : void 0;
        this.receivedMessages.enqueue({
          name: header.name,
          metadata: header.metadata,
          payload: { codec: header.codec, payload: this.protocol.decodePayload(header, payload) },
          flowId: flow?.flowId,
          flowOrigin: flow?.flowOrigin
        }, signal);
      }
    }
    async dispatchControl(connection, header, payload, signal) {
      if (header.name === ZLINK_SESSION_CLOSING) {
        const closing = decodeSessionClosing(payload);
        await this.serverClosing?.(closing.closeReason);
        return;
      }
      if (payload.length !== 0) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Control packet payload must be empty.");
      }
      if (header.name === ZLINK_STREAM_HEARTBEAT_PING) {
        try {
          await this.frameSender.sendControl(connection, ZLINK_STREAM_HEARTBEAT_PONG, signal);
        } catch (cause) {
          throw connectorError(
            "sendFailed" /* SendFailed */,
            cause instanceof Error ? cause.message : "Heartbeat pong send failed."
          );
        }
        return;
      }
      if (header.name !== ZLINK_STREAM_HEARTBEAT_PONG) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown control packet.");
      }
    }
  };
  function decodeRemoteError(protocol, header, payload) {
    const decodedPayload = protocol.decodePayload(header, payload);
    let decoded;
    try {
      decoded = JSON.parse(utf8Decode2(decodedPayload));
    } catch (cause) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Remote error payload must be a JSON object.", cause);
    }
    if (decoded === null || typeof decoded !== "object" || Array.isArray(decoded) || typeof decoded.code !== "string" || typeof decoded.message !== "string") {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Remote error payload must contain string code and message fields."
      );
    }
    const remote = decoded;
    return { code: "remoteError" /* RemoteError */, message: remote.message, cause: remote };
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamConnectorLifecycle.ts
  var ZlinkStreamConnectorLifecycle = class {
    constructor(options, pendingRequests, frameSender, receiveDispatcher, events, metrics) {
      this.options = options;
      this.pendingRequests = pendingRequests;
      this.frameSender = frameSender;
      this.receiveDispatcher = receiveDispatcher;
      this.events = events;
      this.metrics = metrics;
    }
    receiveLoopAbort;
    currentConnection;
    connectionGeneration = 0;
    currentState = "created" /* Created */;
    heartbeatTimer;
    lastInboundAt = 0;
    closeTask;
    connectTask;
    disconnectTask;
    closeRequested = false;
    disconnectedPublished = false;
    closeReasonValue;
    lateConnectCleanupError;
    get isConnected() {
      return this.currentState === "connected" /* Connected */;
    }
    get state() {
      return this.currentState;
    }
    get closeReason() {
      return this.closeReasonValue;
    }
    async connect(signal) {
      throwIfAborted(signal);
      await this.disconnectTask?.catch(() => void 0);
      if (this.closeRequested || this.currentState === "closed" /* Closed */) {
        throw connectorError("disconnected" /* Disconnected */, "Connector is closed.");
      }
      if (this.currentState === "connected" /* Connected */) {
        return;
      }
      if (this.connectTask !== void 0) {
        return await this.connectTask;
      }
      this.connectTask = this.connectOnce(signal).finally(() => {
        this.connectTask = void 0;
      });
      return await this.connectTask;
    }
    async connectOnce(signal) {
      await this.setState("connecting" /* Connecting */, void 0, signal);
      try {
        const connection = await this.connectWithReconnect(signal);
        if (this.closeRequested) {
          try {
            await connection.close(signal);
          } catch (error) {
            this.lateConnectCleanupError = error;
            throw error;
          }
          throw connectorError("disconnected" /* Disconnected */, "Connector closed while connecting.");
        }
        this.currentConnection = connection;
        this.connectionGeneration += 1;
        this.disconnectedPublished = false;
        this.lastInboundAt = Date.now();
        await this.setState("connected" /* Connected */, void 0, signal);
        this.startHeartbeat();
        this.startReceiveLoop();
      } catch (cause) {
        if (this.closeRequested) {
          const message = cause instanceof Error ? cause.message : "Connector closed while connecting.";
          const error2 = toStreamError(cause, "disconnected" /* Disconnected */, message);
          throw new ZlinkStreamException(error2);
        }
        const error = toStreamError(cause, "connectTimeout" /* ConnectTimeout */, "Connect failed.");
        await this.setState("disconnected" /* Disconnected */, error, signal);
        throw new ZlinkStreamException(error);
      }
    }
    async close(signal) {
      this.closeReasonValue = "ClientClose";
      this.closeRequested = true;
      if (this.closeTask !== void 0) {
        return await this.closeTask;
      }
      if (this.currentState === "closed" /* Closed */) {
        return;
      }
      this.closeTask = this.closeOnce(signal).finally(() => {
        this.closeTask = void 0;
      });
      return await this.closeTask;
    }
    async serverClosing(reason) {
      this.closeReasonValue = reason;
      const error = { code: "disconnected" /* Disconnected */, message: `Server closed the session: ${reason}.` };
      await this.disconnectForTransportFailure(error, this.currentConnection, this.connectionGeneration);
    }
    async closeOnce(signal) {
      await this.connectTask?.catch(() => void 0);
      await this.disconnectTask?.catch(() => void 0);
      const connection = this.currentConnection;
      this.stopHeartbeat();
      this.stopReceiveLoop();
      this.currentConnection = void 0;
      const errors = [];
      if (this.lateConnectCleanupError !== void 0) {
        errors.push(this.lateConnectCleanupError);
        this.lateConnectCleanupError = void 0;
      }
      try {
        await this.frameSender.drain(signal);
      } catch (error) {
        errors.push(error);
      }
      try {
        await connection?.close(signal);
      } catch (error) {
        errors.push(error);
      }
      this.pendingRequests.failAll({ code: "disconnected" /* Disconnected */, message: "Connector closed." });
      await this.setState("closed" /* Closed */, void 0, signal);
      await this.publishDisconnectedOnce(signal);
      if (errors.length === 1) throw errors[0];
      if (errors.length > 1) throw new AggregateError(errors, "Stream connector close failed.");
    }
    async dispatch(signal) {
      const connection = this.currentConnection;
      const generation = this.connectionGeneration;
      try {
        await this.dispatchAvailable(connection, generation, signal);
      } catch (cause) {
        const error = toStreamError(cause, "frameDecodeFailed" /* FrameDecodeFailed */, "Stream dispatch failed.");
        await this.disconnectForTransportFailure(error, connection, generation);
        throw new ZlinkStreamException(error);
      }
    }
    connectionForSend() {
      if (this.currentConnection === void 0 || this.currentState !== "connected" /* Connected */) {
        throw connectorError("disconnected" /* Disconnected */, "Connector is not connected.");
      }
      return this.currentConnection;
    }
    async dispatchAvailable(connection, generation, signal) {
      throwIfAborted(signal);
      const result = await this.receiveDispatcher.readAndDispatch(
        connection,
        signal,
        () => this.isCurrentConnection(connection, generation)
      );
      if (result.inbound && this.isCurrentConnection(connection, generation)) {
        this.lastInboundAt = Date.now();
      }
      return result.available;
    }
    async connectWithReconnect(signal) {
      let attempt = 0;
      let delayMs = this.options.reconnect.initialDelayMs;
      let lastError;
      const maxAttempts = this.options.reconnect.enabled ? this.options.reconnect.maxAttempts : 1;
      while (attempt < maxAttempts) {
        attempt += 1;
        if (attempt > 1) {
          this.metrics.reconnect();
        }
        const handshakeStartedAt = performance.now();
        try {
          const connection = await this.options.transportFactory.connect(this.options, signal);
          this.metrics.handshakeCompleted(handshakeStartedAt);
          return connection;
        } catch (cause) {
          this.metrics.handshakeCompleted(handshakeStartedAt);
          this.metrics.handshakeFailed(cause);
          lastError = toStreamError(cause, "connectTimeout" /* ConnectTimeout */, "Connect failed.");
          if (!this.options.reconnect.enabled || attempt >= maxAttempts) {
            break;
          }
          await this.setState("reconnecting" /* Reconnecting */, lastError, signal);
          await delay(delayMs, signal);
          delayMs = Math.min(
            this.options.reconnect.maxDelayMs,
            Math.ceil(delayMs * this.options.reconnect.backoffFactor)
          );
        }
      }
      throw new ZlinkStreamException(
        lastError ?? { code: "connectTimeout" /* ConnectTimeout */, message: "Connect failed." }
      );
    }
    startHeartbeat() {
      this.stopHeartbeat();
      if (!this.options.heartbeat.enabled) {
        return;
      }
      this.heartbeatTimer = setInterval(() => {
        void this.runHeartbeatTick();
      }, this.options.heartbeat.intervalMs);
    }
    stopHeartbeat() {
      if (this.heartbeatTimer !== void 0) {
        clearInterval(this.heartbeatTimer);
        this.heartbeatTimer = void 0;
      }
    }
    startReceiveLoop() {
      if (this.options.dispatchMode !== "immediate" /* Immediate */ || this.currentConnection?.read === void 0) {
        return;
      }
      this.stopReceiveLoop();
      const abort = new AbortController();
      const connection = this.currentConnection;
      const generation = this.connectionGeneration;
      this.receiveLoopAbort = abort;
      void this.runReceiveLoop(connection, generation, abort.signal);
    }
    stopReceiveLoop() {
      this.receiveLoopAbort?.abort();
      this.receiveLoopAbort = void 0;
    }
    async runReceiveLoop(connection, generation, signal) {
      try {
        while (this.shouldContinueReceiveLoop(connection, generation, signal)) {
          const dispatched = await this.dispatchAvailable(connection, generation, signal);
          if (!dispatched && this.shouldContinueReceiveLoop(connection, generation, signal)) {
            await delay(1, signal);
          }
        }
      } catch (cause) {
        if (signal.aborted) return;
        const error = toStreamError(cause, "frameDecodeFailed" /* FrameDecodeFailed */, "Receive loop failed.");
        await this.disconnectForTransportFailure(error, connection, generation);
      }
    }
    shouldContinueReceiveLoop(connection, generation, signal) {
      return !signal.aborted && this.currentState === "connected" /* Connected */ && this.isCurrentConnection(connection, generation);
    }
    async runHeartbeatTick() {
      if (!this.isConnected) {
        return;
      }
      if (Date.now() - this.lastInboundAt > this.options.heartbeat.timeoutMs) {
        this.closeReasonValue = "HeartbeatTimeout";
        const error = { code: "disconnected" /* Disconnected */, message: "Heartbeat timed out." };
        await this.disconnectForTransportFailure(error, this.currentConnection, this.connectionGeneration);
        return;
      }
      const connection = this.currentConnection;
      const generation = this.connectionGeneration;
      try {
        await this.frameSender.sendControl(this.connectionForSend(), ZLINK_STREAM_HEARTBEAT_PING);
      } catch (cause) {
        const error = toStreamError(cause, "sendFailed" /* SendFailed */, "Heartbeat send failed.");
        await this.disconnectForTransportFailure(error, connection, generation);
      }
    }
    async disconnectForTransportFailure(error, origin, generation) {
      this.closeReasonValue ??= "TransportError";
      if (this.closeRequested || this.currentState === "closed" /* Closed */) {
        return;
      }
      if (origin !== void 0 && !this.isCurrentConnection(origin, generation)) {
        return;
      }
      if (this.disconnectTask !== void 0) {
        return await this.disconnectTask;
      }
      this.disconnectTask = this.disconnectOnce(error).finally(() => {
        this.disconnectTask = void 0;
      });
      return await this.disconnectTask;
    }
    isCurrentConnection(connection, generation) {
      return !this.closeRequested && this.currentConnection === connection && this.connectionGeneration === generation;
    }
    async disconnectOnce(error) {
      this.stopHeartbeat();
      this.stopReceiveLoop();
      const connection = this.currentConnection;
      this.currentConnection = void 0;
      this.pendingRequests.failAll(error);
      try {
        await connection?.close();
      } catch {
      }
      if (this.closeRequested) return;
      await this.setState("disconnected" /* Disconnected */, error);
      await this.publishDisconnectedOnce();
      if (this.shouldReconnect()) {
        queueMicrotask(() => {
          void this.connect().catch(() => void 0);
        });
      }
    }
    shouldReconnect() {
      return this.options.reconnect.enabled && !this.closeRequested;
    }
    async publishDisconnectedOnce(signal) {
      if (this.disconnectedPublished) return;
      this.disconnectedPublished = true;
      await this.events.publishDisconnected(signal);
    }
    async setState(current, error, signal) {
      const previous = this.currentState;
      this.currentState = current;
      if (previous === current && error === void 0) {
        return;
      }
      await this.events.publishStateChanged({ previous, current, error }, signal);
      if (error !== void 0) {
        await this.events.publishError(error, signal);
      }
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamConnectorEvents.ts
  var ZlinkStreamConnectorEvents = class {
    errorHandlers = /* @__PURE__ */ new Set();
    disconnectedHandlers = /* @__PURE__ */ new Set();
    stateHandlers = /* @__PURE__ */ new Set();
    onError(handler) {
      this.errorHandlers.add(handler);
      return subscription(() => this.errorHandlers.delete(handler));
    }
    onDisconnected(handler) {
      this.disconnectedHandlers.add(handler);
      return subscription(() => this.disconnectedHandlers.delete(handler));
    }
    onStateChanged(handler) {
      this.stateHandlers.add(handler);
      return subscription(() => this.stateHandlers.delete(handler));
    }
    async publishError(error, signal) {
      await this.publish([...this.errorHandlers].map((handler) => () => handler(error, signal)));
    }
    async publishDisconnected(signal) {
      await this.publish([...this.disconnectedHandlers].map((handler) => () => handler(signal)));
    }
    async publishStateChanged(change, signal) {
      await this.publish([...this.stateHandlers].map((handler) => () => handler(change, signal)));
    }
    async publish(handlers) {
      await Promise.allSettled(handlers.map(async (handler) => handler()));
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkFlowContext.ts
  var BrowserZlinkFlowContext = class {
    currentOrCreate(explicit) {
      return explicit ?? { flowId: this.createUuidV7(), flowOrigin: "Application" };
    }
    createInbound(flowId, flowOrigin) {
      return { flowId: flowId ?? this.createUuidV7(), flowOrigin: flowOrigin ?? "Inbound" };
    }
    createUuidV7() {
      const crypto = globalThis.crypto;
      if (crypto === void 0) {
        throw connectorError(
          "configurationError" /* ConfigurationError */,
          "The browser entrypoint requires the platform Web Crypto API."
        );
      }
      return formatUuidV7(crypto.getRandomValues(new Uint8Array(16)));
    }
  };
  function formatUuidV7(bytes) {
    const timestamp = BigInt(Date.now());
    for (let index = 5; index >= 0; index -= 1) {
      bytes[index] = Number(timestamp >> BigInt((5 - index) * 8) & 0xffn);
    }
    bytes[6] = 112 | bytes[6] & 15;
    bytes[8] = 128 | bytes[8] & 63;
    const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  }

  // packages/stream-connector/src/Runtime/Transport/BrowserWebSocketConnection.ts
  var BrowserStreamTransportFactory = class {
    async connect(options, signal) {
      throwIfAborted(signal);
      const WebSocketConstructor = globalThis.WebSocket;
      if (WebSocketConstructor === void 0) {
        throw connectorError(
          "configurationError" /* ConfigurationError */,
          "The browser entrypoint requires the platform WebSocket API."
        );
      }
      const socket = new WebSocketConstructor(options.endpoint);
      socket.binaryType = "arraybuffer";
      await waitForOpen(socket, options.connectTimeoutMs, signal);
      return new BrowserWebSocketConnection(socket);
    }
  };
  var BrowserWebSocketConnection = class {
    constructor(socket) {
      this.socket = socket;
      socket.addEventListener("message", this.onMessage);
      socket.addEventListener("close", this.onClose);
      socket.addEventListener("error", this.onError);
    }
    messages = [];
    messageHead = 0;
    closed = false;
    error;
    readWaiter;
    async write(frame, signal) {
      throwIfAborted(signal);
      if (this.closed || this.socket.readyState !== 1) {
        throw connectorError("disconnected" /* Disconnected */, "Remote stream is not connected.");
      }
      try {
        this.socket.send(frame);
      } catch (cause) {
        throw connectorError("sendFailed" /* SendFailed */, "Send failed.", cause);
      }
    }
    async read(signal) {
      throwIfAborted(signal);
      for (; ; ) {
        const message = this.takeMessage();
        if (message !== void 0) {
          return message;
        }
        if (this.error !== void 0) {
          throw this.error;
        }
        if (this.closed) {
          return void 0;
        }
        await this.waitForMessage(signal);
      }
    }
    async close(signal) {
      throwIfAborted(signal);
      if (!this.closed) {
        this.closed = true;
        this.socket.close();
        this.wakeReader();
      }
      try {
        await waitForClose(this.socket, signal);
      } finally {
        this.removeListeners();
      }
    }
    onMessage = (event) => {
      try {
        const message = toUint8Array(event.data);
        this.messages.push(message);
      } catch (cause) {
        this.error = cause instanceof Error ? cause : connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "WebSocket message decode failed.", cause);
        this.closed = true;
        this.socket.close();
      }
      this.wakeReader();
    };
    onClose = () => {
      if (!this.closed) {
        this.error = connectorError("disconnected" /* Disconnected */, "Remote stream closed the WebSocket connection.");
      }
      this.closed = true;
      this.wakeReader();
    };
    onError = () => {
      this.error = connectorError("disconnected" /* Disconnected */, "Remote stream closed after a WebSocket error.");
      this.closed = true;
      this.wakeReader();
    };
    waitForMessage(signal) {
      if (this.readWaiter !== void 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Only one pending stream read is supported.");
      }
      return new Promise((resolve, reject) => {
        const onAbort = () => {
          this.readWaiter = void 0;
          reject(connectorError("disconnected" /* Disconnected */, "Operation canceled."));
        };
        signal?.addEventListener("abort", onAbort, { once: true });
        this.readWaiter = () => {
          signal?.removeEventListener("abort", onAbort);
          this.readWaiter = void 0;
          resolve();
        };
        if (this.hasQueuedMessage() || this.closed) {
          this.wakeReader();
        }
      });
    }
    wakeReader() {
      this.readWaiter?.();
    }
    removeListeners() {
      this.socket.removeEventListener("message", this.onMessage);
      this.socket.removeEventListener("close", this.onClose);
      this.socket.removeEventListener("error", this.onError);
    }
    hasQueuedMessage() {
      return this.messageHead < this.messages.length;
    }
    takeMessage() {
      if (!this.hasQueuedMessage()) return void 0;
      const message = this.messages[this.messageHead];
      this.messages[this.messageHead] = void 0;
      this.messageHead += 1;
      if (this.messageHead >= 1024 && this.messageHead * 2 >= this.messages.length) {
        this.messages.splice(0, this.messageHead);
        this.messageHead = 0;
      }
      return message;
    }
  };
  function waitForClose(socket, signal) {
    if (socket.readyState === 3) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const onClose = () => finish();
      const onAbort = () => finish(connectorError("disconnected" /* Disconnected */, "Close canceled."));
      const finish = (error) => {
        socket.removeEventListener("close", onClose);
        signal?.removeEventListener("abort", onAbort);
        if (error === void 0) resolve();
        else reject(error);
      };
      socket.addEventListener("close", onClose, { once: true });
      signal?.addEventListener("abort", onAbort, { once: true });
    });
  }
  async function waitForOpen(socket, connectTimeoutMs, signal) {
    throwIfAborted(signal);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => finish(
        connectorError("connectTimeout" /* ConnectTimeout */, "Connect timed out.")
      ), connectTimeoutMs);
      const onOpen = () => finish();
      const onClose = () => finish(connectorError("connectTimeout" /* ConnectTimeout */, "Connect closed before opening."));
      const onError = () => finish(connectorError("connectTimeout" /* ConnectTimeout */, "Connect failed."));
      const onAbort = () => finish(connectorError("disconnected" /* Disconnected */, "Connect canceled."));
      const finish = (error) => {
        clearTimeout(timeout);
        socket.removeEventListener("open", onOpen);
        socket.removeEventListener("close", onClose);
        socket.removeEventListener("error", onError);
        signal?.removeEventListener("abort", onAbort);
        if (error === void 0) {
          resolve();
        } else {
          socket.close();
          reject(error);
        }
      };
      socket.addEventListener("open", onOpen, { once: true });
      socket.addEventListener("close", onClose, { once: true });
      socket.addEventListener("error", onError, { once: true });
      signal?.addEventListener("abort", onAbort, { once: true });
    });
  }
  function toUint8Array(data) {
    if (data instanceof ArrayBuffer) {
      return new Uint8Array(data);
    }
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "WebSocket text messages are not supported.");
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamRuntimeMetrics.ts
  var ZlinkStreamRuntimeMetrics = class {
    constructor(options) {
      this.options = options;
      const meter = options.meterProvider?.getMeter("zlink.framework");
      this.reconnects = meter?.createCounter("zlink.stream.reconnects", { unit: "{event}" });
      this.handshakeDuration = meter?.createHistogram("zlink.stream.handshake.duration", { unit: "s" });
      this.handshakeFailures = meter?.createCounter("zlink.stream.handshake.failures", { unit: "{failure}" });
      this.inboundBytes = meter?.createCounter("zlink.stream.inbound.bytes", { unit: "By" });
      this.outboundBytes = meter?.createCounter("zlink.stream.outbound.bytes", { unit: "By" });
    }
    reconnects;
    handshakeDuration;
    handshakeFailures;
    inboundBytes;
    outboundBytes;
    reconnect() {
      this.safe(() => this.reconnects?.add(1, { transport: this.transportLabel() }));
    }
    handshakeCompleted(startedAt) {
      const seconds = (performance.now() - startedAt) / 1e3;
      this.safe(() => this.handshakeDuration?.record(seconds, { transport: this.transportLabel() }));
    }
    handshakeFailed(error) {
      const reason = error instanceof Error && (error.name === "AbortError" || error.name === "TimeoutError") ? "canceled" : "transport_error";
      this.safe(() => this.handshakeFailures?.add(1, { transport: this.transportLabel(), reason }));
    }
    inbound(byteCount) {
      this.safe(() => this.inboundBytes?.add(byteCount, { transport: this.transportLabel() }));
    }
    outbound(byteCount) {
      this.safe(() => this.outboundBytes?.add(byteCount, { transport: this.transportLabel() }));
    }
    transportLabel() {
      return this.options.endpoint.startsWith("wss:") ? "wss" : "ws";
    }
    safe(record) {
      try {
        record();
      } catch {
      }
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamConnector.ts
  var DefaultZlinkStreamConnector = class {
    static heartbeatPingName = ZLINK_STREAM_HEARTBEAT_PING;
    static heartbeatPongName = ZLINK_STREAM_HEARTBEAT_PONG;
    receivedMessages;
    lifecycle;
    events = new ZlinkStreamConnectorEvents();
    correlationCounter = 0n;
    pendingRequests = new ZlinkStreamPendingRequests();
    frameSender;
    receiveDispatcher;
    diagnosticsLevelCell;
    options;
    constructor(options) {
      const flowContext = new BrowserZlinkFlowContext();
      this.options = normalizeOptions(options, new BrowserStreamTransportFactory());
      this.diagnosticsLevelCell = new ZlinkStreamDiagnosticsLevelCell(this.options.diagnosticsLevel);
      Object.defineProperty(this.options, "diagnosticsLevel", {
        enumerable: true,
        configurable: true,
        get: () => this.diagnosticsLevelCell.level
      });
      const metrics = new ZlinkStreamRuntimeMetrics(this.options);
      const protocol = new ZlinkStreamFrameProtocol(this.options);
      this.frameSender = new ZlinkStreamFrameSender(protocol, flowContext, metrics);
      this.receivedMessages = new ZlinkStreamReceivedMessages(this.events);
      this.receiveDispatcher = new ZlinkStreamReceiveDispatcher(
        protocol,
        this.pendingRequests,
        this.receivedMessages,
        this.frameSender,
        this.events,
        flowContext,
        metrics,
        (reason) => this.lifecycle.serverClosing(reason)
      );
      this.lifecycle = new ZlinkStreamConnectorLifecycle(
        this.options,
        this.pendingRequests,
        this.frameSender,
        this.receiveDispatcher,
        this.events,
        metrics
      );
    }
    get isConnected() {
      return this.lifecycle.isConnected;
    }
    get closeReason() {
      return this.lifecycle.closeReason;
    }
    get state() {
      return this.lifecycle.state;
    }
    get pendingDispatchCount() {
      return this.pendingRequests.count;
    }
    /**
     * Current diagnostics level (spec 26 §4.1, spec stream-connector 32 §13).
     * Reflects the level set by the most recent {@link setDiagnosticsLevel}
     * call, or the construction-time option (default
     * {@link ZlinkStreamDiagnosticsLevel.Errors}) if it was never changed.
     */
    get diagnosticsLevel() {
      return this.diagnosticsLevelCell.level;
    }
    /**
     * Changes the diagnostics level in place without recreating the connector
     * (spec 26 §4.1, spec stream-connector 32 §13). The change is an atomic
     * state update: it applies to processing points that read the level after
     * this call returns and is never applied retroactively to frames already
     * built. Rejects unknown values with {@link ZlinkStreamErrorCode.ConfigurationError}.
     * Do not call this synchronous bridge from a framework execution context such
     * as a handler or callback; use setDiagnosticsLevelAsync there.
     */
    setDiagnosticsLevel(level) {
      void this.setDiagnosticsLevelAsync(level);
    }
    setDiagnosticsLevelAsync(level) {
      this.diagnosticsLevelCell.set(level);
      return Promise.resolve();
    }
    onErrorReceived(handler) {
      return this.events.onError(handler);
    }
    onDisconnected(handler) {
      return this.events.onDisconnected(handler);
    }
    onConnectionStateChanged(handler) {
      return this.events.onStateChanged(handler);
    }
    async connect(signal) {
      await this.lifecycle.connect(signal);
    }
    async close(signal) {
      await this.lifecycle.close(signal);
    }
    async dispatch(signal) {
      await this.lifecycle.dispatch(signal);
    }
    send(payload, messageType) {
      const encoded = this.encodePayload(payload, messageType);
      return new ZlinkStreamSendBuilder(this, this.resolveNameOrDefault(encoded), encoded);
    }
    request(payload, messageType) {
      const encoded = this.encodePayload(payload, messageType);
      return new ZlinkStreamRequestBuilder(this, this.resolveNameOrDefault(encoded), encoded);
    }
    on(name, handler, messageType) {
      const encodedHandler = (message, signal) => handler({
        name: message.name,
        metadata: message.metadata,
        payload: this.decodePayload(message.payload, messageType),
        flowId: message.flowId,
        flowOrigin: message.flowOrigin
      }, signal);
      return this.receivedMessages.on(name, encodedHandler);
    }
    waitFor(name) {
      validateName(name);
      return new ZlinkStreamWaitBuilder(this, name);
    }
    expectNone(name) {
      validateName(name);
      return new ZlinkStreamExpectNoneBuilder(this, name);
    }
    waitForSequence(name) {
      validateName(name);
      return new ZlinkStreamSequenceBuilder(this, name);
    }
    waitForMessage(name, timeoutMs, predicate, signal) {
      validateName(name);
      if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Timeout must be a non-negative finite number.");
      }
      throwIfAborted(signal);
      return new Promise((resolve, reject) => {
        let done = false;
        let timer;
        let disposable;
        const onAbort = () => finish(connectorError("disconnected" /* Disconnected */, "Operation canceled."));
        const finish = (error, message) => {
          if (done) {
            return;
          }
          done = true;
          signal?.removeEventListener("abort", onAbort);
          if (timer !== void 0) {
            clearTimeout(timer);
          }
          disposable?.dispose();
          if (error !== void 0) {
            reject(error);
          } else {
            resolve(message);
          }
        };
        timer = setTimeout(() => {
          finish(connectorError("requestTimeout" /* RequestTimeout */, "Wait for stream message timed out."));
        }, timeoutMs);
        signal?.addEventListener("abort", onAbort, { once: true });
        disposable = this.receivedMessages.on(name, (message) => {
          try {
            const decoded = {
              name: message.name,
              metadata: message.metadata,
              payload: this.decodeWaitPayload(message.payload),
              flowId: message.flowId,
              flowOrigin: message.flowOrigin
            };
            if (predicate(decoded)) {
              finish(void 0, decoded);
            }
          } catch (cause) {
            finish(cause);
          }
        });
      });
    }
    encodePayload(payload, messageType) {
      if (isEncodedPayload(payload)) {
        return payload;
      }
      const codec = this.options.codec ?? zlinkStreamJsonCodec;
      return codec.encode(payload, messageType);
    }
    decodePayload(payload, messageType) {
      if (messageType === void 0 && this.options.codec === void 0) {
        return payload;
      }
      return (this.options.codec ?? zlinkStreamJsonCodec).decode(payload, messageType);
    }
    decodeWaitPayload(payload) {
      if (this.options.codec !== void 0 || payload.codec === 1 /* Json */) {
        return (this.options.codec ?? zlinkStreamJsonCodec).decode(payload);
      }
      return payload;
    }
    async sendEncoded(kind, name, payload, metadata, compress, requestSeq, signal, flow, correlationId) {
      await this.frameSender.send(
        this.lifecycle.connectionForSend(),
        kind,
        name,
        payload,
        metadata,
        compress,
        requestSeq,
        signal,
        correlationId,
        flow
      );
    }
    /**
     * Per-connector monotonic correlation id (hex). The client generates it on each request
     * and the server echoes it back on the reply, so flows can be joined across the wire.
     */
    nextCorrelationId() {
      this.correlationCounter += 1n;
      return this.correlationCounter.toString(16);
    }
    async requestEncoded(name, payload, metadata, compress, timeoutMs, signal, flow) {
      const pending = this.pendingRequests.create(name, timeoutMs);
      try {
        await this.sendEncoded(
          2 /* Request */,
          name,
          payload,
          metadata,
          compress,
          pending.requestSeq,
          signal,
          flow,
          this.nextCorrelationId()
        );
        return await pending.promise;
      } catch (error) {
        this.pendingRequests.cancel(pending.requestSeq);
        throw error;
      }
    }
    resolveNameOrDefault(payload) {
      if (payload.messageType === void 0) {
        return void 0;
      }
      return this.options.nameResolver.resolve(payload.messageType);
    }
  };
  function isEncodedPayload(value) {
    if (value === null || typeof value !== "object") {
      return false;
    }
    const candidate = value;
    return typeof candidate.codec === "number" && candidate.payload instanceof Uint8Array;
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamAssertions.ts
  var zlinkStreamAssert = {
    ensure(condition, message) {
      if (!condition) {
        throw connectorError("validationFailed" /* ValidationFailed */, message);
      }
    },
    async expectFailure(action, errorKind) {
      let failure;
      try {
        await action();
      } catch (error) {
        failure = error;
      }
      if (failure === void 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Expected action to fail.");
      }
      const streamError = unwrapStreamError(failure);
      if (errorKind !== void 0 && streamError.code !== errorKind) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          `Expected failure kind '${errorKind}', got '${streamError.code}'.`,
          failure
        );
      }
      return streamError;
    },
    async expectTimeout(action) {
      let failure;
      try {
        await action();
      } catch (error) {
        failure = error;
      }
      if (failure === void 0) {
        throw connectorError("validationFailed" /* ValidationFailed */, "Expected action to time out.");
      }
      const code = unwrapStreamError(failure).code;
      if (code !== "requestTimeout" /* RequestTimeout */ && code !== "connectTimeout" /* ConnectTimeout */) {
        throw failure;
      }
    }
  };

  // packages/stream-connector/src/index.ts
  var zlinkStreamConnectorFactory = {
    create(options) {
      return new DefaultZlinkStreamConnector(options);
    }
  };
  return __toCommonJS(index_exports);
})();

// The emscripten module body and the jslib functions share one closure, so the
// `var` above is already visible to ZlinkStreamConnector.jslib. The globalThis
// assignment is the fallback for emscripten link modes that place pre-js in a
// different scope, and it is what the Node jslib harness reads.
if (typeof globalThis !== 'undefined') { globalThis.ZlinkStreamConnectorBundle = ZlinkStreamConnectorBundle; }
