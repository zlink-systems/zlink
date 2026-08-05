import { describe, expect, it } from 'vitest';
import { isAdjacentZone, isBorderBand, isWorldCoordinate, nodeOf, zoneOf } from './model';

describe('ZoneWorld domain rules', () => {
  it('splits the 100 by 100 world at x=50 and y=50', () => {
    expect(zoneOf(49, 49)).toBe('zone-nw');
    expect(zoneOf(50, 49)).toBe('zone-ne');
    expect(zoneOf(49, 50)).toBe('zone-sw');
    expect(zoneOf(50, 50)).toBe('zone-se');
  });

  it('maps zones to the two authoritative nodes', () => {
    expect(nodeOf('zone-nw')).toBe('zone-node-1');
    expect(nodeOf('zone-se')).toBe('zone-node-2');
  });

  it('recognizes the border band and rejects diagonal adjacency', () => {
    expect(isBorderBand(40, 10)).toBe(true);
    expect(isBorderBand(10, 10)).toBe(false);
    expect(isAdjacentZone('zone-nw', 'zone-ne')).toBe(true);
    expect(isAdjacentZone('zone-nw', 'zone-se')).toBe(false);
  });

  it('accepts integer coordinates inside the world only', () => {
    expect(isWorldCoordinate(0)).toBe(true);
    expect(isWorldCoordinate(99)).toBe(true);
    expect(isWorldCoordinate(100)).toBe(false);
    expect(isWorldCoordinate(1.5)).toBe(false);
  });
});
