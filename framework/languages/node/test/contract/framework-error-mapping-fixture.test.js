const assert = require('node:assert/strict');
const { test } = require('node:test');
const fixture = require('../../../../runtime/conformance/framework-error-mapping-v1.json');
const f = {
  ...require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException'),
  ...require('../../packages/framework/dist/runtime/framework-errors-internal')
};
test('error model fixture classifies all 25 receive codes', () => {
  assert.equal(fixture.receive.length, 25);
  for (const row of fixture.receive) {
    const internal = f.internalFrameworkErrorKindFromWireFailureCode(row.failureCode);
    assert.notEqual(internal, undefined, row.code);
    assert.equal(
      f.createInternalFrameworkException(internal, 'fixture').kind,
      f.ZLinkFrameworkErrorKind[row.kind],
      row.code
    );
  }
});
test('error model fixture owns all 12 public send representatives', () => {
  assert.equal(fixture.send.length, 12);
  for (const row of fixture.send)
    assert.deepEqual(
      f.internalFrameworkWireReply(
        new f.ZLinkFrameworkException(f.ZLinkFrameworkErrorKind[row.kind], 'fixture')
      ),
      { failureCode: row.failureCode, terminalResult: row.terminalResult },
      row.kind
    );
});
test('error model fixture owns code-only send representatives', () => {
  for (const row of fixture.send)
    assert.equal(
      f.frameworkRelocationFailureCode(
        new f.ZLinkFrameworkException(f.ZLinkFrameworkErrorKind[row.kind], 'fixture')
      ),
      row.codeOnlyFailureCode ?? row.failureCode,
      row.kind
    );
});
test('known fine causes survive forwarding and other kinds use representatives', () => {
  for (const row of fixture.receive) {
    const incoming = f.wireReplyFailureException(row.terminalResult, row.failureCode, 'remote');
    assert.deepEqual(
      f.internalFrameworkWireReply(incoming),
      { terminalResult: row.terminalResult, failureCode: row.failureCode },
      row.code
    );
    for (const target of fixture.send) {
      const wrapper = new f.ZLinkFrameworkException(
        f.ZLinkFrameworkErrorKind[target.kind],
        'forward',
        incoming
      );
      const expected = target.kind === row.kind ? row : target;
      assert.deepEqual(
        f.internalFrameworkWireReply(wrapper),
        { terminalResult: expected.terminalResult, failureCode: expected.failureCode },
        `${target.kind}/${row.code}`
      );
      assert.equal(
        f.frameworkRelocationFailureCode(wrapper),
        target.kind === row.kind
          ? row.failureCode
          : (target.codeOnlyFailureCode ?? target.failureCode),
        `code-only ${target.kind}/${row.code}`
      );
    }
  }
});
