import { useState } from 'preact/hooks';

export function JoinForm({ onJoin, busy }: { onJoin: (playerId: string) => Promise<void>; busy: boolean }) {
  const [playerId, setPlayerId] = useState(`player-${Math.random().toString(36).slice(2, 8)}`);
  return (
    <form class="join-form" onSubmit={(event) => { event.preventDefault(); void onJoin(playerId); }}>
      <label><span>Player ID</span><input value={playerId} onInput={(event) => setPlayerId(event.currentTarget.value)} /></label>
      <button disabled={busy}>{busy ? 'Connecting…' : 'Enter world'}</button>
    </form>
  );
}
