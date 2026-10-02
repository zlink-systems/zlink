import { resolve } from 'node:path';
import type { ZLinkFrameworkErrorKind } from '../../packages/framework/src/contracts/Errors/ZLinkFrameworkException';

export const frameworkErrorMappingFixture = require(
  resolve(__dirname, '../../../../../../../../runtime/conformance/framework-error-mapping-v1.json')
) as {
  send: {
    kind: keyof typeof ZLinkFrameworkErrorKind;
    terminalResult: number;
    failureCode: number;
    codeOnlyFailureCode?: number;
  }[];
};
