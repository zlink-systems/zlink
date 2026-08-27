interface ShoppingMallSampleConfig {
  apiAHttpUrl: string;
  apiBHttpUrl: string;
  pendingOrderId: string;
  pendingIdempotencyKey: string;
  resumedOrderId: string;
  interruptedOrderId: string;
  relocatedOrderId?: string;
  rebuiltOrderId: string;
}

function loadSampleConfig(): ShoppingMallSampleConfig {
  return {
    apiAHttpUrl: requireOption('--api-a-http'),
    apiBHttpUrl: requireOption('--api-b-http'),
    pendingOrderId: requireOption('--pending-order'),
    pendingIdempotencyKey: requireOption('--pending-idempotency-key'),
    resumedOrderId: requireOption('--resumed-order'),
    interruptedOrderId: requireOption('--interrupted-order'),
    relocatedOrderId: optionalOption('--relocated-order'),
    rebuiltOrderId: requireOption('--rebuilt-order')
  };
}

function requireOption(name: string): string {
  const index = process.argv.indexOf(name);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (value === undefined || value.startsWith('--')) throw new Error(`${name} <url> is required.`);
  return value;
}

function optionalOption(name: string): string | undefined {
  const index = process.argv.indexOf(name);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (value === undefined || value.startsWith('--')) return undefined;
  return value;
}

export { loadSampleConfig };
export type { ShoppingMallSampleConfig };
