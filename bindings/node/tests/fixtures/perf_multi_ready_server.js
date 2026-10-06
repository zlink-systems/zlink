// SPDX-License-Identifier: MPL-2.0
'use strict';

const fs = require('node:fs');
const endpoint = process.argv[process.argv.indexOf('--endpoint') + 1];
fs.writeFileSync(process.env.PERF_TEST_SERVER_PID_FILE, String(process.pid));
process.stdout.write(`READY,${endpoint}\n`);
process.stdin.resume();
