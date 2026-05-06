/**
 * DataService — shared reactive store for module data.
 *
 * Provides:
 *  - Latest snapshot of all module info (from REST poll)
 *  - Live runtime + stats (from WebSocket)
 *  - Live write helper (debounced, fires-and-forgets POST)
 */
import { useEffect, useState, useCallback, useContext, useRef, createContext } from 'react';
import { getModules, getModule, patchModuleData } from './rest';
import type { ModuleInfo, ModuleDetail, LiveUpdate, DataSection } from '../types';

// Server message kinds emitted on /ws.
interface RuntimeUpdate {
  type: 'runtime';
  modules: Record<string, { runtime: Record<string, unknown> }>;
}
type WsMessage = LiveUpdate | RuntimeUpdate;

// ---------- types ----------

export interface ModuleState {
  info: ModuleInfo;
  detail: ModuleDetail | null;
  liveRuntime: Record<string, unknown>;
  liveSummary: Record<string, unknown>;
  liveStats: { cycleCount: number; lastCycleTimeUs: number; maxCycleTimeUs: number; overrunCount: number; lastJitterUs: number } | undefined;
}

export interface DataServiceState {
  modules: Record<string, ModuleState>;
  liveClasses: Record<string, import('../types').ClassLiveStats>;
  wsConnected: boolean;
  lastError: string;
  fetchDetail: (id: string) => Promise<void>;
  writeField: (id: string, section: DataSection, path: string[], value: unknown) => void;
  // Refcounted subscription to a module's live runtime stream over /ws.
  // Each call must be paired with unsubscribeRuntime; the actual server
  // unsubscribe only happens when the count drops to zero.
  subscribeRuntime: (moduleId: string) => void;
  unsubscribeRuntime: (moduleId: string) => void;
}

// ---------- context ----------

export const DataServiceContext = createContext<DataServiceState>({
  modules: {},
  liveClasses: {},
  wsConnected: false,
  lastError: '',
  fetchDetail: async () => {},
  writeField: () => {},
  subscribeRuntime: () => {},
  unsubscribeRuntime: () => {},
});

// ---------- hook ----------

export function useDataService(): DataServiceState {
  return useContext(DataServiceContext);
}

// ---------- provider hook (used once at App level) ----------

export function useDataServiceProvider(): DataServiceState {
  const [modules, setModules] = useState<Record<string, ModuleState>>({});
  const [liveClasses, setLiveClasses] = useState<Record<string, import('../types').ClassLiveStats>>({});
  const [wsConnected, setWsConnected] = useState(false);
  const [lastError, setLastError] = useState('');

  // ---------- REST poll for module list ----------
  useEffect(() => {
    let cancelled = false;
    async function poll() {
      try {
        const list = await getModules();
        if (cancelled) return;
        setModules((prev) => {
          const next = { ...prev };
          for (const info of list) {
            next[info.id] = {
              ...next[info.id],
              info,
              detail: next[info.id]?.detail ?? null,
              liveRuntime: next[info.id]?.liveRuntime ?? {},
              liveSummary: next[info.id]?.liveSummary ?? {},
              liveStats: next[info.id]?.liveStats,
            };
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

  // ---------- WebSocket for live runtime ----------
  // Refcounted runtime subscriptions. The active socket and current refcounts
  // are stored in refs so the subscribe/unsubscribe callbacks remain stable
  // across re-renders.
  const wsRef = useRef<WebSocket | null>(null);
  const runtimeRefcounts = useRef<Map<string, number>>(new Map());

  const sendSubscribe = useCallback((moduleIds: string[]) => {
    const sock = wsRef.current;
    if (!sock || sock.readyState !== WebSocket.OPEN || moduleIds.length === 0) return;
    sock.send(JSON.stringify({
      type: 'subscribe',
      topics: moduleIds.map(id => `module/${id}/runtime`),
    }));
  }, []);

  const sendUnsubscribe = useCallback((moduleIds: string[]) => {
    const sock = wsRef.current;
    if (!sock || sock.readyState !== WebSocket.OPEN || moduleIds.length === 0) return;
    sock.send(JSON.stringify({
      type: 'unsubscribe',
      topics: moduleIds.map(id => `module/${id}/runtime`),
    }));
  }, []);

  const subscribeRuntime = useCallback((moduleId: string) => {
    const counts = runtimeRefcounts.current;
    const prev = counts.get(moduleId) ?? 0;
    counts.set(moduleId, prev + 1);
    if (prev === 0) sendSubscribe([moduleId]);
  }, [sendSubscribe]);

  const unsubscribeRuntime = useCallback((moduleId: string) => {
    const counts = runtimeRefcounts.current;
    const prev = counts.get(moduleId) ?? 0;
    if (prev <= 0) return;
    if (prev === 1) {
      counts.delete(moduleId);
      sendUnsubscribe([moduleId]);
    } else {
      counts.set(moduleId, prev - 1);
    }
  }, [sendUnsubscribe]);

  useEffect(() => {
    let cancelled = false;
    let ws: WebSocket | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let backoffMs = 1000;
    const maxBackoffMs = 30_000;
    // Permissive UTF-8 decoder: replaces invalid byte sequences with U+FFFD
    // instead of throwing, so a single bad byte from a device payload can't
    // poison the live stream.
    const decoder = new TextDecoder('utf-8', { fatal: false });

    // Throttle live-frame React updates. The server pushes at 10Hz but
    // nothing on screen needs to re-render that fast; flushing at 2Hz cuts
    // dev-mode profiler overhead by 5x. The newest frame always wins.
    const LIVE_FLUSH_MS = 500;
    let pendingLive: LiveUpdate | null = null;
    let liveFlushTimer: ReturnType<typeof setTimeout> | null = null;

    function shallowStatsEqual(
      a: ModuleState['liveStats'] | undefined,
      b: ModuleState['liveStats'] | undefined,
    ): boolean {
      if (a === b) return true;
      if (!a || !b) return false;
      return a.cycleCount === b.cycleCount
        && a.lastCycleTimeUs === b.lastCycleTimeUs
        && a.maxCycleTimeUs === b.maxCycleTimeUs
        && a.overrunCount === b.overrunCount
        && a.lastJitterUs === b.lastJitterUs;
    }

    function classStatsEqual(
      a: import('../types').ClassLiveStats | undefined,
      b: import('../types').ClassLiveStats | undefined,
    ): boolean {
      if (a === b) return true;
      if (!a || !b) return false;
      return a.lastJitterUs === b.lastJitterUs
        && a.lastCycleTimeUs === b.lastCycleTimeUs
        && a.maxCycleTimeUs === b.maxCycleTimeUs
        && a.tickCount === b.tickCount
        && a.memberCount === b.memberCount
        && a.lastTickStartMs === b.lastTickStartMs;
    }

    // Cheap structural equality for the small "summary" snapshot. JSON.stringify
    // is fine here because summaries are flat objects of primitives.
    function summaryEqual(a: unknown, b: unknown): boolean {
      if (a === b) return true;
      try { return JSON.stringify(a) === JSON.stringify(b); } catch { return false; }
    }

    function applyLive(data: LiveUpdate) {
      setModules((prev) => {
        let mutated = false;
        const next: Record<string, ModuleState> = prev;
        const draft: Record<string, ModuleState> = { ...prev };
        for (const [id, live] of Object.entries(data.modules)) {
          const cur = prev[id];
          if (!cur) continue;
          const newSummary = live.summary ?? cur.liveSummary;
          const newStats = live.stats;
          const summarySame = newSummary === cur.liveSummary || summaryEqual(newSummary, cur.liveSummary);
          const statsSame = shallowStatsEqual(newStats, cur.liveStats);
          if (summarySame && statsSame) continue;
          draft[id] = {
            ...cur,
            liveSummary: summarySame ? cur.liveSummary : newSummary,
            liveStats: statsSame ? cur.liveStats : newStats,
          };
          mutated = true;
        }
        return mutated ? draft : next;
      });
      if (data.classes) {
        const incoming = data.classes;
        setLiveClasses((prev) => {
          let mutated = false;
          const draft = { ...prev };
          for (const [name, stats] of Object.entries(incoming)) {
            if (classStatsEqual(prev[name], stats)) continue;
            draft[name] = stats;
            mutated = true;
          }
          // Drop classes that disappeared
          for (const name of Object.keys(prev)) {
            if (!(name in incoming)) { delete draft[name]; mutated = true; }
          }
          return mutated ? draft : prev;
        });
      }
    }

    function scheduleLiveFlush() {
      if (liveFlushTimer) return;
      liveFlushTimer = setTimeout(() => {
        liveFlushTimer = null;
        if (cancelled || !pendingLive) return;
        const frame = pendingLive;
        pendingLive = null;
        applyLive(frame);
      }, LIVE_FLUSH_MS);
    }

    function scheduleReconnect() {
      if (cancelled) return;
      if (reconnectTimer) return; // already scheduled
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        connect();
      }, backoffMs);
      // Exponential backoff so a flapping/broken server doesn't spawn
      // hundreds of failed sockets per minute.
      backoffMs = Math.min(maxBackoffMs, backoffMs * 2);
    }

    function connect() {
      if (cancelled) return;
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const sock = new WebSocket(`${proto}//${window.location.host}/ws`);
      // Server sends binary frames (JSON encoded as bytes) so non-UTF-8
      // bytes in device data don't kill the connection.
      sock.binaryType = 'arraybuffer';
      ws = sock;
      wsRef.current = sock;

      sock.onopen = () => {
        if (cancelled || ws !== sock) { try { sock.close(); } catch { /* ignore */ } return; }
        backoffMs = 1000; // reset backoff on success
        setWsConnected(true);
        // Re-send any active subscriptions after a (re)connect.
        const ids = Array.from(runtimeRefcounts.current.keys());
        if (ids.length > 0) {
          sock.send(JSON.stringify({
            type: 'subscribe',
            topics: ids.map(id => `module/${id}/runtime`),
          }));
        }
      };
      sock.onclose = () => {
        // Only react if this is still the active socket and we haven't unmounted.
        if (cancelled || ws !== sock) return;
        ws = null;
        if (wsRef.current === sock) wsRef.current = null;
        setWsConnected(false);
        scheduleReconnect();
      };
      sock.onerror = () => {
        // Don't call close here — onclose will fire on its own and we'd risk
        // closing a *different* socket if `ws` was reassigned.
      };
      sock.onmessage = (ev) => {
        if (cancelled || ws !== sock) return;
        try {
          const text = typeof ev.data === 'string'
            ? ev.data
            : decoder.decode(ev.data as ArrayBuffer);
          const data = JSON.parse(text) as WsMessage;
          if (data.type === 'live') {
            // Coalesce: keep only the newest pending frame and flush at LIVE_FLUSH_MS.
            pendingLive = data;
            scheduleLiveFlush();
          } else if (data.type === 'runtime') {
            setModules((prev) => {
              let mutated = false;
              const next: Record<string, ModuleState> = { ...prev };
              for (const [id, mod] of Object.entries(data.modules)) {
                const cur = prev[id];
                if (!cur) continue;
                if (summaryEqual(mod.runtime, cur.liveRuntime)) continue;
                next[id] = { ...cur, liveRuntime: mod.runtime };
                mutated = true;
              }
              return mutated ? next : prev;
            });
          }
        } catch { /* ignore parse errors */ }
      };
    }

    connect();
    return () => {
      cancelled = true;
      if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
      if (liveFlushTimer) { clearTimeout(liveFlushTimer); liveFlushTimer = null; }
      pendingLive = null;
      if (ws) {
        const sock = ws;
        ws = null;
        if (wsRef.current === sock) wsRef.current = null;
        // Detach handlers so a late close/error after unmount can't update state.
        sock.onopen = null;
        sock.onclose = null;
        sock.onerror = null;
        sock.onmessage = null;
        try { sock.close(); } catch { /* ignore */ }
      }
    };
  }, []);

  // ---------- fetch full module detail ----------
  const fetchDetail = useCallback(async (id: string) => {
    try {
      const detail = await getModule(id);
      setModules((prev) => {
        const existing = prev[id];
        return {
          ...prev,
          [id]: {
            info: existing?.info ?? {
              id: detail.id,
              name: detail.name,
              version: detail.version,
              state: detail.state,
              path: detail.path,
              stats: detail.stats,
            },
            detail,
            liveRuntime: detail.data.runtime,
            liveSummary: existing?.liveSummary ?? detail.data.summary,
            liveStats: existing?.liveStats,
          },
        };
      });
    } catch (e: unknown) {
      setLastError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  // ---------- live write a single field ----------
  const writeField = useCallback((
    id: string,
    section: DataSection,
    path: string[],
    value: unknown,
  ) => {
    // Optimistically update local state.
    setModules((prev) => {
      const mod = prev[id];
      if (!mod?.detail) return prev;
      const newData = deepSet({ ...mod.detail.data[section] } as Record<string, unknown>, path, value) as Record<string, unknown>;
      return {
        ...prev,
        [id]: {
          ...mod,
          detail: {
            ...mod.detail,
            data: { ...mod.detail.data, [section]: newData },
          },
        },
      };
    });

    // Send a server-side read-modify-write PATCH with just the pointer + value.
    // The server reads the current live section, applies the change, and writes back.
    const ptr = '/' + path.join('/');
    patchModuleData(id, section, ptr, value)
      .catch((e: unknown) => setLastError(e instanceof Error ? e.message : String(e)));
  }, []);

  return { modules, liveClasses, wsConnected, lastError, fetchDetail, writeField, subscribeRuntime, unsubscribeRuntime };
}

// ---------- deep-set utility ----------
// Builds a nested structure from a path. Numeric path segments produce arrays;
// non-numeric segments produce objects. This ensures axes/0/myval round-trips
// as {"axes": [{"myval": v}]} rather than {"axes": {"0": {"myval": v}}}.
function deepSet(target: unknown, path: string[], value: unknown): unknown {
  if (path.length === 0) return value;
  const [head, ...tail] = path;
  const numericIndex = /^\d+$/.test(head) ? parseInt(head, 10) : null;

  if (numericIndex !== null) {
    // Array branch: head is a numeric index
    const arr: unknown[] = Array.isArray(target) ? [...target] : [];
    arr[numericIndex] = deepSet(arr[numericIndex], tail, value);
    return arr;
  } else {
    // Object branch: head is a string key
    const obj: Record<string, unknown> =
      target !== null && typeof target === 'object' && !Array.isArray(target)
        ? { ...(target as Record<string, unknown>) }
        : {};
    obj[head] = deepSet(obj[head], tail, value);
    return obj;
  }
}
