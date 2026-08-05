export interface Message {
  data(): Buffer;
  toBytes(): Uint8Array;
  copy(): Message;
  size(): number;
  isEmpty(): boolean;
  getString(encoding?: BufferEncoding): string;
  close(): void;
}
