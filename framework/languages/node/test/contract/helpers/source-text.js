'use strict';

const fs = require('node:fs');

//  계약 test가 저장소의 파일 내용을 단정할 때 쓰는 유일한 읽기 경로다 (#582).
//  `.gitattributes`가 `* text=auto eol=lf`이지만 git은 이미 disk에 있는 파일을 다시 쓰지
//  않으므로 그 규칙보다 먼저 만들어진 checkout은 CRLF로 남는다. 읽는 자리에서 한 번
//  `\n`으로 정규화하면 단정문이 줄바꿈을 글자로 적어도 checkout에 따라 어긋나지 않는다.
//  Java testkit(#530)과 .NET `SampleRegressionTests`(#578)가 같은 규칙을 쓴다.
function readSourceText(filePath) {
  return normalizeNewlines(fs.readFileSync(filePath, 'utf8'));
}

function normalizeNewlines(text) {
  return text.replace(/\r\n/gu, '\n');
}

module.exports = { normalizeNewlines, readSourceText };
