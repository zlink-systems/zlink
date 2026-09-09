import type { NodeView } from '../../shared/api/contracts';

export function nextMaintenance(node: NodeView): boolean {
  return !node.maintenance;
}
