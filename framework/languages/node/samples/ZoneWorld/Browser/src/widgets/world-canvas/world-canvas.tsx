import { useEffect, useRef } from 'preact/hooks';
import type { PlayerModel } from '../../entities/player/model';
import { isAdjacentZone, isBorderBand } from '../../entities/zone/model';
import { BORDER_BAND, WORLD_SIZE, ZONE_SPLIT } from '../../shared/config/world';

interface Props {
  model: PlayerModel;
}

export function WorldCanvas({ model }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    let frame = 0;
    const render = () => {
      const canvas = canvasRef.current;
      if (canvas !== null) drawWorld(canvas, model);
      frame = requestAnimationFrame(render);
    };
    frame = requestAnimationFrame(render);
    return () => cancelAnimationFrame(frame);
  }, [model]);

  return <canvas ref={canvasRef} class="world-canvas" width={900} height={700} aria-label="ZoneWorld map" />;
}

function drawWorld(canvas: HTMLCanvasElement, model: PlayerModel): void {
  const context = canvas.getContext('2d');
  if (context === null) return;
  const scaleX = canvas.width / WORLD_SIZE;
  const scaleY = canvas.height / WORLD_SIZE;

  context.clearRect(0, 0, canvas.width, canvas.height);
  context.fillStyle = '#101a2b';
  context.fillRect(0, 0, canvas.width, canvas.height);

  context.fillStyle = 'rgba(89, 210, 193, 0.08)';
  context.fillRect((ZONE_SPLIT - BORDER_BAND) * scaleX, 0, BORDER_BAND * 2 * scaleX, canvas.height);
  context.fillRect(0, (ZONE_SPLIT - BORDER_BAND) * scaleY, canvas.width, BORDER_BAND * 2 * scaleY);

  context.strokeStyle = '#5e718f';
  context.lineWidth = 1;
  for (let position = 0; position <= WORLD_SIZE; position += 10) {
    context.beginPath();
    context.moveTo(position * scaleX, 0);
    context.lineTo(position * scaleX, canvas.height);
    context.stroke();
    context.beginPath();
    context.moveTo(0, position * scaleY);
    context.lineTo(canvas.width, position * scaleY);
    context.stroke();
  }
  context.strokeStyle = '#f5b95b';
  context.lineWidth = 3;
  context.beginPath();
  context.moveTo(ZONE_SPLIT * scaleX, 0);
  context.lineTo(ZONE_SPLIT * scaleX, canvas.height);
  context.moveTo(0, ZONE_SPLIT * scaleY);
  context.lineTo(canvas.width, ZONE_SPLIT * scaleY);
  context.stroke();

  for (const player of model.visiblePlayers.value) {
    const own = player.playerId === model.id.value;
    const adjacent = isAdjacentZone(model.zoneId.value, player.zoneId);
    context.fillStyle = own ? '#ffffff' : player.isBot ? '#f5b95b' : adjacent ? '#a78bfa' : '#59d2c1';
    context.strokeStyle = adjacent ? '#d8c8ff' : '#101a2b';
    context.lineWidth = 2;
    const x = player.x * scaleX;
    const y = player.y * scaleY;
    context.beginPath();
    if (player.isBot) {
      context.rect(x - 5, y - 5, 10, 10);
    } else {
      context.arc(x, y, own ? 7 : 5, 0, Math.PI * 2);
    }
    context.fill();
    context.stroke();
    if (own || isBorderBand(player.x, player.y)) {
      context.fillStyle = '#eef4ff';
      context.font = '12px ui-monospace, monospace';
      context.fillText(player.playerId, x + 9, y - 7);
    }
  }
}
