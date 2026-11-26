class FakeLivenessClock {
  constructor() {
    this.current = 0;
    this.nextTimer = 1;
    this.timers = new Map();
  }

  now = () => this.current;

  setTimer = (callback, delayMs) => {
    const id = this.nextTimer++;
    this.timers.set(id, { callback, due: this.current + delayMs });
    return id;
  };

  clearTimer = (id) => {
    this.timers.delete(id);
  };

  async advance(delayMs) {
    const target = this.current + delayMs;
    for (;;) {
      const next = [...this.timers.entries()]
        .filter(([, timer]) => timer.due <= target)
        .sort((left, right) => left[1].due - right[1].due)[0];
      if (next === undefined) break;
      const [id, timer] = next;
      this.timers.delete(id);
      this.current = Math.max(this.current, timer.due);
      timer.callback();
      await this.flush();
    }
    this.current = target;
    await this.flush();
  }

  async flush() {
    await new Promise((resolve) => setImmediate(resolve));
  }
}

module.exports = { FakeLivenessClock };
