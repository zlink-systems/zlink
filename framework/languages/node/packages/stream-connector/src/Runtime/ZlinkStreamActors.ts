import {
  ZlinkStreamControlPacket,
  decodeStreamWireActorBoundPayload,
  decodeStreamWireActorUnboundPayload
} from '@zlink-systems/stream-wire';
import {
  type Disposable,
  type ZlinkStreamActor,
  type ZlinkStreamEncodedPayload,
  type ZlinkStreamMessage,
  type ZlinkStreamRequestCall,
  type ZlinkStreamSendCall,
  ZlinkStreamErrorCode
} from '../Contracts';

import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import type { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import {
  connectorError,
  currentRegistrations,
  registerHandler,
  type HandlerRegistration
} from './ZlinkStreamSupport';

const ACTOR_BOUND = ZlinkStreamControlPacket.ActorBound;
const ACTOR_UNBOUND = ZlinkStreamControlPacket.ActorUnbound;
export const zlinkStreamActorBinding = Symbol('zlink.stream.actorBinding');

interface ActorConnector {
  sendForActor(
    actor: DefaultZlinkStreamActor,
    payload: unknown,
    messageType?: Function
  ): ZlinkStreamSendCall;
  requestForActor(
    actor: DefaultZlinkStreamActor,
    payload: unknown,
    messageType?: Function
  ): ZlinkStreamRequestCall;
  onActorMessage<TPayload>(
    actor: DefaultZlinkStreamActor,
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;
}

export class DefaultZlinkStreamActor implements ZlinkStreamActor {
  private bound = true;

  constructor(
    private readonly connector: ActorConnector,
    readonly actorId: string,
    readonly slot: number
  ) {}

  get isBound(): boolean {
    return this.bound;
  }

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall {
    this.ensureBound();
    return this.connector.sendForActor(this, payload, messageType);
  }

  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall {
    this.ensureBound();
    return this.connector.requestForActor(this, payload, messageType);
  }

  on<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable {
    return this.connector.onActorMessage(this, nameOrType, handler, messageType);
  }

  ensureBound(): void {
    if (!this.bound) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        `Actor '${this.actorId}' is no longer bound.`
      );
    }
  }

  close(): void {
    this.bound = false;
  }
}

export class ZlinkStreamActors {
  private readonly bySlot = new Map<number, DefaultZlinkStreamActor>();
  private readonly byId = new Map<string, DefaultZlinkStreamActor>();
  private readonly boundHandlers = new Set<
    HandlerRegistration<(actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void>
  >();
  private readonly unboundHandlers = new Set<
    HandlerRegistration<(actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void>
  >();

  constructor(
    private readonly connector: ActorConnector,
    private readonly receivedMessages: ZlinkStreamReceivedMessages,
    private readonly events: ZlinkStreamConnectorEvents
  ) {}

  get snapshot(): readonly ZlinkStreamActor[] {
    return Object.freeze(Array.from(this.bySlot.values()));
  }

  find(actorId: string): ZlinkStreamActor | undefined {
    return this.byId.get(actorId);
  }

  onBound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    return registerHandler(this.boundHandlers, handler);
  }

  onUnbound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    return registerHandler(this.unboundHandlers, handler);
  }

  processControl(name: string, payload: Uint8Array, signal?: AbortSignal): boolean {
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

  resolve(slot: number): DefaultZlinkStreamActor {
    const actor = this.bySlot.get(slot);
    if (actor === undefined) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        `Actor slot '${slot}' is not bound.`
      );
    }
    return actor;
  }

  closeAll(signal?: AbortSignal): void {
    for (const actor of this.bySlot.values()) {
      this.bySlot.delete(actor.slot);
      this.byId.delete(actor.actorId);
      actor.close();
      this.queue(this.unboundHandlers, actor, signal);
    }
  }

  private bind(payload: Uint8Array, signal?: AbortSignal): void {
    let binding: ReturnType<typeof decodeStreamWireActorBoundPayload>;
    try {
      binding = decodeStreamWireActorBoundPayload(payload);
    } catch (cause) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        cause instanceof Error ? cause.message : 'Actor bound payload is invalid.',
        cause instanceof Error ? cause.cause : undefined
      );
    }
    const slot = binding.slot;
    const actorId = binding.actorId;
    if (actorId.length === 0 || this.bySlot.has(slot) || this.byId.has(actorId)) {
      throw invalidControl('Actor bound identity is already in use.');
    }
    const actor = new DefaultZlinkStreamActor(this.connector, actorId, slot);
    this.bySlot.set(slot, actor);
    this.byId.set(actorId, actor);
    this.queue(this.boundHandlers, actor, signal);
  }

  private unbind(payload: Uint8Array, signal?: AbortSignal): void {
    let slot: number;
    try {
      slot = decodeStreamWireActorUnboundPayload(payload);
    } catch (cause) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        cause instanceof Error ? cause.message : 'Actor unbound payload is invalid.'
      );
    }
    const actor = this.bySlot.get(slot);
    if (slot === 0 || actor === undefined) {
      throw invalidControl(`Actor slot '${slot}' is not bound.`);
    }
    this.bySlot.delete(slot);
    this.byId.delete(actor.actorId);
    actor.close();
    this.queue(this.unboundHandlers, actor, signal);
  }

  private queue(
    handlers: Set<
      HandlerRegistration<(actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void>
    >,
    actor: ZlinkStreamActor,
    signal?: AbortSignal
  ): void {
    this.receivedMessages.enqueueCallback(
      () => {
        for (const { handler } of currentRegistrations(handlers)) {
          this.events.runUserCallback(
            () => handler(actor, signal),
            'Actor lifecycle handler failed.',
            signal
          );
        }
      },
      () => handlers.size
    );
  }
}

function invalidControl(message: string): Error {
  return connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, message);
}
