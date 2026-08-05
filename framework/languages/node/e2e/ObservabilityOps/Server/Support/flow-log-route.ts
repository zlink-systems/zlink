import fs from 'node:fs';
import path from 'node:path';
import type { HttpRoute } from './http-server';

export function createFlowLogRoute(logDir: string, rid: string): HttpRoute {
  const file = path.join(logDir, `${rid}-flow.log`);
  return {
    method: 'GET',
    path: '/flow-log',
    handle: () => ({ content: fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '' })
  };
}
