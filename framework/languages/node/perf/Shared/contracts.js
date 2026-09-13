'use strict';

// Application DTOs use the Framework's default typed JSON path.
class PerfEchoRequest { constructor(fields) { Object.assign(this, { scheduledTicks: null }, fields); } }
class PerfEchoReply { constructor(fields) { Object.assign(this, fields); } }
class PerfDriveRequest { constructor(echo) { this.echo = echo; } }
class PerfDriveReply { constructor(started, echo = null) { this.started = started; this.echo = echo; } }
class PerfPublishEvent { constructor(fields) { Object.assign(this, { scheduledTicks: null }, fields); } }
class PerfBindRequest { constructor(runId, cellId, clientId) { Object.assign(this, { runId, cellId, clientId }); } }
class PerfBindReply { constructor(actorId, bound) { Object.assign(this, { actorId, bound }); } }
class PerfProbeRequest extends PerfEchoRequest {}

function registerPackets(framework) {
  for (const type of [PerfEchoRequest, PerfEchoReply, PerfDriveRequest, PerfDriveReply, PerfPublishEvent, PerfBindRequest, PerfBindReply, PerfProbeRequest]) {
    framework.ZLinkPacket(type.name)(type);
  }
}
function validateCcu(workload) {
  for (const key of ['connections', 'logicalStreams']) {
    const value = workload?.[key];
    if (value != null && (!Number.isInteger(value) || value < 1 || value > 1000)) throw new Error(`workload.${key} must be between 1 and 1000; inflight is separate from CCU.`);
  }
}
function pattern(size) {
  const bytes = Buffer.alloc(size);
  for (let i = 0; i < size; i++) bytes[i] = (31 * i + 17 * Math.floor(i / 251) + 29) % 256;
  return bytes.toString('base64');
}
function validatePattern(payload, size, expected) {
  const bytes = Buffer.from(payload, 'base64');
  if (bytes.length !== size || payload !== expected) throw validation('PayloadMismatch', 'Noncanonical payload or logical byte count.');
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== (31 * i + 17 * Math.floor(i / 251) + 29) % 256) throw validation('PayloadMismatch', 'Logical payload pattern differs.');
  }
}
function decimal(text, signed = false) {
  if (typeof text !== 'string' || !(signed ? /^-?(0|[1-9]\d*)$/ : /^(0|[1-9]\d*)$/).test(text)) throw validation('SchemaMismatch', 'Canonical decimal string required.');
  const value = BigInt(text);
  if (String(value) !== text || value < (signed ? -(1n << 63n) : 0n) || value > (signed ? (1n << 63n) - 1n : (1n << 64n) - 1n)) throw validation('SchemaMismatch', 'Decimal value outside wire field range.');
  return value;
}
function validation(kind, message) { return Object.assign(new Error(message), { name: 'PerfValidationError', harnessKind: kind }); }
function validateIdentity(request, reply) {
  for (const field of ['runId', 'cellId', 'resetSeq', 'phase', 'clientId', 'sequence', 'correlationId']) {
    if (request[field] !== reply[field]) throw validation('IdentityMismatch', `Reply ${field} differs.`);
  }
  decimal(reply.receivedTicks, true);
  if (!reply.clockDomainId) throw validation('IdentityMismatch', 'Reply clock domain missing.');
}
function json(value) {
  return JSON.stringify(value, (_key, item) => typeof item === 'bigint' ? String(item) : item instanceof Map ? Object.fromEntries(item) : item);
}
module.exports = { PerfEchoRequest, PerfEchoReply, PerfDriveRequest, PerfDriveReply, PerfPublishEvent, PerfBindRequest, PerfBindReply, PerfProbeRequest, registerPackets, validateCcu, pattern, validatePattern, validateIdentity, decimal, validation, json };
