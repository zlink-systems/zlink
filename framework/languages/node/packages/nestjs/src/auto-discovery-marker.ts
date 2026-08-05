import type { Type } from '@zlink-systems/framework';

const autoDiscoveredProviders = new WeakSet<Type>();

export function markAutoDiscoveredProvider(provider: Type): Type {
  autoDiscoveredProviders.add(provider);
  return provider;
}

export function isAutoDiscoveredProvider(provider: Type): boolean {
  return autoDiscoveredProviders.has(provider);
}
