import React, { useEffect, useState, useMemo } from 'react';
import { useNavigate } from 'react-router-dom';
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, Legend, ResponsiveContainer,
} from 'recharts';
import { useDataService } from '../api/dataService';
import type { ModuleState } from '../api/dataService';
import { getSchedulerClasses, instantiateModule, getAvailableModules } from '../api/rest';
import { useClassesHistory } from '../api/useHistory';
import type { ClassInfo } from '../types';
import type { AvailableModule } from '../api/rest';
import { MODULE_STATES } from '../types';
import { DataTree } from '../components/DataTree';

const CLASS_COLORS = ['#4a8fe0', '#3dba70', '#f59e0b', '#e05ca0', '#a78bfa', '#34d399'];
const WINDOW_MS = 15_000;
const TICK_MS   = 500;

function classColor(idx: number) { return CLASS_COLORS[idx % CLASS_COLORS.length]; }

function statusColor(state: number) {
  if (state === 3) return '#3dba70';
  if (state === 2) return '#60a5fa';
  if (state === 5) return '#f87171';
  if (state === 4) return '#f59e0b';
  return '#3a3d52';
}

// ─── Module card ─────────────────────────────────────────────────────────────
function ModuleCard({ mod }: { mod: ModuleState }) {
  const navigate = useNavigate();
  const { info, liveStats } = mod;
  const stats = liveStats ?? info.stats;
  const cycleMs = stats ? (stats.lastCycleTimeUs / 1000).toFixed(3) : '—';
  const jitterMs = stats ? (stats.lastJitterUs / 1000).toFixed(3) : '—';
  return (
    <div className="pv-module-card" onClick={() => navigate(`/module/${info.id}`)} role="button" tabIndex={0} onKeyDown={e => e.key === 'Enter' && navigate(`/module/${info.id}`)}>
      <div className="pv-card-header">
        <span className="pv-card-dot" style={{ background: statusColor(info.state) }} />
        <span className="pv-card-name">
          {info.id}
          {info.className && info.className !== info.id && <span className="pv-card-type"> ({info.className})</span>}
        </span>
        <span className="pv-card-status" style={{ color: statusColor(info.state) }}>{MODULE_STATES[info.state] ?? 'Unknown'}</span>
      </div>
      <div className="pv-card-stats">
        <div>Cycle Time: <b>{cycleMs}ms</b></div>
        <div>Jitter: <b>{jitterMs}ms</b></div>
      </div>
      <div>
        <DataTree data={mod.liveSummary ?? {}} onChange={undefined} compact={true} />
      </div>
    </div>
  );
}

// ─── Per-class line chart ─────────────────────────────────────────────────────
interface ClassPoint { t: number; [cls: string]: number }
interface ClassChartProps { title: string; history: ClassPoint[]; classNames: string[]; colors: Map<string, string>; }

const ClassChart = React.memo(({ title, history, classNames, colors, now }: ClassChartProps & { now: number }) => {
  // Use the captured `now` so the X-axis domain is stable per render and doesn't
  // force a Recharts re-layout on every paint.
  const domainMin = now - WINDOW_MS;
  return (
    <div className="pv-chart-card">
      <div className="pv-chart-title">{title}</div>
      <div className="pv-chart-body">
        <ResponsiveContainer width="100%" height={220}>
          <LineChart data={history} margin={{ top: 8, right: 16, bottom: 4, left: 0 }}>
            <CartesianGrid strokeDasharray="3 3" stroke="#1a2037" />
            <XAxis dataKey="t" type="number" domain={[domainMin, now]} tickFormatter={(t: number) => { const s = Math.round((now - t) / 1000); return s <= 0 ? 'now' : `-${s}s`; }} stroke="#252d3e" tick={{ fill: '#3d4862', fontSize: 10 }} tickCount={7} />
            <YAxis stroke="#252d3e" tick={{ fill: '#3d4862', fontSize: 10 }} width={42} tickFormatter={(v: number) => v < 1 ? v.toPrecision(2) : v.toFixed(0)} />
            <Tooltip contentStyle={{ background: '#141924', border: '1px solid #252d3e', borderRadius: 6, fontSize: 11 }} itemStyle={{ color: '#aab8d8' }} labelFormatter={(t: unknown) => new Date(Number(t)).toLocaleTimeString()} />
            <Legend wrapperStyle={{ fontSize: 11, color: '#5a6480' }} />
            {classNames.map(cls => <Line key={cls} type="monotone" dataKey={cls} stroke={colors.get(cls) ?? '#888'} strokeWidth={1.5} dot={false} isAnimationActive={false} connectNulls={true} />)}
          </LineChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
});

// ─── Dashboard page ───────────────────────────────────────────────────────────
export default function Dashboard() {
  const { modules, lastError } = useDataService();
  const moduleList = useMemo(() => Object.values(modules), [modules]);

  const [classes, setClasses] = useState<ClassInfo[]>([]);
  useEffect(() => { getSchedulerClasses().then(setClasses).catch(() => {}); const t = setInterval(() => getSchedulerClasses().then(setClasses).catch(() => {}), 5_000); return () => clearInterval(t); }, []);

  const classNames = useMemo(() => classes.map(c => c.name), [classes]);
  const colorMap   = useMemo(() => new Map(classes.map((c, i) => [c.name, classColor(i)])), [classes]);

  // Poll per-class history at 1Hz over REST. The dashboard charts a 15s window,
  // so the WS path no longer carries cycleHistory. Server bins samples into
  // 100ms buckets and reports max cycle / max |jitter| per bucket -- that
  // matches one X-axis grid step at our zoom and keeps the payload tiny.
  const classHistoryMap = useClassesHistory(classNames, WINDOW_MS, 1000, 100);

  const [chartNow, setChartNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setChartNow(Date.now()), TICK_MS);
    return () => clearInterval(t);
  }, []);

  const history = useMemo<ClassPoint[]>(() => {
    // Different classes tick at different periods, so two classes will rarely
    // share the exact same `t`. We keep each sample at its true timestamp;
    // ClassChart uses connectNulls so each class's line stays continuous
    // across points where another class produced the value.
    const pointMap = new Map<number, ClassPoint>();
    for (const [name, samples] of classHistoryMap.entries()) {
      for (const sample of samples) {
        let p = pointMap.get(sample.t);
        if (!p) { p = { t: sample.t }; pointMap.set(sample.t, p); }
        p[name] = sample.cycle / 1000;
        p[`jitter_${name}`] = sample.jitter / 1000;
      }
    }
    return Array.from(pointMap.values()).sort((a, b) => a.t - b.t);
  }, [classHistoryMap]);

  // ── New instance form ────────────────────────────────────────────────────
  const [showInstForm, setShowInstForm] = useState(false);
  const [instSo,       setInstSo]       = useState('');
  const [instId,       setInstId]       = useState('');
  const [instBusy,     setInstBusy]     = useState(false);
  const [instError,    setInstError]    = useState('');
  const [available,    setAvailable]    = useState<AvailableModule[]>([]);

  useEffect(() => {
    if (!showInstForm) return;
    getAvailableModules().then(setAvailable).catch(() => {});
  }, [showInstForm]);

  // Derive suggested ID from selected .so (strip lib prefix + extension)
  function soToDefaultId(so: string): string {
    return so.replace(/^lib/, '').replace(/\.so$/, '').replace(/[^a-zA-Z0-9_]/g, '_');
  }

  async function handleInstantiate(e: React.FormEvent) {
    e.preventDefault();
    if (!instSo || !instId) return;
    setInstBusy(true); setInstError('');
    try {
      await instantiateModule(instSo, instId);
      setInstSo(''); setInstId(''); setShowInstForm(false);
    } catch (err: unknown) {
      setInstError(err instanceof Error ? err.message : String(err));
    } finally {
      setInstBusy(false);
    }
  }
  // ────────────────────────────────────────────────────────────────────────

  const jitterNames  = useMemo(() => classNames.map(c => `jitter_${c}`), [classNames]);
  const jitterColors = useMemo(() => new Map(classNames.map((c, i) => [`jitter_${c}`, classColor(i)])), [classNames]);

  return (
    <div className="pv-layout">
      <div className="pv-content">
        <div className="pv-page-title" style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
          <span>Module Dashboard</span>
          <button className="btn secondary" style={{ fontSize: '0.8rem', padding: '0.25rem 0.75rem' }}
            onClick={() => { setShowInstForm(v => !v); setInstError(''); }}>
            {showInstForm ? 'Cancel' : '+ New Instance'}
          </button>
        </div>

        {showInstForm && (
          <form className="inst-form" onSubmit={handleInstantiate}>
            <label>
              Module class
              <select value={instSo} disabled={instBusy}
                onChange={e => {
                  setInstSo(e.target.value);
                  if (!instId) setInstId(soToDefaultId(e.target.value));
                }}
                required>
                <option value="">— select .so —</option>
                {available.map(a => (
                  <option key={a.filename} value={a.filename}>
                    {a.className} v{a.version} ({a.filename})
                  </option>
                ))}
              </select>
            </label>
            <label>
              Instance ID
              <input value={instId} onChange={e => setInstId(e.target.value)}
                placeholder="left_motor" disabled={instBusy} required />
            </label>
            <button className="btn primary" type="submit" disabled={instBusy || !instSo || !instId}>
              {instBusy ? 'Creating…' : 'Create'}
            </button>
            {instError && <span className="disk-msg" style={{ color: '#f87171' }}>{instError}</span>}
          </form>
        )}

        {lastError && <div className="error-msg" style={{ margin: '0 1.5rem 0.75rem' }}>{lastError}</div>}
        <div className="pv-cards-grid">
          {moduleList.map(m => <ModuleCard key={m.info.id} mod={m} />)}
          {moduleList.length === 0 && <p style={{ color: '#3d4862', gridColumn: '1/-1' }}>No modules loaded.</p>}
        </div>
        {classNames.length > 0 && (
          <div className="pv-charts-row">
            <ClassChart title="Cycle Time (ms)" history={history} classNames={classNames} colors={colorMap} now={chartNow} />
            <ClassChart title="Jitter (ms)" history={history} classNames={jitterNames} colors={jitterColors} now={chartNow} />
          </div>
        )}
      </div>
    </div>
  );
}
