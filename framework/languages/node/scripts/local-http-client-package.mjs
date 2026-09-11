import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { localNpmPackageCandidates } from './local-package-root.mjs';

const HTTP_CLIENT_PACKAGE_NAME = '@zlink-systems/http-client';

export function localHttpClientArchiveCandidates({
  repoRoot,
  version,
  configuredRoot,
  platform = process.platform
}) {
  return localNpmPackageCandidates({
    repoRoot,
    configuredRoot,
    platform,
    filename: httpClientArchiveFilename(version)
  });
}

export function stagedHttpClientArchive({ repoRoot, version }) {
  return path.join(
    repoRoot,
    '.artifacts',
    'node-install',
    'npm',
    httpClientArchiveFilename(version)
  );
}

export function materializeLocalHttpClientArchive({
  repoRoot,
  version,
  configuredRoot,
  platform = process.platform
}) {
  const destination = stagedHttpClientArchive({ repoRoot, version });
  const candidates = localHttpClientArchiveCandidates({
    repoRoot,
    version,
    configuredRoot,
    platform
  });
  const source = candidates.find((candidate) => fs.existsSync(candidate));

  if (source === undefined) {
    removeFile(destination);
    throw new Error(
      `Local HTTP client package was not found. Build it with the platform local-package publisher `
      + `or set ZLINK_LOCAL_PACKAGE_ROOT. Checked: ${candidates.join(', ')}`
    );
  }

  const sourcePath = path.resolve(source);
  const destinationPath = path.resolve(destination);
  try {
    assertHttpClientArchive(sourcePath, version);
    if (samePath(sourcePath, destinationPath)) return destinationPath;

    fs.mkdirSync(path.dirname(destinationPath), { recursive: true });
    const temporary = `${destinationPath}.${process.pid}.${crypto.randomUUID()}.tmp`;
    try {
      fs.copyFileSync(sourcePath, temporary);
      assertHttpClientArchive(temporary, version);
      const sourceHash = sha256(sourcePath);
      if (sha256(temporary) !== sourceHash) {
        throw new Error('Staged HTTP client package does not match the publisher output.');
      }
      fs.renameSync(temporary, destinationPath);
      if (sha256(destinationPath) !== sourceHash) {
        throw new Error('Materialized HTTP client package does not match the publisher output.');
      }
    } finally {
      removeFile(temporary);
    }
  } catch (error) {
    removeFile(destinationPath);
    throw error;
  }

  return destinationPath;
}

export function readPackageManifestFromNpmArchive(archive) {
  const compressed = fs.readFileSync(archive);
  if (compressed.length === 0) throw new Error(`Local HTTP client package is empty: ${archive}`);

  let tar;
  try {
    tar = zlib.gunzipSync(compressed);
  } catch (error) {
    throw new Error(`Local HTTP client package is not a valid gzip archive: ${archive}`, { cause: error });
  }

  for (let offset = 0; offset + 512 <= tar.length;) {
    const header = tar.subarray(offset, offset + 512);
    if (header.every((value) => value === 0)) break;
    const name = readTarString(header, 0, 100);
    const prefix = readTarString(header, 345, 155);
    const entry = prefix === '' ? name : `${prefix}/${name}`;
    const sizeText = readTarString(header, 124, 12).trim();
    const size = sizeText === '' ? 0 : Number.parseInt(sizeText, 8);
    if (!Number.isSafeInteger(size) || size < 0) {
      throw new Error(`Local HTTP client package has an invalid tar entry size: ${archive}`);
    }

    const contentOffset = offset + 512;
    const nextOffset = contentOffset + Math.ceil(size / 512) * 512;
    if (nextOffset > tar.length) {
      throw new Error(`Local HTTP client package has a truncated tar entry: ${archive}`);
    }
    if (entry === 'package/package.json') {
      try {
        return JSON.parse(tar.subarray(contentOffset, contentOffset + size).toString('utf8'));
      } catch (error) {
        throw new Error(`Local HTTP client package has an invalid package.json: ${archive}`, { cause: error });
      }
    }
    offset = nextOffset;
  }

  throw new Error(`Local HTTP client package does not contain package/package.json: ${archive}`);
}

function assertHttpClientArchive(archive, version) {
  const stats = fs.statSync(archive);
  if (!stats.isFile() || stats.size === 0) {
    throw new Error(`Local HTTP client package is not a non-empty file: ${archive}`);
  }
  const manifest = readPackageManifestFromNpmArchive(archive);
  if (manifest.name !== HTTP_CLIENT_PACKAGE_NAME) {
    throw new Error(
      `Local HTTP client package name is ${String(manifest.name)}; expected ${HTTP_CLIENT_PACKAGE_NAME}.`
    );
  }
  if (manifest.version !== version) {
    throw new Error(
      `Local HTTP client package version is ${String(manifest.version)}; expected ${version}.`
    );
  }
}

function httpClientArchiveFilename(version) {
  return `zlink-systems-http-client-${version}.tgz`;
}

function readTarString(buffer, offset, length) {
  const end = buffer.indexOf(0, offset);
  const boundedEnd = end === -1 || end > offset + length ? offset + length : end;
  return buffer.subarray(offset, boundedEnd).toString('utf8');
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function samePath(left, right) {
  return process.platform === 'win32'
    ? left.toLowerCase() === right.toLowerCase()
    : left === right;
}

function removeFile(file) {
  fs.rmSync(file, { force: true });
}
