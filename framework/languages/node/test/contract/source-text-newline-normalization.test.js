'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { normalizeNewlines, readSourceText } = require('./helpers/source-text');

const contractRoot = __dirname;

//  여러 줄에 걸친 호출을 단정하는 계약 test의 바늘과 같은 모양이다. 줄바꿈을 `\n` 글자로
//  적는다 — 그것이 정규화가 없으면 CRLF checkout에서 어긋나는 모양이기 때문이다 (#582).
const needle = 'objectServer.addSpotFactory(\n            SampleNames.roomSpotType,\n            RoomSpot,';
const sourceLf = `const module = () => {\n  ${needle}\n            (factory) => factory.disableRelocation()\n  );\n};\n`;

test('source reads see the file content, not the checkout line endings', (t) => {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-node-newline-'));
  t.after(() => fs.rmSync(tempDir, { recursive: true, force: true }));

  const lfFile = path.join(tempDir, 'lf-checkout.ts');
  const crlfFile = path.join(tempDir, 'crlf-checkout.ts');
  fs.writeFileSync(lfFile, sourceLf);
  fs.writeFileSync(crlfFile, sourceLf.replace(/\n/gu, '\r\n'));

  //  음성 대조. 정규화를 거치지 않은 읽기는 CRLF 사본에서 같은 바늘을 찾지 못한다.
  //  이 단정이 없으면 아래 두 단정이 무엇을 잡는지 알 수 없다.
  assert.equal(fs.readFileSync(crlfFile, 'utf8').includes(needle), false);
  assert.equal(fs.readFileSync(crlfFile, 'utf8').includes('\r\n'), true);

  assert.equal(readSourceText(crlfFile).includes(needle), true);
  assert.equal(readSourceText(lfFile), readSourceText(crlfFile));
  assert.equal(readSourceText(crlfFile).includes('\r'), false);
});

test('normalizeNewlines leaves an LF checkout untouched', () => {
  assert.equal(normalizeNewlines(sourceLf), sourceLf);
});

//  줄바꿈 규칙은 읽는 자리 하나가 소유한다. 이 두 파일은 저장소의 source를 읽어 여러 줄
//  바늘로 단정하므로, 여기서 raw `fs.readFileSync`가 다시 생기면 #582가 되돌아온다.
//  다른 계약 test는 줄바꿈이 주제인 바늘을 쓰지 않아 목록에 없다.
test('sample and cross-language source assertions read through the shared helper', () => {
  const offenders = [];
  for (const file of ['sample-regression.test.js', 'cross-language-smoke-wrapper.test.js']) {
    const source = readSourceText(path.join(contractRoot, file));
    if (source.includes('fs.readFileSync(')) {
      offenders.push(file);
    }
    if (!source.includes("require('./helpers/source-text')")) {
      offenders.push(`${file} does not require ./helpers/source-text`);
    }
  }

  assert.deepEqual(offenders, []);
});
