/**
 * DataService — shared REST metadata store for modules.
 *
 * Live runtime/summary/stats now stream from the OPC-UA facade via LuxReact
 * (useVariable); this context only holds the REST-polled module list and the
 * full per-module detail (config/recipe + schema), plus the last error. Writes
 * and live reads go through the OPC-UA machine, not here.
 */
import { useEffect, useState, useCallback, useContext, createContext } from 'react';
import { getModules, getModule } from './rest';
import type { ModuleInfo, ModuleDetail } from '../types';

// ---------- types ----------

export interface ModuleState {
  info: ModuleInfo;
  detail: ModuleDetail | null;
}

export interface DataServiceState {
  modules: Record<string, ModuleState>;
  lastError: string;
  fetchDetail: (id: string) => Promise<void>;
}

// ---------- context ----------

export const DataServiceContext = createContext<DataServiceState>({
  modules: {},
  lastError: '',
  fetchDetail: async () => {},
});

export function useDataService(): DataServiceState {
  return useContext(DataServiceContext);
}

// ---------- provider hook (used once at App level) ----------

export function useDataServiceProvider(): DataServiceState {
  const [modules, setModules] = useState<Record<string, ModuleState>>({});
  const [lastError, setLastError] = useState('');

  // REST poll for the module list (info/state/version/class). Live values come
  // from LuxReact, so this only needs to track the set of modules + metadata.
  useEffect(() => {
    let cancelled = false;
    async function poll() {
      try {
        const list = await getModules();
        if (cancelled) return;
        setModules((prev) => {
          const next: Record<string, ModuleState> = {};
          for (const info of list) {
            next[info.id] = { info, detail: prev[info.id]?.detail ?? null };
          }
          return next;
        });
      } catch (e: unknown) {
        if (!cancelled) setLastError(e instanceof Error ? e.message : String(e));
      }
    }
    poll();
    const t = setInterval(poll, 5000);
    return () => { cancelled = true; clearInterval(t); };
  }, []);

  // Fetch full module detail (config/recipe values + schema) via REST.
  const fetchDetail = useCallback(async (id: string) => {
    try {
      const detail = await getModule(id);
      setModules((prev) => {
        const { data: _data, ...info } = detail;
        return { ...prev, [id]: { info: prev[id]?.info ?? (info as ModuleInfo), detail } };
      });
    } catch (e: unknown) {
      setLastError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  return { modules, lastError, fetchDetail };
}
