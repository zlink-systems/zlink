import { Injectable } from '@nestjs/common';
import type { NodeView, ReportNodeStatusMsg } from '../../Shared/contracts';

@Injectable()
class NodeRegistry {
  private readonly nodes = new Map<string, NodeView>();
  private readonly nodeByRoutingId = new Map<string, string>();
  private readonly routingIdByNode = new Map<string, string>();
  private readonly transportConnected = new Map<string, boolean>();
  private liveRoutingIds = new Set<string>();

  report(message: ReportNodeStatusMsg, sourceNodeRid: string): NodeView {
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
    this.transportConnected.set(message.nodeId, true);

    const registered = this.liveRoutingIds.has(sourceNodeRid);
    const view: NodeView = {
      nodeId: message.nodeId,
      registered,
      connected: registered,
      maintenance: message.maintenance,
      zones: [...message.zones],
      playerCount: message.playerCount
    };
    this.nodes.set(message.nodeId, view);
    return view;
  }

  applyLiveRoutingIds(liveRoutingIds: ReadonlySet<string>): NodeView[] {
    const previousLiveRoutingIds = this.liveRoutingIds;
    this.liveRoutingIds = new Set(liveRoutingIds);
    const changed: NodeView[] = [];
    for (const [nodeId, current] of this.nodes) {
      const routingId = this.routingIdByNode.get(nodeId);
      const registered = routingId !== undefined && this.liveRoutingIds.has(routingId);
      if (routingId !== undefined
        && previousLiveRoutingIds.has(routingId) !== this.liveRoutingIds.has(routingId)) {
        this.transportConnected.set(nodeId, registered);
      }
      const connected = registered && this.transportConnected.get(nodeId) === true;
      const next = { ...current, registered, connected };
      if (current.registered !== next.registered || current.connected !== next.connected) {
        this.nodes.set(nodeId, next);
        changed.push(next);
      }
    }
    return changed;
  }

  snapshot(): NodeView[] {
    return [...this.nodes.values()]
      .sort((left, right) => Buffer.from(left.nodeId).compare(Buffer.from(right.nodeId)));
  }
}

export { NodeRegistry };
