import fs from 'node:fs';
import { localNpmPackageCandidates } from './local-package-root.mjs';

export function localBindingArchiveCandidates({
  repoRoot,
  version,
  configuredRoot,
  platform = process.platform
}) {
  const filename = `zlink-systems-zlink-${version}.tgz`;
  return localNpmPackageCandidates({
    repoRoot,
    configuredRoot,
    platform,
    filename
  });
}

export function findLocalBindingArchive(options) {
  const candidates = localBindingArchiveCandidates(options);
  const archive = candidates.find((candidate) => fs.existsSync(candidate));
  if (archive !== undefined) return archive;
  throw new Error(
    `Local bindings package was not found. Build it with the platform local-package publisher `
    + `or set ZLINK_LOCAL_PACKAGE_ROOT. Checked: ${candidates.join(', ')}`
  );
}
