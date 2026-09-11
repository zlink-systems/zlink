import path from 'node:path';

export function localNpmPackageRoots({
  repoRoot,
  configuredRoot,
  platform = process.platform
}) {
  if (configuredRoot !== undefined && configuredRoot !== '') {
    const root = path.resolve(configuredRoot);
    return [path.join(root, 'npm'), root];
  }

  return [path.join(
    repoRoot,
    '.artifacts',
    platform === 'win32' ? 'windows' : 'wsl',
    'npm'
  )];
}

export function localNpmPackageCandidates({ filename, ...options }) {
  return localNpmPackageRoots(options).map((root) => path.join(root, filename));
}
