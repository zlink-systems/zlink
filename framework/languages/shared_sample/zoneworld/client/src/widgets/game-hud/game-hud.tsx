import type { AnnouncementModel } from '../../entities/announcement/model';
import type { NodeView } from '../../shared/api/contracts';
import type { PlayerModel } from '../../entities/player/model';

interface Props {
  player: PlayerModel;
  announcements: AnnouncementModel;
  connectionState: string;
  nodes: readonly NodeView[];
}

export function GameHud({ player, announcements, connectionState, nodes }: Props) {
  const owner = nodes.find((node) => node.zones.includes(player.zoneId.value));
  return (
    <aside class="stack game-hud">
      <section class="panel">
        <p class="eyebrow">Live authority</p>
        <h2>{player.id.value || 'Not joined'}</h2>
        <dl class="stat-grid">
          <div><dt>Zone</dt><dd data-testid="player-zone">{player.zoneId.value}</dd></div>
          <div><dt>Owner (Ops)</dt><dd data-testid="player-node">{owner?.nodeId ?? '—'}</dd></div>
          <div><dt>Position</dt><dd data-testid="player-position">{player.x.value}, {player.y.value}</dd></div>
          <div><dt>Socket</dt><dd data-testid="socket-state"><Status value={connectionState} /></dd></div>
        </dl>
        {player.rejection.value !== null && (
          <p class="notice danger">Move rejected: {player.rejection.value.reason}</p>
        )}
      </section>
      <section class="panel">
        <p class="eyebrow">Announcements</p>
        <div class="announcement-list">
          {announcements.items.value.length === 0 && <p class="muted">No announcements yet.</p>}
          {announcements.items.value.map((item) => (
            <article key={item.announcementId}>
              <code>{item.announcementId}</code>
              <p>{item.text}</p>
            </article>
          ))}
        </div>
      </section>
    </aside>
  );
}

function Status({ value }: { value: string }) {
  return <span class={`status status-${value}`}><i aria-hidden="true" />{value}</span>;
}
