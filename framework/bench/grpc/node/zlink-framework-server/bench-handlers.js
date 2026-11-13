// SPDX-License-Identifier: MPL-2.0
'use strict';

function toBuffer(body) {
  if (body === undefined || body === null) return Buffer.alloc(0);
  if (Buffer.isBuffer(body)) return body;
  if (typeof body === 'string') return Buffer.from(body, 'base64');
  if (body instanceof Uint8Array) return Buffer.from(body.buffer, body.byteOffset, body.byteLength);
  return Buffer.from(body);
}

class EchoHandler {
  constructor(metrics) { this.metrics = metrics; }
  async handle(request) {
    this.metrics.record(toBuffer(request && request.body));
    return request;
  }
  handleL0(body) {
    this.metrics.record(body);
    return body;
  }
}

class CommandHandler {
  constructor(metrics) { this.metrics = metrics; }
  async handle(message) {
    this.metrics.record(toBuffer(message && message.body));
    return undefined;
  }
  handleL0(body) {
    this.metrics.record(body);
    return undefined;
  }
}

module.exports = { EchoHandler, CommandHandler, toBuffer };
