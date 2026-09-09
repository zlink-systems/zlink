import { signal } from '@preact/signals';
import type {
  NodeAlertNotify,
  NodeStatusNotify,
  NodeView,
  WatchNodesRes,
} from '../../shared/api/contracts';

export class NodeModel {
  readonly nodes = signal<readonly NodeView[]>([]);
  readonly alerts = signal<readonly NodeAlertNotify[]>([]);

  applySnapshot(snapshot: WatchNodesRes): void {
    this.nodes.value = order(snapshot.nodes);
  }

  applyStatus(status: NodeStatusNotify): void {
    const next = new Map(this.nodes.value.map((node) => [node.nodeId, node]));
    next.set(status.nodeId, status);
    this.nodes.value = order([...next.values()]);
  }

  applyAlert(alert: NodeAlertNotify): void {
    this.alerts.value = [...this.alerts.value, alert]
      .sort((left, right) => left.occurredAt.localeCompare(right.occurredAt))
      .slice(-40);
  }
}

function order(nodes: readonly NodeView[]): readonly NodeView[] {
  return [...nodes].sort((left, right) => left.nodeId.localeCompare(right.nodeId));
}
