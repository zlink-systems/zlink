export class GoldBalance {
  private current = 0;
  get value(): number {
    return this.current;
  }
  constructor(value: number) {
    this.restore(value);
  }
  afterEarning(amount: number): number {
    if (!Number.isSafeInteger(amount) || amount <= 0)
      throw new Error('Amount must be a positive safe integer');
    const next = this.value + amount;
    if (!Number.isSafeInteger(next)) throw new Error('Gold overflow');
    return next;
  }
  restore(value: number): void {
    if (!Number.isSafeInteger(value) || value < 0) throw new Error('Invalid gold');
    this.current = value;
  }
}
