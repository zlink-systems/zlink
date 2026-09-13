// SPDX-License-Identifier: MPL-2.0

const zlink = require('@zlink-systems/zlink');

/** Caller-owned public completion poller used by async binding contract tests. */
export class CompletionPollerDriver {
  private readonly poller = zlink.createPoller();
  private readonly events: any;
  private closed = false;

  constructor(sockets: any | readonly any[]) {
    const sources = Array.isArray(sockets) ? sockets : [sockets];
    this.events = zlink.createPollEvents(Math.max(1, sources.length));
    try {
      sources.forEach((socket, index) => {
        this.poller.add(socket, [zlink.PollEventFlag.PollCompletion], index);
      });
    } catch (error) {
      this.close();
      throw error;
    }
  }

  wait(timeoutMs = 5_000): number {
    return this.poller.wait(this.events, timeoutMs);
  }

  async settle<T>(promise: Promise<T>, timeoutMs = 5_000): Promise<T> {
    type Outcome = { readonly ok: true; readonly value: T }
      | { readonly ok: false; readonly error: unknown };
    let outcome: Outcome | undefined;
    void promise.then(
      value => { outcome = { ok: true, value }; },
      error => { outcome = { ok: false, error }; }
    );
    const deadline = Date.now() + timeoutMs;
    while (outcome === undefined) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) throw new Error('completion poller deadline expired');
      this.wait(Math.min(remaining, 100));
      await Promise.resolve();
    }
    const completed = outcome as Outcome;
    if ('error' in completed) throw completed.error;
    return completed.value;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.events.close();
    this.poller.close();
  }
}
