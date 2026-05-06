export interface MetricSample {
  t: number;      // timestamp ms
  cycle: number;  // cycle time µs
  jitter: number; // jitter µs
}

export interface ModuleStats {
  cycleCount: number;
  overrunCount: number;
  lastCycleTimeUs: number;
  maxCycleTimeUs: number;
  lastJitterUs: number;
}

export interface ModuleInfo {
  id: string;
  name: string;
  className: string;
  version: string;
  state: number;
  path: string;
  cyclicClass?: string;
  stats?: ModuleStats;
}

export interface ModuleDetail extends ModuleInfo {
  data: {
    config: Record<string, unknown>;
    recipe: Record<string, unknown>;
    runtime: Record<string, unknown>;
    summary: Record<string, unknown>;
  };
}

export interface ClassLiveStats {
  lastJitterUs: number;
  lastCycleTimeUs: number;
  maxCycleTimeUs: number;
  tickCount: number;
  memberCount: number;
  lastTickStartMs: number;
}

export interface ClassInfo {
  name: string;
  period_us: number;
  cpu_affinity: number;
  priority: number;
  spin_us: number;
  stats?: ClassLiveStats;
  modules?: string[];
}

export interface LiveUpdate {
  type: 'live';
  modules: Record<
    string,
    {
      // `runtime` is no longer sent on the broadcast `live` frame; it now
      // arrives via a separate `{type:"runtime"}` frame to subscribers only.
      // Kept optional here for backward compatibility with any external
      // consumers; the dataService no longer reads it from `live`.
      runtime?: Record<string, unknown>;
      summary?: Record<string, unknown>;
      stats?: { cycleCount: number; lastCycleTimeUs: number; maxCycleTimeUs: number; overrunCount: number; lastJitterUs: number };
    }
  >;
  classes?: Record<string, ClassLiveStats>;
}

export interface ServiceCallResult {
  ok: boolean;
  response?: unknown;
  error?: string;
}

export interface ServiceInfo {
  name: string;
  schema: Record<string, unknown> | null; // JSON Schema for request payload
}

export type DataSection = 'config' | 'recipe' | 'runtime' | 'summary';

export const MODULE_STATES: Record<number, string> = {
  0: 'Unloaded',
  1: 'Loaded',
  2: 'Initialized',
  3: 'Running',
  4: 'Stopping',
  5: 'Error',
};

// --- Oscilloscope ---

export interface ProbeInfo {
  id: number;
  moduleId: string;
  path: string;
  label: string;
}

export interface ScopeSample {
  t: number; // timestamp ms
  v: number; // value
}
