import type { NodeDiagnosticsRes } from '../../shared/api/contracts';

export function DiagnosticsCard({ value }: { value: NodeDiagnosticsRes | null }) {
  if (value === null) return null;
  return (
    <section class="panel diagnostics-card" data-testid="diagnostics-card">
      <p class="eyebrow">Owner-consistent response</p><h2>{value.nodeId}</h2>
      {value.error !== undefined && value.error !== null ? <p class="notice danger">{value.error}</p> : (
        <dl class="stat-grid">
          <div><dt>zones</dt><dd>{value.zones.join(' · ')}</dd></div>
          <div><dt>players</dt><dd>{value.playerCount}</dd></div>
          <div><dt>maintenance</dt><dd>{value.maintenance ? 'enabled' : 'disabled'}</dd></div>
        </dl>
      )}
    </section>
  );
}
