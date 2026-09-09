import type { NodeView } from '../../shared/api/contracts';

interface Props {
  nodes: readonly NodeView[];
  onMaintenance: (node: NodeView) => void;
  onDiagnose: (node: NodeView) => void;
}

export function NodeTable({ nodes, onMaintenance, onDiagnose }: Props) {
  return (
    <section class="panel node-panel">
      <div class="section-heading">
        <div><p class="eyebrow">Push topology</p><h2>Zone nodes</h2></div>
        <span class="count">{nodes.length} known</span>
      </div>
      <div class="table-wrap">
        <table>
          <thead><tr><th>Node</th><th>Runtime</th><th>zones</th><th>players</th><th>Actions</th></tr></thead>
          <tbody>
            {nodes.map((node) => (
              <tr key={node.nodeId} data-testid={`node-${node.nodeId}`}>
                <td><strong>{node.nodeId}</strong></td>
                <td>
                  <div class="status-stack">
                    <Flag on={node.registered} label="registered" testId="registered-state" />
                    <Flag on={node.connected} label="connected" testId="connected-state" />
                    <Flag on={node.maintenance} label="maintenance" testId="maintenance-state" warning />
                  </div>
                </td>
                <td>{node.zones.join(' · ') || '—'}</td>
                <td class="numeric">{node.playerCount}</td>
                <td><div class="actions">
                  <button class="secondary" onClick={() => onMaintenance(node)}>
                    {node.maintenance ? 'Restore' : 'Maintain'}
                  </button>
                  <button class="ghost" onClick={() => onDiagnose(node)}>Diagnose</button>
                </div></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

function Flag({ on, label, testId, warning = false }: { on: boolean; label: string; testId: string; warning?: boolean }) {
  const kind = warning && on ? 'warning' : on ? 'good' : 'off';
  return <span class={`flag ${kind}`} data-testid={testId} data-on={String(on)}><i aria-hidden="true" />{label}</span>;
}
