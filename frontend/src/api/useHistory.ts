import { useEffect, useRef, useState } from 'react';
import { getClassHistory, getModuleHistory } from './rest';
import type { MetricSample } from '../types';

/**
 * Polls a per-target history endpoint and accumulates samples into a sliding
 * window. The server returns deltas (`?since=<lastT>`), so each poll is small.
 *
 * Returns a stable object reference per render whose `samples` array changes
 * only when new data arrives.
 */
function useHistoryImpl(
  fetcher: ((since: number) => Promise<{ samples: MetricSample[]; latest: number }>) | null,
  windowMs: number,
  intervalMs: number,
): MetricSample[] {
  const [samples, setSamples] = useState<MetricSample[]>([]);
  const sinceRef = useRef(0);
  const samplesRef = useRef<MetricSample[]>([]);

  useEffect(() => {
    if (!fetcher) {
      sinceRef.current = 0;
      samplesRef.current = [];
      setSamples([]);
      return;
    }

    let cancelled = false;
    sinceRef.current = 0;
    samplesRef.current = [];
    setSamples([]);

    const poll = async () => {
      try {
        const resp = await fetcher(sinceRef.current);
        if (cancelled) return;
        if (resp.samples.length === 0 && sinceRef.current !== 0) {
          // Trim to window even if no new data
          const cutoff = Date.now() - windowMs;
          if (samplesRef.current.length > 0 && samplesRef.current[0].t < cutoff) {
            samplesRef.current = samplesRef.current.filter(s => s.t >= cutoff);
            setSamples(samplesRef.current);
          }
          return;
        }
        sinceRef.current = Math.max(sinceRef.current, resp.latest);
        const cutoff = Date.now() - windowMs;
        const merged = [...samplesRef.current, ...resp.samples].filter(s => s.t >= cutoff);
        samplesRef.current = merged;
        setSamples(merged);
      } catch {
        // Network error; will retry next tick.
      }
    };

    poll();
    const t = setInterval(poll, intervalMs);
    return () => { cancelled = true; clearInterval(t); };
  }, [fetcher, windowMs, intervalMs]);

  return samples;
}

export function useModuleHistory(id: string | null, windowMs = 15_000, intervalMs = 1000, binMs = 0): MetricSample[] {
  const fetcher = id ? (since: number) => getModuleHistory(id, since, binMs) : null;
  // The fetcher closure changes every render; useHistoryImpl keys on it, so
  // wrap with a stable ref keyed by id.
  const ref = useRef<{ id: string | null; bin: number; fn: typeof fetcher }>({ id: null, bin: 0, fn: null });
  if (ref.current.id !== id || ref.current.bin !== binMs) ref.current = { id, bin: binMs, fn: fetcher };
  return useHistoryImpl(ref.current.fn, windowMs, intervalMs);
}

export function useClassHistory(name: string | null, windowMs = 15_000, intervalMs = 1000, binMs = 0): MetricSample[] {
  const fetcher = name ? (since: number) => getClassHistory(name, since, binMs) : null;
  const ref = useRef<{ name: string | null; bin: number; fn: typeof fetcher }>({ name: null, bin: 0, fn: null });
  if (ref.current.name !== name || ref.current.bin !== binMs) ref.current = { name, bin: binMs, fn: fetcher };
  return useHistoryImpl(ref.current.fn, windowMs, intervalMs);
}

/**
 * Polls history for many classes in parallel. Returns a `Map<name, samples>`.
 * Re-creates internal pollers when the set of names changes.
 */
export function useClassesHistory(
  names: string[],
  windowMs = 15_000,
  intervalMs = 1000,
  binMs = 0,
): Map<string, MetricSample[]> {
  const [data, setData] = useState<Map<string, MetricSample[]>>(new Map());
  const sinceMap = useRef<Map<string, number>>(new Map());
  const samplesMap = useRef<Map<string, MetricSample[]>>(new Map());

  // Stable key for the names set so the effect only re-runs when the set actually changes.
  const key = names.slice().sort().join('|');

  useEffect(() => {
    if (names.length === 0) {
      sinceMap.current = new Map();
      samplesMap.current = new Map();
      setData(new Map());
      return;
    }

    let cancelled = false;

    // Drop tracking for names that disappeared.
    for (const k of Array.from(sinceMap.current.keys())) {
      if (!names.includes(k)) {
        sinceMap.current.delete(k);
        samplesMap.current.delete(k);
      }
    }

    const pollOnce = async () => {
      const cutoff = Date.now() - windowMs;
      const results = await Promise.allSettled(
        names.map(async n => {
          const since = sinceMap.current.get(n) ?? 0;
          const r = await getClassHistory(n, since, binMs);
          return { name: n, ...r };
        }),
      );
      if (cancelled) return;
      let changed = false;
      for (const res of results) {
        if (res.status !== 'fulfilled') continue;
        const { name, samples, latest } = res.value;
        sinceMap.current.set(name, Math.max(sinceMap.current.get(name) ?? 0, latest));
        const prev = samplesMap.current.get(name) ?? [];
        if (samples.length === 0) {
          if (prev.length > 0 && prev[0].t < cutoff) {
            samplesMap.current.set(name, prev.filter(s => s.t >= cutoff));
            changed = true;
          }
          continue;
        }
        const merged = [...prev, ...samples].filter(s => s.t >= cutoff);
        samplesMap.current.set(name, merged);
        changed = true;
      }
      if (changed) setData(new Map(samplesMap.current));
    };

    pollOnce();
    const t = setInterval(pollOnce, intervalMs);
    return () => { cancelled = true; clearInterval(t); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key, windowMs, intervalMs, binMs]);

  return data;
}
