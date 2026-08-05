// The world rules from scenario §2. The servers hold the same values; the client
// uses them to render and to predict nothing — the server stays authoritative (§9.1).

export const WORLD_SIZE = 100;
export const ZONE_SPLIT = 50;
export const BORDER_BAND = 10;
export const MAX_STEP_PER_AXIS = 5;
export const TICK_PERIOD_MS = 100;

export const ZONE_NW = 'zone-nw';
export const ZONE_NE = 'zone-ne';
export const ZONE_SW = 'zone-sw';
export const ZONE_SE = 'zone-se';

export const ZONE_IDS = [ZONE_NW, ZONE_NE, ZONE_SW, ZONE_SE] as const;
export type zoneId = (typeof ZONE_IDS)[number];

export const NODE_WEST = 'zone-node-1';
export const NODE_EAST = 'zone-node-2';
