import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';

export async function expectStartupFailure(
  mainPath: string,
  args: readonly string[],
  logDir: string,
  name: string
): Promise<string> {
  fs.mkdirSync(logDir, { recursive: true });
  const child = spawn(process.execPath, [mainPath, ...args], { stdio: ['ignore', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  child.stdout?.on('data', (chunk) => { stdout += Buffer.from(chunk).toString('utf8'); });
  child.stderr?.on('data', (chunk) => { stderr += Buffer.from(chunk).toString('utf8'); });
  const exitCode = await new Promise<number | null>((resolve) => {
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      resolve(null);
    }, 10_000);
    child.once('exit', (code) => {
      clearTimeout(timer);
      resolve(code);
    });
  });
  fs.writeFileSync(path.join(logDir, `${name}.stdout.log`), stdout);
  fs.writeFileSync(path.join(logDir, `${name}.stderr.log`), stderr);
  if (exitCode === null || exitCode === 0) {
    throw new Error(`RC-A6 expected '${name}' server startup to fail.`);
  }
  return `${stdout}\n${stderr}`;
}
