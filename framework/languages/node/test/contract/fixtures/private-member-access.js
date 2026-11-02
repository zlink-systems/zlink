const fs = require('node:fs');
const crypto = require('node:crypto');
const path = require('node:path');
const ts = require('typescript');

function sourceFiles(directory, extension) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory()
      ? sourceFiles(file, extension)
      : extension.test(entry.name)
        ? [file]
        : [];
  });
}

function isPrivate(declaration) {
  return (
    declaration.name !== undefined &&
    (ts.isPrivateIdentifier(declaration.name) ||
      declaration.modifiers?.some((modifier) => modifier.kind === ts.SyntaxKind.PrivateKeyword))
  );
}

function accessExpression(location, source) {
  const access = location.parent;
  if (
    (ts.isPropertyAccessExpression(access) || ts.isElementAccessExpression(access)) &&
    ts.isCallExpression(access.parent) &&
    access.parent.expression === access
  )
    return access.parent.getText(source);
  if (ts.isStringLiteralLike(location) && ts.isCallExpression(location.parent))
    return location.parent.getText(source);
  return access.getText(source);
}

function accessIdentity(row) {
  const normalized = row.expression.trim().replace(/\s+/gu, ' ');
  return crypto
    .createHash('sha256')
    .update(JSON.stringify([row.member, normalized]))
    .digest('hex');
}

function accessAllowlist(accesses) {
  const files = new Map();
  for (const access of accesses) {
    const counts = files.get(access.file) ?? new Map();
    const identity = accessIdentity(access);
    counts.set(identity, (counts.get(identity) ?? 0) + 1);
    files.set(access.file, counts);
  }
  return Object.fromEntries(
    [...files]
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([file, counts]) => [
        file,
        {
          reason: 'Legacy private access tracked by #2309; migrate to public operations or internal exports.',
          accesses: Object.fromEntries([...counts].sort(([left], [right]) => left.localeCompare(right)))
        }
      ])
  );
}

function compareAccesses(accesses, allowlist) {
  const actual = accessAllowlist(accesses);
  const errors = [];
  for (const file of [...new Set([...Object.keys(actual), ...Object.keys(allowlist)])].sort()) {
    const found = actual[file]?.accesses ?? {};
    const allowed = allowlist[file]?.accesses ?? {};
    const unlisted = Object.entries(found)
      .map(([identity, count]) => [identity, count - (allowed[identity] ?? 0)])
      .filter(([, count]) => count > 0);
    const stale = Object.entries(allowed)
      .map(([identity, count]) => [identity, count - (found[identity] ?? 0)])
      .filter(([, count]) => count > 0);
    if (unlisted.length > 0)
      errors.push(`Unlisted private access: ${file} (${unlisted.reduce((sum, [, count]) => sum + count, 0)} accesses)`);
    if (stale.length > 0)
      errors.push(`Stale allowance; remove migrated accesses: ${file} (${stale.reduce((sum, [, count]) => sum + count, 0)} accesses)`);
  }
  return errors;
}

// Test imports resolve to sources; emitted declarations omit private member types.
// A spelling shared with a public member is not sufficient evidence of a violation.
function scanPrivateAccess(nodeRoot, additionalSources = new Map()) {
  const packages = path.join(nodeRoot, 'packages');
  const sources = fs.readdirSync(packages).flatMap((name) => {
    const directory = path.join(packages, name, 'src');
    return fs.existsSync(directory) ? sourceFiles(directory, /\.ts$/u) : [];
  });
  const tests = sourceFiles(path.join(nodeRoot, 'test'), /\.[cm]?[jt]s$/u);
  const config = ts.readConfigFile(path.join(nodeRoot, 'tsconfig.base.json'), ts.sys.readFile);
  if (config.error)
    throw new Error(ts.flattenDiagnosticMessageText(config.error.messageText, '\n'));
  const parsed = ts.convertCompilerOptionsFromJson(config.config.compilerOptions, nodeRoot);
  if (parsed.errors.length) throw new Error('Invalid TypeScript compiler options');
  const options = { ...parsed.options, allowJs: true, checkJs: true, noEmit: true };
  const host = ts.createCompilerHost(options);
  const readFile = host.readFile.bind(host);
  const fileExists = host.fileExists.bind(host);
  host.readFile = (file) => additionalSources.get(file) ?? readFile(file);
  host.fileExists = (file) => additionalSources.has(file) || fileExists(file);
  host.resolveModuleNames = (names, containingFile) =>
    names.map(
      (name) =>
        ts.resolveModuleName(name.replace(/\/dist(?=\/|$)/u, '/src'), containingFile, options, host)
          .resolvedModule
    );
  const program = ts.createProgram(
    [...sources, ...tests, ...additionalSources.keys()],
    options,
    host
  );
  const checker = program.getTypeChecker();
  const sourceSet = new Set(sources);
  const privateMembers = new Map();
  for (const file of sources) {
    const source = program.getSourceFile(file);
    function visit(node) {
      if (isPrivate(node)) {
        const name = node.name.text;
        const declarations = privateMembers.get(name) ?? [];
        declarations.push(node);
        privateMembers.set(name, declarations);
      }
      ts.forEachChild(node, visit);
    }
    visit(source);
  }
  const accesses = [];
  const unresolved = [];
  for (const file of new Set(
    [...tests, ...additionalSources.keys()].filter((file) => !sourceSet.has(file))
  )) {
    const source = program.getSourceFile(file);
    function inspect(receiver, name, location) {
      if (!privateMembers.has(name)) return;
      function receiverTypes(expression, seen = new Set()) {
        while (
          ts.isParenthesizedExpression(expression) ||
          ts.isAsExpression(expression) ||
          ts.isTypeAssertionExpression(expression) ||
          ts.isNonNullExpression(expression)
        ) {
          expression = expression.expression;
        }
        if (seen.has(expression)) return [];
        seen.add(expression);
        const types = [checker.getTypeAtLocation(expression)];
        if (ts.isIdentifier(expression)) {
          for (const declaration of checker.getSymbolAtLocation(expression)?.declarations ?? []) {
            if (ts.isVariableDeclaration(declaration) && declaration.initializer) {
              types.push(...receiverTypes(declaration.initializer, seen));
            }
          }
        }
        if (
          ts.isCallExpression(expression) &&
          expression.expression.getText() === 'Object.create' &&
          expression.arguments[0]
        )
          types.push(...receiverTypes(expression.arguments[0], seen));
        return types;
      }
      const types = receiverTypes(receiver);
      const declarations = types.flatMap((type) => {
        const receiverType = checker.getNonNullableType(type);
        const symbol =
          checker.getPropertyOfType(receiverType, name) ??
          (ts.isPrivateIdentifier(location)
            ? receiverType
                .getProperties()
                .find((candidate) =>
                  candidate.declarations?.some((declaration) => declaration.name?.text === name)
                )
            : undefined);
        return symbol?.declarations ?? [];
      });
      const position = source.getLineAndCharacterOfPosition(location.getStart());
      const row = {
        file: path.relative(nodeRoot, file).split(path.sep).join('/'),
        line: position.line + 1,
        member: name,
        expression: accessExpression(location, source).replace(/\s+/gu, ' ')
      };
      if (
        declarations.some(
          (declaration) =>
            sourceSet.has(declaration.getSourceFile().fileName) && isPrivate(declaration)
        )
      ) {
        accesses.push(row);
      } else if (!declarations.length) {
        unresolved.push(row);
      }
    }
    function visit(node) {
      if (ts.isPropertyAccessExpression(node)) inspect(node.expression, node.name.text, node.name);
      if (
        ts.isElementAccessExpression(node) &&
        node.argumentExpression &&
        ts.isStringLiteralLike(node.argumentExpression)
      )
        inspect(node.expression, node.argumentExpression.text, node.argumentExpression);
      // mock.method and Reflect.get/set invoke/read members at runtime as well.
      if (
        ts.isCallExpression(node) &&
        ts.isPropertyAccessExpression(node.expression) &&
        ['method', 'get', 'set', 'defineProperty'].includes(node.expression.name.text) &&
        node.arguments.length >= 2 &&
        ts.isStringLiteralLike(node.arguments[1])
      ) {
        inspect(node.arguments[0], node.arguments[1].text, node.arguments[1]);
      }
      ts.forEachChild(node, visit);
    }
    visit(source);
  }
  return { accesses, unresolved, privateMembers };
}

module.exports = { accessAllowlist, compareAccesses, scanPrivateAccess };
