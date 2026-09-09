import { useEffect, useMemo, useState } from 'preact/hooks';
import { AnnounceForm } from '../../features/announce-world/announce-form';
import { DiagnosticsCard } from '../../features/node-diagnostics/diagnostics-card';
import { nextMaintenance } from '../../features/set-maintenance/action';
import { OpsController } from '../../features/watch-nodes/model';
import { AlertList } from '../../widgets/alert-list/alert-list';
import { NodeTable } from '../../widgets/node-table/node-table';

export function OpsPage({ ops }: { ops: string }) {
  const controller = useMemo(() => new OpsController(ops), [ops]);
  const [error, setError] = useState<string | null>(null);
  const [connecting, setConnecting] = useState(false);
  useEffect(() => () => { void controller.close(); }, [controller]);
  const perform = async (operation: () => Promise<void>) => {
    setError(null);
    try { await operation(); } catch (reason) { setError(String(reason)); }
  };

  return (
    <div class="shell">
      <header class="topbar"><div><p class="eyebrow">Push-only operations</p><h1>ZoneWorld Ops</h1></div><nav><a href="/game.html">Game</a><a class="active" href="/ops.html">Operations</a></nav></header>
      <section class="ops-hero">
        <div><p class="kicker">Runtime event console</p><h2>No polling. Every transition arrives from the server.</h2></div>
        <div class="connection-control"><span class={`status status-${controller.stream.state.value}`}><i />{controller.stream.state.value}</span><button disabled={connecting} onClick={() => { setConnecting(true); void perform(() => controller.connect()).finally(() => setConnecting(false)); }}>{connecting ? 'Connecting…' : 'Connect console'}</button></div>
      </section>
      {error !== null && <p class="notice danger">{error}</p>}
      <NodeTable nodes={controller.topology.nodes.value} onMaintenance={(node) => { void perform(() => controller.setMaintenance(node.nodeId, nextMaintenance(node))); }} onDiagnose={(node) => { void perform(() => controller.diagnose(node.nodeId)); }} />
      <div class="ops-grid">
        <AnnounceForm onAnnounce={(text) => perform(() => controller.announce(text))} result={controller.lastAnnouncementId.value} />
        <DiagnosticsCard value={controller.diagnostics.value} />
        <AlertList alerts={controller.topology.alerts.value} />
      </div>
    </div>
  );
}
