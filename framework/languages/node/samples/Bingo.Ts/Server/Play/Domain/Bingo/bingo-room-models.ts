type BingoMode = 'two-player';

type BingoRoomSettingsInput = {
  mode?: BingoMode;
  roomName?: string;
  requiredPlayers?: number;
  maxDrawNumber?: number;
  purpose?: string;
  observedRoomId?: string | null;
};

type BingoRoomSettings = {
  mode: BingoMode;
  roomName: string;
  requiredPlayers: number;
  maxDrawNumber: number;
  drawDeck: number[];
  purpose: 'Game' | 'Observer';
  observedRoomId: string | null;
};

function createRoomSettings(roomSeqOrMode: number | BingoMode = 0, mode: BingoMode = 'two-player'): BingoRoomSettings {
  const roomSeq = typeof roomSeqOrMode === 'number' ? roomSeqOrMode : 0;
  const resolvedMode = typeof roomSeqOrMode === 'number' ? mode : roomSeqOrMode;
  const maxDrawNumber = 15;
  return {
    mode: resolvedMode,
    roomName: `Bingo Room ${String(roomSeq).padStart(3, '0')}`,
    requiredPlayers: 2,
    maxDrawNumber,
    drawDeck: Array.from({ length: maxDrawNumber }, (_value, index) => index + 1),
    purpose: 'Game',
    observedRoomId: null
  };
}

function createObserverRoomSettings(observedRoomId: string, observerActorId: string): BingoRoomSettings {
  const maxDrawNumber = 15;
  return {
    mode: 'two-player',
    roomName: `Bingo Reward Observer ${observerActorId}`,
    requiredPlayers: 1,
    maxDrawNumber,
    drawDeck: Array.from({ length: maxDrawNumber }, (_value, index) => index + 1),
    purpose: 'Observer',
    observedRoomId
  };
}

function roomSettingsFromPayload(payload: BingoRoomSettingsInput): BingoRoomSettings {
  const maxDrawNumber = payload.maxDrawNumber ?? 15;
  const purpose = payload.purpose === 'Observer' ? 'Observer' : 'Game';
  return {
    mode: payload.mode ?? 'two-player',
    roomName: payload.roomName ?? 'Bingo Room 000',
    requiredPlayers: payload.requiredPlayers ?? 2,
    maxDrawNumber,
    drawDeck: Array.from({ length: maxDrawNumber }, (_value, index) => index + 1),
    purpose,
    observedRoomId: payload.observedRoomId ?? null
  };
}

export { createObserverRoomSettings, createRoomSettings, roomSettingsFromPayload };
export type { BingoMode, BingoRoomSettings, BingoRoomSettingsInput };
