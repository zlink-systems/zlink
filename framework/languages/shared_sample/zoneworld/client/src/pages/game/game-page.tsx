import { useCallback, useEffect, useMemo, useState } from 'preact/hooks';
import { JoinForm } from '../../features/join-world/join-form';
import { GameController } from '../../features/join-world/model';
import { useKeyboardMovement } from '../../features/move-player/use-keyboard-movement';
import { GameHud } from '../../widgets/game-hud/game-hud';
import { WorldCanvas } from '../../widgets/world-canvas/world-canvas';

export function GamePage({ gateway }: { gateway: string }) {
  const controller = useMemo(() => new GameController(gateway), [gateway]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const move = useCallback((dx: number, dy: number) => controller.move(dx, dy), [controller]);
  useKeyboardMovement(move, controller.player.joined.value);
  useEffect(() => () => { void controller.close(); }, [controller]);

  const join = async (playerId: string) => {
    setBusy(true); setError(null);
    try { await controller.join(playerId); } catch (reason) { setError(String(reason)); } finally { setBusy(false); }
  };

  return (
    <div class="shell">
      <header class="topbar">
        <div><p class="eyebrow">Distributed actor world</p><h1>ZoneWorld</h1></div>
        <nav><a class="active" href="/game.html">Game</a><a href="/ops.html">Operations</a></nav>
      </header>
      {!controller.player.joined.value && <section class="hero"><div><p class="kicker">One socket. Four zones.</p><h2>Move through the world without reconnecting.</h2><p>Server pushes are authoritative; arrow-key input never changes the map first.</p></div><JoinForm onJoin={join} busy={busy} /></section>}
      {error !== null && <p class="notice danger">{error}</p>}
      <div class="game-layout">
        <section class="canvas-panel"><WorldCanvas model={controller.player} /><div class="canvas-legend"><span><i class="human" />human</span><span><i class="bot" />bot</span><span><i class="adjacent" />adjacent zone</span></div></section>
        <GameHud player={controller.player} announcements={controller.announcements} connectionState={controller.stream.state.value} />
      </div>
    </div>
  );
}
