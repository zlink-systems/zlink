// SPDX-License-Identifier: MPL-2.0

import * as fs from 'node:fs';
import * as path from 'node:path';

import type { NativeBinding } from './binding';

function linuxSoname(abiSoversion: string): string {
  return `libzlink.so.${abiSoversion}`;
}

function abiSoversionFromProvenance(abiMajor: unknown): string {
  if (typeof abiMajor !== 'number'
      || !Number.isSafeInteger(abiMajor)
      || abiMajor < 0) {
    throw new Error(`Invalid Core ABI major: ${String(abiMajor)}`);
  }
  return String(abiMajor);
}

function sourceTreeAbiSoversion(packageRoot: string): string {
  const version = fs.readFileSync(
    path.join(packageRoot, '..', '..', 'VERSION'),
    'utf8'
  );
  const match = /^LIBZLINK_ABI_SOVERSION=(0|[1-9]\d*)$/m.exec(version);
  if (!match) {
    throw new Error('Root VERSION has no valid LIBZLINK_ABI_SOVERSION');
  }
  return match[1];
}

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
  const soname = linuxSoname(sourceTreeAbiSoversion(packageRoot));
  refreshAddonRuntimeLink(
    path.join(addonDir, soname),
    runtimeDirs.map((entry) => path.join(entry, soname))
  );
  prependLibraryPath([...runtimeDirs, addonDir]);
}

export function preparePrebuiltRuntimePath(prebuiltDir: string): void {
  if (process.platform === 'linux') {
    const packageRoot = path.dirname(path.dirname(prebuiltDir));
    try {
      const provenance = JSON.parse(
        fs.readFileSync(
          path.join(packageRoot, 'provenance', 'core-package-provenance.json'),
          'utf8'
        )
      ) as { version?: string; abiMajor?: unknown };
      if (provenance.version !== undefined) {
        const soname = path.join(
          prebuiltDir,
          linuxSoname(abiSoversionFromProvenance(provenance.abiMajor))
        );
        const versionedLibrary = path.join(prebuiltDir, `libzlink.so.${provenance.version}`);
        if (!fs.existsSync(soname) && fs.existsSync(versionedLibrary)) {
          fs.symlinkSync(versionedLibrary, soname);
        }
      }
    } catch {
      // The normal package path has Core provenance. If it is unavailable,
      // leave native loading to the development fallback below.
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
