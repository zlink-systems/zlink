// SPDX-License-Identifier: MPL-2.0

import * as fs from 'node:fs';
import * as path from 'node:path';

import type { NativeBinding } from './binding';

const LINUX_SONAME = 'libzlink.so.0';

export interface NativeLoadFailure {
  target: string;
  error: unknown;
}

export interface NativeLoadPaths {
  packageRoot: string;
  buildAddon: string;
  prebuiltDir: string;
  prebuiltAddon: string;
}

export function nativeLoadPaths(): NativeLoadPaths {
  const packageRoot = path.join(__dirname, '..', '..', '..', '..');
  const prebuiltDir = path.join(
    packageRoot,
    'prebuilds',
    `${process.platform}-${process.arch}`
  );
  return {
    packageRoot,
    buildAddon: path.join(packageRoot, 'build', 'Release', 'zlink.node'),
    prebuiltDir,
    prebuiltAddon: path.join(prebuiltDir, 'zlink.node')
  };
}

export function describeLoadFailure(failure: NativeLoadFailure): string {
  const message = failure.error instanceof Error
    ? failure.error.message
    : String(failure.error);
  return `${failure.target}: ${message}`;
}

export function prepareDevelopmentRuntimeLink(packageRoot: string): void {
  if (process.platform !== 'linux') {
    return;
  }
  const addonDir = path.join(packageRoot, 'build', 'Release');
  const coreDir = path.join(packageRoot, '..', '..', 'core', 'build', 'lib');
  const coreAltDir = path.join(packageRoot, '..', 'build_cpp', 'lib');
  const releaseDir = process.env.ZLINK_CORE_SOURCE === 'release'
    && process.env.ZLINK_CORE_PACKAGE_PREFIX
    ? path.join(process.env.ZLINK_CORE_PACKAGE_PREFIX, 'lib')
    : undefined;
  const localDir = process.env.ZLINK_CORE_SOURCE === 'local'
    ? process.env.ZLINK_CORE_LIB_DIR
    : undefined;
  const runtimeDirs = [localDir, releaseDir, coreDir, coreAltDir].filter(
    (entry): entry is string => entry !== undefined
  );
  refreshAddonRuntimeLink(
    path.join(addonDir, LINUX_SONAME),
    runtimeDirs.map((entry) => path.join(entry, LINUX_SONAME))
  );
  prependLibraryPath([...runtimeDirs, addonDir]);
}

export function preparePrebuiltRuntimePath(prebuiltDir: string): void {
  if (process.platform === 'linux') {
    const soname = path.join(prebuiltDir, LINUX_SONAME);
    if (!fs.existsSync(soname)) {
      const packageRoot = path.dirname(path.dirname(prebuiltDir));
      let versionedLibrary: string | undefined;
      try {
        const packageJson = JSON.parse(
          fs.readFileSync(path.join(packageRoot, 'package.json'), 'utf8')
        ) as { version?: string };
        if (packageJson.version !== undefined) {
          const candidate = path.join(
            prebuiltDir,
            `libzlink.so.${packageJson.version}`
          );
          if (fs.existsSync(candidate)) versionedLibrary = candidate;
        }
      } catch {
        // The normal package path has package.json. If it is unavailable,
        // leave native loading to the development fallback below.
      }
      if (versionedLibrary !== undefined) fs.symlinkSync(versionedLibrary, soname);
    }
    prependLibraryPath([prebuiltDir]);
    return;
  }
  if (process.platform !== 'win32') {
    return;
  }
  prependPathEntries([
    prebuiltDir,
    windowsRuntimePath(process.env.ZLINK_LIBRARY_PATH),
    process.env.ZLINK_CORE_PREFIX
      ? path.join(process.env.ZLINK_CORE_PREFIX, 'bin')
      : undefined,
    process.env.ZLINK_CORE_PREFIX,
    process.env.ZLINK_OPENSSL_BIN,
    process.env.OPENSSL_BIN,
    'C:\\Program Files\\OpenSSL-Win64\\bin',
    'C:\\Program Files\\Git\\mingw64\\bin',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\TeamFoundation\\Team Explorer\\Git\\mingw64\\bin'
  ]);
}

function windowsRuntimePath(entry: string | undefined): string | undefined {
  if (!entry) return undefined;
  try {
    if (fs.statSync(entry).isFile()) return path.dirname(entry);
  } catch {
    // Let the native loader report a missing runtime with its normal error.
  }
  return entry;
}

export function requireNativeAt(target: string): NativeBinding {
  return require(target) as NativeBinding;
}

function refreshAddonRuntimeLink(addonLib: string, sourceLibs: string[]): void {
  for (const sourceLib of sourceLibs) {
    if (!fs.existsSync(sourceLib)) continue;

    const sourceReal = fs.realpathSync(sourceLib);
    const currentReal = fs.existsSync(addonLib) ? fs.realpathSync(addonLib) : null;
    if (currentReal === sourceReal) return;

    fs.rmSync(addonLib, { force: true });
    fs.symlinkSync(sourceLib, addonLib);
    return;
  }
}

function prependLibraryPath(entries: string[]): void {
  const existing = (process.env.LD_LIBRARY_PATH || '').split(':').filter(Boolean);
  for (const entry of entries) {
    if (!existing.includes(entry)) existing.unshift(entry);
  }
  process.env.LD_LIBRARY_PATH = existing.join(':');
}

function prependPathEntries(entries: Array<string | undefined>): void {
  const existing = (process.env.PATH || '').split(';').filter(Boolean);
  for (const entry of entries) {
    if (!entry || !fs.existsSync(entry)) continue;
    if (!existing.includes(entry)) existing.unshift(entry);
  }
  process.env.PATH = existing.join(';');
}
