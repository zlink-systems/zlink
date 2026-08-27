import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

export const sampleName = 'ShoppingMall.Ts';

const waitAttempts = Number.parseInt(process.env.SHOPPINGMALL_WAIT_ATTEMPTS ?? '300', 10);
const waitIntervalMs = 100;

export async function runSample(ctx) {
  if (waitAttempts !== 300) {
    throw new Error('ShoppingMall evidence waits require exactly 300 attempts.');
  }
  const sample = {
    apiAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    apiBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowAHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowBHttpUrl: `http://127.0.0.1:${await ctx.port()}`,
    workflowAChannelEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBChannelEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowASpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBSpotEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowASpotPubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    workflowBSpotPubEndpoint: `tcp://127.0.0.1:${await ctx.port()}`,
    redisEndpoint: ctx.redisEndpoint,
    redisKeyPrefix: `shoppingmall:node:${process.pid}:`,
    logDir: ctx.logDir,
    workDir: ctx.workDir
  };
  const roleConfig = (name, keys) => ctx.writeConfig(name,
    Object.fromEntries(keys.map((key) => [key, sample[key]])));
  const workflows = [
    ['workflow-a', 'dist/Server/WorkflowA/main.js', sample.workflowAHttpUrl,
      roleConfig('workflow-a', ['workflowAHttpUrl', 'workflowAChannelEndpoint', 'workflowASpotEndpoint', 'workflowASpotPubEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['workflow-b', 'dist/Server/WorkflowB/main.js', sample.workflowBHttpUrl,
      roleConfig('workflow-b', ['workflowBHttpUrl', 'workflowBChannelEndpoint', 'workflowBSpotEndpoint', 'workflowBSpotPubEndpoint', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ];
  for (const [role, entry, _ready, configPath] of workflows) {
    await ctx.start(role, entry, ['--config', configPath]);
  }
  for (const [, , ready] of workflows) await ctx.waitHttp(ready);

  const apis = [
    ['api-a', 'dist/Server/ApiA/main.js', sample.apiAHttpUrl,
      roleConfig('api-a', ['apiAHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])],
    ['api-b', 'dist/Server/ApiB/main.js', sample.apiBHttpUrl,
      roleConfig('api-b', ['apiBHttpUrl', 'redisEndpoint', 'redisKeyPrefix', 'logDir', 'workDir'])]
  ];
  for (const [role, entry, _ready, configPath] of apis) {
    await ctx.start(role, entry, ['--config', configPath]);
  }
  for (const [, , ready] of apis) await ctx.waitHttp(ready);

  await waitLogCount(ctx, 'api-a', 'shoppingmall-ready kind=http node=api-a', 1);
  await waitLogCount(ctx, 'api-b', 'shoppingmall-ready kind=http node=api-b', 1);
  for (const api of ['api-a', 'api-b']) {
    for (const workflow of ['workflow-a', 'workflow-b']) {
      await waitLogCount(ctx, api, `shoppingmall-ready kind=object-route node=${api} target=${workflow}`, 1);
    }
  }

  await postJson(sample.apiAHttpUrl, '/self-check/seed', {});
  await startWorkflowWitnesses(ctx, sample.apiAHttpUrl);

  const pendingKey = `runner-pending-${process.pid}`;
  const pending = await postJson(sample.apiAHttpUrl, '/self-check/idempotency/pending', request('cart-success', 'addr-office', 'pm-ok', pendingKey));
  const resumed = await postJson(sample.apiAHttpUrl, '/self-check/workflow/inventory-reserved', request('cart-success', 'addr-home', 'pm-ok', `runner-resume-${process.pid}`));
  const interrupted = await postJson(sample.apiAHttpUrl, '/self-check/workflow/inventory-effect', request('cart-success', 'addr-home', 'pm-ok', `runner-interrupted-${process.pid}`));
  const rebuilt = await postJson(sample.apiAHttpUrl, '/orders/start', request('cart-success', 'addr-office', 'pm-ok', `runner-rebuild-${process.pid}`));
  await waitForOrder(sample.apiAHttpUrl, rebuilt.state.orderId, 'Confirmed');
  await postJson(sample.apiAHttpUrl, `/self-check/projection/${encodeURIComponent(rebuilt.state.orderId)}/delete`, {});

  const clientLog = runClient(ctx, sample, {
    pendingOrderId: requireString(pending.orderId, 'Pending fixture orderId'),
    pendingIdempotencyKey: pendingKey,
    resumedOrderId: requireString(resumed.state?.orderId, 'Resume fixture orderId'),
    interruptedOrderId: requireString(interrupted.state?.orderId, 'Interrupted fixture orderId'),
    rebuiltOrderId: requireString(rebuilt.state?.orderId, 'Rebuild fixture orderId')
  });
  requireExactLine(clientLog, 'shoppingmall=completed');

  const orders = {
    successfulOrderId: clientOrder(clientLog, 'success'),
    pendingRecoveredOrderId: clientOrder(clientLog, 'pending'),
    concurrentOrderId: clientOrder(clientLog, 'concurrent'),
    resumedOrderId: clientOrder(clientLog, 'resumed'),
    interruptedOrderId: clientOrder(clientLog, 'interrupted'),
    inventoryFailureOrderId: clientOrder(clientLog, 'inventory-failure'),
    paymentFailureOrderId: clientOrder(clientLog, 'payment-failure'),
    scaleOutOrderIds: [clientOrder(clientLog, 'scale-a'), clientOrder(clientLog, 'scale-b')]
  };
  const assertion = await postJson(sample.apiAHttpUrl, '/self-check/assert', orders);
  if (assertion.passed !== true) throw new Error('ShoppingMall server evidence assertion failed.');

  const checkpoint = await postJson(
    sample.apiAHttpUrl,
    '/self-check/workflow/relocation-checkpoint',
    request('cart-success', 'addr-office', 'pm-ok', `runner-relocation-${process.pid}`)
  );
  const checkpointOrderId = requireString(checkpoint.state?.orderId, 'Relocation checkpoint orderId');
  const checkpointGeneration = requireString(checkpoint.objectGeneration, 'Relocation checkpoint ObjectGeneration');
  const relocationSource = await findRelocationSource(ctx, checkpointOrderId);
  const relocationEndpoint = relocationSource === 'workflow-a' ? sample.workflowAHttpUrl : sample.workflowBHttpUrl;
  const relocation = await postJson(relocationEndpoint, '/self-check/relocate', {});
  if (relocation.outcome !== 'relocated') {
    throw new Error(`Planned relocation was not completed (outcome=${String(relocation.outcome)}).`);
  }
  await continueAfterRelocation(sample.apiAHttpUrl, checkpointOrderId);

  await waitLogAtLeast(ctx, 'workflow-a', 'shoppingmall-order started order=', 1);
  await waitLogAtLeast(ctx, 'workflow-b', 'shoppingmall-order started order=', 1);
  await waitLogAtLeast(ctx, 'api-a', 'shoppingmall-evidence order=', 1);
  await waitCombinedLogCount(ctx, ['workflow-a', 'workflow-b'],
    `shoppingmall-order replayed order=${checkpointOrderId} generation=${checkpointGeneration}`, 1);
  await waitCombinedLogCount(ctx, ['workflow-a', 'workflow-b'],
    `shoppingmall-order external-effect-repeated order=${checkpointOrderId}`, 0);
  console.log('shoppingmall-placement=completed');
}

function runClient(ctx, sample, fixtures) {
  const result = spawnSync(process.execPath, [path.join(ctx.sampleRoot, 'dist/Client/main.js'),
    '--api-a-http', sample.apiAHttpUrl,
    '--api-b-http', sample.apiBHttpUrl,
    '--pending-order', fixtures.pendingOrderId,
    '--pending-idempotency-key', fixtures.pendingIdempotencyKey,
    '--resumed-order', fixtures.resumedOrderId,
    '--interrupted-order', fixtures.interruptedOrderId,
    '--rebuilt-order', fixtures.rebuiltOrderId
  ], {
    cwd: ctx.sampleRoot,
    env: ctx.env,
    encoding: 'utf8'
  });
  if (result.error) throw result.error;
  const output = `${result.stdout ?? ''}${result.stderr ?? ''}`;
  fs.writeFileSync(path.join(ctx.logDir, 'client.log'), output, { mode: 0o600 });
  process.stdout.write(output);
  if (result.status !== 0) throw new Error(`ShoppingMall client exited with ${result.status}.`);
  return output;
}

async function startWorkflowWitnesses(ctx, apiUrl) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (logCount(ctx, 'workflow-a', 'shoppingmall-order started order=') >= 1
      && logCount(ctx, 'workflow-b', 'shoppingmall-order started order=') >= 1) return;
    await postJson(apiUrl, '/orders/start', request(
      'cart-success', 'addr-home', 'pm-ok', `runner-witness-${process.pid}-${attempt}`
    ));
    await delay(waitIntervalMs);
  }
  throw new Error('ShoppingMall workflow start evidence did not reach both Workflow nodes.');
}

async function findRelocationSource(ctx, orderId) {
  const marker = `shoppingmall-order started order=${orderId} spot=`;
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (logCount(ctx, 'workflow-a', marker) >= 1) return 'workflow-a';
    if (logCount(ctx, 'workflow-b', marker) >= 1) return 'workflow-b';
    await delay(waitIntervalMs);
  }
  throw new Error(`Could not identify the Workflow source for '${orderId}'.`);
}

async function waitForOrder(apiUrl, orderId, expectedStatus) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    try {
      const response = await fetch(new URL(`/orders/${encodeURIComponent(orderId)}`, apiUrl), {
        signal: AbortSignal.timeout(5_000)
      });
      if (response.ok && (await response.json()).state?.status === expectedStatus) return;
    } catch {
      // A bounded public-state poll observes the asynchronous workflow completion.
    }
    await delay(waitIntervalMs);
  }
  throw new Error(`Order '${orderId}' did not reach '${expectedStatus}'.`);
}

async function continueAfterRelocation(apiUrl, orderId) {
  let lastFailure = 'no response';
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    try {
      const response = await fetch(new URL(`/orders/${encodeURIComponent(orderId)}/continue`, apiUrl), {
        method: 'POST',
        signal: AbortSignal.timeout(5_000)
      });
      const payload = await response.json();
      if (response.ok && payload.state?.status === 'Confirmed') return;
      lastFailure = response.ok
        ? `unexpected state ${String(payload.state?.status)}`
        : JSON.stringify(payload);
    } catch (error) {
      lastFailure = error instanceof Error ? error.message : String(error);
    }
    await delay(waitIntervalMs);
  }
  throw new Error(`Relocated order '${orderId}' did not resume on its target: ${lastFailure}.`);
}

async function postJson(apiUrl, route, body) {
  const response = await fetch(new URL(route, apiUrl), {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(30_000)
  });
  const payload = await response.json();
  if (!response.ok) throw new Error(`POST ${route} failed: ${JSON.stringify(payload)}`);
  return payload;
}

async function waitLogCount(ctx, role, marker, expected) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (logCount(ctx, role, marker) === expected) return;
    await delay(waitIntervalMs);
  }
  throw new Error(`Timed out waiting for ${expected} '${marker}' in ${role}.`);
}

async function waitLogAtLeast(ctx, role, marker, expected) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (logCount(ctx, role, marker) >= expected) return;
    await delay(waitIntervalMs);
  }
  throw new Error(`Timed out waiting for ${expected}+ '${marker}' in ${role}.`);
}

async function waitCombinedLogCount(ctx, roles, marker, expected) {
  for (let attempt = 0; attempt < waitAttempts; attempt += 1) {
    if (roles.reduce((total, role) => total + logCount(ctx, role, marker), 0) === expected) return;
    await delay(waitIntervalMs);
  }
  throw new Error(`Timed out waiting for ${expected} '${marker}' across ${roles.join('/')}.`);
}

function logCount(ctx, role, marker) {
  const file = path.join(ctx.logDir, `${role}.log`);
  if (!fs.existsSync(file)) return 0;
  return fs.readFileSync(file, 'utf8').split(marker).length - 1;
}

function request(cartId, shippingAddressId, paymentMethodId, idempotencyKey) {
  return { cartId, shippingAddressId, paymentMethodId, idempotencyKey };
}

function clientOrder(output, name) {
  const match = output.match(new RegExp(`^shoppingmall-client-order name=${name} order=(\\S+)$`, 'm'));
  if (match === null) throw new Error(`Client did not report the '${name}' order ID.`);
  return match[1];
}

function requireString(value, description) {
  if (typeof value !== 'string' || value.length === 0) throw new Error(`${description} is required.`);
  return value;
}

function requireExactLine(output, line) {
  if (!output.split(/\r?\n/).includes(line)) throw new Error(`Client did not emit '${line}'.`);
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
