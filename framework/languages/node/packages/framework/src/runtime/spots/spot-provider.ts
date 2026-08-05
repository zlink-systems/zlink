import type { Type } from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';

export async function createProviderInstance<T>(
  type: Type<T>,
  resolver: ZLinkProviderResolver | undefined,
  fallbackArg?: unknown
): Promise<T> {
  const existing = resolver?.get?.(type);
  if (existing !== undefined) {
    return existing;
  }
  const created = await resolver?.create?.(type);
  if (created !== undefined) {
    return created;
  }
  if (fallbackArg !== undefined) {
    return new (type as new (arg: unknown) => T)(fallbackArg);
  }
  return new (type as new () => T)();
}

export async function createFreshProviderInstance<T>(
  type: Type<T>,
  resolver: ZLinkProviderResolver | undefined,
  fallbackArg?: unknown
): Promise<T> {
  const created = await resolver?.create?.(type);
  if (created !== undefined) {
    return created;
  }
  if (fallbackArg !== undefined) {
    return new (type as new (arg: unknown) => T)(fallbackArg);
  }
  return new (type as new () => T)();
}
