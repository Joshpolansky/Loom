import { useEffect, useState, useCallback, useRef, useMemo } from 'react';
import { Link } from 'react-router-dom';
import { getSchedulerClasses, updateClassDef, reassignModuleClass, createClass } from '../api/rest';
import { useClassesHistory } from '../api/useHistory';
import type { ClassInfo } from '../types';

// ─── Constants ─────────────────────────────────────────────────────────────
const CLASS_COLORS = [
  '#4a8fe0', '#3dba70', '#f59e0b', '#e05ca0',
  '#a78bfa', '#34d399', '#fb923c', '#38bdf8',
];
function classColor(idx: number) { return CLASS_COLORS[idx % CLASS_COLORS.length]; }

// Keep a generous history buffer relative to the largest zoom so blocks live
// long enough to scroll across the entire visible window. Anything shorter and
// blocks disappear at the left edge before they should.
const MAX_HISTORY_MS = 5_000;
const ZOOM_LEVELS = [ 10, 50, 100, 200, 500, 1_000, 2_000];

function formatZoomLabel(ms: number) {
  return ms < 1000 ? `${ms}ms` : `${ms / 1000}s`;
}

function makeTickLabels(windowMs: number, count = 7): string[] {
  return Array.from({ length: count }, (_, i) => {
    const t = (i / (count - 1)) * windowMs;
    if (windowMs < 1000) return `${Math.round(t)}ms`;
    const s = t / 1000;
    return s % 1 === 0 ? `${s}s` : `${s.toFixed(1)}s`;
  });
}

// ─── Execution block tracking ─────────────────────────────────────────────────
interface ExecBlock { startMs: number; durationMs: number }

// ─── Editable numeric field ─────────────────────────────────────────────────
function NumericField({ value, min, max, unit, onSave }: {
  value: number; min?: number; max?: number; unit?: string; onSave: (v: number) => void;
}) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(String(value));
  useEffect(() => { setDraft(String(value)); }, [value]);

  function commit() {
    const n = parseInt(draft, 10);
    if (!isNaN(n) && (min == null || n >= min) && (max == null || n <= max)) onSave(n);
    else setDraft(String(value));
    setEditing(false);
  }

  if (editing) return (
    <span className="sc-field-edit">
      <input autoFocus type="number" min={min} max={max} value={draft}
        onChange={e => setDraft(e.target.value)}
        onBlur={commit}
        onKeyDown={e => { if (e.key === 'Enter') commit(); if (e.key === 'Escape') { setDraft(String(value)); setEditing(false); } }}
      />
      {unit && <span className="sc-field-unit">{unit}</span>}
    </span>
  );

  return (
    <span className="sc-field-value editable" title="Click to edit" onClick={() => { setDraft(String(value)); setEditing(true); }}>
      {value}{unit ? ` ${unit}` : ''}
      <span className="sc-edit-hint">✎</span>
    </span>
  );
}

// ─── Class config panel ─────────────────────────────────────────────────────
function ClassPanel({ info, color, onUpdate, onClose }: {
  info: ClassInfo; color: string;
  onUpdate: (name: string, patch: Partial<Pick<ClassInfo, 'period_us' | 'priority' | 'cpu_affinity' | 'spin_us'>>) => void;
  onClose: () => void;
}) {
  const stats = info.stats;
  const budget = stats ? Math.round((stats.lastCycleTimeUs / info.period_us) * 100) : null;
  return (
    <div className="sc-class-panel" style={{ borderLeft: `3px solid ${color}` }}>
      <div className="sc-panel-header">
        <span className="sc-panel-dot" style={{ background: color }} />
        <span className="sc-panel-name">{info.name}</span>
        <span className="sc-panel-badge">{stats ? 'running' : 'idle'}</span>
        <button className="sc-panel-close" onClick={onClose}>✕</button>
      </div>
      <dl className="sc-config-grid">
        <dt>Period</dt><dd><NumericField value={info.period_us} min={100} max={10000000} unit="µs" onSave={v => onUpdate(info.name, { period_us: v })} /></dd>
        <dt>Priority</dt><dd><NumericField value={info.priority} min={1} max={99} onSave={v => onUpdate(info.name, { priority: v })} /></dd>
        <dt>CPU Affinity</dt><dd><NumericField value={info.cpu_affinity} min={-1} max={63} onSave={v => onUpdate(info.name, { cpu_affinity: v })} /></dd>
        <dt>Spin Wait</dt><dd><NumericField value={info.spin_us} min={0} max={info.period_us} unit="µs" onSave={v => onUpdate(info.name, { spin_us: v })} /></dd>
      </dl>
      {stats && (
        <dl className="sc-stats-grid">
          <dt>Last Cycle</dt><dd>{stats.lastCycleTimeUs} µs</dd>
          <dt>Max Cycle</dt><dd>{stats.maxCycleTimeUs} µs</dd>
          <dt>Budget</dt><dd className={budget != null && budget > 80 ? 'sc-warn' : ''}>{budget != null ? `${budget}%` : '—'}</dd>
          <dt>Jitter</dt><dd>{stats.lastJitterUs} µs</dd>
          <dt>Ticks</dt><dd>{stats.tickCount}</dd>
        </dl>
      )}
      {info.modules && info.modules.length > 0 && (
        <div className="sc-panel-modules">
          <div className="sc-panel-modules-label">Modules in class</div>
          <ul>{info.modules.map(id => <li key={id}><Link to={`/module/${id}`}>{id}</Link></li>)}</ul>
        </div>
      )}
    </div>
  );
}

// ─── Timeline row ─────────────────────────────────────────────────────────────
function TimelineRow({ classInfo, color, blocks, windowEnd, windowMs, isDragOver, onDragOver, onDrop, onDragLeave }: {
  classInfo: ClassInfo; color: string;
  blocks: ExecBlock[]; windowEnd: number; windowMs: number;
  isDragOver: boolean;
  onDragOver: (e: React.DragEvent) => void;
  onDrop: (e: React.DragEvent) => void;
  onDragLeave: () => void;
}) {
  const windowStart = windowEnd - windowMs;
  const visible = useMemo(() =>
    blocks.filter(b => b.startMs + b.durationMs >= windowStart && b.startMs <= windowEnd),
  [blocks, windowStart, windowEnd]);

  return (
    <div
      className={`sc-row${isDragOver ? ' sc-row-dragover' : ''}`}
      onDragOver={onDragOver}
      onDrop={onDrop}
      onDragLeave={onDragLeave}
    >
      <div className="sc-row-label">
        <span className="sc-row-dot" style={{ background: color }} />
        <span>{classInfo.name}</span>
        <span className="sc-row-period">{classInfo.period_us >= 1000 ? `${classInfo.period_us / 1000}ms` : `${classInfo.period_us}µs`}</span>
      </div>
      <div className="sc-row-track">
        <div className="sc-row-idle">Idle</div>
        {visible.map((b, i) => {
          const leftPct = ((b.startMs - windowStart) / windowMs) * 100;
          const widthPct = (b.durationMs / windowMs) * 100;
          return (
            <div key={i} className="sc-block" style={{ left: `${Math.max(0, leftPct)}%`, width: `${Math.max(0.15, widthPct)}%`, background: color }}>
              <span className="sc-block-label">{classInfo.name}</span>
            </div>
          );
        })}
        {/* Draggable module chips at end of track */}
        {(classInfo.modules ?? []).map(modId => (
          <div
            key={modId}
            className="sc-module-chip"
            draggable
            onDragStart={e => { e.dataTransfer.setData('moduleId', modId); e.dataTransfer.setData('srcClass', classInfo.name); }}
          >
            {modId}
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── New class form ─────────────────────────────────────────────────────────
function NewClassForm({ onDone, onCancel }: { onDone: () => void; onCancel: () => void; }) {
  const [name, setName] = useState('');
  const [period, setPeriod] = useState('10000');
  const [priority, setPriority] = useState('50');
  const [error, setError] = useState('');

  async function submit() {
    if (!name.trim()) { setError('Name required'); return; }
    const p = parseInt(period, 10), pr = parseInt(priority, 10);
    if (isNaN(p) || p < 100) { setError('Invalid period'); return; }
    try {
      await createClass({ name: name.trim(), period_us: p, priority: pr, cpu_affinity: -1, spin_us: 0 });
      onDone();
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <div className="sc-new-class-form">
      <div className="sc-new-class-title">New Thread Class</div>
      {error && <div className="sc-form-error">{error}</div>}
      <label>Name<input value={name} onChange={e => setName(e.target.value)} placeholder="e.g. medium" autoFocus /></label>
      <label>Period (µs)<input type="number" min={100} value={period} onChange={e => setPeriod(e.target.value)} /></label>
      <label>Priority<input type="number" min={1} max={99} value={priority} onChange={e => setPriority(e.target.value)} /></label>
      <div className="sc-form-actions">
        <button className="sc-btn-create" onClick={submit}>Create</button>
        <button className="sc-btn-cancel" onClick={onCancel}>Cancel</button>
      </div>
    </div>
  );
}

// ─── SchedulerView ────────────────────────────────────────────────────────────
export default function SchedulerView() {
  // const { liveClasses } = useDataService();
  const [classes, setClasses] = useState<ClassInfo[]>([]);
  const [error, setError] = useState('');
  const [filter, setFilter] = useState('');
  const [expanded, setExpanded] = useState<Set<string>>(new Set());
  const [selected, setSelected] = useState<string | null>(null);
  const [windowEnd, setWindowEnd] = useState(Date.now());
  const [windowMs, setWindowMs] = useState(1_000);
  const [showNewForm, setShowNewForm] = useState(false);
  const [dragOver, setDragOver] = useState<string | null>(null);

  const blockHistory = useRef<Map<string, ExecBlock[]>>(new Map());

  const load = useCallback(async () => {
    try {
      const data = await getSchedulerClasses();
      setClasses(data);
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  useEffect(() => { load(); const t = setInterval(load, 2_000); return () => clearInterval(t); }, [load]);

  // Poll per-class history at 1Hz over REST. (cycleHistory is no longer
  // included in the WS broadcast — too large.) The chart still renders the
  // 1-second execution-block window using these samples.
  const classNames = useMemo(() => classes.map(c => c.name), [classes]);
  const classHistoryMap = useClassesHistory(classNames, MAX_HISTORY_MS, 1000);

  // Update execution blocks from polled history. windowEnd is anchored to the
  // end of the newest block we actually have, so the right edge always sits
  // exactly at the latest data -- no scrolling away between polls.
  useEffect(() => {
    let latestEnd = 0;
    for (const samples of classHistoryMap.values()) {
      if (samples.length > 0) {
        const last = samples[samples.length - 1];
        latestEnd = Math.max(latestEnd, last.t + last.cycle / 1000);
      }
    }
    if (latestEnd === 0) return;  // no samples yet -- keep last windowEnd

    const cutoff = latestEnd - MAX_HISTORY_MS;
    for (const [name, samples] of classHistoryMap.entries()) {
      if (!samples || samples.length === 0) continue;
      const blocks: ExecBlock[] = samples
        .filter(sample => sample.t + sample.cycle / 1000 >= cutoff)
        .map(sample => ({
          startMs: sample.t,
          durationMs: Math.max(sample.cycle / 1000, 0.05),
        }));
      blockHistory.current.set(name, blocks);
    }
    setWindowEnd(latestEnd);
  }, [classHistoryMap]);

  async function handleUpdate(name: string, patch: Partial<Pick<ClassInfo, 'period_us' | 'priority' | 'cpu_affinity' | 'spin_us'>>) {
    try {
      await updateClassDef(name, patch);
      setClasses(prev => prev.map(c => c.name === name ? { ...c, ...patch } : c));
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  async function handleDrop(e: React.DragEvent, targetClass: string) {
    e.preventDefault();
    setDragOver(null);
    const moduleId = e.dataTransfer.getData('moduleId');
    const srcClass = e.dataTransfer.getData('srcClass');
    if (!moduleId || srcClass === targetClass) return;
    try {
      await reassignModuleClass(moduleId, targetClass);
      await load();
    } catch (err: unknown) {
      setError(err instanceof Error ? err.message : String(err));
    }
  }

  const filteredClasses = useMemo(() => {
    const q = filter.toLowerCase();
    if (!q) return classes;
    return classes.filter(c => c.name.toLowerCase().includes(q) || (c.modules ?? []).some(m => m.toLowerCase().includes(q)));
  }, [classes, filter]);

  const selectedClass = selected ? classes.find(c => c.name === selected) : null;
  const selectedIdx   = selected ? classes.findIndex(c => c.name === selected) : -1;

  return (
    <div className="sc-layout">
      {/* Sidebar */}
      <aside className="sc-sidebar">
        <div className="sc-sidebar-header">
          <span>Thread Groups</span>
          <button className="sc-btn-new" onClick={() => setShowNewForm(v => !v)} title="Create new class">+</button>
        </div>
        <div className="sc-search">
          <span className="sc-search-icon">⌕</span>
          <input placeholder="Filter fields…" value={filter} onChange={e => setFilter(e.target.value)} />
        </div>

        {showNewForm && (
          <NewClassForm onDone={() => { setShowNewForm(false); load(); }} onCancel={() => setShowNewForm(false)} />
        )}

        <div className="sc-tree">
          {filteredClasses.map((c) => {
            const color = classColor(classes.indexOf(c));
            const isOpen = expanded.has(c.name);
            return (
              <div key={c.name} className="sc-group">
                <div className={`sc-group-row${selected === c.name ? ' active' : ''}`} onClick={() => {
                  setSelected(prev => prev === c.name ? null : c.name);
                  setExpanded(prev => { const s = new Set(prev); if (s.has(c.name)) s.delete(c.name); else s.add(c.name); return s; });
                }}>
                  <span className="sc-row-chevron">{isOpen ? '∨' : '›'}</span>
                  <span className="sc-group-dot" style={{ background: color }} />
                  <span className="sc-group-name">{c.name}</span>
                  <span className="sc-group-period">{c.period_us >= 1000 ? `${c.period_us / 1000}ms` : `${c.period_us}µs`}</span>
                </div>
                {isOpen && (c.modules ?? []).map(m => (
                  <div key={m} className="sc-member-row"
                    draggable
                    onDragStart={e => { e.dataTransfer.setData('moduleId', m); e.dataTransfer.setData('srcClass', c.name); }}
                  >
                    <span style={{ color, fontSize: '0.55rem' }}>▸</span>
                    <span>{m}</span>
                  </div>
                ))}
              </div>
            );
          })}
        </div>
      </aside>

      {/* Main */}
      <div className="sc-main">
        <div className="sc-page-title">Scheduler Timeline</div>

        {error && <div className="error-msg" style={{ margin: '0 1.5rem 0.5rem' }}>{error}</div>}

        {/* Selected class config panel */}
        {selectedClass && (
          <div style={{ padding: '0 1.25rem 0.75rem', flexShrink: 0 }}>
            <ClassPanel info={selectedClass} color={classColor(selectedIdx)} onUpdate={handleUpdate} onClose={() => setSelected(null)} />
          </div>
        )}

        {/* Timeline */}
        <div className="sc-timeline">
          <div className="sc-timeline-toolbar">
            <span className="sc-zoom-label">Window:</span>
            {ZOOM_LEVELS.map(z => (
              <button
                key={z}
                className={`sc-zoom-btn${windowMs === z ? ' active' : ''}`}
                onClick={() => setWindowMs(z)}
              >{formatZoomLabel(z)}</button>
            ))}
          </div>
          <div className="sc-time-axis">
            {makeTickLabels(windowMs).map(l => <span key={l} className="sc-tick">{l}</span>)}
          </div>

          {classes.map((c, i) => (
            <TimelineRow
              key={c.name}
              classInfo={c}
              color={classColor(i)}
              blocks={blockHistory.current.get(c.name) ?? []}
              windowEnd={windowEnd}
              windowMs={windowMs}
              isDragOver={dragOver === c.name}
              onDragOver={e => { e.preventDefault(); setDragOver(c.name); }}
              onDrop={e => handleDrop(e, c.name)}
              onDragLeave={() => setDragOver(null)}
            />
          ))}

          {classes.length === 0 && !error && (
            <p style={{ color: '#3d4862', padding: '2rem', textAlign: 'center' }}>No scheduler classes loaded.</p>
          )}
        </div>
      </div>
    </div>
  );
}
