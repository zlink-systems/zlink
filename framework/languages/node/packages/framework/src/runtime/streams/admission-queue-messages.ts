import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';
import { ZLinkPacket } from '../../contracts/Handlers/Attributes';
import { admissionTicketEncodedSize } from './admission-tickets';

export const enqueuePacketName = '$zlink.admission.enqueue';
export const positionPacketName = '$zlink.admission.position';
export const ticketPacketName = '$zlink.admission.ticket';
export const queueRequiredMetadata = { '$zlink.admission.queueRequired': '1' } as const;

export interface AdmissionNodeIdentity {
  readonly nodeRid: string;
  readonly nodeLifecycleGeneration: string;
}

export interface AdmissionSessionIdentity {
  readonly streamName: string;
  readonly sessionRid: string;
}

export interface AdmissionConnection extends AdmissionNodeIdentity, AdmissionSessionIdentity {}

export interface AdmissionOwner {
  readonly ownerId: string;
  readonly leaseGeneration: string;
}

export interface AdmissionPosition {
  readonly position: number;
  readonly estimatedWaitMs: number;
}

export interface AdmissionTicket {
  readonly ticket: string;
  readonly expiresInMs: number;
}

export type AdmissionUpdate = AdmissionPosition | AdmissionTicket;

export type AdmissionDelivery = AdmissionSessionIdentity &
  (
    | ({ readonly kind: 'position' } & AdmissionPosition)
    | ({ readonly kind: 'ticket' } & AdmissionTicket)
  );

export interface AdmissionEnqueueReply {
  readonly epoch: string;
  readonly delivery: AdmissionDelivery;
}

export interface AdmissionReport extends AdmissionNodeIdentity {
  readonly limit: number;
  readonly inFlight: number;
  readonly usedTickets: readonly AdmissionSessionIdentity[];
  readonly endedSessions: readonly AdmissionSessionIdentity[];
  readonly heldPermits: readonly string[];
  readonly resolvedRequestSeq: string;
}

export interface AdmissionReportReply {
  readonly epoch: string;
  readonly deliveries: readonly AdmissionDelivery[];
}

export interface AdmitPermitRequest extends AdmissionConnection, AdmissionOwner {
  readonly epoch: string;
  readonly requestSeq: string;
}

export type AdmitPermitReply =
  | { readonly granted: true; readonly permitId: string }
  | { readonly granted: false; readonly permitId?: string };

export interface ReleasePermitMessage extends AdmissionNodeIdentity {
  readonly permitId: string;
}

export const admissionEnqueueName = '$zlink.admission-queue.enqueue';
export const admissionNodeReportName = '$zlink.admission-queue.report';
export const admissionAdmitPermitName = '$zlink.admission-queue.admit-permit';
export const admissionReleasePermitName = '$zlink.admission-queue.release-permit';

const replyNames = new Map(
  [admissionEnqueueName, admissionNodeReportName, admissionAdmitPermitName].map(
    (name): [string, string] => [name, `${name}.reply`]
  )
);
const packetNames = new Set([
  ...replyNames.keys(),
  ...replyNames.values(),
  admissionReleasePermitName
]);

export function admissionQueueReplyName(requestName: string | undefined): string | undefined {
  return requestName === undefined ? undefined : replyNames.get(requestName);
}

export function isAdmissionQueuePacket(packetName: string | undefined): boolean {
  return packetName !== undefined && packetNames.has(packetName);
}

@ZLinkPacket(admissionEnqueueName)
export class AdmissionEnqueue {
  readonly connection: AdmissionConnection;

  constructor({ connection }: { readonly connection: AdmissionConnection }) {
    this.connection = connection;
  }
}

@ZLinkPacket(admissionNodeReportName)
export class AdmissionNodeReport {
  constructor(report: AdmissionReport) {
    Object.assign(this, report);
  }
}

@ZLinkPacket(admissionAdmitPermitName)
export class AdmissionAdmitPermit {
  constructor(request: AdmitPermitRequest) {
    Object.assign(this, request);
  }
}

@ZLinkPacket(admissionReleasePermitName)
export class AdmissionReleasePermit {
  constructor(release: ReleasePermitMessage) {
    Object.assign(this, release);
  }
}

export function admissionNodeKey(node: AdmissionNodeIdentity): string {
  return `${node.nodeRid}/${node.nodeLifecycleGeneration}`;
}

export function admissionConnectionKey(connection: AdmissionConnection): string {
  return JSON.stringify([
    connection.nodeRid,
    connection.nodeLifecycleGeneration,
    connection.streamName,
    connection.sessionRid
  ]);
}

export function admissionSessionIdentity({
  streamName,
  sessionRid
}: AdmissionSessionIdentity): AdmissionSessionIdentity {
  return { streamName, sessionRid };
}

export function admissionUpdate(delivery: AdmissionDelivery): AdmissionUpdate {
  if (delivery.kind === 'ticket') {
    const { ticket, expiresInMs } = delivery;
    return { ticket, expiresInMs };
  }
  const { position, estimatedWaitMs } = delivery;
  return { position, estimatedWaitMs };
}

export function readEnqueue(value: unknown): AdmissionEnqueue {
  return { connection: readConnection(record(value).connection) };
}

export function readEnqueueReply(value: unknown): AdmissionEnqueueReply {
  const { epoch, delivery } = record(value);
  return { epoch: decimal(epoch, 'epoch'), delivery: readDelivery(delivery) };
}

export function readReport(value: unknown): AdmissionReport {
  const report = record(value);
  return {
    ...readNode(report),
    limit: int32(report.limit, 'limit'),
    inFlight: int32(report.inFlight, 'inFlight'),
    usedTickets: array(report.usedTickets, 'usedTickets').map(readSession),
    endedSessions: array(report.endedSessions, 'endedSessions').map(readSession),
    heldPermits: array(report.heldPermits, 'heldPermits').map((id) => decimal(id, 'heldPermits')),
    resolvedRequestSeq: decimal(report.resolvedRequestSeq, 'resolvedRequestSeq')
  };
}

export function readReportReply(value: unknown): AdmissionReportReply {
  const { epoch, deliveries } = record(value);
  return {
    epoch: decimal(epoch, 'epoch'),
    deliveries: array(deliveries, 'deliveries').map(readDelivery)
  };
}

export function readAdmitPermit(value: unknown): AdmitPermitRequest {
  const request = record(value);
  return {
    ...readConnection(request),
    ownerId: string(request.ownerId, 'ownerId'),
    leaseGeneration: decimal(request.leaseGeneration, 'leaseGeneration'),
    epoch: decimal(request.epoch, 'epoch'),
    requestSeq: decimal(request.requestSeq, 'requestSeq')
  };
}

export function readAdmitPermitReply(value: unknown): AdmitPermitReply {
  const reply = record(value);
  if (reply.granted === true) {
    const permitId = decimal(reply.permitId, 'permitId');
    if (permitId === '0') throw protocolError('permitId');
    return { granted: true, permitId };
  }
  if (reply.granted !== false) throw protocolError('granted');
  return reply.permitId === undefined
    ? { granted: false }
    : { granted: false, permitId: decimal(reply.permitId, 'permitId') };
}

export function readReleasePermit(value: unknown): ReleasePermitMessage {
  const release = record(value);
  return { ...readNode(release), permitId: decimal(release.permitId, 'permitId') };
}

function readConnection(value: unknown): AdmissionConnection {
  const connection = record(value);
  return { ...readNode(connection), ...readSession(connection) };
}

function readNode(value: Record<string, unknown>): AdmissionNodeIdentity {
  return {
    nodeRid: base64url(value.nodeRid, 'nodeRid'),
    nodeLifecycleGeneration: decimal(value.nodeLifecycleGeneration, 'nodeLifecycleGeneration')
  };
}

function readSession(value: unknown): AdmissionSessionIdentity {
  const session = record(value);
  return {
    streamName: string(session.streamName, 'streamName'),
    sessionRid: base64url(session.sessionRid, 'sessionRid')
  };
}

function readDelivery(value: unknown): AdmissionDelivery {
  const delivery = record(value);
  const session = readSession(delivery);
  const { kind } = delivery;
  if (kind !== 'position' && kind !== 'ticket') throw protocolError('kind');
  const fields = {
    ...(kind === 'position' || delivery.position !== undefined
      ? { position: int32(delivery.position, 'position', 1) }
      : {}),
    ...(kind === 'position' || delivery.estimatedWaitMs !== undefined
      ? { estimatedWaitMs: int32(delivery.estimatedWaitMs, 'estimatedWaitMs') }
      : {}),
    ...(kind === 'ticket' || delivery.ticket !== undefined
      ? { ticket: base64url(delivery.ticket, 'ticket', admissionTicketEncodedSize) }
      : {}),
    ...(kind === 'ticket' || delivery.expiresInMs !== undefined
      ? { expiresInMs: int32(delivery.expiresInMs, 'expiresInMs') }
      : {})
  };
  return { ...session, kind, ...fields } as AdmissionDelivery;
}

function record(value: unknown): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw protocolError('object');
  }
  return value as Record<string, unknown>;
}

function string(value: unknown, field: string): string {
  if (typeof value !== 'string') throw protocolError(field);
  return value;
}

function array(value: unknown, field: string): unknown[] {
  if (!Array.isArray(value)) throw protocolError(field);
  return value;
}

function int32(value: unknown, field: string, min = -2_147_483_648): number {
  if (
    typeof value !== 'number' ||
    !Number.isInteger(value) ||
    value < min ||
    value > 2_147_483_647
  ) {
    throw protocolError(field);
  }
  return value;
}

function decimal(value: unknown, field: string): string {
  const text = string(value, field);
  if (!/^(?:0|[1-9][0-9]{0,19})$/u.test(text) || BigInt(text) > 0xffff_ffff_ffff_ffffn) {
    throw protocolError(field);
  }
  return text;
}

function base64url(value: unknown, field: string, length?: number): string {
  const text = string(value, field);
  if (
    text.length === 0 ||
    (length !== undefined && text.length !== length) ||
    Buffer.from(text, 'base64url').toString('base64url') !== text
  ) {
    throw protocolError(field);
  }
  return text;
}

function protocolError(field: string): ZLinkFrameworkException {
  return new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.ProtocolError,
    `Invalid admission queue protocol field '${field}'.`
  );
}
