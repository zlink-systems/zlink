const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const sourceRoot = path.resolve(__dirname, '../src');
const configPath = path.resolve(__dirname, '../tsconfig.json');

test('Cocos loose transform does not receive iterable array spreads', () => {
  const config = ts.readConfigFile(configPath, ts.sys.readFile);
  assert.equal(config.error, undefined);
  const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, path.dirname(configPath));
  assert.deepEqual(parsed.errors, []);
  const program = ts.createProgram(parsed.fileNames, parsed.options);
  const checker = program.getTypeChecker();
  const spreads = [];
  for (const source of program.getSourceFiles().filter((file) => file.fileName.startsWith(`${sourceRoot}${path.sep}`))) {
    function visit(node) {
      if (ts.isArrayLiteralExpression(node)) {
        for (const element of node.elements) {
          if (ts.isSpreadElement(element)) {
            const type = checker.getTypeAtLocation(element.expression);
            if (!isArrayType(type)) {
              const line = source.getLineAndCharacterOfPosition(element.getStart(source)).line + 1;
              spreads.push(`${path.relative(sourceRoot, source.fileName)}:${line}`);
            }
          }
        }
      }
      ts.forEachChild(node, visit);
    }
    visit(source);
  }
  assert.deepEqual(spreads, [], 'Cocos loose transform compiles iterable array spreads as [].concat(iterable)');

  function isArrayType(type) {
    return type.isUnion()
      ? type.types.every(isArrayType)
      : checker.isArrayType(type) || checker.isTupleType(type);
  }
});
