const fs = require("node:fs");
const { spawn } = require("node:child_process");

async function runProviderBuild(command, args, cwd, logPath) {
  const output = fs.openSync(logPath, "w");
  try {
    await new Promise((resolve, reject) => {
      const child = spawn(command, args, {
        cwd,
        stdio: ["ignore", output, output],
      });
      child.once("error", reject);
      child.once("exit", (code, signal) =>
        code === 0
          ? resolve()
          : reject(
              new Error(
                `not-built: ${command} exited ${code ?? signal}; see ${logPath}`,
              ),
            ),
      );
    });
  } finally {
    fs.closeSync(output);
  }
}

module.exports = { runProviderBuild };
