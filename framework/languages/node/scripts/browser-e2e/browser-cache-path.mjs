import path from 'node:path';
import { fileURLToPath } from 'node:url';

const workspaceRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

export function browserCachePath(environment = process.env) {
  return environment.PLAYWRIGHT_BROWSERS_PATH ?? path.join(workspaceRoot, '.cache/ms-playwright');
}
