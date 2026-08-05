import { ZLinkLocationAutoConnectType, ZLinkLocationRole } from './internal-location-contracts';

const autoConnectCanonicalNames = new Map<ZLinkLocationAutoConnectType, string>([
  [ZLinkLocationAutoConnectType.RouteMesh, 'route-mesh'],
  [ZLinkLocationAutoConnectType.Fanout, 'fanout']
]);

const roleCanonicalNames = new Map<ZLinkLocationRole, string>([
  [ZLinkLocationRole.Router, 'router'],
  [ZLinkLocationRole.Dealer, 'dealer'],
  [ZLinkLocationRole.Pub, 'pub'],
  [ZLinkLocationRole.Sub, 'sub'],
  [ZLinkLocationRole.Spot, 'spot']
]);

const canonicalAutoConnectTypes = new Map<string, ZLinkLocationAutoConnectType>(
  [...autoConnectCanonicalNames.entries()].map(([type, name]) => [name, type])
);

const canonicalRoles = new Map<string, ZLinkLocationRole>(
  [...roleCanonicalNames.entries()].map(([role, name]) => [name, role])
);

export function zlinkLocationAutoConnectTypeName(type: ZLinkLocationAutoConnectType): string {
  const name = autoConnectCanonicalNames.get(type);
  if (name === undefined) {
    throw new RangeError(`Unknown location auto-connect type: ${type}`);
  }
  return name;
}

export function zlinkLocationRoleName(role: ZLinkLocationRole): string {
  const name = roleCanonicalNames.get(role);
  if (name === undefined) {
    throw new RangeError(`Unknown location role: ${role}`);
  }
  return name;
}

export function isKnownZLinkLocationAutoConnectType(type: ZLinkLocationAutoConnectType): boolean {
  return autoConnectCanonicalNames.has(type);
}

export function isKnownZLinkLocationRole(role: ZLinkLocationRole): boolean {
  return roleCanonicalNames.has(role);
}

export function tryParseZLinkLocationAutoConnectType(value: string): ZLinkLocationAutoConnectType | undefined {
  return canonicalAutoConnectTypes.get(value);
}

export function tryParseZLinkLocationRole(value: string): ZLinkLocationRole | undefined {
  return canonicalRoles.get(value);
}
