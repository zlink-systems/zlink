#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const ts = require('typescript');

const SCRIPT_NAME = 'generate-framework-json-schemas.mjs';
const MAX_SCHEMA_DEPTH = 64;

const options = parseArguments(process.argv.slice(2));
const projectPath = path.resolve(options.project);
const config = readProject(projectPath);
const program = ts.createProgram(config.fileNames, config.options);
const checker = program.getTypeChecker();
const sourceFiles = program.getSourceFiles().filter((sourceFile) =>
  !sourceFile.isDeclarationFile
  && !sourceFile.fileName.includes(`${path.sep}node_modules${path.sep}`)
);
const scanSourceFiles = options.sources.length === 0
  ? sourceFiles
  : sourceFiles.filter((sourceFile) => options.sources.some((source) => {
    const relative = path.relative(path.dirname(projectPath), sourceFile.fileName);
    const normalized = source.replaceAll('\\', '/').replace(/^\.\//u, '');
    const normalizedRelative = relative.replaceAll('\\', '/');
    return normalizedRelative === normalized
      || normalizedRelative.startsWith(`${normalized}/`);
  }));

reportDiagnostics(program, config.errors);

const packetValues = collectPacketValues(scanSourceFiles, checker);
const typeSymbols = collectTypeSymbols(sourceFiles, checker);
const packetNames = collectUsedPacketNames(
  scanSourceFiles,
  checker,
  packetValues,
  typeSymbols,
  options.sources.length > 0
);
const handlerTypes = collectHandlerTypes(sourceFiles, checker);
const contracts = new Map();
const unmatched = [];

for (const packetName of [...packetNames].sort()) {
  const handler = handlerTypes.get(packetName);
  const payloadSymbol = findTypeSymbol(packetName, typeSymbols, checker);
  const payloadType = payloadSymbol === undefined
    ? handler?.payload
    : checker.getDeclaredTypeOfSymbol(payloadSymbol);
  const replyType = handler?.reply;
  if (payloadType === undefined) {
    unmatched.push(packetName);
    continue;
  }
  try {
    const contract = {
      payload: schemaForType(payloadType, checker, `${packetName}.payload`)
    };
    if (replyType !== undefined) {
      contract.reply = schemaForType(replyType, checker, `${packetName}.reply`);
    }
    contracts.set(packetName, contract);
  } catch (error) {
    throw new Error(`${formatError(error)} (packet '${packetName}')`);
  }
}

if (unmatched.length > 0) {
  throw new Error(
    `Unable to resolve DTO declarations for used packet name(s): ${unmatched.join(', ')}. `
    + 'Define a class, interface, or type alias with the packet name, or provide a typed handler.'
  );
}

const serialized = serializeContracts(contracts);
writeOutput(options.out, renderCommonJs(serialized));
if (options.browserOut !== undefined) {
  writeOutput(options.browserOut, renderEsModule(serialized));
}

process.stdout.write(
  `${SCRIPT_NAME}: generated ${contracts.size} packet schema(s)`
  + ` from ${sourceFiles.length} source file(s)\n`
);

function parseArguments(args) {
  const parsed = { project: undefined, out: undefined, browserOut: undefined, sources: [] };
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    const value = args[++index];
    if (!value || value.startsWith('--')) {
      throw new Error(`Usage: node scripts/${SCRIPT_NAME} --project <tsconfig.json> --out <server.cjs> [--browser-out <browser.mjs>]`);
    }
    if (argument === '--project' || argument === '-p') parsed.project = value;
    else if (argument === '--out') parsed.out = value;
    else if (argument === '--browser-out') parsed.browserOut = value;
    else if (argument === '--source') parsed.sources.push(value);
    else throw new Error(`Unknown argument '${argument}'.`);
  }
  if (parsed.project === undefined || parsed.out === undefined) {
    throw new Error(`Usage: node scripts/${SCRIPT_NAME} --project <tsconfig.json> --out <server.cjs> [--browser-out <browser.mjs>] [--source <relative-dir-or-file>]`);
  }
  return parsed;
}

function readProject(project) {
  const read = ts.readConfigFile(project, ts.sys.readFile);
  if (read.error) {
    throw new Error(formatDiagnostic(read.error));
  }
  const parsed = ts.parseJsonConfigFileContent(
    read.config,
    ts.sys,
    path.dirname(project),
    undefined,
    project
  );
  // Schema extraction must preserve the wire-level distinction between T and
  // T | null even when an application uses a non-strict application build.
  // The application compiler has already validated the source; this checker
  // only reads types and therefore enables strictNullChecks for extraction.
  parsed.options.strictNullChecks = true;
  return parsed;
}

function reportDiagnostics(program, configErrors) {
  const diagnostics = [
    ...configErrors,
    ...ts.getPreEmitDiagnostics(program).filter((diagnostic) => diagnostic.category === ts.DiagnosticCategory.Error)
  ];
  if (diagnostics.length === 0) return;
  throw new Error(diagnostics.map(formatDiagnostic).join('\n'));
}

function formatDiagnostic(diagnostic) {
  const message = ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n');
  if (diagnostic.file === undefined || diagnostic.start === undefined) return message;
  const position = diagnostic.file.getLineAndCharacterOfPosition(diagnostic.start);
  return `${diagnostic.file.fileName}:${position.line + 1}:${position.character + 1}: ${message}`;
}

function formatError(error) {
  return error instanceof Error ? error.message : String(error);
}

function collectPacketValues(sourceFiles, checker) {
  const values = new Map();
  for (const sourceFile of sourceFiles) {
    visit(sourceFile, (node) => {
      if (!ts.isVariableDeclaration(node) || !ts.isIdentifier(node.name) || node.name.text !== 'PacketNames') return;
      const initializer = unwrapExpression(node.initializer);
      if (!ts.isObjectLiteralExpression(initializer)) return;
      for (const property of initializer.properties) {
        if (!ts.isPropertyAssignment(property)) continue;
        const propertyName = propertyNameText(property.name);
        const value = evaluateString(property.initializer, checker);
        if (propertyName !== undefined && value !== undefined) values.set(`${node.name.text}.${propertyName}`, value);
      }
    });
  }
  return values;
}

function collectUsedPacketNames(sourceFiles, checker, packetValues, typeSymbols, contractSourcesOnly) {
  const names = new Set();
  for (const sourceFile of sourceFiles) {
    visit(sourceFile, (node) => {
      if (ts.isPropertyAccessExpression(node)
        && ts.isIdentifier(node.expression)
        && node.expression.text === 'PacketNames') {
        const value = packetValues.get(`PacketNames.${node.name.text}`);
        if (value !== undefined) names.add(value);
      }
      if (ts.isElementAccessExpression(node)
        && ts.isIdentifier(node.expression)
        && node.expression.text === 'PacketNames') {
        const property = evaluateString(node.argumentExpression, checker);
        const value = property === undefined ? undefined : packetValues.get(`PacketNames.${property}`);
        if (value !== undefined) names.add(value);
      }
      if (!ts.isCallExpression(node)) return;
      const calledName = expressionName(node.expression);
      if (calledName === 'ZLinkPacket' && node.arguments.length > 0) {
        const value = evaluateString(node.arguments[0], checker);
        if (value !== undefined) names.add(value);
      }
      for (const argument of node.arguments) collectPacketNameProperties(argument, checker, names);
      if (ts.isPropertyAccessExpression(node.expression) && node.expression.name.text === 'packetName') {
        const value = evaluateString(node.arguments[0], checker);
        if (value !== undefined) names.add(value);
      }
    });
  }
  if (contractSourcesOnly) {
    for (const value of packetValues.values()) {
      if (findTypeSymbol(value, typeSymbols, checker) !== undefined) names.add(value);
    }
  }
  return names;
}

function collectPacketNameProperties(node, checker, names) {
  if (!ts.isObjectLiteralExpression(unwrapExpression(node))) return;
  const object = unwrapExpression(node);
  for (const property of object.properties) {
    if (!ts.isPropertyAssignment(property) || propertyNameText(property.name) !== 'packetName') continue;
    const value = evaluateString(property.initializer, checker);
    if (value !== undefined) names.add(value);
  }
}

function collectTypeSymbols(sourceFiles, checker) {
  const symbols = new Map();
  for (const sourceFile of sourceFiles) {
    visit(sourceFile, (node) => {
      if (!isSchemaDeclaration(node) || node.name === undefined) return;
      const symbol = checker.getSymbolAtLocation(node.name);
      if (symbol === undefined) return;
      const existing = symbols.get(node.name.text);
      if (existing === undefined) symbols.set(node.name.text, [symbol]);
      else if (!existing.includes(symbol)) existing.push(symbol);
    });
  }
  return symbols;
}

function findTypeSymbol(name, symbols, checker) {
  const candidates = symbols.get(name);
  if (candidates === undefined || candidates.length === 0) return undefined;
  const resolved = candidates.map((candidate) => resolveAlias(candidate, checker));
  const preferred = resolved.find((candidate) => candidate.declarations?.some((declaration) =>
    ts.isClassDeclaration(declaration)
    || ts.isInterfaceDeclaration(declaration)
    || ts.isTypeAliasDeclaration(declaration)
    || ts.isEnumDeclaration(declaration)));
  return preferred ?? resolved[0];
}

function collectHandlerTypes(sourceFiles, checker) {
  const handlers = new Map();
  for (const sourceFile of sourceFiles) {
    visit(sourceFile, (node) => {
      if (!ts.isClassDeclaration(node)) return;
      const packetNames = decoratorPacketNames(node, checker);
      if (packetNames.length === 0) return;
      let payload;
      let reply;
      for (const clause of node.heritageClauses ?? []) {
        for (const heritage of clause.types) {
          const typeName = expressionName(heritage.expression) ?? '';
          const typeArguments = heritage.typeArguments ?? [];
          const selected = selectHandlerTypeArguments(typeName, typeArguments, checker);
          if (selected.payload !== undefined) payload = selected.payload;
          if (selected.reply !== undefined) reply = selected.reply;
        }
      }
      if (payload === undefined || reply === undefined) {
        const methodTypes = inferHandlerMethodTypes(node, checker);
        payload ??= methodTypes.payload;
        reply ??= methodTypes.reply;
      }
      for (const packetName of packetNames) {
        const current = handlers.get(packetName) ?? {};
        handlers.set(packetName, {
          payload: payload ?? current.payload,
          reply: reply ?? current.reply
        });
      }
    });
  }
  return handlers;
}

function decoratorPacketNames(node, checker) {
  const names = [];
  for (const decorator of ts.getDecorators?.(node) ?? node.decorators ?? []) {
    const expression = unwrapExpression(decorator.expression);
    if (!ts.isCallExpression(expression)) continue;
    const calledName = expressionName(expression.expression) ?? '';
    if (calledName === 'ZLinkPacket') {
      const value = evaluateString(expression.arguments[0], checker);
      if (value !== undefined) names.push(value);
    }
    for (const argument of expression.arguments) {
      collectPacketNameProperties(argument, checker, namesSetProxy(names));
    }
    if (expression.arguments.length > 1) {
      const value = evaluateString(expression.arguments[1], checker);
      if (value !== undefined && (calledName.toLowerCase().includes('handler') || calledName.startsWith('zlink'))) {
        names.push(value);
      }
    }
  }
  return [...new Set(names)];
}

function namesSetProxy(values) {
  return {
    add(value) {
      if (value !== undefined && !values.includes(value)) values.push(value);
    }
  };
}

function selectHandlerTypeArguments(typeName, typeArguments, checker) {
  if (typeArguments.length === 0) return {};
  const lower = typeName.toLowerCase();
  if (lower.includes('sendhandler') || lower.includes('publishhandler') || lower.includes('packethandler')) {
    return { payload: checker.getTypeAtLocation(typeArguments.at(-1)) };
  }
  if (lower.includes('requesthandler') || lower.includes('actionhandler') || typeArguments.length >= 2) {
    return {
      payload: checker.getTypeAtLocation(typeArguments.at(-2)),
      reply: checker.getTypeAtLocation(typeArguments.at(-1))
    };
  }
  return { payload: checker.getTypeAtLocation(typeArguments[0]) };
}

function inferHandlerMethodTypes(node, checker) {
  for (const member of node.members) {
    if (!ts.isMethodDeclaration(member) || member.name === undefined || propertyNameText(member.name) !== 'handle') continue;
    const parameters = member.parameters;
    const request = parameters.at(-1);
    const payload = request?.type === undefined ? undefined : checker.getTypeAtLocation(request.type);
    const signature = checker.getSignatureFromDeclaration(member);
    const returnType = signature === undefined ? undefined : unwrapPromiseType(checker.getReturnTypeOfSignature(signature), checker);
    const reply = returnType === undefined || (returnType.flags & ts.TypeFlags.Void) !== 0
      ? undefined
      : returnType;
    return { payload, reply };
  }
  return {};
}

function unwrapPromiseType(type, checker) {
  if (!type.symbol || type.symbol.getName() !== 'Promise' || !checker.getTypeArguments) return type;
  const argumentsOfType = checker.getTypeArguments(type);
  return argumentsOfType[0] ?? type;
}

function schemaForType(type, checker, pathName, state = { stack: [], depth: 0 }) {
  if (state.depth > MAX_SCHEMA_DEPTH) throw new Error(`schema nesting exceeds ${MAX_SCHEMA_DEPTH} levels at ${pathName}`);
  const marker = type.id ?? type;
  if (state.stack.includes(marker)) throw new Error(`recursive DTO type is not supported at ${pathName}`);
  const nextState = { stack: [...state.stack, marker], depth: state.depth + 1 };

  const markerSchema = schemaMarker(type, checker);
  if (markerSchema !== undefined) return markerSchema;
  if ((type.flags & ts.TypeFlags.Any) !== 0 || (type.flags & ts.TypeFlags.Unknown) !== 0) {
    throw new Error(`unsupported '${checker.typeToString(type)}' type at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.Never) !== 0 || (type.flags & ts.TypeFlags.Void) !== 0) {
    throw new Error(`unsupported '${checker.typeToString(type)}' type at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.Null) !== 0) {
    throw new Error(`null is only supported as part of a nullable union at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.Undefined) !== 0) {
    throw new Error(`undefined is only supported on an optional property at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.Boolean) !== 0 || (type.flags & ts.TypeFlags.BooleanLiteral) !== 0) {
    return { type: 'boolean' };
  }
  if ((type.flags & ts.TypeFlags.String) !== 0) return { type: 'string' };
  if ((type.flags & ts.TypeFlags.Number) !== 0) return { type: 'number' };
  if ((type.flags & ts.TypeFlags.BigInt) !== 0 || (type.flags & ts.TypeFlags.BigIntLiteral) !== 0) {
    return { type: 'int64' };
  }
  if ((type.flags & ts.TypeFlags.StringLiteral) !== 0) return { type: 'enum', names: [type.value] };
  if ((type.flags & ts.TypeFlags.NumberLiteral) !== 0) return { type: 'number' };
  if ((type.flags & ts.TypeFlags.Union) !== 0) return schemaForUnion(type, checker, pathName, nextState);
  if ((type.flags & ts.TypeFlags.Intersection) !== 0) return schemaForIntersection(type, checker, pathName, nextState);
  const enumDeclaration = type.symbol?.declarations?.find((declaration) => ts.isEnumDeclaration(declaration));
  if (enumDeclaration !== undefined) return schemaForEnum(enumDeclaration, checker, pathName);
  if (checker.isArrayType(type)) {
    const element = checker.getTypeArguments(type)[0];
    if (element === undefined) throw new Error(`array element type is missing at ${pathName}`);
    return { type: 'array', items: schemaForType(element, checker, `${pathName}[]`, nextState) };
  }
  if (checker.isTupleType(type)) {
    throw new Error(`tuple types are not supported at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.TypeParameter) !== 0) {
    throw new Error(`generic type parameter '${checker.typeToString(type)}' is not supported at ${pathName}`);
  }
  if ((type.flags & ts.TypeFlags.Object) !== 0) return schemaForObject(type, checker, pathName, nextState);
  throw new Error(`unsupported '${checker.typeToString(type)}' type at ${pathName}`);
}

function schemaMarker(type, checker) {
  const symbolName = type.aliasSymbol?.getName?.() ?? type.symbol?.getName?.() ?? checker.typeToString(type);
  if (/^(?:ZLink)?(?:U?Int32)$/iu.test(symbolName)) {
    return { type: symbolName.toLowerCase().includes('uint') ? 'uint32' : 'int32' };
  }
  if (/^(?:ZLink)?(?:U?Int64)$/iu.test(symbolName)) {
    return { type: symbolName.toLowerCase().includes('uint') ? 'uint64' : 'int64' };
  }
  if (/^(?:ZLink)?(?:Bytes|ByteArray)$/iu.test(symbolName) || symbolName === 'Uint8Array' || symbolName === 'Buffer') {
    return { type: 'bytes' };
  }
  return undefined;
}

function schemaForUnion(type, checker, pathName, state) {
  const members = type.types;
  const nullable = members.some((member) => (member.flags & ts.TypeFlags.Null) !== 0);
  const defined = members.filter((member) =>
    (member.flags & ts.TypeFlags.Null) === 0 && (member.flags & ts.TypeFlags.Undefined) === 0
  );
  if (defined.length === 0) throw new Error(`nullable union has no value type at ${pathName}`);
  const literalStrings = defined.every((member) => (member.flags & ts.TypeFlags.StringLiteral) !== 0);
  if (literalStrings) {
    const schema = { type: 'enum', names: [...new Set(defined.map((member) => member.value))] };
    return nullable ? { type: 'nullable', value: schema } : schema;
  }
  if (defined.every((member) => (member.flags & ts.TypeFlags.BooleanLiteral) !== 0)) {
    const schema = { type: 'boolean' };
    return nullable ? { type: 'nullable', value: schema } : schema;
  }
  if (defined.every((member) => (member.flags & ts.TypeFlags.NumberLiteral) !== 0)) {
    const schema = { type: 'number' };
    return nullable ? { type: 'nullable', value: schema } : schema;
  }
  if (defined.length !== 1) throw new Error(`union type '${checker.typeToString(type)}' is not supported at ${pathName}`);
  const value = schemaForType(defined[0], checker, pathName, state);
  return nullable ? { type: 'nullable', value } : value;
}

function schemaForIntersection(type, checker, pathName, state) {
  const schemas = type.types.map((part, index) => schemaForType(part, checker, `${pathName}&${index}`, state));
  const primitive = schemas.find((schema) => schema.type !== 'object');
  if (primitive !== undefined) return primitive;
  const objects = schemas.filter((schema) => schema.type === 'object');
  if (objects.length !== schemas.length) throw new Error(`intersection type is not supported at ${pathName}`);
  const properties = {};
  const required = new Set();
  let additionalProperties;
  for (const object of objects) {
    Object.assign(properties, object.properties);
    for (const name of object.required) required.add(name);
    if (object.additionalProperties !== undefined) additionalProperties = object.additionalProperties;
  }
  return {
    type: 'object',
    properties,
    required: [...required].sort(),
    ...(additionalProperties === undefined ? {} : { additionalProperties })
  };
}

function schemaForEnum(declaration, checker, pathName) {
  const names = [];
  for (const member of declaration.members) {
    const value = checker.getConstantValue(member);
    if (typeof value !== 'string' || value.length === 0) {
      throw new Error(`enum '${declaration.name.text}' must contain non-empty string values at ${pathName}`);
    }
    if (names.includes(value)) {
      throw new Error(`enum '${declaration.name.text}' contains duplicate value '${value}' at ${pathName}`);
    }
    names.push(value);
  }
  if (names.length === 0) throw new Error(`enum '${declaration.name.text}' must contain at least one value at ${pathName}`);
  return { type: 'enum', names };
}

function schemaForObject(type, checker, pathName, state) {
  const symbolName = type.symbol?.getName?.();
  if (symbolName === 'Date' || symbolName === 'Map' || symbolName === 'Set' || symbolName === 'Function') {
    throw new Error(`unsupported '${symbolName}' type at ${pathName}`);
  }
  const indexType = checker.getIndexTypeOfType(type, ts.IndexKind.String);
  const properties = {};
  const required = [];
  for (const property of checker.getPropertiesOfType(type)) {
    const propertyName = property.getName();
    if (propertyName.startsWith('__@')) continue;
    if (propertyName === '__proto__' || propertyName === 'prototype' || propertyName === 'constructor') {
      throw new Error(`property '${propertyName}' is not supported at ${pathName}`);
    }
    const declaration = property.valueDeclaration ?? property.declarations?.[0];
    if (declaration === undefined) continue;
    const propertyType = checker.getTypeOfSymbolAtLocation(property, declaration);
    const propertyPath = `${pathName}.${propertyName}`;
    try {
      properties[propertyName] = schemaForType(propertyType, checker, propertyPath, state);
    } catch (error) {
      throw new Error(formatError(error));
    }
    if ((property.flags & ts.SymbolFlags.Optional) === 0 && !isOptionalDeclaration(declaration)) required.push(propertyName);
  }
  if (indexType !== undefined) {
    const values = schemaForType(indexType, checker, `${pathName}[*]`, state);
    if (Object.keys(properties).length === 0) return { type: 'record', values };
    return {
      type: 'object',
      properties,
      required: required.sort(),
      additionalProperties: true
    };
  }
  return { type: 'object', properties, required: required.sort() };
}

function isOptionalDeclaration(declaration) {
  return (ts.isPropertyDeclaration(declaration) || ts.isPropertySignature(declaration) || ts.isParameter(declaration))
    && declaration.questionToken !== undefined;
}

function serializeContracts(contracts) {
  return JSON.stringify(Object.fromEntries(contracts), (_key, value) => value, 2);
}

function renderCommonJs(serialized) {
  return `'use strict';\n\nconst packetContracts = Object.freeze(${serialized});\n\nfunction register(registerPacket) {\n  for (const [packetName, contract] of Object.entries(packetContracts)) {\n    registerPacket(packetName, contract)(class GeneratedZLinkPacket {});\n  }\n}\n\nconst framework = require('@zlink-systems/framework');\nregister(framework.ZLinkPacket);\nmodule.exports = Object.freeze({ packetContracts, register });\n`;
}

function renderEsModule(serialized) {
  return `const packetContracts = Object.freeze(${serialized});\n\nexport function register(registerPacket) {\n  for (const [packetName, contract] of Object.entries(packetContracts)) {\n    registerPacket(packetName, contract)(class GeneratedZLinkPacket {});\n  }\n}\n\nexport { packetContracts };\nexport default packetContracts;\n`;
}

function writeOutput(filePath, content) {
  const target = path.resolve(filePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, content, 'utf8');
}

function isSchemaDeclaration(node) {
  return ts.isClassDeclaration(node)
    || ts.isInterfaceDeclaration(node)
    || ts.isTypeAliasDeclaration(node)
    || ts.isEnumDeclaration(node);
}

function resolveAlias(symbol, checker) {
  return (symbol.flags & ts.SymbolFlags.Alias) !== 0 ? checker.getAliasedSymbol(symbol) : symbol;
}

function evaluateString(expression, checker, seen = new Set()) {
  if (expression === undefined || expression === null) return undefined;
  const value = unwrapExpression(expression);
  if (ts.isStringLiteral(value) || ts.isNoSubstitutionTemplateLiteral(value)) return value.text;
  if (ts.isIdentifier(value) || ts.isPropertyAccessExpression(value) || ts.isElementAccessExpression(value)) {
    const symbol = checker.getSymbolAtLocation(ts.isIdentifier(value) ? value : value.name ?? value.argumentExpression);
    if (symbol === undefined || seen.has(symbol)) return undefined;
    const nextSeen = new Set(seen).add(symbol);
    if (ts.isPropertyAccessExpression(value) || ts.isElementAccessExpression(value)) {
      const object = unwrapExpression(value.expression);
      const declaration = resolveValueDeclaration(symbol, checker);
      const property = declaration?.initializer;
      if (property !== undefined) return evaluateString(property, checker, nextSeen);
      if (ts.isIdentifier(object) && object.text === 'PacketNames') return undefined;
    }
    const declaration = resolveValueDeclaration(symbol, checker);
    return declaration?.initializer === undefined ? undefined : evaluateString(declaration.initializer, checker, nextSeen);
  }
  return undefined;
}

function resolveValueDeclaration(symbol, checker) {
  const resolved = resolveAlias(symbol, checker);
  return resolved.valueDeclaration ?? resolved.declarations?.find((declaration) => ts.isVariableDeclaration(declaration) || ts.isEnumMember(declaration));
}

function unwrapExpression(expression) {
  let current = expression;
  while (current !== undefined && (
    ts.isParenthesizedExpression(current)
    || ts.isAsExpression(current)
    || ts.isTypeAssertionExpression(current)
    || ts.isNonNullExpression(current)
    || ts.isSatisfiesExpression?.(current)
  )) {
    current = current.expression;
  }
  if (ts.isCallExpression(current)
    && ts.isPropertyAccessExpression(current.expression)
    && current.expression.expression.getText() === 'Object'
    && current.expression.name.text === 'freeze') {
    return unwrapExpression(current.arguments[0]);
  }
  return current;
}

function propertyNameText(name) {
  if (name === undefined) return undefined;
  if (ts.isIdentifier(name) || ts.isStringLiteral(name) || ts.isNumericLiteral(name)) return name.text;
  return undefined;
}

function expressionName(expression) {
  if (expression === undefined) return undefined;
  if (ts.isIdentifier(expression)) return expression.text;
  if (ts.isPropertyAccessExpression(expression)) return expression.name.text;
  if (ts.isExpressionWithTypeArguments(expression)) return expressionName(expression.expression);
  return undefined;
}

function visit(node, callback) {
  callback(node);
  ts.forEachChild(node, (child) => visit(child, callback));
}
