import * as fs from 'node:fs';
import { Module, type DynamicModule } from '@nestjs/common';
import { ConfigModule, ConfigService } from '@nestjs/config';
type BingoSampleConfig = {
  sessionEndpoint: string;
  sessionSpotEndpoint: string;
  sessionSpotPubSubEndpoint: string;
  playEndpoint: string;
  playSpotEndpoint: string;
  playSpotPubSubEndpoint: string;
  apiEndpoint: string;
  apiMatchmakingEndpoint: string;
  matchmakingEndpoint: string;
  redisEndpoint: string;
  redisKeyPrefix: string;
  logDir: string;
};

const BINGO_SAMPLE_CONFIG = Symbol.for('BINGO_SAMPLE_CONFIG');
class BingoConfigurationModule {}
Module({})(BingoConfigurationModule);

function createBingoConfigurationModule(requiredKeys: readonly (keyof BingoSampleConfig)[]): DynamicModule {
  const configPath = readConfigPath(process.argv.slice(2));
  return {
    module: BingoConfigurationModule,
    imports: [
      ConfigModule.forRoot({
        cache: true,
        ignoreEnvFile: true,
        isGlobal: false,
        load: [() => ({ sample: readSampleConfig(configPath) })],
        skipProcessEnv: true,
        validatePredefined: false
      })
    ],
    providers: [{
      provide: BINGO_SAMPLE_CONFIG,
      inject: [ConfigService],
      useFactory: (config: ConfigService) => validateSampleConfig(config.get('sample'), requiredKeys)
    }],
    exports: [BINGO_SAMPLE_CONFIG]
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
  requiredKeys: readonly (keyof BingoSampleConfig)[]
): BingoSampleConfig {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error("Configuration section 'sample' must be an object.");
  }
  const config = value as Record<string, unknown>;
  for (const key of requiredKeys) {
    if (typeof config[key] !== 'string' || (config[key] as string).length === 0) {
      throw new Error(`Configuration value 'sample.${key}' must be a non-empty string.`);
    }
  }
  return config as BingoSampleConfig;
}

export { BINGO_SAMPLE_CONFIG, createBingoConfigurationModule };
export type { BingoSampleConfig };
