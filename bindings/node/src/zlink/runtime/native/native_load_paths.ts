// SPDX-License-Identifier: MPL-2.0

import * as fs from 'node:fs';
import * as path from 'node:path';

import type { NativeBinding } from './binding';

const LINUX_SONAME = 'libzlink.so.11';

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
  refreshAddonRuntimeLink(path.join(addonDir, LINUX_SONAME), [
    path.join(coreDir, LINUX_SONAME),
    path.join(coreAltDir, LINUX_SONAME)
  ]);
  prependLibraryPath([coreDir, coreAltDir, addonDir]);
}

export function preparePrebuiltRuntimePath(prebuiltDir: string): void {
  if (process.platform !== 'win32') {
    return;
  }
  prependPathEntries([
    prebuiltDir,
    process.env.ZLINK_OPENSSL_BIN,
    process.env.OPENSSL_BIN,
    'C:\\Program Files\\OpenSSL-Win64\\bin',
    'C:\\Program Files\\Git\\mingw64\\bin',
    'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\TeamFoundation\\Team Explorer\\Git\\mingw64\\bin'
  ]);
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
