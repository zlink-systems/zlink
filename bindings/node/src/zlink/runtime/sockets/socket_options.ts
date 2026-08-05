// SPDX-License-Identifier: MPL-2.0

import { Message, RoutingId, type MessageLike } from '../../contracts';
import {
  RidDuplicatePolicy,
  type PollEventFlagValue,
  type RidDuplicatePolicy as RidDuplicatePolicyValue
} from '../../contracts/sockets/socket_constants';
import { normalizeMessageLikePayload } from '../buffers/message_conversion';
import { normalizeRoutingId } from '../core/routing_id';
import { SocketBase } from './socket_base';
import { SocketOption } from '../options/option_mapping';
import { readUInt64Option, uint64Buffer } from '../options/byte_values';

export function int32Buffer(value: number, name: string): Buffer {
  if (!Number.isInteger(value)) throw new TypeError(`${name} must be an integer`);
  if (value < -2147483648 || value > 2147483647) {
    throw new RangeError(`${name} must fit in int32`);
  }
  const buf = Buffer.allocUnsafe(4);
  buf.writeInt32LE(value, 0);
  return buf;
}

export function int64Buffer(value: bigint, name: string): Buffer {
  if (typeof value !== 'bigint') {
    throw new TypeError(`${name} must be a bigint`);
  }
  const normalized = value;
  const min = -(1n << 63n);
  const max = (1n << 63n) - 1n;
  if (normalized < min || normalized > max) {
    throw new RangeError(`${name} must fit in int64`);
  }
  const buf = Buffer.allocUnsafe(8);
  buf.writeBigInt64LE(normalized, 0);
  return buf;
}

export function boolBuffer(value: boolean): Buffer {
  const buf = Buffer.allocUnsafe(4);
  buf.writeUInt32LE(value ? 1 : 0, 0);
  return buf;
}

export function flagsToMask(events: readonly PollEventFlagValue[]): number {
  if (!Array.isArray(events)) {
    throw new TypeError('events must be an array');
  }
  return events.reduce((mask, event) => mask | (event | 0), 0);
}

export function readBoolOption(buffer: Buffer, name: string): boolean {
  if (buffer.length < 4) throw new Error(`${name} option returned an invalid payload`);
  return buffer.readUInt32LE(0) !== 0;
}

export function readInt32Option(buffer: Buffer, name: string): number {
  if (buffer.length < 4) throw new Error(`${name} option returned an invalid payload`);
  return buffer.readInt32LE(0);
}

export function readInt64Option(buffer: Buffer, name: string): bigint {
  if (buffer.length < 8) throw new Error(`${name} option returned an invalid payload`);
  return buffer.readBigInt64LE(0);
}

export function readRoutingIdOption(buffer: Buffer): RoutingId | null {
  return buffer.length === 0 ? null : RoutingId.from(buffer);
}

export function readStringOption(buffer: Buffer): string {
  const nul = buffer.indexOf(0);
  return buffer.subarray(0, nul >= 0 ? nul : buffer.length).toString();
}

export const OPTION_CREATE_TOKEN = Symbol('OptionFacade.create');

export class CommonSocketOptions {
  /** @internal */
  protected readonly _socket: SocketBase;

  /** @internal */
  protected constructor(token: symbol, socket: SocketBase) {
    if (token !== OPTION_CREATE_TOKEN) {
      throw new TypeError('socket options are created by sockets');
    }
    this._socket = socket;
  }

  /** @internal */
  static create(socket: SocketBase): CommonSocketOptions {
    return new CommonSocketOptions(OPTION_CREATE_TOKEN, socket);
  }

  protected readInt32(option: number, name: string): number {
    return readInt32Option(this._socket.getSockOptRaw(option), name);
  }

  protected writeInt32(option: number, value: number, name: string): void {
    this._socket.setSockOptRaw(option, int32Buffer(value, name));
  }

  protected readBool(option: number, name: string): boolean {
    return readBoolOption(this._socket.getSockOptRaw(option), name);
  }

  protected writeBool(option: number, value: boolean): void {
    this._socket.setSockOptRaw(option, boolBuffer(value));
  }

  get linger(): number { return this.readInt32(SocketOption.LINGER, 'linger'); }
  set linger(value: number) { this.writeInt32(SocketOption.LINGER, value, 'linger'); }
  get sendHwm(): bigint { return readUInt64Option(this._socket.getSockOptRaw(SocketOption.SNDHWM), 'sendHwm'); }
  set sendHwm(value: bigint) { this._socket.setSockOptRaw(SocketOption.SNDHWM, uint64Buffer(value, 'sendHwm')); }
  get recvHwm(): bigint { return readUInt64Option(this._socket.getSockOptRaw(SocketOption.RCVHWM), 'recvHwm'); }
  set recvHwm(value: bigint) { this._socket.setSockOptRaw(SocketOption.RCVHWM, uint64Buffer(value, 'recvHwm')); }
  get sendTimeout(): number { return this.readInt32(SocketOption.SNDTIMEO, 'sendTimeout'); }
  set sendTimeout(value: number) { this.writeInt32(SocketOption.SNDTIMEO, value, 'sendTimeout'); }
  get recvTimeout(): number { return this.readInt32(SocketOption.RCVTIMEO, 'recvTimeout'); }
  set recvTimeout(value: number) { this.writeInt32(SocketOption.RCVTIMEO, value, 'recvTimeout'); }
  get immediate(): boolean { return this.readBool(SocketOption.IMMEDIATE, 'immediate'); }
  set immediate(value: boolean) { this.writeBool(SocketOption.IMMEDIATE, value); }
  get ridDuplicatePolicy(): RidDuplicatePolicyValue { return this.readInt32(SocketOption.RID_DUPLICATE_POLICY, 'ridDuplicatePolicy') as RidDuplicatePolicyValue; }
  set ridDuplicatePolicy(value: RidDuplicatePolicyValue) { this.writeInt32(SocketOption.RID_DUPLICATE_POLICY, value, 'ridDuplicatePolicy'); }
  get connectTimeout(): number { return this.readInt32(SocketOption.CONNECT_TIMEOUT, 'connectTimeout'); }
  set connectTimeout(value: number) { this.writeInt32(SocketOption.CONNECT_TIMEOUT, value, 'connectTimeout'); }
  get ipv6(): boolean { return this.readBool(SocketOption.IPV6, 'ipv6'); }
  set ipv6(value: boolean) { this.writeBool(SocketOption.IPV6, value); }
  get tcpNoDelay(): boolean { return this.readBool(SocketOption.TCP_NODELAY, 'tcpNoDelay'); }
  set tcpNoDelay(value: boolean) { this.writeBool(SocketOption.TCP_NODELAY, value); }
  get tcpKeepalive(): number { return this.readInt32(SocketOption.TCP_KEEPALIVE, 'tcpKeepalive'); }
  set tcpKeepalive(value: number) { this.writeInt32(SocketOption.TCP_KEEPALIVE, value, 'tcpKeepalive'); }
  get maxMsgSize(): bigint { return readInt64Option(this._socket.getSockOptRaw(SocketOption.MAXMSGSIZE), 'maxMsgSize'); }
  set maxMsgSize(value: bigint) { this._socket.setSockOptRaw(SocketOption.MAXMSGSIZE, int64Buffer(value, 'maxMsgSize')); }
  get lastEndpoint(): string { return readStringOption(this._socket.getSockOptRaw(SocketOption.LAST_ENDPOINT)); }
  get backlog(): number { return this.readInt32(SocketOption.BACKLOG, 'backlog'); }
  set backlog(value: number) { this.writeInt32(SocketOption.BACKLOG, value, 'backlog'); }
  get reconnectInterval(): number { return this.readInt32(SocketOption.RECONNECT_IVL, 'reconnectInterval'); }
  set reconnectInterval(value: number) { this.writeInt32(SocketOption.RECONNECT_IVL, value, 'reconnectInterval'); }
  get reconnectIntervalMax(): number { return this.readInt32(SocketOption.RECONNECT_IVL_MAX, 'reconnectIntervalMax'); }
  set reconnectIntervalMax(value: number) { this.writeInt32(SocketOption.RECONNECT_IVL_MAX, value, 'reconnectIntervalMax'); }
  get submitRetryMode(): number { return this.readInt32(SocketOption.SUBMIT_RETRY_MODE, 'submitRetryMode'); }
  set submitRetryMode(value: number) { this.writeInt32(SocketOption.SUBMIT_RETRY_MODE, value, 'submitRetryMode'); }
  get submitRetryTimeout(): number { return this.readInt32(SocketOption.SUBMIT_RETRY_TIMEOUT, 'submitRetryTimeout'); }
  set submitRetryTimeout(value: number) { this.writeInt32(SocketOption.SUBMIT_RETRY_TIMEOUT, value, 'submitRetryTimeout'); }
  get submitRetryAttempts(): number { return this.readInt32(SocketOption.SUBMIT_RETRY_ATTEMPTS, 'submitRetryAttempts'); }
  set submitRetryAttempts(value: number) { this.writeInt32(SocketOption.SUBMIT_RETRY_ATTEMPTS, value, 'submitRetryAttempts'); }
}

export class DealerSocketOptions extends CommonSocketOptions {
  /** @internal */
  private constructor(token: symbol, socket: SocketBase) { super(token, socket); }
  /** @internal */
  static create(socket: SocketBase): DealerSocketOptions {
    return new DealerSocketOptions(OPTION_CREATE_TOKEN, socket);
  }
  get probe(): boolean { return this.readBool(SocketOption.DEALER_PROBE, 'probe'); }
  set probe(value: boolean) { this.writeBool(SocketOption.DEALER_PROBE, value); }
  get requestTimeout(): number { return this.readInt32(SocketOption.DEALER_REQUEST_TIMEOUT_MS, 'requestTimeout'); }
  set requestTimeout(value: number) { this.writeInt32(SocketOption.DEALER_REQUEST_TIMEOUT_MS, value, 'requestTimeout'); }
  get peerWeight(): number { return this.readInt32(SocketOption.DEALER_WEIGHT, 'peerWeight'); }
  set peerWeight(value: number) { this.writeInt32(SocketOption.DEALER_WEIGHT, value, 'peerWeight'); }
}

export class RouterSocketOptions extends CommonSocketOptions {
  /** @internal */
  private constructor(token: symbol, socket: SocketBase) { super(token, socket); }
  /** @internal */
  static create(socket: SocketBase): RouterSocketOptions {
    return new RouterSocketOptions(OPTION_CREATE_TOKEN, socket);
  }
  get mandatory(): boolean { return this.readBool(SocketOption.ROUTER_MANDATORY, 'mandatory'); }
  set mandatory(value: boolean) { this.writeBool(SocketOption.ROUTER_MANDATORY, value); }
  get handover(): boolean { return this.ridDuplicatePolicy === RidDuplicatePolicy.Handover; }
  set handover(value: boolean) { this.ridDuplicatePolicy = value ? RidDuplicatePolicy.Handover : RidDuplicatePolicy.Reject; }
  get probe(): boolean { return this.readBool(SocketOption.PROBE_ROUTER, 'probe'); }
  set probe(value: boolean) { this.writeBool(SocketOption.PROBE_ROUTER, value); }
  get connectRoutingId(): RoutingId | null { return readRoutingIdOption(this._socket.getSockOptRaw(SocketOption.CONNECT_ROUTING_ID)); }
  setConnectRoutingId(routingId: RoutingId): void {
    this._socket.setSockOptRaw(
      SocketOption.CONNECT_ROUTING_ID,
      normalizeRoutingId(routingId, 'routingId')
    );
  }
  get requestTimeout(): number { return this.readInt32(SocketOption.ROUTER_REQUEST_TIMEOUT_MS, 'requestTimeout'); }
  set requestTimeout(value: number) { this.writeInt32(SocketOption.ROUTER_REQUEST_TIMEOUT_MS, value, 'requestTimeout'); }
  get peerWeight(): number { return this.readInt32(SocketOption.ROUTER_WEIGHT, 'peerWeight'); }
  set peerWeight(value: number) { this.writeInt32(SocketOption.ROUTER_WEIGHT, value, 'peerWeight'); }
}

export class StreamSocketOptions extends CommonSocketOptions {
  /** @internal */
  private constructor(token: symbol, socket: SocketBase) { super(token, socket); }
  /** @internal */
  static create(socket: SocketBase): StreamSocketOptions {
    return new StreamSocketOptions(OPTION_CREATE_TOKEN, socket);
  }
  get notify(): boolean { return this.readBool(SocketOption.STREAM_NOTIFY, 'notify'); }
  set notify(value: boolean) { this.writeBool(SocketOption.STREAM_NOTIFY, value); }
}

export class PubSocketOptions extends CommonSocketOptions {
  private _welcomeMessage = Buffer.alloc(0);

  /** @internal */
  private constructor(token: symbol, socket: SocketBase) { super(token, socket); }
  /** @internal */
  static create(socket: SocketBase): PubSocketOptions {
    return new PubSocketOptions(OPTION_CREATE_TOKEN, socket);
  }
  get verbose(): boolean { return this.readBool(SocketOption.XPUB_VERBOSE, 'verbose'); }
  set verbose(value: boolean) {
    this.writeBool(SocketOption.XPUB_VERBOSE, value);
    this.writeBool(SocketOption.XPUB_VERBOSER, false);
  }
  get verboser(): boolean { return this.readBool(SocketOption.XPUB_VERBOSER, 'verboser'); }
  set verboser(value: boolean) {
    this.writeBool(SocketOption.XPUB_VERBOSE, value);
    this.writeBool(SocketOption.XPUB_VERBOSER, value);
  }
  get noDrop(): boolean { return this.readBool(SocketOption.XPUB_NODROP, 'noDrop'); }
  set noDrop(value: boolean) {
    this.writeBool(SocketOption.XPUB_NODROP, value);
  }
  get manual(): boolean { return this.readBool(SocketOption.XPUB_MANUAL, 'manual'); }
  set manual(value: boolean) {
    this.writeBool(SocketOption.XPUB_MANUAL, value);
  }
  get manualLastValue(): boolean { return this.readBool(SocketOption.XPUB_MANUAL_LAST_VALUE, 'manualLastValue'); }
  set manualLastValue(value: boolean) {
    this.writeBool(SocketOption.XPUB_MANUAL, value);
    this.writeBool(SocketOption.XPUB_MANUAL_LAST_VALUE, value);
  }
  get topicsCount(): number { return this.readInt32(SocketOption.XPUB_TOPICS_COUNT, 'topicsCount'); }
  welcomeMessage(): Message { return Message.from(this._welcomeMessage); }
  setWelcomeMessage(message: MessageLike): void {
    const payload = normalizeMessageLikePayload(message);
    if (Array.isArray(payload)) throw new TypeError('welcome payload must contain one frame');
    const data = Buffer.isBuffer(payload) ? payload : payload.data;
    this._socket.setSockOptRaw(SocketOption.XPUB_WELCOME_MSG, data);
    this._welcomeMessage = Buffer.from(data);
  }
  approveSubscribe(routingId: RoutingId): void {
    this._socket.setSockOptRaw(SocketOption.XPUB_APPROVE_SUBSCRIBE, normalizeRoutingId(routingId));
  }
  rejectSubscribe(routingId: RoutingId): void {
    this._socket.setSockOptRaw(SocketOption.XPUB_REJECT_SUBSCRIBE, normalizeRoutingId(routingId));
  }
}

export class SubSocketOptions extends CommonSocketOptions {
  /** @internal */
  private constructor(token: symbol, socket: SocketBase) { super(token, socket); }
  /** @internal */
  static create(socket: SocketBase): SubSocketOptions {
    return new SubSocketOptions(OPTION_CREATE_TOKEN, socket);
  }
  get topicsCount(): number { return this.readInt32(SocketOption.SUB_TOPICS_COUNT, 'topicsCount'); }
}
