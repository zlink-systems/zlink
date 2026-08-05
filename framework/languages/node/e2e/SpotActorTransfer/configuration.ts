import * as fs from 'node:fs';
import { Module, type DynamicModule, type InjectionToken } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

function createSpotActorTransferConfigurationModule<T>(
  token: InjectionToken,
  validate: (value: unknown) => T
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  class SpotActorTransferConfigurationModule {}
  Module({})(SpotActorTransferConfigurationModule);
  return {
    module: SpotActorTransferConfigurationModule,
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
  streamEndpoint?: string;
  evidenceFile: string;
  logDir: string;
}

const SPOT_ACTOR_TRANSFER_OPTIONS = Symbol.for('SPOT_ACTOR_TRANSFER_OPTIONS');

export {
  SPOT_ACTOR_TRANSFER_OPTIONS,
  createSpotActorTransferConfigurationModule,
  validateServerOptions
};
export type { ServerOptions };
