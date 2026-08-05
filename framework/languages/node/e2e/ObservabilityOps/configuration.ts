import * as fs from 'node:fs';
import { Module, type DynamicModule, type InjectionToken } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

function createObservabilityOpsConfigurationModule<T>(
  token: InjectionToken,
  validate: (value: unknown) => T
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  class ObservabilityOpsConfigurationModule {}
  Module({})(ObservabilityOpsConfigurationModule);
  return {
    module: ObservabilityOpsConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: readConfiguration(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: token,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validate(config.get('e2e'))
    }],
    exports: [token]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readConfiguration(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return document.e2e;
}

function validateServerOptions(value: unknown): ServerOptions {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'e2e' must be an object.");
  }
  const values = value as Record<string, unknown>;
  const required = (key: keyof ServerOptions): string => {
    const candidate = values[key];
    if (typeof candidate !== 'string' || candidate.length === 0) {
      throw new Error(`Configuration value 'e2e.${key}' must be a non-empty string.`);
    }
    return candidate;
  };
  return {
    rid: required('rid'),
    httpUrl: required('httpUrl'),
    redisEndpoint: required('redisEndpoint'),
    redisKeyPrefix: required('redisKeyPrefix'),
    routerEndpoint: required('routerEndpoint'),
    pubSubEndpoint: required('pubSubEndpoint'),
    fanoutEndpoint: typeof values.fanoutEndpoint === 'string' ? values.fanoutEndpoint : undefined,
    stateFile: typeof values.stateFile === 'string' ? values.stateFile : undefined,
    metricsEnabled: values.metricsEnabled !== false,
    messageFlowEnabled: values.messageFlowEnabled !== false,
    streamEndpoint: typeof values.streamEndpoint === 'string' ? values.streamEndpoint : undefined,
    evidenceFile: required('evidenceFile'),
    logDir: required('logDir')
  };
}

interface ServerOptions {
  rid: string;
  httpUrl: string;
  redisEndpoint: string;
  redisKeyPrefix: string;
  routerEndpoint: string;
  pubSubEndpoint: string;
  fanoutEndpoint?: string;
  stateFile?: string;
  metricsEnabled: boolean;
  messageFlowEnabled: boolean;
  streamEndpoint?: string;
  evidenceFile: string;
  logDir: string;
}

const OBSERVABILITY_OPS_OPTIONS = Symbol.for('OBSERVABILITY_OPS_OPTIONS');

export {
  OBSERVABILITY_OPS_OPTIONS,
  createObservabilityOpsConfigurationModule,
  validateServerOptions
};
export type { ServerOptions };
