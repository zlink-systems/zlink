const { spawn } = require("node:child_process");
const readline = require("node:readline");
const wrap = (value) => ({ value });
function text(value) {
  if (typeof value !== "string")
    throw new Error("adapter-error: expected string");
  return value;
}
function milliseconds(value) {
  if (
    !/^(0|[1-9][0-9]*)$/.test(text(value)) ||
    !Number.isSafeInteger(Number(value))
  )
    throw new Error("adapter-error: invalid milliseconds");
  return new Date(Number(value));
}
function nativeValue(v) {
  const bytes = Buffer.from(text(v.bytesBase64), "base64");
  if (bytes.toString("base64") !== v.bytesBase64)
    throw new Error("adapter-error: invalid base64");
  return {
    bytes,
    version: wrap(text(v.version)),
    expiresAt: v.expiresAtMs === null ? undefined : milliseconds(v.expiresAtMs),
    storeNow: milliseconds(v.storeNowMs),
  };
}
function nativeResult(r) {
  if (r.kind === "found") return { kind: r.kind, value: nativeValue(r.value) };
  if (r.kind === "page")
    return {
      kind: r.kind,
      value: {
        storeNow: milliseconds(r.value.storeNowMs),
        nextCursor:
          r.value.nextCursor === null
            ? undefined
            : wrap(text(r.value.nextCursor)),
        items: r.value.items.map((i) => ({
          key: wrap(text(i.key)),
          value: nativeValue(i.value),
        })),
      },
    };
  if (r.kind === "expired") return r;
  if (!["missing", "conflict", "applied"].includes(r.kind))
    throw new Error(`adapter-error: unknown result ${r.kind}`);
  return {
    kind: r.kind,
    storeNow: milliseconds(r.storeNowMs),
    ...(r.kind === "applied"
      ? {
          putVersions: r.putVersions.map((p) => ({
            key: wrap(text(p.key)),
            version: wrap(text(p.version)),
          })),
        }
      : {}),
  };
}
class AdapterProcess {
  constructor(config, errors) {
    this.errors = errors;
    this.serial = 0;
    this.storeSerial = 0;
    this.pending = new Map();
    this.output = "";
    this.child = spawn(config.command, config.args || [], {
      cwd: config.cwd,
      env: { ...process.env, ...config.env },
      windowsHide: true,
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.child.stderr.on("data", (b) => {
      this.output = (this.output + b).slice(-32768);
    });
    const fail = (error) => {
      error.category = "adapter-error";
      this.errors.push(error.message);
      for (const p of this.pending.values()) p.reject(error);
      this.pending.clear();
    };
    this.child.stdin.on("error", fail);
    readline
      .createInterface({ input: this.child.stdout, crlfDelay: Infinity })
      .on("line", (line) => {
        try {
          const m = JSON.parse(line),
            p = this.pending.get(m.id);
          if (m.v !== 1 || !p || typeof m.ok !== "boolean")
            throw new Error(`invalid adapter response: ${line}`);
          if (
            !m.ok &&
            (!["argument-error", "store-failure", "adapter-error"].includes(
              m.error?.category,
            ) ||
              (m.error.category === "store-failure" &&
                !["none", "unknown"].includes(m.error.commit)))
          )
            throw new Error("invalid error category or commit");
          this.pending.delete(m.id);
          if (m.ok) p.resolve(m.result);
          else {
            const e =
              m.error.category === "argument-error"
                ? new RangeError(m.error.message)
                : new Error(m.error.message);
            Object.assign(e, m.error);
            if (e.category === "adapter-error") this.errors.push(e.message);
            p.reject(e);
          }
        } catch (error) {
          fail(error);
          this.child.kill();
        }
      });
    this.exited = new Promise((resolve) => {
      this.child.once("error", (error) => {
        fail(error);
        resolve();
      });
      this.child.once("exit", (code, signal) => {
        if (!this.closing || code !== 0)
          fail(new Error(`adapter crash ${code}/${signal}: ${this.output}`));
        resolve();
      });
    });
  }
  request(body) {
    const id = String(++this.serial);
    const promise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.errors.push(`adapter request timed out: ${body.op}`);
        this.child.kill();
        reject(new Error("adapter-error: response timeout"));
      }, 45000);
      this.pending.set(id, {
        resolve: (v) => {
          clearTimeout(timer);
          resolve(v);
        },
        reject: (e) => {
          clearTimeout(timer);
          reject(e);
        },
      });
      this.child.stdin.write(
        `${JSON.stringify({ v: 1, id, ...body })}\n`,
        (error) => {
          if (error) this.pending.get(id)?.reject(error);
        },
      );
    });
    return { id, promise };
  }
  store(options) {
    const store = `s${++this.storeSerial}`;
    const created = this.request({ op: "create", store, options }).promise;
    created.catch(() => {});
    const call = async (op, body, signal) => {
      await created;
      if (signal?.aborted)
        throw Object.assign(new Error("Store operation stopped"), {
          category: "store-failure",
          commit: "none",
        });
      const task = this.request({ op, store, ...body });
      const cancel = () => {
        this.request({ op: "cancel", target: task.id }).promise.catch(() => {});
      };
      signal?.addEventListener("abort", cancel, { once: true });
      try {
        const response = await task.promise;
        try {
          return nativeResult(response);
        } catch (error) {
          this.errors.push(error.message);
          error.category = "adapter-error";
          throw error;
        }
      } finally {
        signal?.removeEventListener("abort", cancel);
      }
    };
    let disposed;
    return {
      read: (key, signal) => call("read", { key: key.value }, signal),
      write: (r, signal) =>
        call(
          "write",
          {
            request: {
              conditions: r.conditions.map((c) => ({
                kind: c.kind,
                key: c.key.value,
                ...(c.kind === "missing"
                  ? {}
                  : c.kind === "version"
                    ? { version: c.expected.value }
                    : {
                        bytesBase64: Buffer.from(c.expected).toString("base64"),
                      }),
              })),
              mutations: r.mutations.map((m) => ({
                kind: m.kind,
                key: m.key.value,
                ...(m.kind === "put"
                  ? {
                      bytesBase64: Buffer.from(m.bytes).toString("base64"),
                      ...(m.retentionMs === undefined
                        ? {}
                        : { retentionMs: String(m.retentionMs) }),
                    }
                  : {}),
              })),
            },
          },
          signal,
        ),
      scan: (r, signal) =>
        call(
          "scan",
          { request: { ...r, cursor: r.cursor?.value ?? null } },
          signal,
        ),
      dispose: () =>
        (disposed ??= created.then(
          () => this.request({ op: "dispose", store }).promise,
        )),
    };
  }
  async close() {
    if (this.closing) return this.exited;
    this.closing = true;
    this.child.stdin.end();
    const timer = setTimeout(() => this.child.kill(), 5000);
    try {
      await this.exited;
    } finally {
      clearTimeout(timer);
    }
  }
}
module.exports = { AdapterProcess };
