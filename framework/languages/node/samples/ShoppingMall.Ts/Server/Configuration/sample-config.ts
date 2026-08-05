import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';

interface ShoppingMallServerConfig {
  readonly apiAHttpUrl: string;
  readonly apiBHttpUrl: string;
  readonly workflowAHttpUrl: string;
  readonly workflowBHttpUrl: string;
  readonly workflowAChannelEndpoint: string;
  readonly workflowBChannelEndpoint: string;
  readonly workflowASpotEndpoint: string;
  readonly workflowBSpotEndpoint: string;
  readonly workflowASpotPubEndpoint: string;
  readonly workflowBSpotPubEndpoint: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly logDir: string;
  readonly workDir: string;
}

const SHOPPINGMALL_SAMPLE_CONFIG = Symbol.for('SHOPPINGMALL_SAMPLE_CONFIG');
class ShoppingMallConfigurationModule {}
Module({})(ShoppingMallConfigurationModule);

function createShoppingMallConfigurationModule(
  requiredKeys: readonly (keyof ShoppingMallServerConfig)[]
): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  return {
    module: ShoppingMallConfigurationModule,
    imports: [ConfigModule.forRoot({
      cache: true,
      ignoreEnvFile: true,
      isGlobal: false,
      load: [() => ({ sample: readSampleConfig(configPath) })],
      skipProcessEnv: true,
      validatePredefined: false
    })],
    providers: [{
      provide: SHOPPINGMALL_SAMPLE_CONFIG,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validateSampleConfig(config.get('sample'), requiredKeys)
    }],
    exports: [SHOPPINGMALL_SAMPLE_CONFIG]
  };
}

function readConfigPath(args: readonly string[]): string {
  if (args.length !== 2 || args[0] !== '--config' || args[1].startsWith('--')) {
    throw new Error('--config <path> is the only supported framework host argument.');
  }
  return args[1];
}

function readSampleConfig(configPath: string): unknown {
  const document = JSON.parse(fs.readFileSync(configPath, 'utf8')) as { sample?: unknown };
  if (document.sample === undefined) throw new Error("Configuration section 'sample' is required.");
  return document.sample;
}

function validateSampleConfig(
  value: unknown,
  requiredKeys: readonly (keyof ShoppingMallServerConfig)[]
): ShoppingMallServerConfig {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'sample' must be an object.");
  }
  const config = value as Record<string, unknown>;
  for (const key of requiredKeys) {
    if (typeof config[key] !== 'string' || (config[key] as string).length === 0) {
      throw new Error(`Configuration value 'sample.${key}' must be a non-empty string.`);
    }
  }
  return config as unknown as ShoppingMallServerConfig;
}

export { SHOPPINGMALL_SAMPLE_CONFIG, createShoppingMallConfigurationModule };
export type { ShoppingMallServerConfig };
