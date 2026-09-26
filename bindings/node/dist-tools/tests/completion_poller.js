"use strict";
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.CompletionPollerDriver = void 0;
const zlink = require('@zlink-systems/zlink');
/** Caller-owned public completion poller used by async binding contract tests. */
class CompletionPollerDriver {
    poller;
    events;
    closed = false;
    constructor(context, sockets) {
        const sources = Array.isArray(sockets) ? sockets : [sockets];
        this.poller = zlink.createPoller();
        this.events = zlink.createPollEvents(Math.max(1, sources.length));
        try {
            sources.forEach((socket, index) => {
                this.poller.add(socket, [zlink.PollEventFlag.PollCompletion], index);
            });
        }
        catch (error) {
            this.close();
            throw error;
        }
    }
    wait(timeoutMs = 5_000) {
        return this.poller.wait(this.events, timeoutMs);
    }
    async settle(promise, timeoutMs = 5_000) {
        let outcome;
        void promise.then(value => { outcome = { ok: true, value }; }, error => { outcome = { ok: false, error }; });
        const deadline = Date.now() + timeoutMs;
        while (outcome === undefined) {
            const remaining = deadline - Date.now();
            if (remaining <= 0)
                throw new Error('completion poller deadline expired');
            this.wait(Math.min(remaining, 100));
            await Promise.resolve();
        }
        const completed = outcome;
        if ('error' in completed)
            throw completed.error;
        return completed.value;
    }
    close() {
        if (this.closed)
            return;
        this.closed = true;
        this.events.close();
        this.poller.close();
    }
}
exports.CompletionPollerDriver = CompletionPollerDriver;
