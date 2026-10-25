export function encodeRoomSnapshot(value: object): Uint8Array {
  return new TextEncoder().encode(JSON.stringify(value));
}

export function decodeRoomSnapshot<T>(payload: Uint8Array): T {
  return JSON.parse(new TextDecoder().decode(payload)) as T;
}
