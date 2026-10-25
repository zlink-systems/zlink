import { Game } from './constants';

export class Vec3 {
  constructor(
    readonly x: number,
    readonly y: number,
    readonly z: number
  ) {}
}

export type Vec3Like = Pick<Vec3, 'x' | 'y' | 'z'>;

export function isFiniteVec(value: Vec3Like): boolean {
  return Number.isFinite(value.x) && Number.isFinite(value.y) && Number.isFinite(value.z);
}

export function vecLength(value: Vec3Like): number {
  const scale = Math.max(Math.abs(value.x), Math.abs(value.y), Math.abs(value.z));
  if (scale === 0 || !Number.isFinite(scale)) return scale;
  const x = value.x / scale,
    y = value.y / scale,
    z = value.z / scale;
  return scale * Math.sqrt(x * x + y * y + z * z);
}

export function normalizeVec(value: Vec3Like): Vec3 | null {
  const scale = Math.max(Math.abs(value.x), Math.abs(value.y), Math.abs(value.z));
  if (!isFiniteVec(value) || scale === 0) return null;
  const scaled = new Vec3(value.x / scale, value.y / scale, value.z / scale),
    length = vecLength(scaled);
  return new Vec3(scaled.x / length, scaled.y / length, scaled.z / length);
}

export function addVec(a: Vec3Like, b: Vec3Like): Vec3 {
  return new Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

export function subtractVec(a: Vec3Like, b: Vec3Like): Vec3 {
  return new Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

export class Aabb {
  constructor(
    readonly min: Vec3,
    readonly max: Vec3
  ) {}

  contains(point: Vec3Like): boolean {
    return (
      point.x >= this.min.x &&
      point.x <= this.max.x &&
      point.y >= this.min.y &&
      point.y <= this.max.y &&
      point.z >= this.min.z &&
      point.z <= this.max.z
    );
  }

  intersect(origin: Vec3Like, direction: Vec3Like, limit: number): number | null {
    const interval = { near: 0, far: limit };
    if (
      !Aabb.slab(origin.x, direction.x, this.min.x, this.max.x, interval) ||
      !Aabb.slab(origin.y, direction.y, this.min.y, this.max.y, interval) ||
      !Aabb.slab(origin.z, direction.z, this.min.z, this.max.z, interval)
    )
      return null;
    return interval.near;
  }

  private static slab(
    origin: number,
    direction: number,
    min: number,
    max: number,
    interval: { near: number; far: number }
  ): boolean {
    if (direction === 0) return origin >= min && origin <= max;
    const a = (min - origin) / direction,
      b = (max - origin) / direction;
    interval.near = Math.max(interval.near, Math.min(a, b));
    interval.far = Math.min(interval.far, Math.max(a, b));
    return interval.near <= interval.far;
  }
}

export const ArenaMap = {
  bounds: new Aabb(new Vec3(0, 0, 0), new Vec3(100, 10, 100)),
  walls: [
    new Aabb(new Vec3(49, 0, 20), new Vec3(51, 10, 60)),
    new Aabb(new Vec3(80, 0, 0), new Vec3(82, 10, 20))
  ],
  spawns: [new Vec3(48, 0, 18), new Vec3(52, 0, 18)] as const,
  playerBox(feet: Vec3Like): Aabb {
    return new Aabb(
      new Vec3(feet.x - Game.playerHalfWidth, feet.y, feet.z - Game.playerHalfWidth),
      new Vec3(feet.x + Game.playerHalfWidth, feet.y + Game.playerHeight, feet.z + Game.playerHalfWidth)
    );
  }
} as const;
