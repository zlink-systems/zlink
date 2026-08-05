import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

interface ServerOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly routerEndpoint: string;
  readonly pubSubEndpoint: string;
  readonly streamEndpoint?: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

const TO_ACTOR_OPTIONS = Symbol.for('TO_ACTOR_OPTIONS');

function createToActorConfigurationModule(): DynamicModule {
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  const configPath = args[1];
  class ToActorConfigurationModule {}
  Module({})(ToActorConfigurationModule);
  return {
    module: ToActorConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ e2e: readConfig(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: TO_ACTOR_OPTIONS,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validateOptions(config.get('e2e'))
    }],
    exports: [TO_ACTOR_OPTIONS]
  };
}

function readConfig(path: string): unknown {
  const document = JSON.parse(fs.readFileSync(path, 'utf8')) as { e2e?: unknown };
  if (document.e2e === undefined) throw new Error("Configuration section 'e2e' is required.");
  return document.e2e;
}

function validateOptions(value: unknown): ServerOptions {
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
    evidenceFile: typeof values.evidenceFile === 'string' ? values.evidenceFile : undefined,
    logDir: required('logDir')
  };
}

export { TO_ACTOR_OPTIONS, createToActorConfigurationModule };
export type { ServerOptions };
