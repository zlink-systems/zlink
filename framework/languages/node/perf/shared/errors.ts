import { ZLinkFrameworkErrorKind } from '@zlink-systems/framework';
import { PerfValidationException } from './contracts';

export interface ClassifiedError {
  namespace: 'byKind' | 'harness' | 'language';
  key: string;
  category: 'failed' | 'timeout' | 'cancelled';
  publicKind: string | null;
  harnessKind: string | null;
  connectorCode: string | null;
}

export function classifyError(error: unknown): ClassifiedError {
  if (error instanceof PerfValidationException) {
    return { namespace: 'harness', key: error.kind, category: error.kind === 'CorrelationExpired' ? 'timeout' : 'failed', publicKind: null, harnessKind: error.kind, connectorCode: null };
  }
  const shaped = error as { name?: string; kind?: unknown; error?: { code?: unknown } };
  if (error instanceof Error && shaped.name === 'ZLinkFrameworkException' && typeof shaped.kind === 'number') {
    const kind = ZLinkFrameworkErrorKind[shaped.kind] ?? `Unknown(${shaped.kind})`;
    return { namespace: 'byKind', key: kind, category: kind === 'DeadlineExceeded' ? 'timeout' : 'failed', publicKind: kind, harnessKind: null, connectorCode: null };
  }
  const name = error instanceof Error ? (error.constructor?.name || error.name) : typeof error;
  if (error instanceof Error && shaped.name === 'ZlinkStreamException' && typeof shaped.error?.code === 'string') {
    return { namespace: 'language', key: name, category: shaped.error.code === 'requestTimeout' ? 'timeout' : 'failed', publicKind: null, harnessKind: null, connectorCode: shaped.error.code };
  }
  const cancelled = error instanceof Error && error.name === 'AbortError';
  const timedOut = error instanceof Error && error.name === 'TimeoutError';
  return { namespace: 'language', key: name, category: cancelled ? 'cancelled' : timedOut ? 'timeout' : 'failed', publicKind: null, harnessKind: null, connectorCode: null };
}
