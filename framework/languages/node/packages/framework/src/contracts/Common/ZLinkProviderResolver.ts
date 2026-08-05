import type { Type } from './CoreTypes';

export interface ZLinkProviderResolver {
  get?<T>(type: Type<T>): T | undefined;
  create?<T>(type: Type<T>): T | Promise<T>;
}
