/** Keeps terminal replay records in bounded least-recently-used order. */
export class BoundedReplayMap<K, V> implements Iterable<[K, V]> {
  private readonly values = new Map<K, V>();

  constructor(private readonly capacity: number) {
    if (!Number.isSafeInteger(capacity) || capacity < 1) {
      throw new RangeError('Bounded replay capacity must be a positive safe integer.');
    }
  }

  get(key: K): V | undefined {
    return this.values.get(key);
  }

  remember(key: K, value: V): void {
    this.values.delete(key);
    this.values.set(key, value);
    while (this.values.size > this.capacity) {
      const oldest = this.values.keys().next().value as K;
      this.values.delete(oldest);
    }
  }

  touch(key: K): void {
    if (!this.values.has(key)) return;
    const value = this.values.get(key) as V;
    this.values.delete(key);
    this.values.set(key, value);
  }

  delete(key: K): boolean {
    return this.values.delete(key);
  }

  clear(): void {
    this.values.clear();
  }

  [Symbol.iterator](): MapIterator<[K, V]> {
    return this.values[Symbol.iterator]();
  }
}
