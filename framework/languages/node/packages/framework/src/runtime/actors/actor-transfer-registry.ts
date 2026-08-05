import type {
  Type,
  ZLinkActor,
  ZLinkMessageSerializer,
} from '../../contracts';
import type { ZLinkActorRelocationAdapter } from '../../contracts/Configuration/ObjectRoles';
import type { ZLinkSpotNodeOptions } from '../../contracts/Configuration/RegistrationTypes';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkEncodedPayload, ZLinkMessage } from '../../contracts';
import { throwIfAborted } from '../abort';

export interface ZLinkActorTransferPayloadState {
  readonly adapterKey?: string;
  readonly state: ZLinkMessage;
}

export class ZLinkActorTransferRegistry {
  private readonly byKey = new Map<string, Type>();

  constructor(
    spotNodes: ReadonlyMap<string, ZLinkSpotNodeOptions>,
    private readonly providerResolver?: ZLinkProviderResolver,
    private readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>
  ) {
    for (const node of spotNodes.values()) {
      for (const [stableType, registration] of Object.entries(
        node.actorFactoryRegistrations ?? {}
      )) {
        if (registration.relocation.kind !== 'snapshot') continue;
        const adapterType = registration.relocation.adapterType;
        const current = this.byKey.get(stableType);
        if (current !== undefined && current !== adapterType) {
          throw new Error(
            `Actor relocation adapter for stable type '${stableType}' is registered more than once.`
          );
        }
        this.byKey.set(stableType, adapterType);
      }
    }
  }

  async transferOut(
    actor: ZLinkActor,
    actorType: string | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferPayloadState> {
    throwIfAborted(signal);
    const adapterType = actorType === undefined
      ? undefined
      : this.byKey.get(actorType);
    if (adapterType === undefined) {
      return { state: emptyTransferState(this.messageSerializers) };
    }
    const adapter = await this.createAdapter(adapterType) as ZLinkActorRelocationAdapter<ZLinkActor>;
    const state = await adapter.capture(
      actor,
      signal ?? new AbortController().signal
    );
    if (!(state instanceof Uint8Array)) {
      throw new Error(
        `Actor relocation adapter '${adapterType.name}' returned invalid state bytes.`
      );
    }
    return {
      adapterKey: actorType,
      state: ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(state))
    };
  }

  policy(actorType: string | undefined): 'recreate' | 'snapshot' {
    return actorType !== undefined && this.byKey.has(actorType) ? 'snapshot' : 'recreate';
  }

  async restore(
    adapterKey: string,
    actor: ZLinkActor,
    state: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const adapterType = this.byKey.get(adapterKey);
    if (adapterType === undefined) {
      throw new Error(
        `Actor relocation adapter for stable type '${adapterKey}' is not registered on the target node.`
      );
    }
    const adapter = await this.createAdapter(adapterType) as ZLinkActorRelocationAdapter<ZLinkActor>;
    await adapter.restore(
      actor,
      state.toEncodedPayload().toBytes(),
      signal ?? new AbortController().signal
    );
  }

  private async createAdapter(adapterType: Type): Promise<unknown> {
    const existing = this.providerResolver?.get?.(adapterType);
    if (existing !== undefined) {
      return existing;
    }
    const created = await this.providerResolver?.create?.(adapterType);
    if (created !== undefined) {
      return created;
    }
    return new (adapterType as new () => unknown)();
  }
}

function emptyTransferState(
  _serializers?: ReadonlyMap<string, ZLinkMessageSerializer>
): ZLinkMessage {
  return ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(Buffer.alloc(0)));
}
