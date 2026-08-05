import { ZLinkHttpClient, type RawHttpResponse } from '@zlink-systems/http-client';

const defaultTimeoutMs = 3_000;

export async function getJson<T>(url: string): Promise<T>;
export async function getJson<T>(baseUrl: string, path: string): Promise<T>;
export async function getJson<T>(baseUrl: string, path?: string): Promise<T> {
  const target = requestTarget(baseUrl, path);
  return await ZLinkHttpClient.create(target.origin)
    .timeout(defaultTimeoutMs)
    .get(target.path)
    .fetch<T>();
}

export async function getJsonWithin<T>(baseUrl: string, path: string, timeoutMs: number): Promise<T> {
  const target = requestTarget(baseUrl, path);
  return await ZLinkHttpClient.create(target.origin)
    .timeout(timeoutMs)
    .get(target.path)
    .fetch<T>();
}

export async function postJson<T>(url: string, body: unknown): Promise<T>;
export async function postJson<T>(baseUrl: string, path: string, body?: unknown): Promise<T>;
export async function postJson<T>(baseUrl: string, pathOrBody: string | unknown, body?: unknown): Promise<T> {
  const hasSeparatePath = typeof pathOrBody === 'string' && pathOrBody.startsWith('/');
  const target = requestTarget(baseUrl, hasSeparatePath ? pathOrBody : undefined);
  const requestBody = hasSeparatePath ? body : pathOrBody;
  const request = ZLinkHttpClient.create(target.origin).timeout(defaultTimeoutMs).post(target.path);
  if (requestBody !== undefined) request.body(requestBody);
  return await request.fetch<T>();
}

export async function postJsonWithin<T>(
  baseUrl: string,
  path: string,
  body: unknown,
  timeoutMs: number,
): Promise<T> {
  const target = requestTarget(baseUrl, path);
  return await ZLinkHttpClient.create(target.origin)
    .timeout(timeoutMs)
    .post(target.path)
    .body(body)
    .fetch<T>();
}

export async function getStatus(url: string, timeoutMs = defaultTimeoutMs): Promise<number> {
  return (await rawRequest('GET', url, timeoutMs)).status;
}

export async function postStatus(url: string, timeoutMs = defaultTimeoutMs): Promise<number> {
  return (await rawRequest('POST', url, timeoutMs)).status;
}

async function rawRequest(method: 'GET' | 'POST', url: string, timeoutMs: number): Promise<RawHttpResponse> {
  const target = requestTarget(url);
  const client = ZLinkHttpClient.create(target.origin).timeout(timeoutMs);
  return await (method === 'GET' ? client.get(target.path) : client.post(target.path)).submitRaw();
}

function requestTarget(baseUrl: string, path?: string): { readonly origin: string; readonly path: string } {
  const base = new URL(baseUrl);
  const prefix = base.pathname === '/' ? '' : base.pathname.replace(/\/$/, '');
  const suffix = path === undefined ? '' : path.startsWith('/') ? path : `/${path}`;
  const url = path === undefined ? base : new URL(`${prefix}${suffix}`, base.origin);
  return { origin: url.origin, path: `${url.pathname}${url.search}` };
}
