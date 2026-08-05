import { createRequire } from 'node:module';

type ZLinkBindingModule = {
  version(): [number, number, number];
};

const requireBinding = createRequire(__filename);

export function loadBinding(): ZLinkBindingModule {
  return requireBinding('@zlink-systems/zlink') as ZLinkBindingModule;
}

export interface ZLinkNodeBindingInfo {
  readonly version: readonly [number, number, number];
}

export function getNodeBindingInfo(): ZLinkNodeBindingInfo {
  return { version: loadBinding().version() };
}
