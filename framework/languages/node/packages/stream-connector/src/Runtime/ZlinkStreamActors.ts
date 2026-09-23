import type {
  Disposable,
  ZlinkStreamActor,
  ZlinkStreamEncodedPayload,
  ZlinkStreamMessage,
  ZlinkStreamRequestCall,
  ZlinkStreamSendCall
} from '../Contracts';
import { ZlinkStreamErrorCode } from '../Contracts';
import { connectorError, subscription } from './ZlinkStreamSupport';
import type { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';

const ACTOR_BOUND = '$zlink.actor.bound';
const ACTOR_UNBOUND = '$zlink.actor.unbound';
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
  private readonly issued: DefaultZlinkStreamActor[] = [];
  private readonly boundHandlers = new Set<
    (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  >();
  private readonly unboundHandlers = new Set<
    (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  >();

  constructor(
    private readonly connector: ActorConnector,
    private readonly receivedMessages: ZlinkStreamReceivedMessages,
    private readonly events: ZlinkStreamConnectorEvents
  ) {}

  get snapshot(): readonly ZlinkStreamActor[] {
    return Object.freeze([...this.bySlot.values()]);
  }

  find(actorId: string): ZlinkStreamActor | undefined {
    return this.byId.get(actorId);
  }

  onBound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    this.boundHandlers.add(handler);
    return subscription(() => this.boundHandlers.delete(handler));
  }

  onUnbound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    this.unboundHandlers.add(handler);
    return subscription(() => this.unboundHandlers.delete(handler));
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
    for (const actor of this.issued) {
      if (!actor.isBound) continue;
      this.bySlot.delete(actor.slot);
      this.byId.delete(actor.actorId);
      actor.close();
      this.queue(this.unboundHandlers, actor, signal);
    }
    this.issued.length = 0;
  }

  private bind(payload: Uint8Array, signal?: AbortSignal): void {
    if (payload.length < 5 || payload[0] !== 1) {
      throw invalidControl('Actor bound payload is invalid.');
    }
    const slot = (payload[1] << 8) | payload[2];
    const idLength = payload[3];
    if (slot === 0 || idLength === 0 || payload.length !== 4 + idLength) {
      throw invalidControl('Actor bound payload is invalid.');
    }
    let actorId: string;
    try {
      actorId = new TextDecoder('utf-8', { fatal: true }).decode(payload.subarray(4));
    } catch (cause) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        'Actor id is not valid UTF-8.',
        cause
      );
    }
    if (actorId.length === 0 || this.bySlot.has(slot) || this.byId.has(actorId)) {
      throw invalidControl('Actor bound identity is already in use.');
    }
    const actor = new DefaultZlinkStreamActor(this.connector, actorId, slot);
    this.bySlot.set(slot, actor);
    this.byId.set(actorId, actor);
    this.issued.push(actor);
    this.queue(this.boundHandlers, actor, signal);
  }

  private unbind(payload: Uint8Array, signal?: AbortSignal): void {
    if (payload.length !== 3 || payload[0] !== 1) {
      throw invalidControl('Actor unbound payload is invalid.');
    }
    const slot = (payload[1] << 8) | payload[2];
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
    handlers: Set<(actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void>,
    actor: ZlinkStreamActor,
    signal?: AbortSignal
  ): void {
    this.receivedMessages.enqueueCallback(() => {
      for (const handler of [...handlers]) {
        this.invoke(handler, actor, signal);
      }
    });
  }

  private invoke(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void,
    actor: ZlinkStreamActor,
    signal?: AbortSignal
  ): void {
    try {
      Promise.resolve(handler(actor, signal)).catch((cause) => {
        void this.events.publishError(
          {
            code: ZlinkStreamErrorCode.UserCallbackFailed,
            message: 'Actor lifecycle handler failed.',
            cause
          },
          signal
        );
      });
    } catch (cause) {
      void this.events.publishError(
        {
          code: ZlinkStreamErrorCode.UserCallbackFailed,
          message: 'Actor lifecycle handler failed.',
          cause
        },
        signal
      );
    }
  }
}

function invalidControl(message: string): Error {
  return connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, message);
}
