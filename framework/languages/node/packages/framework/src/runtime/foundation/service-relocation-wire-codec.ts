import { operationRequiresReply } from './service-runtime-contracts';

const MAX_PARTICIPANTS = 1_024;
const MAX_RECORDS = 65_536;

export interface ServiceWireRelocationProgress {
  readonly participantId: bigint;
  readonly acceptedBoundary: bigint;
  readonly replayCursor: bigint;
}

export interface ServiceWireRequestCompletion {
  readonly operationHigh: bigint;
  readonly operationLow: bigint;
  readonly sourceOwnerId: string;
  readonly sourceOwnerLeaseGeneration: bigint;
  readonly sourceNodeRid: string;
  readonly sourceNodeGeneration: bigint;
  readonly participantId: bigint;
  readonly sequence: bigint;
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly deliveryState: number;
  readonly payload?: {
    readonly packetName: string;
    readonly contentType: string;
    readonly bytes: Uint8Array;
  };
}

export interface ServiceWireRelocationEnvelope {
  readonly relocationHigh: bigint;
  readonly relocationLow: bigint;
  readonly applicationVersion: bigint;
  readonly participantProgress: readonly ServiceWireRelocationProgress[];
  readonly terminalCompletions: readonly ServiceWireRequestCompletion[];
  /** Canonical bytes are retained because journal record bodies are immutable. */
  readonly canonicalBytes: Uint8Array;
  readonly canonicalPrefix: Uint8Array;
  readonly canonicalJournalEntries: readonly {
    readonly participantId: bigint;
    readonly sequence: bigint;
    readonly bytes: Uint8Array;
  }[];
  readonly canonicalAfterJournal: Uint8Array;
}

/** Decodes the canonical service-wire `relocation-envelope-v1` logical stream. */
export function decodeServiceWireRelocationEnvelope(
  encoded: Uint8Array
): ServiceWireRelocationEnvelope {
  const reader = new Reader(encoded);
  const relocationHigh = reader.u64();
  const relocationLow = reader.u64();
  if (relocationHigh === 0n && relocationLow === 0n) invalid('relocation id');
  readRelocationObject(reader);
  const applicationVersion = reader.i64();
  if (applicationVersion < 0n) invalid('application version');

  const states = new Set<bigint>();
  const stateCount = reader.count(MAX_PARTICIPANTS, 'application state');
  let previousId = 0n;
  for (let index = 0; index < stateCount; index++) {
    const participantId = reader.nonzeroU64('application-state participant id');
    if (participantId <= previousId) invalid('application-state participant order');
    previousId = participantId;
    states.add(participantId);
    const hasState = reader.bool();
    const body = reader.body64();
    if (hasState) body.bytes64();
    body.end('application state');
  }

  const progressOffset = reader.position;
  const progressCount = reader.count(MAX_PARTICIPANTS, 'participant progress');
  if (progressCount !== stateCount) invalid('participant progress coverage');
  const participantProgress: ServiceWireRelocationProgress[] = [];
  previousId = 0n;
  for (let index = 0; index < progressCount; index++) {
    const participantId = reader.nonzeroU64('progress participant id');
    const acceptedBoundary = reader.u64();
    const replayCursor = reader.u64();
    if (
      participantId <= previousId
      || !states.has(participantId)
      || replayCursor > acceptedBoundary
    ) invalid('participant progress');
    previousId = participantId;
    participantProgress.push({ participantId, acceptedBoundary, replayCursor });
  }

  const canonicalJournalEntries = readJournal(
    reader,
    new Map(participantProgress.map(value => [value.participantId, value]))
  );
  const journalEnd = reader.position;
  const timerNames = readTimerRegistrations(reader, states);
  readPendingTimerTicks(reader, states, timerNames);
  const completionOffset = reader.position;
  const terminalCompletions = readTerminalCompletions(reader, states);
  reader.end('relocation envelope');
  return {
    relocationHigh,
    relocationLow,
    applicationVersion,
    participantProgress,
    terminalCompletions,
    canonicalBytes: Buffer.from(encoded),
    canonicalPrefix: Buffer.from(encoded.subarray(0, progressOffset)),
    canonicalJournalEntries,
    canonicalAfterJournal: Buffer.from(encoded.subarray(journalEnd, completionOffset))
  };
}

/** Re-emits the immutable canonical stream without a language-local envelope. */
export function encodeServiceWireRelocationEnvelope(
  envelope: ServiceWireRelocationEnvelope
): Buffer {
  const result = Buffer.concat([
    Buffer.from(envelope.canonicalPrefix),
    encodeServiceWireRelocationProgress(envelope.participantProgress),
    encodeJournal(envelope.canonicalJournalEntries, envelope.participantProgress),
    Buffer.from(envelope.canonicalAfterJournal),
    encodeCompletions(envelope.terminalCompletions)
  ]);
  const decoded = decodeServiceWireRelocationEnvelope(result);
  if (decoded.relocationHigh !== envelope.relocationHigh
    || decoded.relocationLow !== envelope.relocationLow
    || decoded.applicationVersion !== envelope.applicationVersion) {
    invalid('successor identity');
  }
  return result;
}

function encodeJournal(
  entries: ServiceWireRelocationEnvelope['canonicalJournalEntries'],
  progress: readonly ServiceWireRelocationProgress[]
): Buffer {
  const cursors = new Map(progress.map(value => [value.participantId, value.replayCursor]));
  const pending = entries.filter(entry => entry.sequence > (cursors.get(entry.participantId) ?? 0n));
  return Buffer.concat([u32(pending.length), ...pending.map(entry => Buffer.from(entry.bytes))]);
}

export function encodeServiceWireRelocationProgress(
  values: readonly ServiceWireRelocationProgress[]
): Buffer {
  return Buffer.concat([u32(values.length), ...values.map(value => Buffer.concat([
    u64(value.participantId), u64(value.acceptedBoundary), u64(value.replayCursor)
  ]))]);
}

function encodeCompletions(values: readonly ServiceWireRequestCompletion[]): Buffer {
  return Buffer.concat([u32(values.length), ...values.map(value => Buffer.concat([
    u64(value.operationHigh),
    u64(value.operationLow),
    text8(value.sourceOwnerId),
    u64(value.sourceOwnerLeaseGeneration),
    text8(value.sourceNodeRid),
    u64(value.sourceNodeGeneration),
    u64(value.participantId),
    u64(value.sequence),
    u32(value.terminalResult),
    u32(value.failureCode),
    Buffer.of(value.deliveryState),
    Buffer.of(value.payload === undefined ? 0 : 1),
    ...(value.payload === undefined ? [] : [applicationPayload(value.payload)])
  ]))]);
}

function applicationPayload(value: NonNullable<ServiceWireRequestCompletion['payload']>): Buffer {
  const body = Buffer.concat([text8(value.packetName), text8(value.contentType),
    u32(value.bytes.byteLength), Buffer.from(value.bytes)]);
  return Buffer.concat([Buffer.of(1), u32(body.byteLength), body]);
}

function u32(value: number): Buffer {
  if (!Number.isSafeInteger(value) || value < 0 || value > 0xffff_ffff) invalid('u32');
  const result = Buffer.allocUnsafe(4);
  result.writeUInt32BE(value);
  return result;
}

function u64(value: bigint): Buffer {
  if (value < 0n || value > 0xffff_ffff_ffff_ffffn) invalid('u64');
  const result = Buffer.allocUnsafe(8);
  result.writeBigUInt64BE(value);
  return result;
}

function text8(value: string): Buffer {
  const encoded = Buffer.from(value, 'utf8');
  if (encoded.byteLength < 1 || encoded.byteLength > 255) invalid('text8');
  return Buffer.concat([Buffer.of(encoded.byteLength), encoded]);
}

function readJournal(
  reader: Reader,
  progress: ReadonlyMap<bigint, ServiceWireRelocationProgress>
): ServiceWireRelocationEnvelope['canonicalJournalEntries'] {
  const count = reader.count(MAX_RECORDS, 'journal');
  const entries: Array<{ participantId: bigint; sequence: bigint; bytes: Uint8Array }> = [];
  let previousParticipant = 0n;
  let previousSequence = 0n;
  for (let index = 0; index < count; index++) {
    const start = reader.position;
    const participantId = reader.nonzeroU64('journal participant id');
    const sequence = reader.nonzeroU64('journal sequence');
    const participant = progress.get(participantId);
    if (
      participant === undefined
      || participantId < previousParticipant
      || (participantId === previousParticipant && sequence <= previousSequence)
      || sequence <= participant.replayCursor
      || sequence > participant.acceptedBoundary
    ) invalid('journal order');
    previousSequence = participantId === previousParticipant ? sequence : 0n;
    previousParticipant = participantId;
    previousSequence = sequence;
    readFrozenRecord(reader);
    entries.push({ participantId, sequence, bytes: reader.copy(start) });
  }
  return entries;
}

function readFrozenRecord(reader: Reader): void {
  const kind = reader.u8();
  if (kind < 1 || kind > 14) invalid('journal record kind');
  const sourceKind = reader.u8();
  const source = reader.body16();
  source.text8();
  source.nonzeroU64('source node generation');
  source.text8();
  source.nonzeroU64('source owner lease generation');
  if (sourceKind === 2) source.text8();
  else if (sourceKind === 3) readActorRef(source);
  else if (sourceKind === 4) {
    readActorRef(source);
    source.text8();
    source.nonzeroU64('source binding generation');
    source.nonzeroU64('source session sequence');
  } else if (sourceKind !== 1) invalid('journal source kind');
  source.end('journal source');
  if (reader.bool()) readMetadata(reader);
  reader.u64();
  reader.u64();
  const operationKind = reader.u32();
  const reply = reader.body16();
  if (operationRequiresReply(operationKind)) {
    reply.nonzeroU64('reply route id');
  }
  reply.end('journal reply route');
  readFrozenBody(reader, kind);
}

function readFrozenBody(reader: Reader, kind: number): void {
  if (kind === 1 || kind === 2) {
    readApplicationPayload(reader);
    return;
  }
  if (kind === 3 || kind === 4) {
    reader.text8();
    readApplicationPayload(reader);
    return;
  }
  if (kind === 5 || kind === 6) {
    readSpotRouteFence(reader);
    readApplicationPayload(reader);
    return;
  }
  if (kind === 7) {
    reader.text8();
    reader.text8();
    readApplicationPayload(reader);
    return;
  }
  if (kind === 9 || kind === 10) {
    readActorRouteFence(reader);
    readApplicationPayload(reader);
    return;
  }
  if (kind === 11) {
    terminalResult(reader.u32());
    reader.u32();
    if (reader.bool()) readApplicationPayload(reader);
    return;
  }
  throw new TypeError(`Unsupported canonical relocation journal record kind ${kind}.`);
}

function readTimerRegistrations(
  reader: Reader,
  participants: ReadonlySet<bigint>
): ReadonlyMap<bigint, ReadonlySet<string>> {
  const count = reader.count(MAX_RECORDS, 'timer registration');
  const names = new Map<bigint, Set<string>>();
  let previousParticipant = 0n;
  let previousName = '';
  for (let index = 0; index < count; index++) {
    const participantId = reader.nonzeroU64('timer participant id');
    const name = reader.text8();
    reader.text8();
    reader.nonzeroU64('timer period');
    const policy = reader.u8();
    reader.nonzeroU64('timer catch-up bound');
    reader.bool();
    reader.u64();
    reader.u64();
    reader.u64();
    if (
      !participants.has(participantId)
      || participantId < previousParticipant
      || (participantId === previousParticipant && Buffer.compare(
        Buffer.from(name), Buffer.from(previousName)
      ) <= 0)
      || policy < 1 || policy > 3
    ) invalid('timer registration order');
    const participantNames = names.get(participantId) ?? new Set<string>();
    participantNames.add(name);
    names.set(participantId, participantNames);
    previousParticipant = participantId;
    previousName = name;
  }
  return names;
}

function readPendingTimerTicks(
  reader: Reader,
  participants: ReadonlySet<bigint>,
  timerNames: ReadonlyMap<bigint, ReadonlySet<string>>
): void {
  const count = reader.count(MAX_RECORDS, 'pending timer tick');
  let previousParticipant = 0n;
  let previousSequence = 0n;
  for (let index = 0; index < count; index++) {
    const participantId = reader.nonzeroU64('pending timer participant id');
    const sequence = reader.nonzeroU64('pending timer sequence');
    const name = reader.text8();
    reader.nonzeroU64('timer delivery index');
    reader.nonzeroU64('timer scheduled index');
    reader.u64();
    reader.u64();
    if (
      !participants.has(participantId)
      || timerNames.get(participantId)?.has(name) !== true
      || participantId < previousParticipant
      || (participantId === previousParticipant && sequence <= previousSequence)
    ) invalid('pending timer tick order');
    previousParticipant = participantId;
    previousSequence = sequence;
  }
}

function readTerminalCompletions(
  reader: Reader,
  participants: ReadonlySet<bigint>
): readonly ServiceWireRequestCompletion[] {
  const count = reader.count(MAX_RECORDS, 'terminal completion');
  const values: ServiceWireRequestCompletion[] = [];
  let previousParticipant = 0n;
  let previousSequence = 0n;
  const operations = new Set<string>();
  for (let index = 0; index < count; index++) {
    const operationHigh = reader.u64();
    const operationLow = reader.u64();
    const sourceOwnerId = reader.text8();
    const sourceOwnerLeaseGeneration = reader.nonzeroU64('request source lease');
    const sourceNodeRid = reader.text8();
    const sourceNodeGeneration = reader.nonzeroU64('request source node generation');
    const participantId = reader.nonzeroU64('completion participant id');
    const sequence = reader.nonzeroU64('completion sequence');
    const result = terminalResult(reader.u32());
    const failureCode = reader.u32();
    const deliveryState = reader.u8();
    if (deliveryState > 3) invalid('completion delivery state');
    const hasPayload = reader.bool();
    const payload = hasPayload ? readApplicationPayload(reader) : undefined;
    const operationKey = [sourceOwnerId, sourceOwnerLeaseGeneration, sourceNodeRid,
      sourceNodeGeneration, operationHigh, operationLow].join('\u0000');
    if (
      !participants.has(participantId)
      || participantId < previousParticipant
      || (participantId === previousParticipant && sequence <= previousSequence)
      || operations.has(operationKey)
    ) invalid('terminal completion order');
    operations.add(operationKey);
    previousParticipant = participantId;
    previousSequence = sequence;
    values.push({
      operationHigh,
      operationLow,
      sourceOwnerId,
      sourceOwnerLeaseGeneration,
      sourceNodeRid,
      sourceNodeGeneration,
      participantId,
      sequence,
      terminalResult: result,
      failureCode,
      deliveryState,
      ...(payload === undefined ? {} : { payload })
    });
  }
  return values;
}

function readApplicationPayload(reader: Reader): {
  readonly packetName: string;
  readonly contentType: string;
  readonly bytes: Uint8Array;
} {
  if (reader.u8() !== 1) invalid('application payload version');
  const body = reader.body32();
  const packetName = body.text8();
  const contentType = body.text8();
  const bytes = body.bytes32();
  body.end('application payload');
  return { packetName, contentType, bytes };
}

function readMetadata(reader: Reader): void {
  if (reader.u8() !== 1) invalid('metadata version');
  const count = reader.u8();
  const keys = new Set<string>();
  for (let index = 0; index < count; index++) {
    const key = reader.text8();
    if (keys.has(key)) invalid('metadata key');
    keys.add(key);
    reader.text16();
  }
}

function readRelocationObject(reader: Reader): void {
  const kind = reader.u8();
  const body = reader.body16();
  if (kind === 1) {
    readActorRef(body);
    body.nonzeroU64('Actor owner generation');
  } else if (kind === 2) {
    readSpotRef(body);
    body.nonzeroU64('Spot owner generation');
  } else if (kind === 3) {
    body.text8();
    body.text8();
    body.nonzeroU64('Instance generation');
  } else invalid('relocation object kind');
  body.end('relocation object');
}

function readActorRef(reader: Reader): void {
  reader.text8();
  reader.nonzeroU64('Actor generation');
}

function readSpotRef(reader: Reader): void {
  reader.text8();
  reader.nonzeroU64('Spot generation');
}

function readSpotRouteFence(reader: Reader): void {
  readSpotRef(reader);
  reader.text8();
  reader.nonzeroU64('target node generation');
  reader.nonzeroU64('Spot authority owner generation');
  reader.nonzeroU64('Spot owner lease generation');
}

function readActorRouteFence(reader: Reader): void {
  readActorRef(reader);
  reader.text8();
  reader.nonzeroU64('target node generation');
  reader.nonzeroU64('Actor authority owner generation');
  reader.nonzeroU64('Actor owner lease generation');
}

function terminalResult(value: number): number {
  if (value !== 0 && (value < 101 || value > 113)) invalid('terminal result');
  return value;
}

class Reader {
  private offset = 0;

  constructor(private readonly source: Uint8Array) {}

  get position(): number {
    return this.offset;
  }

  copy(start: number): Buffer {
    if (start < 0 || start > this.offset) invalid('reader range');
    return Buffer.from(this.source.subarray(start, this.offset));
  }

  u8(): number {
    return this.take(1)[0]!;
  }

  bool(): boolean {
    const value = this.u8();
    if (value > 1) invalid('boolean');
    return value === 1;
  }

  u16(): number {
    return Buffer.from(this.take(2)).readUInt16BE();
  }

  u32(): number {
    return Buffer.from(this.take(4)).readUInt32BE();
  }

  u64(): bigint {
    return Buffer.from(this.take(8)).readBigUInt64BE();
  }

  i64(): bigint {
    return Buffer.from(this.take(8)).readBigInt64BE();
  }

  nonzeroU64(label: string): bigint {
    const value = this.u64();
    if (value === 0n) invalid(label);
    return value;
  }

  count(maximum: number, label: string): number {
    const value = this.u32();
    if (value > maximum) throw new RangeError(`Relocation ${label} count exceeds its bound.`);
    return value;
  }

  text8(): string {
    return this.text(this.u8());
  }

  text16(): string {
    return this.text(this.u16());
  }

  bytes32(): Buffer {
    return Buffer.from(this.take(this.u32()));
  }

  bytes64(): Buffer {
    const size = this.u64();
    if (size > BigInt(Number.MAX_SAFE_INTEGER)) invalid('byte length');
    return Buffer.from(this.take(Number(size)));
  }

  body16(): Reader {
    return new Reader(this.take(this.u16()));
  }

  body32(): Reader {
    return new Reader(this.take(this.u32()));
  }

  body64(): Reader {
    const size = this.u64();
    if (size > BigInt(Number.MAX_SAFE_INTEGER)) invalid('body length');
    return new Reader(this.take(Number(size)));
  }

  end(label: string): void {
    if (this.offset !== this.source.byteLength) {
      throw new TypeError(`Canonical ${label} contains trailing bytes.`);
    }
  }

  private text(size: number): string {
    if (size === 0) invalid('text');
    return new TextDecoder('utf-8', { fatal: true }).decode(this.take(size));
  }

  private take(size: number): Uint8Array {
    if (!Number.isSafeInteger(size) || size < 0 || size > this.source.byteLength - this.offset) {
      throw new TypeError('Canonical relocation envelope is truncated.');
    }
    const result = this.source.subarray(this.offset, this.offset + size);
    this.offset += size;
    return result;
  }
}

function invalid(field: string): never {
  throw new TypeError(`Invalid canonical relocation ${field}.`);
}
