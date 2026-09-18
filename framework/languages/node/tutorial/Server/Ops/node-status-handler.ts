import { Injectable } from '@nestjs/common';
import type { ZLinkRouteMessageContext, ZLinkRouteRequestHandler } from '@zlink-systems/framework';
import type { GetNodeStatus, NodeStatus } from '../../Shared/contracts';

// --8<-- [start:node-direct-handler]
// A node-direct handler, not a channel handler. It answers only when a caller
// names this node's routing id, so it reports on this one process.
@Injectable()
export class NodeStatusHandler implements ZLinkRouteRequestHandler<GetNodeStatus, NodeStatus> {
  async handle(request: GetNodeStatus, context: ZLinkRouteMessageContext): Promise<NodeStatus> {
    void request;

    // Process start, not first use of this handler, so the number means
    // what an operator expects it to mean.
    const uptimeSeconds = process.uptime();

    return {
      meshName: context.meshName ?? '(none)',
      // "(none)" here proves the point: no channel was involved in the routing.
      // A channel handler would find its channel name in this property.
      channelName: context.channelName ?? '(none)',
      // Node-direct context also carries the caller's routing id.
      calledBy: context.sourceNodeRid,
      uptime: `${Math.round(uptimeSeconds)}s`,
      processId: process.pid
    };
  }
}
// --8<-- [end:node-direct-handler]
