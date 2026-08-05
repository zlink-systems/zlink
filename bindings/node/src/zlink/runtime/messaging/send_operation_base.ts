// SPDX-License-Identifier: MPL-2.0

import { SendFlags } from '../../contracts/sockets/socket_constants';
import { OperationPayload, type OperationPayloadValue } from './operation_payload';

export class SendOperationBase<TInput, TStored = TInput> {
  protected readonly _payload: OperationPayload<TInput, TStored>;
  protected _flags: SendFlags = SendFlags.None;

  constructor(normalize: (value: TInput) => TStored) {
    this._payload = new OperationPayload<TInput, TStored>(normalize);
  }

  message(message: TInput): this {
    this._payload.append(message);
    return this;
  }

  flags(flags: SendFlags): this {
    this._payload.ensureOpen();
    this._flags = flags;
    return this;
  }

  protected ensureOpen(): void {
    this._payload.ensureOpen();
  }

  protected consumePayload(): OperationPayloadValue<TStored> {
    return this._payload.consume();
  }

  protected consumeParts(): readonly TStored[] {
    return this._payload.consumeParts();
  }
}
