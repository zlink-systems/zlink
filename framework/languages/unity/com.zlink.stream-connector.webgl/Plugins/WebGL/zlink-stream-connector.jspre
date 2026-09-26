// GENERATED FILE - DO NOT EDIT.
// Produced by framework/languages/node/scripts/sync-unity-webgl-package.mjs from
// @zlink-systems/stream-connector (package root, IIFE build).
// Package version: 0.25.0
//
// This is the same TypeScript connector the npm package root ships. The UPM
// adapter adds no wire runtime of its own (stream-connector spec 32 section 11).

"use strict";
var ZlinkStreamConnectorBundle = (() => {
  var __defProp = Object.defineProperty;
  var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
  var __getOwnPropNames = Object.getOwnPropertyNames;
  var __hasOwnProp = Object.prototype.hasOwnProperty;
  var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
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
  var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

  // packages/stream-connector/src/index.ts
  var index_exports = {};
  __export(index_exports, {
    ZlinkStreamCodec: () => ZlinkStreamCodec,
    ZlinkStreamCompression: () => ZlinkStreamCompression,
    ZlinkStreamConnectionState: () => ZlinkStreamConnectionState,
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
  function readLz4Pickle(payload) {
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
    return { data, resultDiff, resultLength: data.length + resultDiff };
  }
  function lz4PickledLength(payload) {
    return payload.length === 0 ? 0 : readLz4Pickle(payload).resultLength;
  }
  function lz4UnpicklePayload(payload, maxSize) {
    const maxDecompressedSize = maxSize != null ? maxSize : defaultMaxDecompressedPayloadSize;
    if (payload.length === 0) {
      return new Uint8Array();
    }
    const { data, resultDiff, resultLength } = readLz4Pickle(payload);
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
    hasFlowId: 16,
    hasActorSlot: 32
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
  function encodeStreamWireHeader(header, flagOverrides) {
    const flags = flagOverrides != null ? flagOverrides : defaultHeaderFlags;
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
    const hasActorSlot = header.actorSlot !== void 0;
    if (hasFlow && (header.flowId === void 0 || header.flowOrigin === void 0)) {
      throw new Error("Stream flow id and origin must be provided together.");
    }
    if (header.flowId !== void 0) validateFlowId(header.flowId);
    if (header.flowOrigin !== void 0 && ![1, 2, 3, 4].includes(header.flowOrigin)) {
      throw new Error("Stream flow origin is invalid.");
    }
    if (header.actorSlot !== void 0 && (!Number.isInteger(header.actorSlot) || header.actorSlot < 1 || header.actorSlot > 65535)) {
      throw new Error("Stream actor slot is invalid.");
    }
    let headerFlags = header.flags;
    headerFlags = hasRequestSeq ? headerFlags | flags.hasRequestSeq : headerFlags & ~flags.hasRequestSeq;
    headerFlags = hasMetadata ? headerFlags | flags.hasMetadata : headerFlags & ~flags.hasMetadata;
    headerFlags = hasCorrelation ? headerFlags | flags.hasCorrelationId : headerFlags & ~flags.hasCorrelationId;
    headerFlags = hasFlow ? headerFlags | flags.hasFlowId : headerFlags & ~flags.hasFlowId;
    headerFlags = hasActorSlot ? headerFlags | flags.hasActorSlot : headerFlags & ~flags.hasActorSlot;
    const metadataBytes = hasMetadata ? encodeStreamWireMetadata(header.metadata) : new Uint8Array();
    const size = 4 + (hasRequestSeq ? 8 : 0) + 1 + nameBytes.length + (hasMetadata ? 2 + metadataBytes.length : 0) + (hasCorrelation ? 1 + correlationBytes.length : 0) + (hasFlow ? 37 : 0) + (hasActorSlot ? 2 : 0);
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
    if (hasActorSlot) {
      writeUInt16BE(buffer, offset, header.actorSlot);
    }
    return buffer;
  }
  function decodeStreamWireHeader(header, flagOverrides, includeFlow = true) {
    const flags = flagOverrides != null ? flagOverrides : defaultHeaderFlags;
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
    const hasActorSlot = (headerFlags & flags.hasActorSlot) !== 0;
    if ((headerFlags & ~63) !== 0) {
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
    let actorSlot;
    if (hasActorSlot) {
      if (header.length - offset < 2) {
        throw new Error("Stream actor slot is incomplete.");
      }
      actorSlot = readUInt16BE(header, offset);
      if (actorSlot === 0) {
        throw new Error("Stream actor slot must not be zero.");
      }
      offset += 2;
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
      flowOrigin,
      actorSlot
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
  function lz4PickledLength2(payload) {
    return lz4PickledLength(payload);
  }
  function lz4UnpicklePayload2(payload, maxDecompressedSize) {
    return lz4UnpicklePayload(payload, maxDecompressedSize != null ? maxDecompressedSize : defaultMaxDecompressedPayloadSize);
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
    ZlinkStreamHeaderFlags2[ZlinkStreamHeaderFlags2["HasActorSlot"] = 32] = "HasActorSlot";
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
  var _ZlinkStreamMetadataMap = class _ZlinkStreamMetadataMap {
    constructor(values) {
      this.values = values;
    }
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
  __publicField(_ZlinkStreamMetadataMap, "empty", new _ZlinkStreamMetadataMap(/* @__PURE__ */ new Map()));
  var ZlinkStreamMetadataMap = _ZlinkStreamMetadataMap;
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
      messageType: messageType != null ? messageType : inferMessageType(value)
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
    var _a;
    if (value === null || value === void 0) {
      return void 0;
    }
    const constructor = (_a = Object.getPrototypeOf(value)) == null ? void 0 : _a.constructor;
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
    return {
      code: "remoteError" /* RemoteError */,
      message: error instanceof Error ? error.message : String(error),
      cause: error
    };
  }
  function subscription(dispose) {
    return { dispose };
  }
  function throwIfAborted(signal) {
    if ((signal == null ? void 0 : signal.aborted) === true) {
      throw signal.reason;
    }
  }
  function isCancellation(signal, error) {
    return (signal == null ? void 0 : signal.aborted) === true && error === signal.reason;
  }
  function delay(delayMs, signal) {
    throwIfAborted(signal);
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
        resolve();
      }, delayMs);
      const onAbort = () => {
        clearTimeout(timeout);
        reject(signal == null ? void 0 : signal.reason);
      };
      signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
    });
  }
  function utf8Encode2(value) {
    return new TextEncoder().encode(value);
  }
  function utf8Decode2(value) {
    return new TextDecoder().decode(value);
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamAssertions.ts
  var zlinkStreamAssert = {
    ensure(condition, message) {
      if (typeof message !== "string" || message.trim().length === 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "zlinkStreamAssert.ensure requires a non-empty diagnostic message."
        );
      }
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

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamPacketNameValidator.ts
  function validateName(name, allowReserved = false) {
    if (name.length === 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Message name must not be empty.");
    }
    if (!allowReserved && name.startsWith("$zlink.")) {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Message name uses a reserved zlink prefix."
      );
    }
    if (utf8Encode2(name).length > 255) {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Message name must not exceed 255 UTF-8 bytes."
      );
    }
  }

  // packages/stream-connector/src/Runtime/Calls/ZlinkStreamCallBuilders.ts
  var ZlinkStreamCallBuilderState = class {
    constructor(name, actorSlot, validateActor) {
      this.actorSlot = actorSlot;
      this.validateActor = validateActor;
      __publicField(this, "executed", false);
      __publicField(this, "name");
      __publicField(this, "metadata", ZlinkStreamMetadataMap.empty);
      __publicField(this, "timeoutMs");
      __publicField(this, "compress", false);
      this.name = name;
    }
    ensureNotExecuted() {
      var _a;
      if (this.executed) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Builder instances can be executed only once."
        );
      }
      this.executed = true;
      (_a = this.validateActor) == null ? void 0 : _a.call(this);
    }
    resolveMessageName() {
      if (this.name === void 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Message name is required when the encoded stream payload has no message type."
        );
      }
      return this.name;
    }
  };
  var ZlinkStreamSendBuilder = class {
    constructor(connector, name, payload, actorSlot, validateActor) {
      this.connector = connector;
      this.payload = payload;
      __publicField(this, "state");
      this.state = new ZlinkStreamCallBuilderState(name, actorSlot, validateActor);
    }
    packetName(name) {
      validateName(name);
      this.state.name = name;
      return this;
    }
    metadata(keyOrMetadata, value) {
      this.state.metadata = typeof keyOrMetadata === "string" ? this.state.metadata.with(keyOrMetadata, value != null ? value : "") : keyOrMetadata;
      return this;
    }
    compress() {
      this.state.compress = true;
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
        void 0,
        this.state.actorSlot
      );
    }
  };
  var ZlinkStreamRequestBuilder = class {
    constructor(connector, name, payload, enqueueCallback, actorSlot, validateActor) {
      this.connector = connector;
      this.payload = payload;
      this.enqueueCallback = enqueueCallback;
      __publicField(this, "state");
      this.state = new ZlinkStreamCallBuilderState(name, actorSlot, validateActor);
    }
    packetName(name) {
      validateName(name);
      this.state.name = name;
      return this;
    }
    metadata(keyOrMetadata, value) {
      this.state.metadata = typeof keyOrMetadata === "string" ? this.state.metadata.with(keyOrMetadata, value != null ? value : "") : keyOrMetadata;
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
    submit(signalOrCallback) {
      var _a;
      this.state.ensureNotExecuted();
      const operation = this.connector.requestEncoded(
        this.state.resolveMessageName(),
        this.payload,
        this.state.metadata,
        this.state.compress,
        (_a = this.state.timeoutMs) != null ? _a : this.connector.options.requestTimeoutMs,
        typeof signalOrCallback === "function" ? void 0 : signalOrCallback,
        this.state.actorSlot
      );
      if (typeof signalOrCallback === "function") {
        operation.then(
          (value) => this.enqueueCallback(() => signalOrCallback({ isSuccess: true, value })),
          (error) => this.enqueueCallback(
            () => signalOrCallback({ isSuccess: false, error: unwrapStreamError(error) })
          )
        );
        return;
      }
      return operation.then(
        (value) => {
          var _a2;
          return ((_a2 = this.connector.options.codec) != null ? _a2 : zlinkStreamJsonCodec).decode(value);
        }
      );
    }
    submitEncoded(signal) {
      var _a;
      this.state.ensureNotExecuted();
      return this.connector.requestEncoded(
        this.state.resolveMessageName(),
        this.payload,
        this.state.metadata,
        this.state.compress,
        (_a = this.state.timeoutMs) != null ? _a : this.connector.options.requestTimeoutMs,
        signal,
        this.state.actorSlot
      );
    }
  };
  var ZlinkStreamWaitBuilder = class {
    constructor(connector, name) {
      this.connector = connector;
      this.name = name;
      __publicField(this, "executed", false);
      __publicField(this, "timeoutMs");
      __publicField(this, "predicate", () => true);
    }
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
    async submit(signal) {
      var _a;
      this.markExecuted();
      const timeoutMs = (_a = this.timeoutMs) != null ? _a : this.connector.options.waitTimeoutMs;
      const message = await this.connector.waitForMessage(
        this.name,
        timeoutMs,
        this.predicate,
        signal
      );
      if (message === void 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          `No '${this.name}' message arrived within ${timeoutMs}ms.`
        );
      }
      return message;
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Builder instances can be executed only once."
        );
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
      __publicField(this, "windowMs");
      __publicField(this, "executed", false);
    }
    within(windowMs) {
      this.ensureConfigurable();
      validateTimeout(windowMs);
      this.windowMs = windowMs;
      return this;
    }
    async run(signal) {
      this.markExecuted();
      if (this.windowMs === void 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "expectNone requires within(windowMs)."
        );
      }
      const message = await this.connector.waitForMessage(
        this.name,
        this.windowMs,
        () => true,
        signal
      );
      if (message === void 0) {
        return;
      }
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        `Expected no '${this.name}' message within ${this.windowMs}ms.`
      );
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Builder instances can be executed only once."
        );
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
      __publicField(this, "predicates", []);
      __publicField(this, "timeoutMs");
      __publicField(this, "executed", false);
    }
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
    // Spec stream-connector 32 §10.1: the predicate reads the whole message, and
    // the call answers with the messages themselves, so a caller can assert on the
    // packet name and the metadata and not only on the payload.
    async run(signal) {
      var _a;
      this.markExecuted();
      if (this.predicates.length === 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "waitForSequence requires at least one expectation."
        );
      }
      const timeoutMs = (_a = this.timeoutMs) != null ? _a : this.connector.options.waitTimeoutMs;
      const deadline = Date.now() + timeoutMs;
      const messages = [];
      for (const predicate of this.predicates) {
        const message = await this.connector.waitForMessage(
          this.name,
          Math.max(0, deadline - Date.now()),
          (candidate) => {
            if (!predicate(candidate)) {
              throw connectorError(
                "validationFailed" /* ValidationFailed */,
                `Message '${this.name}' arrived out of the expected sequence.`
              );
            }
            return true;
          },
          signal
        );
        if (message === void 0) {
          throw connectorError(
            "validationFailed" /* ValidationFailed */,
            `The '${this.name}' sequence did not complete within ${timeoutMs}ms.`
          );
        }
        messages.push(message);
      }
      return messages;
    }
    ensureConfigurable() {
      if (this.executed) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Builder instances can be executed only once."
        );
      }
    }
    markExecuted() {
      this.ensureConfigurable();
      this.executed = true;
    }
  };
  function validateTimeout(timeoutMs) {
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Timeout must be a non-negative finite number."
      );
    }
  }

  // packages/stream-connector/src/Runtime/Protocol/Compression/ZlinkStreamCompressionCodec.ts
  var zlinkStreamLz4CompressionCodec = {
    compress(payload) {
      return lz4PickleUncompressed2(payload);
    },
    decompress(payload, maxDecompressedSize) {
      if (lz4PickledLength2(payload) > maxDecompressedSize) {
        throw connectorError(
          "frameTooLarge" /* FrameTooLarge */,
          "LZ4 decoded payload exceeds MaxReceivePayloadSize."
        );
      }
      return lz4UnpicklePayload2(payload, maxDecompressedSize);
    }
  };
  function compressPayload(payload, compression, compressionCodec) {
    const codec = resolveCompressionCodec(compression, compressionCodec);
    if (codec === void 0) {
      throw connectorError(
        "compressionFailed" /* CompressionFailed */,
        "Compression codec is not configured."
      );
    }
    try {
      return codec.compress(payload);
    } catch (cause) {
      throw connectorError("compressionFailed" /* CompressionFailed */, "Compression failed.", cause);
    }
  }
  function decompressIfNeeded(header, payload, compression, compressionCodec, maxDecompressedSize) {
    if ((header.flags & 4 /* PayloadCompressed */) === 0) {
      return payload;
    }
    const codec = resolveCompressionCodec(compression, compressionCodec);
    if (codec === void 0) {
      throw connectorError(
        "decompressionFailed" /* DecompressionFailed */,
        "Compression codec is not configured."
      );
    }
    let decompressed;
    try {
      decompressed = codec.decompress(payload, maxDecompressedSize);
    } catch (cause) {
      if (cause instanceof ZlinkStreamException) throw cause;
      throw connectorError("decompressionFailed" /* DecompressionFailed */, "Decompression failed.", cause);
    }
    if (decompressed.length > maxDecompressedSize) {
      throw connectorError(
        "frameTooLarge" /* FrameTooLarge */,
        "Decompressed payload exceeds MaxReceivePayloadSize."
      );
    }
    return decompressed;
  }
  function resolveCompressionCodec(compression, compressionCodec) {
    if (compression === "none" /* None */) {
      return void 0;
    }
    return compressionCodec != null ? compressionCodec : zlinkStreamLz4CompressionCodec;
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
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Frame length does not match prefix.",
          cause
        );
      }
    }
  };
  function splitZlinkStreamFrames(chunk) {
    if (chunk.length === 0) {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Stream frame prefix is incomplete."
      );
    }
    const frames = [];
    let offset = 0;
    while (offset < chunk.length) {
      const remaining = chunk.length - offset;
      if (remaining < 6) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Stream frame prefix is incomplete."
        );
      }
      const headerLength = chunk[offset] << 8 | chunk[offset + 1];
      const payloadLength = chunk[offset + 2] * 16777216 + (chunk[offset + 3] << 16) + (chunk[offset + 4] << 8) + chunk[offset + 5];
      const frameLength = 6 + headerLength + payloadLength;
      if (frameLength > remaining) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Frame length does not match prefix."
        );
      }
      frames.push(chunk.subarray(offset, offset + frameLength));
      offset += frameLength;
    }
    return frames;
  }
  function validatePayload(payloadLength, maxPayloadSize) {
    if (payloadLength > maxPayloadSize) {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Payload exceeds MaxSendPayloadSize."
      );
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
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          streamWireErrorMessage(cause),
          cause
        );
      }
    }
    static write(metadata, destination) {
      try {
        destination.set(encodeStreamWireMetadata(metadata.values));
      } catch (cause) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          streamWireErrorMessage(cause),
          cause
        );
      }
    }
    static decode(metadata) {
      try {
        const values = decodeStreamWireMetadata(metadata);
        return values.size === 0 ? ZlinkStreamMetadataMap.empty : ZlinkStreamMetadataMap.from(values);
      } catch (cause) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          streamWireErrorMessage(cause),
          cause
        );
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
          actorSlot: header.actorSlot
        });
      } catch (cause) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          streamWireErrorMessage2(cause),
          cause
        );
      }
    }
    static decode(header) {
      let decoded;
      try {
        const wire = decodeStreamWireHeader(header, void 0, false);
        const metadata = wire.metadata.size === 0 ? ZlinkStreamMetadataMap.empty : ZlinkStreamMetadataMap.from(wire.metadata);
        decoded = {
          kind: wire.kind,
          codec: wire.codec,
          flags: wire.flags,
          requestSeq: wire.requestSeq,
          name: wire.name,
          metadata,
          correlationId: wire.correlationId,
          actorSlot: wire.actorSlot
        };
      } catch (cause) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          streamWireErrorMessage2(cause),
          cause
        );
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
  function buildHeader(kind, name, codec, metadata, compress, requestSeq, correlationId, actorSlot) {
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
    if (actorSlot !== void 0) flags |= 32 /* HasActorSlot */;
    return {
      kind,
      codec,
      flags,
      requestSeq,
      name,
      metadata,
      correlationId,
      actorSlot
    };
  }
  function validateHeaderSemantics(header) {
    validateEnum(header.kind, header.codec, header.flags);
    const hasRequestSeq = header.requestSeq !== void 0 || (header.flags & 1 /* HasRequestSeq */) !== 0;
    const hasMetadata = header.metadata.count > 0 || (header.flags & 2 /* HasMetadata */) !== 0;
    if (header.kind === 1 /* Send */ && hasRequestSeq) {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Send packet must not contain a request sequence."
      );
    }
    if ((header.kind === 2 /* Request */ || header.kind === 3 /* Response */) && !hasRequestSeq) {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Request and response packets must contain a request sequence."
      );
    }
    if (header.kind === 4 /* Error */ && header.codec !== 1 /* Json */) {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Error packet must use the JSON codec."
      );
    }
    if (header.kind === 5 /* Control */) {
      const hasCorrelation = header.correlationId !== void 0 && header.correlationId.length > 0 || (header.flags & 8 /* HasCorrelationId */) !== 0;
      const hasFlow = (header.flags & 16 /* HasFlowId */) !== 0;
      const hasActorSlot = header.actorSlot !== void 0 || (header.flags & 32 /* HasActorSlot */) !== 0;
      if (header.flags !== 0 /* None */ || hasRequestSeq || hasMetadata || hasCorrelation || hasFlow || hasActorSlot || header.codec !== 0 /* Raw */) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Control packet must use raw codec and must not contain flags."
        );
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
    const known = 1 /* HasRequestSeq */ | 2 /* HasMetadata */ | 4 /* PayloadCompressed */ | 8 /* HasCorrelationId */ | 16 /* HasFlowId */ | 32 /* HasActorSlot */;
    if ((flags & ~known) !== 0) {
      throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown stream header flag.");
    }
  }

  // packages/stream-connector/src/Runtime/Protocol/ZlinkStreamFrameProtocol.ts
  var ZLINK_STREAM_HEARTBEAT_PING = "$zlink.heartbeat.ping";
  var ZLINK_STREAM_HEARTBEAT_PONG = "$zlink.heartbeat.pong";
  var ZlinkStreamFrameProtocol = class {
    constructor(options) {
      this.options = options;
    }
    encode(kind, name, payload, metadata, compress, requestSeq, correlationId, actorSlot) {
      const payloadBytes = compress ? compressPayload(payload.payload, this.options.compression, this.options.compressionCodec) : payload.payload;
      const header = buildHeader(
        kind,
        name,
        payload.codec,
        metadata,
        compress,
        requestSeq,
        correlationId,
        actorSlot
      );
      return this.encodeFrame(header, payloadBytes);
    }
    encodeControl(name, payload = new Uint8Array()) {
      return this.encodeFrame(
        {
          kind: 5 /* Control */,
          codec: 0 /* Raw */,
          flags: 0 /* None */,
          name,
          metadata: ZlinkStreamMetadataMap.empty
        },
        payload
      );
    }
    decode(frameBytes) {
      const frame = ZlinkStreamFrameCodec.decode(frameBytes);
      return {
        header: ZlinkStreamHeaderCodec.decode(frame.header),
        payload: frame.payload
      };
    }
    decodeFrames(chunk) {
      return splitZlinkStreamFrames(chunk).map((frame) => {
        const decoded = this.decode(frame);
        if (decoded.payload.length > this.options.maxReceivePayloadSize) {
          throw connectorError(
            "frameTooLarge" /* FrameTooLarge */,
            "Payload exceeds MaxReceivePayloadSize."
          );
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
  function normalizeOptions(options, defaultTransportFactory) {
    var _a, _b, _c, _d, _e, _f, _g, _h, _i, _j, _k, _l, _m, _n, _o, _p, _q, _r, _s, _t, _u, _v, _w, _x, _y, _z, _A, _B, _C, _D, _E, _F, _G;
    const endpoint = options.endpoint;
    if (endpoint.trim().length === 0) {
      throw connectorError("configurationError" /* ConfigurationError */, "Endpoint must not be empty.");
    }
    const inferredTransport = inferTransport(endpoint);
    if (options.transport !== void 0 && options.transport !== inferredTransport) {
      throw connectorError(
        "configurationError" /* ConfigurationError */,
        "Configured transport conflicts with endpoint scheme."
      );
    }
    validatePositive((_a = options.connectTimeoutMs) != null ? _a : 5e3, "ConnectTimeout");
    validatePositive((_b = options.requestTimeoutMs) != null ? _b : 3e4, "RequestTimeout");
    validatePositive((_c = options.waitTimeoutMs) != null ? _c : 5e3, "WaitTimeout");
    validatePositive((_d = options.maxSendPayloadSize) != null ? _d : 64 * 1024, "MaxSendPayloadSize");
    validatePositive((_e = options.maxReceivePayloadSize) != null ? _e : 64 * 1024, "MaxReceivePayloadSize");
    validateHeartbeat(options.heartbeat);
    validateReconnect(options.reconnect);
    if (((_f = options.heartbeat) == null ? void 0 : _f.enabled) !== void 0 && typeof options.heartbeat.enabled !== "boolean") {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Heartbeat enabled must be boolean."
      );
    }
    if (((_g = options.reconnect) == null ? void 0 : _g.enabled) !== void 0 && typeof options.reconnect.enabled !== "boolean") {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Reconnect enabled must be boolean."
      );
    }
    if (options.compressionCodec !== void 0 && !hasMethods(options.compressionCodec, "compress", "decompress")) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Compression codec is invalid.");
    }
    if (options.codec !== void 0 && !hasMethods(options.codec, "encode", "decode")) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Payload codec is invalid.");
    }
    if (options.nameResolver !== void 0 && !hasMethods(options.nameResolver, "resolve")) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Packet name resolver is invalid.");
    }
    if (options.transportFactory !== void 0 && !hasMethods(options.transportFactory, "connect")) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Transport factory is invalid.");
    }
    if (!Object.values(ZlinkStreamDispatchMode).includes(
      (_h = options.dispatchMode) != null ? _h : "manual" /* Manual */
    )) {
      throw connectorError("validationFailed" /* ValidationFailed */, "DispatchMode is invalid.");
    }
    if (!Object.values(ZlinkStreamCompression).includes(
      (_i = options.compression) != null ? _i : "lz4" /* Lz4 */
    )) {
      throw connectorError("validationFailed" /* ValidationFailed */, "Compression is invalid.");
    }
    return {
      endpoint,
      transport: inferredTransport,
      connectTimeoutMs: (_j = options.connectTimeoutMs) != null ? _j : 5e3,
      requestTimeoutMs: (_k = options.requestTimeoutMs) != null ? _k : 3e4,
      waitTimeoutMs: (_l = options.waitTimeoutMs) != null ? _l : 5e3,
      heartbeat: {
        enabled: (_n = (_m = options.heartbeat) == null ? void 0 : _m.enabled) != null ? _n : true,
        intervalMs: (_p = (_o = options.heartbeat) == null ? void 0 : _o.intervalMs) != null ? _p : 1e3,
        timeoutMs: (_r = (_q = options.heartbeat) == null ? void 0 : _q.timeoutMs) != null ? _r : 5e3
      },
      reconnect: {
        enabled: (_t = (_s = options.reconnect) == null ? void 0 : _s.enabled) != null ? _t : true,
        initialDelayMs: (_v = (_u = options.reconnect) == null ? void 0 : _u.initialDelayMs) != null ? _v : 250,
        maxDelayMs: (_x = (_w = options.reconnect) == null ? void 0 : _w.maxDelayMs) != null ? _x : 5e3,
        backoffFactor: (_z = (_y = options.reconnect) == null ? void 0 : _y.backoffFactor) != null ? _z : 2,
        maxAttempts: ((_A = options.reconnect) == null ? void 0 : _A.maxAttempts) === void 0 ? 3 : options.reconnect.maxAttempts
      },
      maxSendPayloadSize: (_B = options.maxSendPayloadSize) != null ? _B : 64 * 1024,
      maxReceivePayloadSize: (_C = options.maxReceivePayloadSize) != null ? _C : 64 * 1024,
      dispatchMode: (_D = options.dispatchMode) != null ? _D : "manual" /* Manual */,
      compression: (_E = options.compression) != null ? _E : "lz4" /* Lz4 */,
      compressionCodec: resolveCompressionCodec2(options),
      nameResolver: (_F = options.nameResolver) != null ? _F : defaultPacketNameResolver,
      transportFactory: (_G = options.transportFactory) != null ? _G : defaultTransportFactory,
      codec: options.codec
    };
  }
  var defaultPacketNameResolver = {
    resolve(payloadType) {
      const declared = payloadType.packetName;
      if (typeof declared === "string" && declared.length > 0) {
        return declared;
      }
      return payloadType.name;
    }
  };
  function resolveCompressionCodec2(options) {
    var _a;
    const compression = (_a = options.compression) != null ? _a : "lz4" /* Lz4 */;
    if (compression === "none" /* None */) {
      if (options.compressionCodec !== void 0) {
        throw connectorError(
          "configurationError" /* ConfigurationError */,
          "compressionCodec cannot be set when compression is none."
        );
      }
      return void 0;
    }
    return options.compressionCodec;
  }
  function validatePositive(value, name) {
    if (!Number.isFinite(value) || value <= 0) {
      throw connectorError("validationFailed" /* ValidationFailed */, `${name} must be positive.`);
    }
  }
  function hasMethods(value, ...names) {
    if (value === null || typeof value !== "object") return false;
    const candidate = value;
    return names.every((name) => typeof candidate[name] === "function");
  }
  function validateHeartbeat(options) {
    var _a, _b;
    const intervalMs = (_a = options == null ? void 0 : options.intervalMs) != null ? _a : 1e3;
    const timeoutMs = (_b = options == null ? void 0 : options.timeoutMs) != null ? _b : 5e3;
    validatePositive(intervalMs, "Heartbeat interval");
    validatePositive(timeoutMs, "Heartbeat timeout");
  }
  function validateReconnect(options) {
    var _a, _b, _c;
    validatePositive((_a = options == null ? void 0 : options.initialDelayMs) != null ? _a : 250, "Reconnect InitialDelay");
    validatePositive((_b = options == null ? void 0 : options.maxDelayMs) != null ? _b : 5e3, "Reconnect MaxDelay");
    validatePositive((_c = options == null ? void 0 : options.backoffFactor) != null ? _c : 2, "Reconnect BackoffFactor");
    const maxAttempts = (options == null ? void 0 : options.maxAttempts) === void 0 ? 3 : options.maxAttempts;
    if (maxAttempts !== null && !(Number.isFinite(maxAttempts) && maxAttempts > 0)) {
      throw connectorError(
        "validationFailed" /* ValidationFailed */,
        "Reconnect MaxAttempts must be null or positive."
      );
    }
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamPendingRequests.ts
  var ZlinkStreamPendingRequests = class {
    constructor() {
      __publicField(this, "nextRequestSeq", 1n);
      __publicField(this, "active", /* @__PURE__ */ new Map());
    }
    create(packetName, timeoutMs) {
      if (this.nextRequestSeq > 0xffffffffffffffffn) {
        throw connectorError("sendFailed" /* SendFailed */, "Request sequence is exhausted.");
      }
      const requestSeq = this.nextRequestSeq++;
      let timeout;
      let resolvePending;
      let rejectPending;
      const promise = new Promise(
        (resolve, reject) => {
          resolvePending = resolve;
          rejectPending = (error) => reject(connectorError(error.code, error.message, error.cause));
        }
      );
      const tracked = {
        packetName,
        promise,
        startTimeout: () => {
          if (timeout !== void 0 || this.active.get(requestSeq) !== tracked) return;
          timeout = setTimeout(() => {
            this.active.delete(requestSeq);
            rejectPending({
              code: "requestTimeout" /* RequestTimeout */,
              message: `Request '${packetName}' timed out.`
            });
          }, timeoutMs);
        },
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
      };
      this.active.set(requestSeq, tracked);
      return { requestSeq, promise, startTimeout: tracked.startTimeout };
    }
    /* stream connector spec §5.2: a pending request is matched by request_seq alone. */
    resolve(requestSeq, value, metadata) {
      const pending = this.active.get(requestSeq);
      if (pending === void 0) {
        return false;
      }
      const payload = value();
      this.active.delete(requestSeq);
      pending.resolve({ name: pending.packetName, metadata, payload });
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
    /**
     * @param deliverOnArrival `Immediate` runs registered handlers on the receive
     *   path; `Manual` leaves them queued until {@link pump} runs them on the
     *   caller's thread (spec stream-connector 32 §7). Wait surfaces observe the
     *   queue in both modes, so they never depend on this flag.
     */
    constructor(events, deliverOnArrival) {
      this.events = events;
      this.deliverOnArrival = deliverOnArrival;
      __publicField(this, "handlers", /* @__PURE__ */ new Map());
      __publicField(this, "observers", /* @__PURE__ */ new Map());
      // A handler can be registered after messages for another name arrive, so the
      // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
      // without shifting every later message on the hot receive path.
      __publicField(this, "queue", []);
      __publicField(this, "queueHead", 0);
      __publicField(this, "queuedCount", 0);
      __publicField(this, "drainTask");
      // True for as long as `drain` is on the stack, handler awaits included. It
      // marks the execution context a registered handler runs in, so a `dispatch`
      // made from inside a handler is recognised as re-entry rather than a fresh
      // pump. It is not a lock: a single event loop admits no second thread, and
      // nothing ever waits for this flag to fall.
      __publicField(this, "draining", false);
      // Spec stream-connector 32 §10: arrivals per packet name on the current
      // connection. It is raised where a packet arrives, never where one is taken,
      // so consuming does not lower it and the dispatch mode does not change it.
      __publicField(this, "receivedCounts", /* @__PURE__ */ new Map());
    }
    on(name, handler) {
      validateName(name);
      let set = this.handlers.get(name);
      if (set === void 0) {
        set = /* @__PURE__ */ new Set();
        this.handlers.set(name, set);
      }
      set.add(handler);
      if (this.deliverOnArrival && this.hasQueuedMessage(name)) {
        queueMicrotask(() => this.scheduleDrain());
      }
      return subscription(() => {
        set.delete(handler);
        if (set.size === 0 && this.handlers.get(name) === set) {
          this.handlers.delete(name);
        }
      });
    }
    /**
     * Registers a wait surface over the receive queue. Spec stream-connector 32
     * §7: these are not registered callbacks — they observe and consume the
     * packets the queue has not delivered yet, in both dispatch modes, so
     * `Manual` needs no dispatch pump to complete a wait. The queue is scanned in
     * a microtask so a message that arrived before the wait started is still
     * observed, and so the caller has its subscription in hand by then.
     *
     * @param onConnectionEnded Called, instead of {@link observer}, when
     *   {@link connectionEnded} abandons this registration because the
     *   connection it was watching ended before a message matched (spec
     *   stream-connector 32 §10.1.1: "연결이 끝나 대기를 이어갈 수 없으면
     *   `Disconnected`다", released when that connection ends).
     */
    observe(name, observer, onConnectionEnded) {
      validateName(name);
      let set = this.observers.get(name);
      if (set === void 0) {
        set = /* @__PURE__ */ new Set();
        this.observers.set(name, set);
      }
      const registration = { consume: observer, onConnectionEnded };
      set.add(registration);
      queueMicrotask(() => {
        var _a;
        if (((_a = this.observers.get(name)) == null ? void 0 : _a.has(registration)) === true) {
          this.offerQueued(name, registration);
        }
      });
      return subscription(() => {
        set.delete(registration);
        if (set.size === 0 && this.observers.get(name) === set) {
          this.observers.delete(name);
        }
      });
    }
    /** Spec stream-connector 32 §10: arrivals under `name` on this connection. */
    receivedCount(name) {
      var _a;
      return (_a = this.receivedCounts.get(name)) != null ? _a : 0;
    }
    /**
     * Rebaselines the queue for a connection that was just established. Spec
     * stream-connector 32 §10 (line ~649): the reference point is the moment a
     * connection is established, so counts restart at 0 and whatever the
     * previous connection left unconsumed goes with it — keeping the queue
     * while only the counts reset would let counts and queue describe two
     * different connections, and let `waitFor` hand back a packet from before
     * the drop as if the new connection had delivered it.
     *
     * `ZlinkStreamMessage`/`ZlinkStreamEncodedPayload` are plain data (name,
     * metadata, a `Uint8Array` payload) with no dispose/close of their own —
     * unlike Java's queued frames, which `closeMessage` releases — so dropping
     * the queue's references is the whole of the release here.
     *
     * Wait surfaces are not touched here. The ones of the previous connection
     * were released by {@link connectionEnded} when that connection ended, and
     * one registered since then is waiting for this connection.
     */
    resetForNewConnection() {
      this.receivedCounts.clear();
      const submittedCallbacks = this.queue.slice(this.queueHead).filter((item) => (item == null ? void 0 : item.kind) === "callback");
      this.queue.length = 0;
      this.queue.push(...submittedCallbacks);
      this.queueHead = 0;
      this.queuedCount = submittedCallbacks.length;
    }
    /**
     * Releases every registered wait surface because the connection it was
     * watching has ended — a transport loss, a server close, or `close()`.
     * Spec stream-connector 32 §10.1.1: "푸는 시점은 연결이 끝난 때이지 다음
     * 연결이 성립한 때가 아니다". The release belongs to the ending, so a wait
     * does not hang until its own timeout when no next connection comes
     * (reconnect off, attempts spent) and does not silently rebind to the next
     * one when it does. The queue and the counts stay: they are rebaselined by
     * the next {@link resetForNewConnection}, not by the ending (§10).
     */
    connectionEnded() {
      if (this.observers.size === 0) {
        return;
      }
      const abandoned = Array.from(this.observers.values()).flatMap((set) => Array.from(set));
      this.observers.clear();
      for (const registration of abandoned) {
        registration.onConnectionEnded();
      }
    }
    enqueue(message, signal) {
      var _a, _b;
      this.receivedCounts.set(message.name, ((_a = this.receivedCounts.get(message.name)) != null ? _a : 0) + 1);
      for (const registration of Array.from((_b = this.observers.get(message.name)) != null ? _b : [])) {
        if (registration.consume(message)) {
          return;
        }
      }
      this.queue.push({ kind: "message", message, signal });
      this.queuedCount += 1;
      if (this.deliverOnArrival) {
        this.scheduleDrain();
      }
    }
    enqueueCallback(callback) {
      this.queue.push({ kind: "callback", callback });
      this.queuedCount += 1;
      if (this.deliverOnArrival) {
        this.scheduleDrain();
      }
    }
    /**
     * Runs the registered handlers the receive path left queued. `Manual` calls
     * this from `dispatch`; `Immediate` has already drained on arrival.
     *
     * A handler that calls `dispatch` arrives back here from inside the drain it
     * was started by. `scheduleDrain` would find `drainTask` already set and
     * return, and the await below would then be the drain waiting on itself —
     * a deadlock with neither timeout nor error. The drain loop already takes
     * every message a handler exists for, so there is nothing a second drain
     * would deliver and returning is the whole of the correct behaviour.
     */
    async pump() {
      if (this.draining) {
        return;
      }
      this.scheduleDrain();
      await this.drainTask;
    }
    offerQueued(name, registration) {
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued === void 0 || queued.kind !== "message" || queued.message.name !== name) {
          continue;
        }
        if (!registration.consume(queued.message)) {
          continue;
        }
        this.removeAt(index);
        return;
      }
    }
    scheduleDrain() {
      if (this.drainTask !== void 0) {
        return;
      }
      this.drainTask = this.drain().finally(() => {
        this.drainTask = void 0;
        if (this.deliverOnArrival && this.findDeliverableIndex() >= 0) {
          this.scheduleDrain();
        }
      });
    }
    async drain() {
      this.draining = true;
      try {
        for (let index = this.findDeliverableIndex(); index >= 0; index = this.findDeliverableIndex()) {
          const queued = this.queue[index];
          if (queued === void 0) continue;
          this.removeAt(index);
          if (queued.kind === "callback") {
            await queued.callback();
            continue;
          }
          const message = queued.message;
          const signal = queued.signal;
          const handlers = Array.from(this.handlers.get(message.name));
          for (const handler of handlers) {
            try {
              await handler(message, signal);
            } catch (cause) {
              await this.events.publishError(
                {
                  code: "userCallbackFailed" /* UserCallbackFailed */,
                  message: "Typed message handler failed.",
                  cause
                },
                signal
              );
            }
          }
        }
      } finally {
        this.draining = false;
      }
    }
    removeAt(index) {
      this.queue[index] = void 0;
      this.queuedCount -= 1;
      this.advanceHead();
      this.compactQueue();
    }
    findDeliverableIndex() {
      var _a, _b;
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued !== void 0 && (queued.kind === "callback" || ((_b = (_a = this.handlers.get(queued.message.name)) == null ? void 0 : _a.size) != null ? _b : 0) > 0)) {
          return index;
        }
      }
      return -1;
    }
    hasQueuedMessage(name) {
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if ((queued == null ? void 0 : queued.kind) === "message" && queued.message.name === name) return true;
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
    constructor(protocol) {
      this.protocol = protocol;
      __publicField(this, "pendingWrites", /* @__PURE__ */ new Set());
    }
    async send(connection, kind, name, payload, metadata, compress, requestSeq, signal, correlationId, actorSlot) {
      throwIfAborted(signal);
      await this.write(
        connection,
        this.protocol.encode(
          kind,
          name,
          payload,
          metadata,
          compress,
          requestSeq,
          correlationId,
          actorSlot
        ),
        signal
      );
    }
    async sendControl(connection, name, signal) {
      await this.write(connection, this.protocol.encodeControl(name), signal);
    }
    async drain(signal) {
      while (this.pendingWrites.size > 0) {
        throwIfAborted(signal);
        await Promise.allSettled(Array.from(this.pendingWrites));
      }
    }
    async write(connection, frame, signal) {
      const write = connection.write(frame, signal);
      this.pendingWrites.add(write);
      try {
        await write;
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
    if (payload.length < 4 || payload[0] !== 1)
      throw new Error("Unsupported session-closing version.");
    const closeReason = reasons[payload[1]];
    if (closeReason === void 0) throw new Error("Unknown session-closing reason.");
    const length = payload[2] << 8 | payload[3];
    if (length > 512 || payload.length !== 4 + length)
      throw new Error("Invalid session-closing diagnostic length.");
    const diagnostic = length === 0 ? void 0 : new TextDecoder("utf-8", { fatal: true }).decode(payload.subarray(4));
    return { closeReason, diagnostic };
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamActors.ts
  var ACTOR_BOUND = "$zlink.actor.bound";
  var ACTOR_UNBOUND = "$zlink.actor.unbound";
  var zlinkStreamActorBinding = Symbol("zlink.stream.actorBinding");
  var DefaultZlinkStreamActor = class {
    constructor(connector, actorId, slot) {
      this.connector = connector;
      this.actorId = actorId;
      this.slot = slot;
      __publicField(this, "bound", true);
    }
    get isBound() {
      return this.bound;
    }
    send(payload, messageType) {
      this.ensureBound();
      return this.connector.sendForActor(this, payload, messageType);
    }
    request(payload, messageType) {
      this.ensureBound();
      return this.connector.requestForActor(this, payload, messageType);
    }
    on(nameOrType, handler, messageType) {
      return this.connector.onActorMessage(this, nameOrType, handler, messageType);
    }
    ensureBound() {
      if (!this.bound) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          `Actor '${this.actorId}' is no longer bound.`
        );
      }
    }
    close() {
      this.bound = false;
    }
  };
  var ZlinkStreamActors = class {
    constructor(connector, receivedMessages, events) {
      this.connector = connector;
      this.receivedMessages = receivedMessages;
      this.events = events;
      __publicField(this, "bySlot", /* @__PURE__ */ new Map());
      __publicField(this, "byId", /* @__PURE__ */ new Map());
      __publicField(this, "issued", []);
      __publicField(this, "boundHandlers", /* @__PURE__ */ new Set());
      __publicField(this, "unboundHandlers", /* @__PURE__ */ new Set());
    }
    get snapshot() {
      return Object.freeze(Array.from(this.bySlot.values()));
    }
    find(actorId) {
      return this.byId.get(actorId);
    }
    onBound(handler) {
      this.boundHandlers.add(handler);
      return subscription(() => this.boundHandlers.delete(handler));
    }
    onUnbound(handler) {
      this.unboundHandlers.add(handler);
      return subscription(() => this.unboundHandlers.delete(handler));
    }
    processControl(name, payload, signal) {
      if (name === ACTOR_BOUND) {
        this.bind(payload, signal);
        return true;
      }
      if (name === ACTOR_UNBOUND) {
        this.unbind(payload, signal);
        return true;
      }
      return false;
    }
    resolve(slot) {
      const actor = this.bySlot.get(slot);
      if (actor === void 0) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          `Actor slot '${slot}' is not bound.`
        );
      }
      return actor;
    }
    closeAll(signal) {
      for (const actor of this.issued) {
        if (!actor.isBound) continue;
        this.bySlot.delete(actor.slot);
        this.byId.delete(actor.actorId);
        actor.close();
        this.queue(this.unboundHandlers, actor, signal);
      }
      this.issued.length = 0;
    }
    bind(payload, signal) {
      if (payload.length < 5 || payload[0] !== 1) {
        throw invalidControl("Actor bound payload is invalid.");
      }
      const slot = payload[1] << 8 | payload[2];
      const idLength = payload[3];
      if (slot === 0 || idLength === 0 || payload.length !== 4 + idLength) {
        throw invalidControl("Actor bound payload is invalid.");
      }
      let actorId;
      try {
        actorId = new TextDecoder("utf-8", { fatal: true }).decode(payload.subarray(4));
      } catch (cause) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Actor id is not valid UTF-8.",
          cause
        );
      }
      if (actorId.length === 0 || this.bySlot.has(slot) || this.byId.has(actorId)) {
        throw invalidControl("Actor bound identity is already in use.");
      }
      const actor = new DefaultZlinkStreamActor(this.connector, actorId, slot);
      this.bySlot.set(slot, actor);
      this.byId.set(actorId, actor);
      this.issued.push(actor);
      this.queue(this.boundHandlers, actor, signal);
    }
    unbind(payload, signal) {
      if (payload.length !== 3 || payload[0] !== 1) {
        throw invalidControl("Actor unbound payload is invalid.");
      }
      const slot = payload[1] << 8 | payload[2];
      const actor = this.bySlot.get(slot);
      if (slot === 0 || actor === void 0) {
        throw invalidControl(`Actor slot '${slot}' is not bound.`);
      }
      this.bySlot.delete(slot);
      this.byId.delete(actor.actorId);
      actor.close();
      this.queue(this.unboundHandlers, actor, signal);
    }
    queue(handlers, actor, signal) {
      this.receivedMessages.enqueueCallback(
        () => {
          for (const handler of Array.from(handlers)) {
            this.events.runUserCallback(
              () => handler(actor, signal),
              "Actor lifecycle handler failed.",
              signal
            );
          }
        },
        () => handlers.size
      );
    }
  };
  function invalidControl(message) {
    return connectorError("frameDecodeFailed" /* FrameDecodeFailed */, message);
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamReceivedMessages.ts
  function receives(registration, message) {
    return registration.actor === void 0 || registration.actor === message[zlinkStreamActorBinding];
  }
  var ZlinkStreamReceivedMessages = class {
    /**
     * @param deliverOnArrival `Immediate` runs registered handlers on the receive
     *   path; `Manual` leaves them queued until {@link pump} runs them on the
     *   caller's thread (spec stream-connector 32 §7). Wait surfaces observe the
     *   queue in both modes, so they never depend on this flag.
     */
    constructor(events, deliverOnArrival) {
      this.events = events;
      this.deliverOnArrival = deliverOnArrival;
      __publicField(this, "handlers", /* @__PURE__ */ new Map());
      __publicField(this, "observers", /* @__PURE__ */ new Map());
      // A handler can be registered after messages for another name arrive, so the
      // queue is not a simple FIFO. Tombstones let us remove a deliverable entry
      // without shifting every later message on the hot receive path.
      __publicField(this, "queue", []);
      __publicField(this, "deliverable", []);
      __publicField(this, "queueHead", 0);
      __publicField(this, "queuedCount", 0);
      // True for as long as `pump` is on the stack. It
      // marks the execution context a registered handler runs in, so a `dispatch`
      // made from inside a handler is recognised as re-entry rather than a fresh
      // pump. It is not a lock: a single event loop admits no second thread, and
      // nothing ever waits for this flag to fall.
      __publicField(this, "draining", false);
      // Spec stream-connector 32 §10: arrivals per packet name on the current
      // connection. It is raised where a packet arrives, never where one is taken,
      // so consuming does not lower it and the dispatch mode does not change it.
      __publicField(this, "receivedCounts", /* @__PURE__ */ new Map());
    }
    /**
     * @param actor The Actor handle that registers the handler, if any. Its
     *   handler receives only that Actor's packets.
     */
    on(name, handler, actor) {
      validateName(name);
      let set = this.handlers.get(name);
      if (set === void 0) {
        set = /* @__PURE__ */ new Set();
        this.handlers.set(name, set);
      }
      const registration = { handle: handler, actor };
      set.add(registration);
      let newlyDeliverable = false;
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if ((queued == null ? void 0 : queued.kind) === "message" && queued.indexed !== true && queued.message.name === name && receives(registration, queued.message)) {
          this.indexDeliverable(index);
          newlyDeliverable = true;
        }
      }
      if (this.deliverOnArrival && newlyDeliverable) {
        queueMicrotask(() => this.pump());
      }
      return subscription(() => {
        set.delete(registration);
        if (set.size === 0 && this.handlers.get(name) === set) {
          this.handlers.delete(name);
        }
      });
    }
    /**
     * Registers a wait surface over the receive queue. Spec stream-connector 32
     * §7: these are not registered callbacks — they observe and consume the
     * packets the queue has not delivered yet, in both dispatch modes, so
     * `Manual` needs no dispatch pump to complete a wait. The queue is scanned in
     * a microtask so a message that arrived before the wait started is still
     * observed, and so the caller has its subscription in hand by then.
     *
     * @param onConnectionEnded Called, instead of {@link observer}, when
     *   {@link connectionEnded} abandons this registration because the
     *   connection it was watching ended before a message matched (spec
     *   stream-connector 32 §10.1.1: "연결이 끝나 대기를 이어갈 수 없으면
     *   `Disconnected`다", released when that connection ends).
     */
    observe(name, observer, onConnectionEnded) {
      validateName(name);
      let set = this.observers.get(name);
      if (set === void 0) {
        set = /* @__PURE__ */ new Set();
        this.observers.set(name, set);
      }
      const registration = { consume: observer, onConnectionEnded };
      set.add(registration);
      queueMicrotask(() => {
        var _a;
        if (((_a = this.observers.get(name)) == null ? void 0 : _a.has(registration)) === true) {
          this.offerQueued(name, registration);
        }
      });
      return subscription(() => {
        set.delete(registration);
        if (set.size === 0 && this.observers.get(name) === set) {
          this.observers.delete(name);
        }
      });
    }
    /** Spec stream-connector 32 §10: arrivals under `name` on this connection. */
    receivedCount(name) {
      var _a;
      return (_a = this.receivedCounts.get(name)) != null ? _a : 0;
    }
    /**
     * Rebaselines the queue for a connection that was just established. Spec
     * stream-connector 32 §10 (line ~649): the reference point is the moment a
     * connection is established, so counts restart at 0 and whatever the
     * previous connection left unconsumed goes with it — keeping the queue
     * while only the counts reset would let counts and queue describe two
     * different connections, and let `waitFor` hand back a packet from before
     * the drop as if the new connection had delivered it.
     *
     * `ZlinkStreamMessage`/`ZlinkStreamEncodedPayload` are plain data (name,
     * metadata, a `Uint8Array` payload) with no dispose/close of their own —
     * unlike Java's queued frames, which `closeMessage` releases — so dropping
     * the queue's references is the whole of the release here.
     *
     * Wait surfaces are not touched here. The ones of the previous connection
     * were released by {@link connectionEnded} when that connection ended, and
     * one registered since then is waiting for this connection.
     */
    resetForNewConnection() {
      this.receivedCounts.clear();
      const submittedCallbacks = this.queue.slice(this.queueHead).filter((item) => (item == null ? void 0 : item.kind) === "callback");
      this.queue.length = 0;
      this.queue.push(...submittedCallbacks);
      this.queueHead = 0;
      this.queuedCount = submittedCallbacks.length;
      this.deliverable.length = 0;
      for (let index = 0; index < this.queue.length; index += 1) {
        this.queue[index].indexed = false;
        this.indexDeliverable(index);
      }
    }
    /**
     * Releases every registered wait surface because the connection it was
     * watching has ended — a transport loss, a server close, or `close()`.
     * Spec stream-connector 32 §10.1.1: "푸는 시점은 연결이 끝난 때이지 다음
     * 연결이 성립한 때가 아니다". The release belongs to the ending, so a wait
     * does not hang until its own timeout when no next connection comes
     * (reconnect off, attempts spent) and does not silently rebind to the next
     * one when it does. The queue and the counts stay: they are rebaselined by
     * the next {@link resetForNewConnection}, not by the ending (§10).
     */
    connectionEnded() {
      if (this.observers.size === 0) {
        return;
      }
      const abandoned = Array.from(this.observers.values()).flatMap((set) => Array.from(set));
      this.observers.clear();
      for (const registration of abandoned) {
        registration.onConnectionEnded();
      }
    }
    enqueue(message, signal) {
      var _a, _b;
      this.receivedCounts.set(message.name, ((_a = this.receivedCounts.get(message.name)) != null ? _a : 0) + 1);
      for (const registration of Array.from((_b = this.observers.get(message.name)) != null ? _b : [])) {
        if (registration.consume(message)) {
          return;
        }
      }
      this.queue.push({ kind: "message", message, signal });
      this.queuedCount += 1;
      this.indexDeliverable(this.queue.length - 1);
      if (this.deliverOnArrival) {
        this.pump();
      }
    }
    /**
     * @param callbacks The number of handler calls `callback` makes when it runs
     *   with the handlers registered at that moment; a callback that runs a set
     *   of handlers reads the set's size. One callback by default.
     */
    enqueueCallback(callback, callbacks = () => 1) {
      this.queue.push({ kind: "callback", callback, callbacks });
      this.queuedCount += 1;
      this.indexDeliverable(this.queue.length - 1);
      if (this.deliverOnArrival) {
        this.pump();
      }
    }
    /**
     * Runs the registered handlers the receive path left queued. `Manual` calls
     * this from `dispatch`; `Immediate` drains on arrival.
     *
     * The drain is synchronous: the connector does not wait for a handler
     * (spec stream-connector 32 §7), so nothing inside it awaits. A handler that
     * calls `dispatch` arrives back here from inside the drain it was started by;
     * the running loop already takes every deliverable entry, so returning is the
     * whole of the correct behaviour.
     */
    pump() {
      if (this.draining) {
        return;
      }
      this.draining = true;
      try {
        for (let index = this.findDeliverableIndex(); index >= 0; index = this.findDeliverableIndex()) {
          const queued = this.queue[index];
          if (queued === void 0) continue;
          this.removeAt(index);
          if (queued.kind === "callback") {
            this.events.runUserCallback(queued.callback, "Connector callback failed.");
            continue;
          }
          const { message, signal } = queued;
          const handlers = this.receiversOf(message);
          for (const handler of handlers) {
            this.events.runUserCallback(
              () => handler.handle(message, signal),
              "Typed message handler failed.",
              signal
            );
          }
        }
      } finally {
        this.draining = false;
      }
    }
    /**
     * True while a registered callback runs from {@link pump}: the execution
     * context spec stream-connector 32 §7 calls "inside a handler".
     */
    get dispatching() {
      return this.draining;
    }
    /**
     * Spec stream-connector 32 §7: the callbacks the next dispatch pump runs with
     * the handlers registered now, in either dispatch mode. A packet no
     * registered handler receives is not counted.
     */
    get pendingCallbacks() {
      let count = 0;
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued === void 0) continue;
        count += queued.kind === "callback" ? queued.callbacks() : this.receiversOf(queued.message).length;
      }
      return count;
    }
    offerQueued(name, registration) {
      for (let index = this.queueHead; index < this.queue.length; index += 1) {
        const queued = this.queue[index];
        if (queued === void 0 || queued.kind !== "message" || queued.message.name !== name) {
          continue;
        }
        if (!registration.consume(queued.message)) {
          continue;
        }
        this.removeAt(index);
        return;
      }
    }
    removeAt(index) {
      this.queue[index] = void 0;
      this.queuedCount -= 1;
      this.advanceHead();
      this.compactQueue();
    }
    findDeliverableIndex() {
      while (this.deliverable.length > 0) {
        const index = this.deliverable[0];
        const queued = this.queue[index];
        if (this.isDeliverable(queued)) {
          return index;
        }
        if (queued !== void 0) queued.indexed = false;
        this.removeDeliverable();
      }
      return -1;
    }
    indexDeliverable(index) {
      const queued = this.queue[index];
      if (queued === void 0 || !this.isDeliverable(queued) || queued.indexed === true) return;
      queued.indexed = true;
      let position = this.deliverable.length;
      this.deliverable.push(index);
      while (position > 0) {
        const parent = Math.floor((position - 1) / 2);
        if (this.deliverable[parent] <= index) break;
        this.deliverable[position] = this.deliverable[parent];
        position = parent;
      }
      this.deliverable[position] = index;
    }
    isDeliverable(queued) {
      return (queued == null ? void 0 : queued.kind) === "callback" || (queued == null ? void 0 : queued.kind) === "message" && this.receiversOf(queued.message).length > 0;
    }
    /**
     * Spec stream-connector 32 §7 and §10: the handlers that receive `message`
     * are the connector handlers for its name and the handle handlers of the
     * Actor it carries. A packet no handler receives stays in the queue for the
     * wait surfaces. This is the one place that decides it.
     */
    receiversOf(message) {
      const set = this.handlers.get(message.name);
      if (set === void 0) return [];
      return Array.from(set).filter((registration) => receives(registration, message));
    }
    removeDeliverable() {
      if (this.deliverable.length === 0) return;
      const last = this.deliverable.pop();
      if (this.deliverable.length === 0) return;
      let position = 0;
      for (; ; ) {
        const left = position * 2 + 1;
        if (left >= this.deliverable.length) break;
        const right = left + 1;
        const child = right < this.deliverable.length && this.deliverable[right] < this.deliverable[left] ? right : left;
        if (this.deliverable[child] >= last) break;
        this.deliverable[position] = this.deliverable[child];
        position = child;
      }
      this.deliverable[position] = last;
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
        this.deliverable.length = 0;
        return;
      }
      if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
        this.queue.splice(0, this.queueHead);
        this.queueHead = 0;
        this.deliverable.length = 0;
        for (let index = 0; index < this.queue.length; index += 1) {
          const queued = this.queue[index];
          if (queued === void 0) continue;
          queued.indexed = false;
          this.indexDeliverable(index);
        }
      }
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamFrameSender.ts
  var ZlinkStreamConnectionWriteQueue = class {
    /**
     * @param writeFailed Ends the connection whose transport write failed (spec
     *   stream-connector 32 §9: a transport write failure ends the connection as
     *   `TransportError`).
     */
    constructor(connection, writeFailed) {
      this.connection = connection;
      this.writeFailed = writeFailed;
      __publicField(this, "queue", /* @__PURE__ */ new Set());
      __publicField(this, "active");
    }
    enqueue(operation) {
      this.queue.add(operation);
      this.advance();
    }
    cancel(operation, error) {
      if (this.queue.delete(operation) || this.active === operation) operation.fail(error);
    }
    /**
     * Spec stream-connector 32 §7: when the connection ends, frames that have not
     * reached the transport are not written and the frame being written is not
     * waited for. The operations of both fail with `error`.
     */
    failAll(error) {
      const operations = this.active === void 0 ? [...this.queue] : [this.active, ...this.queue];
      this.queue.clear();
      this.active = void 0;
      for (const operation of operations) operation.fail(error);
    }
    advance() {
      if (this.active !== void 0) return;
      const next = this.queue.values().next().value;
      if (next === void 0) return;
      this.queue.delete(next);
      this.active = next;
      let write;
      try {
        write = Promise.resolve(this.connection.write(next.frame));
      } catch (error) {
        write = Promise.reject(error);
      }
      void write.then(
        () => next.complete(),
        (cause) => this.failWrite(next, cause)
      ).finally(() => {
        if (this.active !== next) return;
        this.active = void 0;
        this.advance();
      });
    }
    /**
     * Spec stream-connector 32 §9: a transport write failure is `SendFailed` for
     * the operation of that write, and it ends the connection. The ending fails
     * every other operation of the connection with `Disconnected`. A write that
     * fails after the connection already ended belongs to that ending, which has
     * already failed its operation with `Disconnected`.
     */
    failWrite(operation, cause) {
      if (this.active !== operation) return;
      const error = {
        code: "sendFailed" /* SendFailed */,
        message: `Frame write failed: ${cause instanceof Error ? cause.message : String(cause)}`,
        cause
      };
      operation.fail(new ZlinkStreamException(error));
      this.writeFailed(error);
    }
  };
  var ZlinkStreamFrameSender = class {
    constructor(protocol) {
      this.protocol = protocol;
      __publicField(this, "queues", /* @__PURE__ */ new Map());
    }
    async send(connection, kind, name, payload, metadata, compress, requestSeq, signal, correlationId, actorSlot, expiry, onAccepted) {
      throwIfAborted(signal);
      await this.write(
        connection,
        this.protocol.encode(
          kind,
          name,
          payload,
          metadata,
          compress,
          requestSeq,
          correlationId,
          actorSlot
        ),
        signal,
        expiry,
        onAccepted
      );
    }
    /**
     * Writes a control frame (heartbeat ping or pong). A control frame belongs to
     * no operation: its write failure ends the connection through the write
     * queue like any other write (spec stream-connector 32 §9), and a connection
     * that has already ended does not write it. The promise settles when the
     * write has ended either way.
     */
    sendControl(connection, name) {
      const queue = this.queues.get(connection);
      if (queue === void 0) return Promise.resolve();
      const frame = this.protocol.encodeControl(name);
      return new Promise((resolve) => {
        queue.enqueue({ frame, complete: resolve, fail: () => resolve() });
      });
    }
    /**
     * Gives a connection that has just been established its own write queue.
     * `writeFailed` ends that connection when one of its transport writes fails.
     */
    open(connection, writeFailed) {
      this.queues.set(connection, new ZlinkStreamConnectionWriteQueue(connection, writeFailed));
    }
    /**
     * Ends the write queue of a connection that has ended, by close and by
     * transport loss alike. Every operation still in it fails with `error`.
     */
    failUnwritten(connection, error) {
      const queue = this.queues.get(connection);
      if (queue === void 0) return;
      this.queues.delete(connection);
      queue.failAll(error);
    }
    write(connection, frame, signal, expiry, onAccepted) {
      throwIfAborted(signal);
      const queue = this.queues.get(connection);
      if (queue === void 0) {
        throw connectorError("disconnected" /* Disconnected */, "Connector is not connected.");
      }
      let operation;
      let settled = false;
      const promise = new Promise((resolve, reject) => {
        const cleanup = () => signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
        const onAbort = () => queue.cancel(operation, signal == null ? void 0 : signal.reason);
        operation = {
          frame,
          complete: () => {
            if (settled) return;
            settled = true;
            cleanup();
            resolve();
          },
          fail: (error) => {
            if (settled) return;
            settled = true;
            cleanup();
            reject(error);
          }
        };
        signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
        onAccepted == null ? void 0 : onAccepted();
        expiry == null ? void 0 : expiry.then(void 0, (error) => queue.cancel(operation, error));
        queue.enqueue(operation);
      });
      return promise;
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
    if (payload.length < 4 || payload[0] !== 1)
      throw new Error("Unsupported session-closing version.");
    const closeReason = reasons[payload[1]];
    if (closeReason === void 0) throw new Error("Unknown session-closing reason.");
    const length = payload[2] << 8 | payload[3];
    if (length > 512 || payload.length !== 4 + length)
      throw new Error("Invalid session-closing diagnostic length.");
    const diagnostic = length === 0 ? void 0 : new TextDecoder("utf-8", { fatal: true }).decode(payload.subarray(4));
    return { closeReason, diagnostic };
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamReceiveDispatcher.ts
  function closeReasonFor(error) {
    return error.code === "frameDecodeFailed" /* FrameDecodeFailed */ || error.code === "frameTooLarge" /* FrameTooLarge */ ? "ProtocolError" : "TransportError";
  }
  var ZlinkStreamConnectionEnd = class extends Error {
    constructor(error) {
      super(error.message);
      this.error = error;
    }
  };
  var ZlinkStreamReceiveDispatcher = class {
    constructor(protocol, pendingRequests, receivedMessages, frameSender, events, actors, serverClosing) {
      this.protocol = protocol;
      this.pendingRequests = pendingRequests;
      this.receivedMessages = receivedMessages;
      this.frameSender = frameSender;
      this.events = events;
      this.actors = actors;
      this.serverClosing = serverClosing;
    }
    /**
     * @param isCurrent Tells whether the connection this batch was read from is
     *   still the connector's connection. It is asked again at the head of every
     *   frame, not only after the read: `dispatch` awaits application code —
     *   registered handlers in `Immediate`, the error handler on a failed frame —
     *   and a disconnect and reconnect can complete inside that await. Without
     *   the recheck the rest of a dead connection's batch lands on the new
     *   connection's state, and the §10 arrival counts, which start at 0 when a
     *   connection is established, take the stale frames on top.
     */
    async readAndDispatch(connection, signal, isCurrent) {
      if ((connection == null ? void 0 : connection.read) === void 0) {
        return { available: false, inbound: false };
      }
      let frameBytes;
      try {
        frameBytes = await connection.read(signal);
      } catch (cause) {
        if ((signal == null ? void 0 : signal.aborted) === true) throw cause;
        throw new ZlinkStreamConnectionEnd(
          toStreamError(cause, "disconnected" /* Disconnected */, "Transport read failed.")
        );
      }
      if (isCurrent !== void 0 && !isCurrent()) {
        return { available: false, inbound: false };
      }
      if (frameBytes === void 0) {
        return { available: false, inbound: false };
      }
      let frames;
      try {
        frames = this.protocol.decodeFrames(frameBytes);
      } catch (cause) {
        throw new ZlinkStreamConnectionEnd(
          toStreamError(cause, "frameDecodeFailed" /* FrameDecodeFailed */, "Frame decode failed.")
        );
      }
      for (const frame of frames) {
        if (isCurrent !== void 0 && !isCurrent()) {
          break;
        }
        try {
          await this.dispatch(connection, frame.header, frame.payload, signal);
        } catch (cause) {
          if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
          const error = toStreamError(
            cause,
            "frameDecodeFailed" /* FrameDecodeFailed */,
            "Frame dispatch failed."
          );
          if (error.code === "frameDecodeFailed" /* FrameDecodeFailed */) {
            throw new ZlinkStreamConnectionEnd(error);
          }
          this.events.publishError(error, signal);
        }
      }
      return { available: true, inbound: true };
    }
    /**
     * Spec stream-connector 32 §4.7 and §9: a decompressed payload over the
     * receive limit is `FrameTooLarge` and ends the connection as a protocol
     * error, like a wire payload over the limit. A payload that does not
     * decompress fails only its packet.
     */
    decodePayload(header, payload) {
      try {
        return this.protocol.decodePayload(header, payload);
      } catch (cause) {
        const error = toStreamError(
          cause,
          "decompressionFailed" /* DecompressionFailed */,
          "Decompression failed."
        );
        if (error.code === "frameTooLarge" /* FrameTooLarge */) {
          throw new ZlinkStreamConnectionEnd(error);
        }
        throw cause;
      }
    }
    async dispatch(connection, header, payload, signal) {
      const actor = header.actorSlot === void 0 ? void 0 : this.actors.resolve(header.actorSlot);
      if (header.kind === 3 /* Response */ && header.requestSeq !== void 0) {
        try {
          this.pendingRequests.resolve(
            header.requestSeq,
            () => ({
              codec: header.codec,
              payload: this.decodePayload(header, payload)
            }),
            header.metadata
          );
        } catch (cause) {
          if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
          const decodeError = toStreamError(
            cause,
            "decompressionFailed" /* DecompressionFailed */,
            "Decompression failed."
          );
          if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
            this.events.publishError(decodeError, signal);
          }
        }
        return;
      }
      if (header.kind === 4 /* Error */ && header.requestSeq !== void 0) {
        try {
          const remoteError = decodeRemoteError(this.decodePayload(header, payload));
          if (!this.pendingRequests.reject(header.requestSeq, remoteError)) {
            this.events.publishError(remoteError, signal);
          }
        } catch (cause) {
          if (cause instanceof ZlinkStreamConnectionEnd) throw cause;
          const decodeError = toStreamError(
            cause,
            "frameDecodeFailed" /* FrameDecodeFailed */,
            "Remote error payload is invalid."
          );
          if (!this.pendingRequests.reject(header.requestSeq, decodeError)) {
            this.events.publishError(decodeError, signal);
          }
        }
        return;
      }
      if (header.kind === 4 /* Error */) {
        this.events.publishError(decodeRemoteError(this.decodePayload(header, payload)), signal);
        return;
      }
      if (header.kind === 5 /* Control */) {
        await this.dispatchControl(connection, header, payload, signal);
        return;
      }
      if (header.kind === 1 /* Send */) {
        this.receivedMessages.enqueue(
          {
            name: header.name,
            metadata: header.metadata,
            payload: { codec: header.codec, payload: this.decodePayload(header, payload) },
            actorId: actor == null ? void 0 : actor.actorId,
            [zlinkStreamActorBinding]: actor
          },
          signal
        );
      }
    }
    async dispatchControl(connection, header, payload, signal) {
      var _a;
      if (this.actors.processControl(header.name, payload, signal)) {
        return;
      }
      if (header.name === ZLINK_SESSION_CLOSING) {
        const closing = decodeSessionClosing(payload);
        await ((_a = this.serverClosing) == null ? void 0 : _a.call(this, closing.closeReason));
        return;
      }
      if (payload.length !== 0) {
        throw connectorError(
          "frameDecodeFailed" /* FrameDecodeFailed */,
          "Control packet payload must be empty."
        );
      }
      if (header.name === ZLINK_STREAM_HEARTBEAT_PING) {
        await this.frameSender.sendControl(connection, ZLINK_STREAM_HEARTBEAT_PONG);
        return;
      }
      if (header.name !== ZLINK_STREAM_HEARTBEAT_PONG) {
        throw connectorError("frameDecodeFailed" /* FrameDecodeFailed */, "Unknown control packet.");
      }
    }
  };
  function decodeRemoteError(decodedPayload) {
    let decoded;
    try {
      decoded = JSON.parse(utf8Decode2(decodedPayload));
    } catch (cause) {
      throw connectorError(
        "frameDecodeFailed" /* FrameDecodeFailed */,
        "Remote error payload must be a JSON object.",
        cause
      );
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
  function randomizedDelay(baseDelayMs) {
    return Math.round(baseDelayMs * (0.5 + Math.random() * 0.5));
  }
  var ZlinkStreamConnectorLifecycle = class {
    constructor(options, pendingRequests, frameSender, receiveDispatcher, receivedMessages, actors, events) {
      this.options = options;
      this.pendingRequests = pendingRequests;
      this.frameSender = frameSender;
      this.receiveDispatcher = receiveDispatcher;
      this.receivedMessages = receivedMessages;
      this.actors = actors;
      this.events = events;
      __publicField(this, "receiveLoopAbort");
      __publicField(this, "receiveLoopSleeping", false);
      __publicField(this, "receiveLoopWake");
      __publicField(this, "receiveLoopSettled", []);
      __publicField(this, "currentConnection");
      __publicField(this, "connectionGeneration", 0);
      __publicField(this, "currentState", "created" /* Created */);
      __publicField(this, "heartbeatTimer");
      __publicField(this, "heartbeatTickRunning", false);
      __publicField(this, "lastInboundAt", 0);
      __publicField(this, "closeTask");
      __publicField(this, "connectTask");
      __publicField(this, "connectAbort");
      __publicField(this, "disconnectTask");
      __publicField(this, "closeRequested", false);
      __publicField(this, "disconnectedPublished", false);
      __publicField(this, "closeReasonValue");
      __publicField(this, "lateConnectCleanupError");
    }
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
      var _a;
      throwIfAborted(signal);
      await ((_a = this.disconnectTask) == null ? void 0 : _a.catch(() => void 0));
      this.throwIfClosed();
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
    /**
     * Spec stream-connector 32 §7: close does not wait for a connect attempt or a
     * reconnect delay to run its course. The attempts listen to one controller
     * the lifecycle owns; close aborts it, and the caller's signal is forwarded
     * to it.
     */
    async connectOnce(signal) {
      const attempts = new AbortController();
      const forwardAbort = () => attempts.abort(signal == null ? void 0 : signal.reason);
      signal == null ? void 0 : signal.addEventListener("abort", forwardAbort, { once: true });
      this.connectAbort = attempts;
      this.setState("connecting" /* Connecting */, void 0, signal);
      try {
        const connection = await this.connectWithReconnect(attempts.signal);
        if (this.closeRequested) {
          try {
            await connection.close(signal);
          } catch (error) {
            this.lateConnectCleanupError = error;
            throw error;
          }
          throw connectorError(
            "disconnected" /* Disconnected */,
            "Connector closed while connecting."
          );
        }
        this.currentConnection = connection;
        this.connectionGeneration += 1;
        const generation = this.connectionGeneration;
        this.frameSender.open(connection, (error) => {
          void this.disconnectForTransportFailure(error, connection, generation);
        });
        this.disconnectedPublished = false;
        this.receivedMessages.resetForNewConnection();
        this.lastInboundAt = Date.now();
        this.setState("connected" /* Connected */, void 0, signal);
        this.startHeartbeat();
        this.startReceiveLoop();
      } catch (cause) {
        if (this.closeRequested) {
          const message = cause instanceof Error ? cause.message : "Connector closed while connecting.";
          throw connectorError("disconnected" /* Disconnected */, message, cause);
        }
        if ((signal == null ? void 0 : signal.aborted) === true) {
          this.setState("disconnected" /* Disconnected */, void 0, signal);
          throw signal.reason;
        }
        const error = toStreamError(cause, "connectTimeout" /* ConnectTimeout */, "Connect failed.");
        this.closeReasonValue = "TransportError";
        this.setState("disconnected" /* Disconnected */, error, signal);
        this.disconnectedPublished = false;
        this.publishDisconnected(signal);
        throw new ZlinkStreamException(error);
      } finally {
        signal == null ? void 0 : signal.removeEventListener("abort", forwardAbort);
        this.connectAbort = void 0;
      }
    }
    /**
     * Spec stream-connector 32 §7: a `close` called outside the registered
     * handlers and callbacks returns once the close work has ended. One called
     * from inside them returns right after starting it; the result goes to the
     * outside caller, so a callback never waits for the close of the path that
     * is running it.
     */
    async close(signal) {
      var _a;
      this.closeReasonValue = "ClientClose";
      this.closeRequested = true;
      if (this.closeTask === void 0 && this.currentState === "closed" /* Closed */) {
        return;
      }
      const closeTask = (_a = this.closeTask) != null ? _a : this.closeTask = this.closeOnce(signal).finally(() => {
        this.closeTask = void 0;
      });
      if (this.receivedMessages.dispatching) {
        void closeTask.catch(() => void 0);
        return;
      }
      return await closeTask;
    }
    /**
     * Spec stream-connector 32 §9: once close is called, connect, Send, Request
     * and the wait surfaces fail with `Disconnected`. Send and Request reach
     * that through the missing connection; connect and the wait surfaces ask
     * here.
     */
    throwIfClosed() {
      if (this.closeRequested) {
        throw connectorError("disconnected" /* Disconnected */, "Connector is closed.");
      }
    }
    async serverClosing(reason) {
      const error = {
        code: "disconnected" /* Disconnected */,
        message: `Server closed the session: ${reason}.`
      };
      await this.disconnectForTransportFailure(
        error,
        this.currentConnection,
        this.connectionGeneration,
        reason
      );
    }
    async closeOnce(signal) {
      var _a, _b, _c;
      (_a = this.connectAbort) == null ? void 0 : _a.abort();
      await ((_b = this.connectTask) == null ? void 0 : _b.catch(() => void 0));
      await ((_c = this.disconnectTask) == null ? void 0 : _c.catch(() => void 0));
      const errors = [];
      if (this.lateConnectCleanupError !== void 0) {
        errors.push(this.lateConnectCleanupError);
        this.lateConnectCleanupError = void 0;
      }
      try {
        await this.tearDownConnection(
          { code: "disconnected" /* Disconnected */, message: "Connector closed." },
          signal
        );
      } catch (error) {
        errors.push(error);
      }
      this.setState("closed" /* Closed */, void 0, signal);
      this.publishDisconnected(signal);
      if (errors.length === 1) throw errors[0];
      if (errors.length > 1) throw new AggregateError(errors, "Stream connector close failed.");
    }
    /**
     * Spec stream-connector 32 §7: `dispatch` runs the callbacks the receive loop
     * queued, it does not drive the transport. Receiving is the receive loop's
     * job in both dispatch modes, which is what lets a `Manual` consumer complete
     * a `waitFor` without pumping, and what keeps this call from blocking on an
     * idle connection. In `Manual` it first lets the loop settle whatever has
     * already arrived, so a packet the transport is holding is delivered by this
     * pump rather than the next one.
     */
    async dispatch(signal) {
      throwIfAborted(signal);
      if (this.options.dispatchMode !== "immediate" /* Immediate */) {
        await this.settleReceiveLoop();
      }
      this.receivedMessages.pump();
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
      const unlimited = maxAttempts === null;
      while (unlimited || attempt < maxAttempts) {
        attempt += 1;
        try {
          return await this.options.transportFactory.connect(this.options, signal);
        } catch (cause) {
          lastError = toStreamError(cause, "connectTimeout" /* ConnectTimeout */, "Connect failed.");
          if (this.closeRequested || !this.options.reconnect.enabled || !unlimited && attempt >= maxAttempts) {
            break;
          }
          this.setState("reconnecting" /* Reconnecting */, lastError, signal);
          await delay(randomizedDelay(delayMs), signal);
          delayMs = Math.min(
            this.options.reconnect.maxDelayMs,
            Math.ceil(delayMs * this.options.reconnect.backoffFactor)
          );
        }
      }
      throw new ZlinkStreamException(
        lastError != null ? lastError : { code: "connectTimeout" /* ConnectTimeout */, message: "Connect failed." }
      );
    }
    startHeartbeat() {
      this.stopHeartbeat();
      if (!this.options.heartbeat.enabled) {
        return;
      }
      const timer = setInterval(() => {
        if (this.heartbeatTickRunning) {
          return;
        }
        this.heartbeatTickRunning = true;
        void this.runHeartbeatTick().finally(() => {
          if (this.heartbeatTimer === timer) {
            this.heartbeatTickRunning = false;
          }
        });
      }, this.options.heartbeat.intervalMs);
      this.heartbeatTimer = timer;
    }
    stopHeartbeat() {
      if (this.heartbeatTimer !== void 0) {
        clearInterval(this.heartbeatTimer);
        this.heartbeatTimer = void 0;
      }
      this.heartbeatTickRunning = false;
    }
    // Spec stream-connector 32 §7: the receive loop runs in both dispatch modes.
    // `Manual` only changes what the loop does with a frame — it queues the
    // registered callbacks instead of running them — never whether frames are
    // read off the transport.
    startReceiveLoop() {
      var _a;
      if (((_a = this.currentConnection) == null ? void 0 : _a.read) === void 0) {
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
      var _a;
      (_a = this.receiveLoopAbort) == null ? void 0 : _a.abort();
      this.receiveLoopAbort = void 0;
      this.receiveLoopSleeping = false;
      this.releaseReceiveLoopSettled();
    }
    async runReceiveLoop(connection, generation, signal) {
      try {
        while (this.shouldContinueReceiveLoop(connection, generation, signal)) {
          const dispatched = await this.dispatchAvailable(connection, generation, signal);
          if (!dispatched && this.shouldContinueReceiveLoop(connection, generation, signal)) {
            await this.sleepUntilWork(signal);
          }
        }
      } catch (cause) {
        if (signal.aborted) return;
        if (!(cause instanceof ZlinkStreamConnectionEnd)) throw cause;
        await this.disconnectForTransportFailure(cause.error, connection, generation);
      } finally {
        this.receiveLoopSleeping = false;
        this.releaseReceiveLoopSettled();
      }
    }
    // A transport whose read resolves only when a frame arrives parks the loop
    // inside that read; one that reports "nothing available" instead parks it
    // here. Both are the loop waiting for new data, and `dispatch` treats them
    // the same way.
    async sleepUntilWork(signal) {
      this.receiveLoopSleeping = true;
      this.releaseReceiveLoopSettled();
      try {
        await new Promise((resolve) => {
          const finish = () => {
            clearTimeout(timer);
            signal.removeEventListener("abort", onAbort);
            this.receiveLoopWake = void 0;
            resolve();
          };
          const onAbort = () => finish();
          const timer = setTimeout(finish, 1);
          signal.addEventListener("abort", onAbort, { once: true });
          this.receiveLoopWake = finish;
        });
      } finally {
        this.receiveLoopSleeping = false;
      }
    }
    // Returns once the loop has consumed everything the transport already had.
    // A loop that is mid-batch, or parked inside a read that has not produced a
    // frame, is already caught up, so only a sleeping loop is woken and awaited.
    async settleReceiveLoop() {
      var _a;
      if (this.receiveLoopAbort === void 0 || !this.receiveLoopSleeping) {
        return;
      }
      const settled = new Promise((resolve) => {
        this.receiveLoopSettled.push(resolve);
      });
      (_a = this.receiveLoopWake) == null ? void 0 : _a.call(this);
      await settled;
    }
    releaseReceiveLoopSettled() {
      for (const resolve of this.receiveLoopSettled.splice(0)) {
        resolve();
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
        const error = { code: "disconnected" /* Disconnected */, message: "Heartbeat timed out." };
        await this.disconnectForTransportFailure(
          error,
          this.currentConnection,
          this.connectionGeneration,
          "HeartbeatTimeout"
        );
        return;
      }
      await this.frameSender.sendControl(this.connectionForSend(), ZLINK_STREAM_HEARTBEAT_PING);
    }
    /**
     * @param reason Given only where the reason is not read from `error`: a
     *   server `session-closing` and the heartbeat timeout. Every other ending
     *   takes it from {@link closeReasonFor}.
     */
    async disconnectForTransportFailure(error, origin, generation, reason = closeReasonFor(error)) {
      if (this.closeRequested || this.currentState === "closed" /* Closed */) {
        return;
      }
      if (origin !== void 0 && !this.isCurrentConnection(origin, generation)) {
        return;
      }
      this.closeReasonValue = reason;
      if (this.disconnectTask !== void 0) {
        return await this.disconnectTask;
      }
      this.disconnectTask = this.tearDownConnection(error).catch(() => {
      }).finally(() => {
        this.disconnectTask = void 0;
      });
      await this.disconnectTask;
      await this.announceDisconnect(error);
    }
    isCurrentConnection(connection, generation) {
      return !this.closeRequested && this.currentConnection === connection && this.connectionGeneration === generation;
    }
    /**
     * Ends the current connection, for close and for transport loss alike. No
     * application callback runs from here. Spec stream-connector 32 §7 and §9:
     * every operation the ending connection fails (the frames it has not
     * written, the one it is writing and the pending requests) fails with
     * `Disconnected`, whatever ended it; the cause stays in the close reason.
     * The transport is closed without waiting for the frames.
     */
    async tearDownConnection(error, signal) {
      this.stopHeartbeat();
      this.stopReceiveLoop();
      const connection = this.currentConnection;
      this.currentConnection = void 0;
      const disconnected = { code: "disconnected" /* Disconnected */, message: error.message };
      if (connection !== void 0) {
        this.frameSender.failUnwritten(
          connection,
          connectorError(disconnected.code, disconnected.message)
        );
      }
      this.pendingRequests.failAll(disconnected);
      this.receivedMessages.connectionEnded();
      this.actors.closeAll(signal);
      await (connection == null ? void 0 : connection.close(signal));
    }
    /**
     * Runs once the teardown promise has settled, so a handler reached from here
     * may call `connect` without waiting for a task its own caller still holds.
     * The reconnect is queued after both notifications have been started for the
     * same reason: spec stream-connector 32 §6 has reconnect on by default, and a
     * handler that is slow — or whose promise never settles at all — must not
     * cost the connector the attempt.
     */
    async announceDisconnect(error) {
      if (this.closeRequested) return;
      this.setState("disconnected" /* Disconnected */, error);
      this.publishDisconnected();
      if (this.shouldReconnect()) {
        queueMicrotask(() => {
          void this.connect().catch(() => void 0);
        });
      }
    }
    shouldReconnect() {
      return this.options.reconnect.enabled && !this.closeRequested;
    }
    /**
     * Spec stream-connector 32 §6 and §7: one disconnect notification per
     * lifecycle event, handed to the dispatch queue. The connector does not wait
     * for the handler; `Manual` runs it when the application pumps dispatch.
     * The flag is tested and set synchronously, so concurrent callers for the
     * same event claim it once.
     */
    publishDisconnected(signal) {
      if (this.disconnectedPublished) return;
      this.disconnectedPublished = true;
      this.events.publishDisconnected(signal);
    }
    setState(current, error, signal) {
      const previous = this.currentState;
      this.currentState = current;
      if (previous === current && error === void 0) return;
      this.events.publishStateChanged({ previous, current, error }, signal);
      if (error !== void 0) this.events.publishError(error, signal);
    }
  };

  // packages/stream-connector/src/Runtime/ZlinkStreamConnectorEvents.ts
  var ZlinkStreamConnectorEvents = class {
    /**
     * @param enqueueCallback Queues a callback that calls the number of handlers
     *   `callbacks` reports for the handlers registered at that moment.
     */
    constructor(enqueueCallback) {
      this.enqueueCallback = enqueueCallback;
      __publicField(this, "errorHandlers", /* @__PURE__ */ new Set());
      __publicField(this, "disconnectedHandlers", /* @__PURE__ */ new Set());
      __publicField(this, "stateHandlers", /* @__PURE__ */ new Set());
    }
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
    publishError(error, signal) {
      this.publish(this.errorHandlers, (handler) => handler(error, signal), signal);
    }
    publishDisconnected(signal) {
      this.publish(this.disconnectedHandlers, (handler) => handler(signal), signal);
    }
    publishStateChanged(change, signal) {
      this.publish(this.stateHandlers, (handler) => handler(change, signal), signal);
    }
    /**
     * Spec stream-connector 32 §7: the connector runs a registered callback and
     * does not wait for it to finish. A thrown or rejected failure reaches the
     * error handlers as `UserCallbackFailed`.
     */
    runUserCallback(run, failureMessage, signal) {
      this.invoke(
        run,
        (cause) => this.publishError(
          { code: "userCallbackFailed" /* UserCallbackFailed */, message: failureMessage, cause },
          signal
        )
      );
    }
    publish(handlers, invoke, signal) {
      this.enqueueCallback(
        () => {
          for (const handler of Array.from(handlers)) {
            const report = (cause) => this.reportFailure(
              cause,
              signal,
              handlers === this.errorHandlers ? handler : void 0
            );
            this.invoke(() => invoke(handler), report);
          }
        },
        () => handlers.size
      );
    }
    reportFailure(cause, signal, failedHandler) {
      const error = {
        code: "userCallbackFailed" /* UserCallbackFailed */,
        message: "Connector event handler failed.",
        cause
      };
      if (failedHandler === void 0) {
        this.publishError(error, signal);
        return;
      }
      this.reportToRemaining(error, signal, /* @__PURE__ */ new Set([failedHandler]));
    }
    reportToRemaining(error, signal, attempted) {
      const remaining = Array.from(this.errorHandlers).filter(
        (candidate) => !attempted.has(candidate)
      );
      if (remaining.length === 0) return;
      const nextAttempted = /* @__PURE__ */ new Set([...attempted, ...remaining]);
      this.enqueueCallback(
        () => {
          for (const handler of remaining) {
            const report = (cause) => this.reportToRemaining({ ...error, cause }, signal, nextAttempted);
            this.invoke(() => handler(error, signal), report);
          }
        },
        () => remaining.length
      );
    }
    invoke(run, report) {
      try {
        Promise.resolve(run()).catch(report);
      } catch (cause) {
        report(cause);
      }
    }
  };

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
      __publicField(this, "messages", []);
      __publicField(this, "messageHead", 0);
      __publicField(this, "closed", false);
      __publicField(this, "error");
      __publicField(this, "readWaiter");
      __publicField(this, "onMessage", (event) => {
        try {
          const message = toUint8Array(event.data);
          this.messages.push(message);
        } catch (cause) {
          this.error = cause instanceof Error ? cause : connectorError(
            "frameDecodeFailed" /* FrameDecodeFailed */,
            "WebSocket message decode failed.",
            cause
          );
          this.closed = true;
          this.socket.close();
        }
        this.wakeReader();
      });
      __publicField(this, "onClose", () => {
        if (!this.closed) {
          this.error = connectorError(
            "disconnected" /* Disconnected */,
            "Remote stream closed the WebSocket connection."
          );
        }
        this.closed = true;
        this.wakeReader();
      });
      __publicField(this, "onError", () => {
        this.error = connectorError(
          "disconnected" /* Disconnected */,
          "Remote stream closed after a WebSocket error."
        );
        this.closed = true;
        this.wakeReader();
      });
      socket.addEventListener("message", this.onMessage);
      socket.addEventListener("close", this.onClose);
      socket.addEventListener("error", this.onError);
    }
    async write(frame, signal) {
      throwIfAborted(signal);
      if (this.closed || this.socket.readyState !== 1) {
        throw connectorError("disconnected" /* Disconnected */, "Remote stream is not connected.");
      }
      this.socket.send(frame);
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
    /**
     * Spec stream-connector 32 §7: closing does not wait for the peer to read or
     * answer. `WebSocket.close` sends the close frame after the data already
     * handed to `send`, so a frame whose write completed is not torn.
     */
    async close(signal) {
      throwIfAborted(signal);
      this.removeListeners();
      if (!this.closed) {
        this.closed = true;
        this.socket.close();
      }
      this.wakeReader();
    }
    waitForMessage(signal) {
      if (this.readWaiter !== void 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Only one pending stream read is supported."
        );
      }
      return new Promise((resolve, reject) => {
        const onAbort = () => {
          this.readWaiter = void 0;
          reject(signal == null ? void 0 : signal.reason);
        };
        signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
        this.readWaiter = () => {
          signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
          this.readWaiter = void 0;
          resolve();
        };
      });
    }
    wakeReader() {
      var _a;
      (_a = this.readWaiter) == null ? void 0 : _a.call(this);
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
  async function waitForOpen(socket, connectTimeoutMs, signal) {
    throwIfAborted(signal);
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => finish(connectorError("connectTimeout" /* ConnectTimeout */, "Connect timed out.")),
        connectTimeoutMs
      );
      const onOpen = () => finish();
      const onClose = () => finish(connectorError("connectTimeout" /* ConnectTimeout */, "Connect closed before opening."));
      const onError = () => finish(connectorError("connectTimeout" /* ConnectTimeout */, "Connect failed."));
      const onAbort = () => finish(signal == null ? void 0 : signal.reason);
      const finish = (error) => {
        clearTimeout(timeout);
        socket.removeEventListener("open", onOpen);
        socket.removeEventListener("close", onClose);
        socket.removeEventListener("error", onError);
        signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
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
      signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
    });
  }
  function toUint8Array(data) {
    if (data instanceof ArrayBuffer) {
      return new Uint8Array(data);
    }
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    throw connectorError(
      "frameDecodeFailed" /* FrameDecodeFailed */,
      "WebSocket text messages are not supported."
    );
  }

  // packages/stream-connector/src/Runtime/ZlinkStreamConnector.ts
  var DefaultZlinkStreamConnector = class {
    constructor(options) {
      __publicField(this, "receivedMessages");
      __publicField(this, "lifecycle");
      __publicField(this, "events", new ZlinkStreamConnectorEvents(
        (callback, callbacks) => this.receivedMessages.enqueueCallback(callback, callbacks)
      ));
      __publicField(this, "correlationCounter", 0n);
      __publicField(this, "pendingRequests", new ZlinkStreamPendingRequests());
      __publicField(this, "frameSender");
      __publicField(this, "receiveDispatcher");
      __publicField(this, "requestSendingHandlers", /* @__PURE__ */ new Set());
      __publicField(this, "replyReceivedHandlers", /* @__PURE__ */ new Set());
      __publicField(this, "boundActors");
      __publicField(this, "options");
      this.options = normalizeOptions(options, new BrowserStreamTransportFactory());
      const protocol = new ZlinkStreamFrameProtocol(this.options);
      this.frameSender = new ZlinkStreamFrameSender(protocol);
      this.receivedMessages = new ZlinkStreamReceivedMessages(
        this.events,
        this.options.dispatchMode === "immediate" /* Immediate */
      );
      this.boundActors = new ZlinkStreamActors(this, this.receivedMessages, this.events);
      this.receiveDispatcher = new ZlinkStreamReceiveDispatcher(
        protocol,
        this.pendingRequests,
        this.receivedMessages,
        this.frameSender,
        this.events,
        this.boundActors,
        (reason) => this.lifecycle.serverClosing(reason)
      );
      this.lifecycle = new ZlinkStreamConnectorLifecycle(
        this.options,
        this.pendingRequests,
        this.frameSender,
        this.receiveDispatcher,
        this.receivedMessages,
        this.boundActors,
        this.events
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
      return this.receivedMessages.pendingCallbacks;
    }
    get actors() {
      return this.boundActors.snapshot;
    }
    actor(actorId) {
      return this.boundActors.find(actorId);
    }
    onActorBound(handler) {
      return this.boundActors.onBound(handler);
    }
    onActorUnbound(handler) {
      return this.boundActors.onUnbound(handler);
    }
    /**
     * Spec stream-connector 32 §10: how many packets carrying `name` arrived on
     * the current connection. Arrivals are what is counted, so a message a
     * handler dispatched or a wait surface consumed still counts, and `Manual`
     * and `Immediate` report the same number. Establishing a connection resets
     * the count to 0, a reconnect included.
     */
    receivedCount(name) {
      validateName(name);
      return this.receivedMessages.receivedCount(name);
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
    onRequestSending(handler) {
      this.requestSendingHandlers.add(handler);
      return subscription(() => this.requestSendingHandlers.delete(handler));
    }
    onReplyReceived(handler) {
      this.replyReceivedHandlers.add(handler);
      return subscription(() => this.replyReceivedHandlers.delete(handler));
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
      return new ZlinkStreamRequestBuilder(
        this,
        this.resolveNameOrDefault(encoded),
        encoded,
        (callback) => this.receivedMessages.enqueueCallback(callback)
      );
    }
    on(nameOrType, handler, messageType) {
      const encodedHandler = (message, signal) => handler(
        {
          name: message.name,
          metadata: message.metadata,
          payload: this.decodePayload(
            message.payload,
            messageType != null ? messageType : typeof nameOrType === "function" ? nameOrType : void 0
          ),
          actorId: message.actorId
        },
        signal
      );
      return this.receivedMessages.on(this.observedName(nameOrType), encodedHandler);
    }
    sendForActor(actor, payload, messageType) {
      const encoded = this.encodePayload(payload, messageType);
      return new ZlinkStreamSendBuilder(
        this,
        this.resolveNameOrDefault(encoded),
        encoded,
        actor.slot,
        () => actor.ensureBound()
      );
    }
    requestForActor(actor, payload, messageType) {
      const encoded = this.encodePayload(payload, messageType);
      return new ZlinkStreamRequestBuilder(
        this,
        this.resolveNameOrDefault(encoded),
        encoded,
        (callback) => this.receivedMessages.enqueueCallback(callback),
        actor.slot,
        () => actor.ensureBound()
      );
    }
    onActorMessage(actor, nameOrType, handler, messageType) {
      const encodedHandler = (message, signal) => handler(
        {
          name: message.name,
          metadata: message.metadata,
          payload: this.decodePayload(
            message.payload,
            messageType != null ? messageType : typeof nameOrType === "function" ? nameOrType : void 0
          ),
          actorId: message.actorId
        },
        signal
      );
      return this.receivedMessages.on(this.observedName(nameOrType), encodedHandler, actor);
    }
    waitFor(nameOrType) {
      return new ZlinkStreamWaitBuilder(this, this.observedName(nameOrType));
    }
    expectNone(nameOrType) {
      return new ZlinkStreamExpectNoneBuilder(this, this.observedName(nameOrType));
    }
    waitForSequence(nameOrType) {
      return new ZlinkStreamSequenceBuilder(this, this.observedName(nameOrType));
    }
    /**
     * Spec stream-connector 32 §10.1: each wait surface offers both ways of
     * naming a packet. A string is the name the caller states; a constructor
     * goes through the options' `nameResolver`, which is the same resolver
     * `send` and `request` use, so both paths land on one name for one type.
     */
    observedName(nameOrType) {
      const name = typeof nameOrType === "function" ? this.options.nameResolver.resolve(nameOrType) : nameOrType;
      if (typeof name !== "string") {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Packet name must be a string or a payload constructor."
        );
      }
      validateName(name);
      return name;
    }
    /**
     * Consumes the first message under `name` that `predicate` accepts, or
     * resolves `undefined` when `timeoutMs` elapses first.
     *
     * A timeout is not a failure here. Each wait surface decides what its own
     * timeout means — `waitFor` fails on it while `expectNone` succeeds — and
     * reports that decision as `ValidationFailed` (spec stream-connector 32
     * §10.1.1; .NET `ZlinkStreamReceivedMessages.WaitForAsync` parity). Losing
     * the connection the wait observes is the one ending decided here, because
     * the wait has no place left to observe: that is `Disconnected` for every
     * surface (§10.1.1).
     */
    waitForMessage(name, timeoutMs, predicate, signal) {
      validateName(name);
      if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
        throw connectorError(
          "validationFailed" /* ValidationFailed */,
          "Timeout must be a non-negative finite number."
        );
      }
      throwIfAborted(signal);
      this.lifecycle.throwIfClosed();
      return new Promise((resolve, reject) => {
        let done = false;
        let timer;
        let disposable;
        const onAbort = () => finish(signal == null ? void 0 : signal.reason);
        const finish = (error, message) => {
          if (done) {
            return;
          }
          done = true;
          signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
          if (timer !== void 0) {
            clearTimeout(timer);
          }
          disposable == null ? void 0 : disposable.dispose();
          if (error !== void 0) {
            reject(error);
          } else {
            resolve(message);
          }
        };
        timer = setTimeout(() => finish(), timeoutMs);
        signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
        disposable = this.receivedMessages.observe(
          name,
          (message) => {
            if (done) {
              return false;
            }
            try {
              const decoded = {
                name: message.name,
                metadata: message.metadata,
                payload: this.decodeWaitPayload(message.payload),
                actorId: message.actorId
              };
              if (!predicate(decoded)) {
                return false;
              }
              finish(void 0, decoded);
            } catch (cause) {
              finish(cause);
            }
            return true;
          },
          () => finish(
            connectorError(
              "disconnected" /* Disconnected */,
              `The connection this wait for '${name}' observed has ended.`
            )
          )
        );
      });
    }
    encodePayload(payload, messageType) {
      var _a;
      if (isEncodedPayload(payload)) {
        return payload;
      }
      const codec = (_a = this.options.codec) != null ? _a : zlinkStreamJsonCodec;
      return codec.encode(payload, messageType);
    }
    decodePayload(payload, messageType) {
      var _a;
      if (messageType === void 0 && this.options.codec === void 0) {
        return payload;
      }
      return ((_a = this.options.codec) != null ? _a : zlinkStreamJsonCodec).decode(payload, messageType);
    }
    decodeWaitPayload(payload) {
      var _a;
      if (this.options.codec !== void 0 || payload.codec === 1 /* Json */) {
        return ((_a = this.options.codec) != null ? _a : zlinkStreamJsonCodec).decode(payload);
      }
      return payload;
    }
    async sendEncoded(kind, name, payload, metadata, compress, requestSeq, signal, correlationId, actorSlot, expiry, onAccepted) {
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
        actorSlot,
        expiry,
        onAccepted
      );
    }
    /**
     * Per-connector monotonic correlation id (hex). The client generates it on each request
     * and the server echoes it back on the reply.
     */
    nextCorrelationId() {
      this.correlationCounter += 1n;
      return this.correlationCounter.toString(16);
    }
    async requestEncoded(name, payload, metadata, compress, timeoutMs, signal, actorSlot) {
      const startedAt = Date.now();
      const actorId = actorSlot === void 0 ? void 0 : this.boundActors.resolve(actorSlot).actorId;
      let requestMetadata = metadata;
      const sendingContext = {
        requestPacketName: name,
        actorId,
        setMetadata(key, value) {
          requestMetadata = requestMetadata.with(key, value);
        }
      };
      let pending;
      let stopCancellation;
      try {
        throwIfAborted(signal);
        for (const handler of Array.from(this.requestSendingHandlers)) {
          try {
            handler(sendingContext);
          } catch (cause) {
            this.events.publishError(
              {
                code: "userCallbackFailed" /* UserCallbackFailed */,
                message: "Request sending hook failed.",
                cause
              },
              signal
            );
          }
        }
        pending = this.pendingRequests.create(name, timeoutMs);
        const accepted = pending;
        const write = this.sendEncoded(
          2 /* Request */,
          name,
          payload,
          requestMetadata,
          compress,
          accepted.requestSeq,
          signal,
          this.nextCorrelationId(),
          actorSlot,
          accepted.promise,
          accepted.startTimeout
        );
        const canceled = new Promise((_, reject) => {
          const onAbort = () => reject(signal == null ? void 0 : signal.reason);
          signal == null ? void 0 : signal.addEventListener("abort", onAbort, { once: true });
          stopCancellation = () => signal == null ? void 0 : signal.removeEventListener("abort", onAbort);
        });
        const reply = await Promise.race([
          write.then(() => accepted.promise),
          accepted.promise,
          canceled
        ]);
        this.publishReplyReceived(
          {
            requestPacketName: name,
            actorId,
            succeeded: true,
            reply: { ...reply, actorId },
            elapsed: Date.now() - startedAt
          },
          signal
        );
        return reply.payload;
      } catch (error) {
        if (pending !== void 0) this.pendingRequests.cancel(pending.requestSeq);
        if (isCancellation(signal, error)) throw error;
        this.publishReplyReceived(
          {
            requestPacketName: name,
            actorId,
            succeeded: false,
            error: unwrapStreamError(error),
            elapsed: Date.now() - startedAt
          },
          signal
        );
        throw error;
      } finally {
        stopCancellation == null ? void 0 : stopCancellation();
      }
    }
    publishReplyReceived(context, signal) {
      if (this.replyReceivedHandlers.size === 0) return;
      this.receivedMessages.enqueueCallback(
        () => {
          for (const handler of Array.from(this.replyReceivedHandlers)) {
            this.events.runUserCallback(
              () => handler(context, signal),
              "Reply received hook failed.",
              signal
            );
          }
        },
        () => this.replyReceivedHandlers.size
      );
    }
    resolveNameOrDefault(payload) {
      if (payload.messageType === void 0) {
        return void 0;
      }
      return this.options.nameResolver.resolve(payload.messageType);
    }
  };
  __publicField(DefaultZlinkStreamConnector, "heartbeatPingName", ZLINK_STREAM_HEARTBEAT_PING);
  __publicField(DefaultZlinkStreamConnector, "heartbeatPongName", ZLINK_STREAM_HEARTBEAT_PONG);
  function isEncodedPayload(value) {
    if (value === null || typeof value !== "object") {
      return false;
    }
    const candidate = value;
    return typeof candidate.codec === "number" && candidate.payload instanceof Uint8Array;
  }

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
