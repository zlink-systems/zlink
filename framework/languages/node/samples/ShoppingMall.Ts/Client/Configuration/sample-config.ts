interface ShoppingMallSampleConfig {
  apiAHttpUrl: string;
  apiBHttpUrl: string;
}

function loadSampleConfig(): ShoppingMallSampleConfig {
  return {
    apiAHttpUrl: requireOption('--api-a-http'),
    apiBHttpUrl: requireOption('--api-b-http')
  };
}

function requireOption(name: string): string {
  const index = process.argv.indexOf(name);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (value === undefined || value.startsWith('--')) throw new Error(`${name} <url> is required.`);
  return value;
}

export { loadSampleConfig };
export type { ShoppingMallSampleConfig };
