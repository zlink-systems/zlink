// SPDX-License-Identifier: MPL-2.0

import type { NativeBinding } from './binding';
import {
  describeLoadFailure,
  nativeLoadPaths,
  prepareDevelopmentRuntimeLink,
  preparePrebuiltRuntimePath,
  requireNativeAt,
  type NativeLoadFailure
} from './native_load_paths';

function loadNative(): NativeBinding {
  const paths = nativeLoadPaths();
  const failures: NativeLoadFailure[] = [];
  try {
    prepareDevelopmentRuntimeLink(paths.packageRoot);
    return requireNativeAt(paths.buildAddon);
  } catch (error) {
    failures.push({ target: paths.buildAddon, error });
    try {
      preparePrebuiltRuntimePath(paths.prebuiltDir);
      return requireNativeAt(paths.prebuiltAddon);
    } catch (error) {
      failures.push({ target: paths.prebuiltAddon, error });
    }
  }
  throw new Error([
    'zlink native addon not found. Build with node-gyp.',
    ...failures.map((failure) => `- ${describeLoadFailure(failure)}`)
  ].join('\n'));
}

let native: NativeBinding | null = null;
let loadError: Error | null = null;
try {
  native = loadNative();
} catch (error) {
  loadError = error instanceof Error ? error : new Error(String(error));
}

export function requireNative(): NativeBinding {
  if (!native) throw loadError ?? new Error('zlink native addon not found. Build with node-gyp.');
  return native;
}
