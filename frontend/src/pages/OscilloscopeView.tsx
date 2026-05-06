import { useEffect, useRef, useState, useCallback, useMemo } from 'react';
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
  ResponsiveContainer,
} from 'recharts';
import {
  getScopeSchema,
  getScopeProbes,
  addScopeProbe,
  removeScopeProbe,
  getScopeData,
} from '../api/rest';
import type { ProbeInfo } from '../types';

// ---------------------------------------------------------------------------
// Colour palette
// ---------------------------------------------------------------------------
const PROBE_COLORS = [
  '#4fc3f7', '#81c784', '#ffb74d', '#f06292',
  '#ba68c8', '#4db6ac', '#e57373', '#fff176',
  '#80cbc4', '#ce93d8', '#ffcc80', '#ef9a9a',
];
function probeColor(idx: number) { return PROBE_COLORS[idx % PROBE_COLORS.length]; }

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
interface ChartConfig {
  id: string;
  title: string;
  probeIds: number[];
  scaleMode: 'shared' | 'separate';
  windowSec: number;
}

/** Serialized form stored in localStorage — uses stable identity instead of transient probe IDs */
interface SavedChart {
  id: string;
  title: string;
  probeRefs: { moduleId: string; path: string }[];
  scaleMode: 'shared' | 'separate';
  windowSec: number;
}

const LS_KEY = 'cruntime:scope:charts';

function loadSavedCharts(): SavedChart[] {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed)) return [];
    return parsed as SavedChart[];
  } catch {
    return [];
  }
}

function saveCharts(charts: ChartConfig[], probes: ProbeInfo[]) {
  const probeById = new Map(probes.map(p => [p.id, p]));
  const saved: SavedChart[] = charts.map(c => ({
    id: c.id,
    title: c.title,
    scaleMode: c.scaleMode,
    windowSec: c.windowSec,
    probeRefs: c.probeIds
      .map(id => probeById.get(id))
      .filter((p): p is ProbeInfo => p != null)
      .map(p => ({ moduleId: p.moduleId, path: p.path })),
  }));
  localStorage.setItem(LS_KEY, JSON.stringify(saved));
}

// ---------------------------------------------------------------------------
// Nested tree helpers for picker
//
// The server hands us `{ moduleId: <runtime JSON object>, ... }` -- the actual
// nested shape of each module's runtime struct. We walk it directly to expose
// numeric leaves as probeable fields. No flat tag list is ever shipped over
// the wire.
// ---------------------------------------------------------------------------
interface FieldRef { moduleId: string; path: string }
interface FlatField { kind: 'field'; field: FieldRef }
interface NestedGroup { kind: 'group'; name: string; children: TreeNode[] }
type TreeNode = FlatField | NestedGroup

/** Walks a runtime JSON object and yields a TreeNode[] in which numeric leaves
 *  become FlatFields with JSON-pointer paths. Strings, nulls, and arrays are
 *  silently ignored (the C++ probe layer only samples numeric leaves). */
function buildModuleTree(moduleId: string, root: unknown): TreeNode[] {
  function walk(value: unknown, parentPath: string): TreeNode[] {
    if (typeof value !== 'object' || value === null || Array.isArray(value)) return [];
    const entries = Object.entries(value as Record<string, unknown>)
      .sort(([a], [b]) => a.localeCompare(b));
    const out: TreeNode[] = [];
    for (const [k, v] of entries) {
      const path = `${parentPath}/${k}`;
      if (typeof v === 'number') {
        out.push({ kind: 'field', field: { moduleId, path } });
      } else if (typeof v === 'object' && v !== null && !Array.isArray(v)) {
        const children = walk(v, path);
        if (children.length > 0) out.push({ kind: 'group', name: k, children });
      }
    }
    return out;
  }
  return walk(root, '');
}

/** Recursively prune a TreeNode[] to only include leaves whose path matches a
 *  search query. Group nodes are kept iff they (or any descendant) survive. */
function filterTree(nodes: TreeNode[], query: string): TreeNode[] {
  const q = query.toLowerCase();
  const out: TreeNode[] = [];
  for (const n of nodes) {
    if (n.kind === 'field') {
      if (n.field.path.toLowerCase().includes(q)) out.push(n);
    } else {
      if (n.name.toLowerCase().includes(q)) {
        out.push(n);
      } else {
        const kept = filterTree(n.children, query);
        if (kept.length > 0) out.push({ kind: 'group', name: n.name, children: kept });
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Probe Picker Modal
// ---------------------------------------------------------------------------
interface ProbePickerProps {
  schema: Record<string, unknown>;
  activeProbeKeys: Set<string>;
  onConfirm: (selected: FieldRef[]) => void;
  onCancel: () => void;
}

function ProbePicker({ schema, activeProbeKeys, onConfirm, onCancel }: ProbePickerProps) {
  const [search, setSearch] = useState('');
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [expanded, setExpanded] = useState<Set<string>>(new Set());

  const byModule = useMemo(() => {
    const result: Record<string, TreeNode[]> = {};
    for (const [mod, runtime] of Object.entries(schema)) {
      result[mod] = buildModuleTree(mod, runtime);
    }
    return result;
  }, [schema]);

  const filteredModules = useMemo(() => {
    if (!search) return byModule;
    const q = search.toLowerCase();
    const result: Record<string, TreeNode[]> = {};
    for (const [mod, nodes] of Object.entries(byModule)) {
      if (mod.toLowerCase().includes(q)) {
        result[mod] = nodes;
      } else {
        const kept = filterTree(nodes, search);
        if (kept.length > 0) result[mod] = kept;
      }
    }
    return result;
  }, [byModule, search]);

  function fieldKey(f: FieldRef) { return `${f.moduleId}::${f.path}`; }

  function toggleField(f: FieldRef) {
    const k = fieldKey(f);
    setSelected(prev => { const s = new Set(prev); if (s.has(k)) s.delete(k); else s.add(k); return s; });
  }

  function toggleModule(_moduleId: string, nodes: TreeNode[]) {
    const allFields: FieldRef[] = [];
    function _flatten(ns: TreeNode[]) {
      for (const x of ns) {
        if (x.kind === 'field') allFields.push(x.field);
        else _flatten(x.children);
      }
    }
    _flatten(nodes);
    const addable = allFields.filter(f => !activeProbeKeys.has(fieldKey(f)));
    const allSel = addable.length > 0 && addable.every(f => selected.has(fieldKey(f)));
    setSelected(prev => {
      const s = new Set(prev);
      for (const f of addable) { if (allSel) s.delete(fieldKey(f)); else s.add(fieldKey(f)); }
      return s;
    });
  }

  function toggleExpand(moduleId: string) {
    setExpanded(prev => {
      const s = new Set(prev);
      if (s.has(moduleId)) s.delete(moduleId); else s.add(moduleId);
      return s;
    });
  }

  const selectedFields = useMemo(
    () => {
      const all: FieldRef[] = [];
      for (const nodes of Object.values(byModule)) flattenNodesToFields(nodes, all);
      return all.filter(f => selected.has(fieldKey(f)));
    },
    [byModule, selected],
  );

  // Helper: flatten a TreeNode[] into FieldRef[]
  function flattenNodesToFields(ns: TreeNode[], out: FieldRef[] = []): FieldRef[] {
    for (const x of ns) {
      if (x.kind === 'field') out.push(x.field);
      else flattenNodesToFields(x.children, out);
    }
    return out;
  }

  // Recursive renderer for group/field nodes
  function renderNode(node: TreeNode, moduleId: string, parentPath: string) {
    if (node.kind === 'field') {
      const k = fieldKey(node.field);
      const isActive = activeProbeKeys.has(k);
      const nested = parentPath !== '';
      return (
        <div
          key={node.field.path}
          className={`picker-field-row${nested ? ' picker-field-row-nested' : ''}${isActive ? ' picker-field-active' : ''}`}
          onClick={() => { if (!isActive) toggleField(node.field); }}
        >
          <input
            type="checkbox"
            checked={isActive || selected.has(k)}
            disabled={isActive}
            onChange={() => {}}
            onClick={e => { e.stopPropagation(); if (!isActive) toggleField(node.field); }}
          />
          <span className="picker-field-label">{nested ? node.field.path.split('/').pop() : node.field.path}</span>
          {isActive && <span className="picker-field-active-badge">live</span>}
        </div>
      );
    }

    // group node
    const groupPath = parentPath ? `${parentPath}/${node.name}` : node.name;
    const groupKey = `${moduleId}::${groupPath}`;
    const groupFields = flattenNodesToFields(node.children);
    const groupAddable = groupFields.filter(f => !activeProbeKeys.has(fieldKey(f)));
    const groupAllSel = groupAddable.length > 0 && groupAddable.every(f => selected.has(fieldKey(f)));
    const groupSomeSel = groupAddable.some(f => selected.has(fieldKey(f)));
    const groupExpanded = expanded.has(groupKey);

    return (
      <div key={groupKey} className="picker-subgroup">
        <div
          className="picker-subgroup-row"
          onClick={() => setExpanded(prev => {
            const s = new Set(prev);
            if (groupExpanded) s.delete(groupKey); else s.add(groupKey);
            return s;
          })}
        >
          <input
            type="checkbox"
            className="picker-module-check"
            checked={groupAllSel}
            ref={el => { if (el) el.indeterminate = groupSomeSel && !groupAllSel; }}
            onChange={() => {
              setSelected(prev => {
                const s = new Set(prev);
                for (const f of groupAddable) { if (groupAllSel) s.delete(fieldKey(f)); else s.add(fieldKey(f)); }
                return s;
              });
            }}
            onClick={e => e.stopPropagation()}
          />
          <span className="picker-module-chevron">{(groupExpanded || !!search) ? '▾' : '▸'}</span>
          <span>{node.name}</span>
          <span className="picker-sel-count" style={{ marginLeft: 'auto' }}>{groupAddable.length}</span>
        </div>
        {(groupExpanded || !!search) && (
          <div>
            {node.children.map(child => renderNode(child, moduleId, groupPath))}
          </div>
        )}
      </div>
    );
  }

  return (
    <div className="scope-picker-overlay" onClick={e => { if (e.target === e.currentTarget) onCancel(); }}>
      <div className="scope-picker-modal">
        <div className="scope-picker-header">
          <span>Add Variables</span>
          <button className="scope-picker-close" onClick={onCancel}>×</button>
        </div>
        <div className="scope-picker-search">
          <span>⌕</span>
          <input
            placeholder="Search fields…"
            value={search}
            onChange={e => setSearch(e.target.value)}
            autoFocus
          />
          {selected.size > 0 && (
            <span className="picker-sel-count">{selected.size} sel</span>
          )}
        </div>
        <div className="scope-picker-tree">
          {Object.entries(filteredModules).map(([moduleId, nodes]) => {
            // Flatten all fields in this module for checkbox state calc
            const allModuleFields: FieldRef[] = flattenNodesToFields(nodes);
            const addable = allModuleFields.filter(f => !activeProbeKeys.has(fieldKey(f)));
            const allChecked = addable.length > 0 && addable.every(f => selected.has(fieldKey(f)));
            const someChecked = addable.some(f => selected.has(fieldKey(f)));
            const isExpanded = expanded.has(moduleId) || !!search;
            return (
              <div key={moduleId}>
                <div className="picker-module-row" onClick={() => toggleExpand(moduleId)}>
                  <input
                    type="checkbox"
                    className="picker-module-check"
                    checked={allChecked}
                    ref={el => { if (el) el.indeterminate = someChecked && !allChecked; }}
                    onChange={() => toggleModule(moduleId, nodes)}
                    onClick={e => e.stopPropagation()}
                  />
                  <span className="picker-module-chevron">{isExpanded ? '▾' : '▸'}</span>
                  <span>{moduleId}</span>
                  <span className="picker-sel-count" style={{ marginLeft: 'auto' }}>{addable.length}</span>
                </div>
                {isExpanded && (
                  <div>
                    {nodes.map(n => renderNode(n, moduleId, ''))}
                  </div>
                )}
              </div>
            );
          })}
          {Object.keys(filteredModules).length === 0 && (
            <div className="picker-empty">
              {Object.keys(schema).length === 0
                ? 'No numeric fields found. Are modules running?'
                : 'No fields match your search.'}
            </div>
          )}
        </div>
        <div className="picker-footer">
          <button className="picker-footer-cancel" onClick={onCancel}>Cancel</button>
          <button
            className="picker-footer-confirm"
            disabled={selectedFields.length === 0}
            onClick={() => onConfirm(selectedFields)}
          >
            Add{selectedFields.length > 0
              ? ` ${selectedFields.length} Variable${selectedFields.length > 1 ? 's' : ''}`
              : ' Variables'}
          </button>
        </div>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------
// Recharts helpers
// ---------------------------------------------------------------------------
function mergeSeriesData(
  probeIds: number[],
  scopeData: Record<string, [number, number][]>,
  windowMs: number,
): Record<string, number>[] {
  const now = Date.now();
  const cutoff = now - windowMs;
  const map = new Map<number, Record<string, number>>();
  for (const id of probeIds) {
    for (const [t, v] of scopeData[String(id)] ?? []) {
      if (t < cutoff) continue;
      if (!map.has(t)) map.set(t, { t });
      map.get(t)![String(id)] = v;
    }
  }
  return Array.from(map.values()).sort((a, b) => (a.t as number) - (b.t as number));
}

// ---------------------------------------------------------------------------
// ProbeTag — draggable chip in the traced-variables panel
// ---------------------------------------------------------------------------
interface ProbeTagProps {
  probe: ProbeInfo;
  color: string;
  onRemove: () => void;
}
function ProbeTag({ probe, color, onRemove }: ProbeTagProps) {
  return (
    <div
      className="probe-tag"
      draggable
      onDragStart={e => {
        e.dataTransfer.setData('probeId', String(probe.id));
        e.dataTransfer.effectAllowed = 'copy';
      }}
      style={{ borderLeftColor: color }}
      title="Drag onto a chart to trace"
    >
      <span className="probe-tag-dot" style={{ background: color }} />
      <div className="probe-tag-text">
        <span className="probe-tag-module">{probe.moduleId}</span>
        <span className="probe-tag-path">{probe.path}</span>
      </div>
      <button className="probe-tag-remove" onClick={onRemove} title="Stop tracing">×</button>
    </div>
  );
}

// ---------------------------------------------------------------------------
// ChartCard
// ---------------------------------------------------------------------------
interface ChartCardProps {
  chart: ChartConfig;
  probes: ProbeInfo[];
  probeColorMap: Map<number, string>;
  scopeData: Record<string, [number, number][]>;
  onRemoveChart: () => void;
  onRemoveProbeFromChart: (probeId: number) => void;
  onDropProbe: (probeId: number) => void;
  onUpdateChart: (patch: Partial<ChartConfig>) => void;
}

function ChartCard({
  chart,
  probes,
  probeColorMap,
  scopeData,
  onRemoveChart,
  onRemoveProbeFromChart,
  onDropProbe,
  onUpdateChart,
}: ChartCardProps) {
  const [isDragOver, setIsDragOver] = useState(false);

  const chartProbes = useMemo(
    () => probes.filter(p => chart.probeIds.includes(p.id)),
    [probes, chart.probeIds],
  );

  const data = useMemo(
    () => mergeSeriesData(chart.probeIds, scopeData, chart.windowSec * 1000),
    [chart.probeIds, scopeData, chart.windowSec],
  );

  const probeMap = useMemo(() => {
    const m = new Map<number, ProbeInfo & { color: string }>();
    for (const p of chartProbes) m.set(p.id, { ...p, color: probeColorMap.get(p.id) ?? '#aaa' });
    return m;
  }, [chartProbes, probeColorMap]);

  const yAxes = chart.scaleMode === 'separate'
    ? chartProbes.map((p, i) => ({
        yAxisId: String(p.id),
        orientation: (i % 2 === 0 ? 'left' : 'right') as 'left' | 'right',
        color: probeColorMap.get(p.id) ?? '#aaa',
      }))
    : null;

  return (
    <div
      className={`chart-card${isDragOver ? ' drag-over' : ''}`}
      onDragOver={e => { e.preventDefault(); setIsDragOver(true); }}
      onDragLeave={() => setIsDragOver(false)}
      onDrop={e => {
        e.preventDefault();
        setIsDragOver(false);
        const id = parseInt(e.dataTransfer.getData('probeId'), 10);
        if (!isNaN(id) && !chart.probeIds.includes(id)) onDropProbe(id);
      }}
    >
      <div className="chart-card-header">
        <input
          className="chart-title-input"
          value={chart.title}
          onChange={e => onUpdateChart({ title: e.target.value })}
        />
        <div className="chart-controls">
          <select
            className="chart-card-select"
            value={chart.scaleMode}
            onChange={e => onUpdateChart({ scaleMode: e.target.value as 'shared' | 'separate' })}
            title="Y-axis scale"
          >
            <option value="shared">Shared Y</option>
            <option value="separate">Separate Y</option>
          </select>
          <select
            className="chart-card-select"
            value={chart.windowSec}
            onChange={e => onUpdateChart({ windowSec: Number(e.target.value) })}
          >
            <option value={10}>10 s</option>
            <option value={30}>30 s</option>
            <option value={60}>60 s</option>
            <option value={120}>2 min</option>
            <option value={300}>5 min</option>
          </select>
          <button className="chart-card-remove" onClick={onRemoveChart} title="Remove chart">×</button>
        </div>
      </div>

      {chartProbes.length > 0 && (
        <div className="chart-probe-list">
          {chartProbes.map(p => (
            <div
              key={p.id}
              className="chart-probe-chip"
              style={{ borderColor: probeColorMap.get(p.id) }}
            >
              <span className="chart-probe-dot" style={{ background: probeColorMap.get(p.id) }} />
              <span className="chart-probe-label">{p.label}</span>
              <button
                className="chart-probe-remove"
                onClick={() => onRemoveProbeFromChart(p.id)}
                title="Remove from chart"
              >×</button>
            </div>
          ))}
        </div>
      )}

      {chartProbes.length === 0 ? (
        <div className="chart-drop-zone">
          {isDragOver ? '↓ Drop to add' : 'Drop variables here to trace'}
        </div>
      ) : (
        <div className="chart-recharts-wrapper">
          <ResponsiveContainer width="100%" height={280}>
            <LineChart
              data={data}
              margin={{
                top: 8,
                right: chart.scaleMode === 'separate' && chartProbes.length > 1 ? 60 : 16,
                bottom: 8,
                left: 8,
              }}
            >
              <CartesianGrid strokeDasharray="3 3" stroke="#252540" />
              <XAxis
                dataKey="t"
                type="number"
                domain={['dataMin', 'dataMax']}
                tickFormatter={(t: number) => {
                  const s = Math.round((Date.now() - t) / 1000);
                  return s === 0 ? 'now' : `-${s}s`;
                }}
                stroke="#444"
                tick={{ fill: '#888', fontSize: 11 }}
                tickCount={6}
              />
              {yAxes ? (
                yAxes.map((ya, i) => (
                  <YAxis
                    key={ya.yAxisId}
                    yAxisId={ya.yAxisId}
                    orientation={ya.orientation}
                    stroke={ya.color}
                    tick={{ fill: ya.color, fontSize: 10 }}
                    width={i === 0 ? 58 : 48}
                    tickFormatter={(v: number) => v.toPrecision(3)}
                  />
                ))
              ) : (
                <YAxis
                  stroke="#444"
                  tick={{ fill: '#888', fontSize: 11 }}
                  width={58}
                  tickFormatter={(v: number) => v.toPrecision(4)}
                />
              )}
              <Tooltip
                formatter={(value: unknown, name: unknown) => {
                  const id = parseInt(String(name), 10);
                  const label = probeMap.get(id)?.label ?? String(name);
                  const formatted = typeof value === 'number' ? value.toPrecision(6) : String(value);
                  return [formatted, label];
                }}
                labelFormatter={(t: unknown) => new Date(Number(t)).toLocaleTimeString()}
                contentStyle={{
                  background: '#12121e',
                  border: '1px solid #333',
                  borderRadius: 4,
                  fontSize: 12,
                }}
                itemStyle={{ color: '#ddd' }}
                labelStyle={{ color: '#888' }}
              />
              <Legend
                formatter={(value: string) => probeMap.get(parseInt(value, 10))?.label ?? value}
                wrapperStyle={{ fontSize: 12, paddingTop: 4 }}
              />
              {chartProbes.map(p => (
                <Line
                  key={p.id}
                  type="monotone"
                  dataKey={String(p.id)}
                  stroke={probeColorMap.get(p.id)}
                  strokeWidth={1.5}
                  dot={false}
                  isAnimationActive={false}
                  yAxisId={yAxes ? String(p.id) : undefined}
                  connectNulls={false}
                />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </div>
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
let _chartSeq = 0;
function newChartId() { return `chart-${++_chartSeq}`; }

export default function OscilloscopeView() {
  const [schema, setSchema] = useState<Record<string, unknown>>({});
  const [probes, setProbes] = useState<ProbeInfo[]>([]);
  const [scopeData, setScopeData] = useState<Record<string, [number, number][]>>({});
  const [charts, setCharts] = useState<ChartConfig[]>(() => {
    const saved = loadSavedCharts();
    if (saved.length > 0) {
      // Restore layout without probeIds yet — they get wired up after probes are re-registered.
      return saved.map(s => ({ ...s, probeIds: [] }));
    }
    return [{ id: newChartId(), title: 'Chart 1', probeIds: [], scaleMode: 'shared', windowSec: 30 }];
  });
  const [pickerOpen, setPickerOpen] = useState(false);
  const [error, setError] = useState('');
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const probeColorMap = useMemo(() => {
    const m = new Map<number, string>();
    probes.forEach((p, i) => m.set(p.id, probeColor(i)));
    return m;
  }, [probes]);

  const activeProbeKeys = useMemo(() => {
    const s = new Set<string>();
    for (const p of probes) s.add(`${p.moduleId}::${p.path}`);
    return s;
  }, [probes]);

  const refreshFields = useCallback(async () => {
    try { setSchema(await getScopeSchema()); }
    catch (e: unknown) { setError(e instanceof Error ? e.message : String(e)); }
  }, []);

  const refreshProbes = useCallback(async () => {
    try { setProbes(await getScopeProbes()); }
    catch (e: unknown) { setError(e instanceof Error ? e.message : String(e)); }
  }, []);

  // Restore saved probe assignments: re-register each saved probe and wire up chart probeIds.
  const restoreFromStorage = useCallback(async () => {
    const saved = loadSavedCharts();
    if (saved.length === 0) return;

    // Collect all unique probeRefs across all charts.
    const allRefs = new Map<string, { moduleId: string; path: string }>();
    for (const sc of saved) {
      for (const ref of sc.probeRefs) {
        allRefs.set(`${ref.moduleId}::${ref.path}`, ref);
      }
    }

    // Check which probes already exist on the backend to avoid duplicates on page refresh.
    const existing = await getScopeProbes();
    const existingByKey = new Map(existing.map(p => [`${p.moduleId}::${p.path}`, p.id]));

    // Reuse existing IDs where possible; only register probes that aren't already tracked.
    const keyToId = new Map<string, number>();
    for (const [key, ref] of allRefs) {
      if (existingByKey.has(key)) {
        keyToId.set(key, existingByKey.get(key)!);
      } else {
        try {
          const result = await addScopeProbe(ref.moduleId, ref.path);
          keyToId.set(key, result.id);
        } catch { /* probe no longer valid — skip */ }
      }
    }

    if (keyToId.size === 0) return;

    // Fetch fresh probe list then update charts.
    const freshProbes = await getScopeProbes();
    setProbes(freshProbes);

    setCharts(prev => prev.map(c => {
      const sc = saved.find(s => s.id === c.id);
      if (!sc) return c;
      const probeIds = sc.probeRefs
        .map(ref => keyToId.get(`${ref.moduleId}::${ref.path}`))
        .filter((id): id is number => id != null);
      return { ...c, probeIds };
    }));
  }, []);

  useEffect(() => {
    // eslint-disable-next-line react-hooks/set-state-in-effect
    refreshFields();
    // Restore saved state first, then fall back to normal probe refresh.
    restoreFromStorage().catch(() => refreshProbes());
    pollRef.current = setInterval(async () => {
      try { setScopeData(await getScopeData()); } catch { /* silent */ }
    }, 500);
    return () => { if (pollRef.current) clearInterval(pollRef.current); };
  }, [refreshFields, refreshProbes, restoreFromStorage]);

  useEffect(() => {
    const t = setInterval(refreshFields, 5000);
    return () => clearInterval(t);
  }, [refreshFields]);

  // Persist chart config whenever charts or probes change.
  useEffect(() => {
    if (probes.length > 0 || charts.some(c => c.probeIds.length === 0)) {
      saveCharts(charts, probes);
    }
  }, [charts, probes]);

  async function handlePickerConfirm(selected: FieldRef[]) {
    setPickerOpen(false);
    for (const f of selected) {
      try { await addScopeProbe(f.moduleId, f.path); } catch { /* skip */ }
    }
    await refreshProbes();
  }

  async function handleRemoveProbe(id: number) {
    try {
      await removeScopeProbe(id);
      setProbes(prev => prev.filter(p => p.id !== id));
      setScopeData(prev => { const n = { ...prev }; delete n[String(id)]; return n; });
      setCharts(prev => prev.map(c => ({ ...c, probeIds: c.probeIds.filter(pid => pid !== id) })));
    } catch (e: unknown) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <div className="scope-layout">
      {error && (
        <div className="scope-error" onClick={() => setError('')}>{error} <span>×</span></div>
      )}

      <div className="scope-sidebar">
        <div className="scope-sidebar-header">
          <span>Traced Variables</span>
          <button className="scope-btn-add" onClick={() => setPickerOpen(true)}>+ Add</button>
        </div>
        <div className="scope-probes">
          {probes.length === 0 ? (
            <div className="scope-empty-hint">
              No variables traced yet.<br />Click <strong>+ Add</strong> to start.
            </div>
          ) : (
            probes.map(p => (
              <ProbeTag
                key={p.id}
                probe={p}
                color={probeColorMap.get(p.id) ?? '#aaa'}
                onRemove={() => handleRemoveProbe(p.id)}
              />
            ))
          )}
        </div>
        {probes.length > 0 && (
          <div className="scope-sidebar-hint">Drag onto a chart to trace.</div>
        )}
      </div>

      <div className="scope-main">
        <div className="scope-main-header">
          <div className="scope-page-title">Scope Engineering View</div>
          <button className="scope-add-chart" onClick={() =>
            setCharts(prev => [
              ...prev,
              { id: newChartId(), title: `Chart ${prev.length + 1}`, probeIds: [], scaleMode: 'shared', windowSec: 30 },
            ])
          }>+ Add Chart</button>
        </div>
        <div className="scope-charts">
          {charts.map(chart => (
            <ChartCard
              key={chart.id}
              chart={chart}
              probes={probes}
              probeColorMap={probeColorMap}
              scopeData={scopeData}
              onRemoveChart={() => setCharts(prev => prev.filter(c => c.id !== chart.id))}
              onRemoveProbeFromChart={id =>
                setCharts(prev => prev.map(c =>
                  c.id === chart.id ? { ...c, probeIds: c.probeIds.filter(pid => pid !== id) } : c,
                ))
              }
              onDropProbe={id =>
                setCharts(prev => prev.map(c =>
                  c.id === chart.id && !c.probeIds.includes(id)
                    ? { ...c, probeIds: [...c.probeIds, id] }
                    : c,
                ))
              }
              onUpdateChart={patch =>
                setCharts(prev => prev.map(c => c.id === chart.id ? { ...c, ...patch } : c))
              }
            />
          ))}
        </div>
      </div>

      {pickerOpen && (
        <ProbePicker
          schema={schema}
          activeProbeKeys={activeProbeKeys}
          onConfirm={handlePickerConfirm}
          onCancel={() => setPickerOpen(false)}
        />
      )}
    </div>
  );
}
