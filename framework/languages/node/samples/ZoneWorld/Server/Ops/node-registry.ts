import { Injectable } from '@nestjs/common';
import type { NodeView, ReportNodeStatusMsg } from '../../Shared/contracts';
import { ZoneIds, ZoneWorldSpec } from '../../Shared/spec';

@Injectable()
class NodeRegistry {
  private readonly nodes = new Map<string, NodeView>();
  private readonly nodeByRoutingId = new Map<string, string>();
  private readonly routingIdByNode = new Map<string, string>();
  private readonly lastReportAt = new Map<string, number>();
  private liveRoutingIds = new Set<string>();

  report(message: ReportNodeStatusMsg, sourceNodeRid: string, now = Date.now()): NodeView {
    const previousRoutingId = this.routingIdByNode.get(message.nodeId);
    if (previousRoutingId !== undefined && previousRoutingId !== sourceNodeRid) {
      this.nodeByRoutingId.delete(previousRoutingId);
    }
    const previousNodeId = this.nodeByRoutingId.get(sourceNodeRid);
    if (previousNodeId !== undefined && previousNodeId !== message.nodeId) {
      this.routingIdByNode.delete(previousNodeId);
    }
    this.nodeByRoutingId.set(sourceNodeRid, message.nodeId);
    this.routingIdByNode.set(message.nodeId, sourceNodeRid);
    this.lastReportAt.set(message.nodeId, now);

    const registered = true;
    const view: NodeView = {
      nodeId: message.nodeId,
      registered,
      connected: this.liveRoutingIds.has(sourceNodeRid),
      maintenance: message.maintenance,
      zones: [...message.zones],
      playerCount: message.playerCount
    };
    this.nodes.set(message.nodeId, view);
    return view;
  }

  applyLiveRoutingIds(liveRoutingIds: ReadonlySet<string>): NodeView[] {
    this.liveRoutingIds = new Set(liveRoutingIds);
    const changed: NodeView[] = [];
    for (const [nodeId, current] of this.nodes) {
      const routingId = this.routingIdByNode.get(nodeId);
      const connected = routingId !== undefined && this.liveRoutingIds.has(routingId);
      const next = { ...current, connected };
      if (current.connected !== next.connected) {
        this.nodes.set(nodeId, next);
        changed.push(next);
      }
    }
    return changed;
  }

  expireReports(now = Date.now()): NodeView[] {
    const changed: NodeView[] = [];
    for (const [nodeId, current] of this.nodes) {
      const lastReportAt = this.lastReportAt.get(nodeId);
      if (!current.registered || lastReportAt === undefined
        || now - lastReportAt < ZoneWorldSpec.nodeStatusReportTtlMs) continue;
      const next = { ...current, registered: false };
      this.nodes.set(nodeId, next);
      changed.push(next);
    }
    return changed;
  }

  relocationPair(): {
    readonly sourceZoneId: string;
    readonly targetZoneId: string;
    readonly sourceOwnerNodeRid: string;
    readonly targetOwnerNodeRid: string;
  } | undefined {
    const sourceZoneId = ZoneIds.northWest;
    const source = this.snapshot().find((node) => node.registered && node.zones.includes(sourceZoneId));
    if (source === undefined) return undefined;
    for (const targetZoneId of [ZoneIds.northEast, ZoneIds.southWest]) {
      const target = this.snapshot().find((node) => node.registered && node.zones.includes(targetZoneId));
      if (target === undefined || target.nodeId === source.nodeId) continue;
      const sourceOwnerNodeRid = this.routingIdByNode.get(source.nodeId);
      const targetOwnerNodeRid = this.routingIdByNode.get(target.nodeId);
      if (sourceOwnerNodeRid === undefined || targetOwnerNodeRid === undefined) continue;
      return { sourceZoneId, targetZoneId, sourceOwnerNodeRid, targetOwnerNodeRid };
    }
    return undefined;
  }

  snapshot(): NodeView[] {
    return [...this.nodes.values()]
      .sort((left, right) => Buffer.from(left.nodeId).compare(Buffer.from(right.nodeId)));
  }
}

export { NodeRegistry };
