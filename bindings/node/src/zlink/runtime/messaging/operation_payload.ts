// SPDX-License-Identifier: MPL-2.0

export type OperationPayloadValue<T> = T | readonly T[];

export class OperationPayload<TInput, TStored = TInput> {
  private readonly _normalize: (value: TInput) => TStored;
  private _single!: TStored;
  private _hasSingle = false;
  private _parts: TStored[] | null = null;
  private _submitted = false;

  constructor(normalize: (value: TInput) => TStored) {
    this._normalize = normalize;
  }

  append(message: TInput): void {
    this.ensureOpen();
    const normalized = this._normalize(message);
    if (this._parts) {
      this._parts.push(normalized);
    } else if (this._hasSingle) {
      this._parts = [this._single, normalized];
      this._hasSingle = false;
      this._single = undefined as TStored;
    } else {
      this._single = normalized;
      this._hasSingle = true;
    }
  }

  ensureOpen(): void {
    if (this._submitted) {
      throw new TypeError('operation has already been submitted');
    }
  }

  consume(): OperationPayloadValue<TStored> {
    this.ensureOpen();
    if (!this._parts && !this._hasSingle) {
      throw new TypeError('operation requires at least one message');
    }
    this._submitted = true;
    return this._parts ?? this._single;
  }

  consumeParts(): readonly TStored[] {
    const value = this.consume();
    return Array.isArray(value) ? value as readonly TStored[] : [value as TStored];
  }
}
