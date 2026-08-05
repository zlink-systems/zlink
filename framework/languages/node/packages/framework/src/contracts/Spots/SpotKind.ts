export enum ZLinkSpotKind {
  Invalid = 'invalid',
  Entry = 'entry',
  User = 'user',
  Instance = 'instance'
}

export function zlinkSpotKindToWire(kind: ZLinkSpotKind): number {
  switch (kind) {
    case ZLinkSpotKind.Entry: return 1;
    case ZLinkSpotKind.User: return 2;
    case ZLinkSpotKind.Instance: return 3;
    default: return 0;
  }
}

export function zlinkSpotKindFromWire(value: number): ZLinkSpotKind {
  switch (value) {
    case 1: return ZLinkSpotKind.Entry;
    case 2: return ZLinkSpotKind.User;
    case 3: return ZLinkSpotKind.Instance;
    default: return ZLinkSpotKind.Invalid;
  }
}
