const fs = require('node:fs');
const path = require('node:path');

class EvidenceStore {
  constructor(rid, filePath) {
    this.rid = rid;
    this.filePath = filePath;
    this.entries = [];
    this.waiters = new Set();
    if (this.filePath !== undefined && this.filePath.length > 0) {
      fs.mkdirSync(path.dirname(this.filePath), { recursive: true });
      fs.writeFileSync(this.filePath, '');
    }
  }

  add(entry) {
    this.entries.push(entry);
    if (this.filePath !== undefined && this.filePath.length > 0) {
      fs.appendFileSync(this.filePath, `${entry}\n`);
    }
    const waiters = [...this.waiters];
    this.waiters.clear();
    for (const waiter of waiters) {
      waiter();
    }
  }

  snapshot() {
    return [...this.entries];
  }

  clear() {
    this.entries.length = 0;
    if (this.filePath !== undefined && this.filePath.length > 0) {
      fs.writeFileSync(this.filePath, '');
    }
  }

  async waitUntil(condition, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (true) {
      const snapshot = this.snapshot();
      const matched = typeof condition === 'function'
        ? condition(snapshot)
        : condition.every((expected) => snapshot.some((entry) => entry.includes(expected)));
      if (matched) {
        return snapshot;
      }
      const remaining = deadline - Date.now();
      if (remaining <= 0) {
        throw new Error('Timed out waiting for e2e evidence.');
      }
      await new Promise((resolve) => {
        let timer;
        const done = () => {
          clearTimeout(timer);
          this.waiters.delete(done);
          resolve();
        };
        timer = setTimeout(done, Math.min(remaining, 250));
        this.waiters.add(done);
      });
    }
  }
}

module.exports = { EvidenceStore };
