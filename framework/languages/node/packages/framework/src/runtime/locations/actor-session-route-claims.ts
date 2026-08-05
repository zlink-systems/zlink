import type { RoutingId } from '../../contracts/Common';
import {
  ZLinkLocationKind,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus,
  ZLinkRouteKind,
  type ZLinkRouteLocation
} from './internal-location-contracts';
import { ZLinkLocationKeyCodec } from './key-codec';
import type {
  IZLinkLocationLifecycleRuntime,
  ZLinkOwnershipLostEvent
} from './lifecycle-runtime';

const encodeRoutingIdHex = ZLinkLocationKeyCodec.encodeRoutingIdHex;

export class ZLinkActorSessionRouteClaims {
  private readonly routes = new Map<string, bigint>();

  constructor(private readonly runtime: IZLinkLocationLifecycleRuntime) {}

  async bind(sessionRid: RoutingId, actorId: string, ownerNodeRid: RoutingId): Promise<void> {
    const routeKey = encodeRoutingIdHex(sessionRid);
    const row: ZLinkRouteLocation = {
      routeKind: ZLinkRouteKind.ActorSession,
      routeKey,
      ownerNodeRid,
      ownerId: '',
      generation: 0n,
      value: Buffer.from(actorId, 'utf8'),
      updatedAt: new Date(0)
    };
    let result = await this.runtime.writeRoute(row, ZLinkLocationWriteIntent.NewClaim);
    if (result.status === ZLinkLocationWriteStatus.RejectedConflict) {
      result = await this.runtime.writeRoute(row, ZLinkLocationWriteIntent.Takeover);
    }
    if (result.status === ZLinkLocationWriteStatus.Stored) {
      this.routes.set(
        ZLinkLocationKeyCodec.encodeRouteKey({ routeKind: ZLinkRouteKind.ActorSession, routeKey }),
        result.generation
      );
    }
  }

  async remove(sessionRid: RoutingId): Promise<void> {
    const key = {
      routeKind: ZLinkRouteKind.ActorSession,
      routeKey: encodeRoutingIdHex(sessionRid)
    };
    const canonical = ZLinkLocationKeyCodec.encodeRouteKey(key);
    const generation = this.routes.get(canonical);
    if (generation === undefined) {
      return;
    }
    this.routes.delete(canonical);
    await this.runtime.removeRoute(key, generation);
  }

  onOwnershipLost(event: ZLinkOwnershipLostEvent): void {
    if (event.kind === ZLinkLocationKind.Route) {
      this.routes.delete(event.key);
    }
  }
}
