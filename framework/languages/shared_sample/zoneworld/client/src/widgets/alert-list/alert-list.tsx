import type { NodeAlertNotify } from '../../shared/api/contracts';

export function AlertList({ alerts }: { alerts: readonly NodeAlertNotify[] }) {
  return (
    <section class="panel alert-panel">
      <p class="eyebrow">Runtime events</p>
      <h2>Node alerts</h2>
      <ol class="alerts">
        {alerts.length === 0 && <li class="muted">No alerts received.</li>}
        {[...alerts].reverse().map((alert, index) => (
          <li key={`${alert.occurredAt}-${index}`}>
            <time>{new Date(alert.occurredAt).toLocaleTimeString()}</time>
            <div><strong>{alert.nodeId} · {alert.kind}</strong><p>{alert.detail}</p></div>
          </li>
        ))}
      </ol>
    </section>
  );
}
