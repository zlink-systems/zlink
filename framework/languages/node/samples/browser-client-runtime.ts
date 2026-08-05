type BrowserSampleStatus = 'running' | 'passed' | 'failed';

type BrowserSampleResult = {
  sample: string;
  status: BrowserSampleStatus;
  error?: string;
};

declare global {
  interface Window {
    __zlinkSampleResult?: BrowserSampleResult;
  }
}

async function loadBrowserConfig<T>(): Promise<T> {
  const response = await fetch('/config.json', { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`Browser sample config failed with HTTP ${response.status}.`);
  }
  return await response.json() as T;
}

async function runBrowserSample(sample: string, scenario: () => Promise<void>): Promise<void> {
  window.__zlinkSampleResult = { sample, status: 'running' };
  try {
    await scenario();
    window.__zlinkSampleResult = { sample, status: 'passed' };
    console.log(`PASS ${sample}`);
  } catch (error) {
    const message = error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    window.__zlinkSampleResult = { sample, status: 'failed', error: message };
    console.error(error);
  }
}

interface BrowserHttpClient {
  post(path: string): BrowserHttpRequest;
  close(): Promise<void>;
}

interface BrowserHttpRequest {
  body(value: unknown): BrowserHttpRequest;
  timeout(timeoutMs: number): BrowserHttpRequest;
  fetch<T>(): Promise<T>;
  submitRaw(): Promise<{ status: number; body: Uint8Array }>;
}

class BrowserHttpClientFactory {
  static create(baseUrl: string): BrowserHttpClientBuilder {
    return new BrowserHttpClientBuilder(baseUrl);
  }
}

class BrowserHttpClientBuilder {
  private timeoutMs = 30_000;

  constructor(private readonly baseUrl: string) {}

  timeout(timeoutMs: number): this {
    this.timeoutMs = timeoutMs;
    return this;
  }

  build(): BrowserHttpClient {
    return new DefaultBrowserHttpClient(this.baseUrl, this.timeoutMs);
  }
}

class DefaultBrowserHttpClient implements BrowserHttpClient {
  constructor(
    private readonly baseUrl: string,
    private readonly timeoutMs: number
  ) {}

  post(path: string): BrowserHttpRequest {
    return new DefaultBrowserHttpRequest(this.resolve(path), this.timeoutMs);
  }

  async close(): Promise<void> {}

  private resolve(path: string): string {
    const base = this.baseUrl.endsWith('/') ? this.baseUrl : `${this.baseUrl}/`;
    return new URL(path.replace(/^\//, ''), new URL(base, window.location.origin)).toString();
  }
}

class DefaultBrowserHttpRequest implements BrowserHttpRequest {
  private requestBody: unknown;

  constructor(
    private readonly url: string,
    private timeoutMs: number
  ) {}

  body(value: unknown): this {
    this.requestBody = value;
    return this;
  }

  timeout(timeoutMs: number): this {
    this.timeoutMs = timeoutMs;
    return this;
  }

  async fetch<T>(): Promise<T> {
    const response = await this.send();
    const text = await response.text();
    return (text.length === 0 ? undefined : JSON.parse(text)) as T;
  }

  async submitRaw(): Promise<{ status: number; body: Uint8Array }> {
    const response = await this.send();
    return { status: response.status, body: new Uint8Array(await response.arrayBuffer()) };
  }

  private async send(): Promise<Response> {
    const response = await fetch(this.url, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: this.requestBody === undefined ? undefined : JSON.stringify(this.requestBody),
      signal: AbortSignal.timeout(this.timeoutMs)
    });
    if (!response.ok) {
      const text = await response.text();
      throw new Error(`HTTP ${response.status} from ${this.url}: ${text}`);
    }
    return response;
  }
}

export {
  BrowserHttpClientFactory,
  loadBrowserConfig,
  runBrowserSample
};

export type {
  BrowserHttpClient,
  BrowserSampleResult
};
