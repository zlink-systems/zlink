import type {
  Type,
  ZLinkSession,
  ZLinkSessionFactory
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { DefaultZLinkSessionContext } from './session-context';

export async function createStreamSessionInstance(
  type: Type<ZLinkSession> | Type<ZLinkSessionFactory>,
  resolver: ZLinkProviderResolver | undefined,
  context: DefaultZLinkSessionContext,
  handlerTypes: readonly Type[] = []
): Promise<ZLinkSession> {
  for (const handlerType of handlerTypes) {
    context.handlers.addHandler(handlerType);
  }
  const created = await createProviderInstance<ZLinkSession | ZLinkSessionFactory>(
    type as Type<ZLinkSession | ZLinkSessionFactory>,
    resolver,
    context
  );
  if (isSessionFactory(created)) {
    return await created.create(context);
  }
  return created as ZLinkSession;
}

async function createProviderInstance<T>(
  type: Type<T>,
  resolver: ZLinkProviderResolver | undefined,
  fallbackArg?: unknown
): Promise<T> {
  const existing = resolver?.get?.(type);
  if (existing !== undefined) {
    return existing;
  }
  const created = await resolver?.create?.(type);
  if (created !== undefined) {
    return created;
  }
  return fallbackArg === undefined
    ? new (type as new () => T)()
    : new (type as new (arg: unknown) => T)(fallbackArg);
}

function isSessionFactory(value: unknown): value is ZLinkSessionFactory {
  return typeof (value as { create?: unknown }).create === 'function'
    && (value as { context?: unknown }).context === undefined;
}
