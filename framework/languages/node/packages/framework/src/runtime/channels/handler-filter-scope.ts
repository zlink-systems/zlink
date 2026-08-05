import type {
  Type,
  ZLinkHandlerFilterContext,
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';

export interface ZLinkHandlerFilterScopeResolver {
  resolve<T>(type: Type<T>): Promise<T>;
}

export type ZLinkHandlerFilterScopeRunner = <T>(
  context: ZLinkHandlerFilterContext,
  callback: (resolver: ZLinkHandlerFilterScopeResolver) => Promise<T>
) => Promise<T>;

const handlerFilterScopes = new WeakMap<ZLinkProviderResolver, ZLinkHandlerFilterScopeRunner>();

export function registerHandlerFilterScope(
  providerResolver: ZLinkProviderResolver,
  runner: ZLinkHandlerFilterScopeRunner
): void {
  handlerFilterScopes.set(providerResolver, runner);
}

export function handlerFilterScope(
  providerResolver: ZLinkProviderResolver | undefined
): ZLinkHandlerFilterScopeRunner | undefined {
  return providerResolver === undefined ? undefined : handlerFilterScopes.get(providerResolver);
}
