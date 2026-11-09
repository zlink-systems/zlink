import { fileURLToPath } from "node:url";
import dependencies from "./node-dependencies.cjs";

const grpc = dependencies("@grpc/grpc-js");
const loader = dependencies("@grpc/proto-loader");
const protoRoot = fileURLToPath(
  new URL("../../framework/runtime/protocol/etcd/v3.6.16/", import.meta.url),
);
const definition = loader.loadSync(
  `${protoRoot}/etcd/api/etcdserverpb/rpc.proto`,
  {
    keepCase: true,
    longs: String,
    bytes: Buffer,
    defaults: true,
    includeDirs: [protoRoot],
  },
);
const services = grpc.loadPackageDefinition(definition).etcdserverpb;
const channelOptions = {
  "grpc.enable_retries": 0,
  "grpc.max_send_message_length": 16777216,
  "grpc.max_receive_message_length": 16777216,
};
const deferred = () => {
  let resolve;
  const promise = new Promise((r) => {
    resolve = r;
  });
  return { promise, resolve };
};
function operations(request) {
  return [...(request.success || []), ...(request.failure || [])];
}
function subjects(method, request) {
  return method === "Txn"
    ? operations(request).flatMap((o) =>
        [
          o.request_put?.key,
          o.request_delete_range?.key,
          o.request_range?.key,
        ].filter(Boolean),
      )
    : request.key
      ? [request.key]
      : [];
}
function matches(rule, method, request) {
  const keys = subjects(method, request);
  if (rule.key && !keys.some((k) => k.equals(Buffer.from(rule.key))))
    return false;
  if (
    rule.prefix &&
    !keys.some((k) =>
      k.subarray(0, rule.prefix.length).equals(Buffer.from(rule.prefix)),
    )
  )
    return false;
  if (rule.boundary === "clock-next") return rule.clockRead;
  if (rule.boundary === "metadata-response")
    return method === "Range" && request.range_end.length > 0;
  if (rule.boundary === "cleanup-before")
    return (
      method === "Txn" &&
      operations(request).some((o) => o.request_delete_range)
    );
  return (
    method === "Txn" &&
    operations(request).some((o) => o.request_put || o.request_delete_range)
  );
}

// Unary HTTP/2 gRPC forwarding; protobuf decoding is exclusively for selection and evidence.
export async function startProxy({ target }) {
  const upstream = new URL(target);
  if (upstream.protocol !== "http:")
    throw new Error("test proxy requires a plaintext test cluster");
  const client = new grpc.Client(
    upstream.host,
    grpc.credentials.createInsecure(),
    channelOptions,
  );
  const server = new grpc.Server(channelOptions);
  const records = [];
  const rules = [];
  const calls = new Set();
  const gates = new Set();
  let serial = 0;
  function arm(selection) {
    if (
      ![
        "apply-before",
        "txn-response",
        "metadata-response",
        "cleanup-before",
        "clock-next",
      ].includes(selection.boundary)
    )
      throw new Error("unknown proxy boundary");
    if (!selection.key && !selection.prefix)
      throw new Error("proxy selection requires key or prefix");
    const entered = deferred(),
      released = deferred();
    const gate = {
      ...selection,
      entered: entered.promise,
      release: () => released.resolve(),
      loseResponse: () => {
        gate.lost = true;
        released.resolve();
      },
      disconnected: released.promise,
      _entered: entered,
      _released: released,
    };
    rules.push(gate);
    gates.add(gate);
    return gate;
  }
  async function hold(gate, record) {
    records.push({ event: "held", boundary: gate.boundary, rpc: record.id });
    gate._entered.resolve(record);
    await gate.disconnected;
    gates.delete(gate);
    if (gate.lost)
      throw Object.assign(new Error("test proxy discarded response"), {
        code: grpc.status.UNAVAILABLE,
        details: "test proxy discarded response",
      });
  }
  for (const name of ["KV", "Auth"]) {
    const handlers = {};
    const forwardingService = {};
    for (const [methodName, descriptor] of Object.entries(
      services[name].service,
    )) {
      const method = descriptor.path.split("/").at(-1);
      forwardingService[methodName] = {
        ...descriptor,
        requestDeserialize: (b) => b,
        responseSerialize: (b) => b,
      };
      handlers[methodName] = async (call, callback) => {
        const request = descriptor.requestDeserialize(call.request);
        const record = { id: ++serial, method, path: descriptor.path, request };
        records.push({ event: "request", ...record });
        let gate = rules.find((r) => matches(r, method, request));
        if (gate) rules.splice(rules.indexOf(gate), 1);
        const before =
          gate &&
          ["apply-before", "cleanup-before", "clock-next"].includes(
            gate.boundary,
          );
        try {
          if (before) await hold(gate, record);
          if (call.cancelled) return;
          const response = await new Promise((resolve, reject) => {
            const pending = client.makeUnaryRequest(
              descriptor.path,
              (b) => b,
              (b) => b,
              call.request,
              call.metadata,
              { deadline: call.getDeadline() },
              (error, value) => {
                calls.delete(pending);
                error ? reject(error) : resolve(value);
              },
            );
            calls.add(pending);
            call.once("cancelled", () => pending.cancel());
          });
          records.push({
            event: "response",
            rpc: record.id,
            method,
            response: descriptor.responseDeserialize(response),
          });
          for (const rule of rules) {
            if (
              rule.boundary === "clock-next" &&
              !rule.clockRead &&
              method === "Range" &&
              request.key.equals(Buffer.from(rule.key))
            )
              rule.clockRead = true;
          }
          if (gate && !before) await hold(gate, record);
          if (!call.cancelled) callback(null, response);
        } catch (error) {
          records.push({
            event: "error",
            rpc: record.id,
            code: error.code,
            message: error.message,
          });
          if (!call.cancelled) callback(error);
        }
      };
    }
    server.addService(forwardingService, handlers);
  }
  const port = await new Promise((resolve, reject) =>
    server.bindAsync(
      "127.0.0.1:0",
      grpc.ServerCredentials.createInsecure(),
      (e, p) => (e ? reject(e) : resolve(p)),
    ),
  );
  return {
    endpoint: `http://127.0.0.1:${port}`,
    records,
    arm,
    disconnect() {
      for (const gate of gates) gate.loseResponse();
      server.forceShutdown();
    },
    async close() {
      for (const gate of gates) gate.release();
      for (const call of calls) call.cancel();
      client.close();
      server.forceShutdown();
    },
  };
}
