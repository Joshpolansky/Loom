/**
 * DataTree — renders a nested data object as an expandable/collapsible tree.
 * Every leaf value is inline-editable (fires onChange with the updated value).
 *
 * Usage:
 *   <DataTree data={config} onChange={(path, value) => writeField(id, 'config', path, value)} />
 */
import { useState } from 'react';
import './DataTree.css';

interface DataTreeProps {
  data: Record<string, unknown>;
  /** Called when a leaf value changes. path = key segments, value = coerced JS value. */
  onChange?: (path: string[], value: unknown) => void;
  /** Called when a node is selected (picker mode). path = key segments. */
  onSelect?: (path: string[]) => void;
  /** Render the tree in picker mode: hide values and show selection checkboxes. */
  pickerMode?: boolean;
  /** Compact view */
  compact?: boolean;
  /** When in pickerMode, the set of selected path strings (e.g. '/status/pos'). */
  selectedKeys?: Set<string>;
  /** Prefix for generating stable keys during recursion (internal use). */
  _prefix?: string[];
  /** Current nesting depth — used for key cell indentation (internal use). */
  _depth?: number;
}

export function DataTree({ data, onChange, onSelect, pickerMode = false, selectedKeys, compact = false, _prefix = [], _depth = 0 }: DataTreeProps) {
  // Separate flat leaves from nested branches for clean table grouping
  const leaves: [string, unknown][] = [];
  const branches: [string, unknown][] = [];

  for (const [key, value] of Object.entries(data)) {
    const isNested = value !== null && typeof value === 'object';
    if (isNested) branches.push([key, value]);
    else leaves.push([key, value]);
  }

  // Compact mode: when the entire node is a single leaf (no branches),
  // hide the expand arrow and the type-hint for a cleaner one-line view.
  const isSingleLeaf = leaves.length === 1 && branches.length === 0;

  return (    
    <div className={`data-tree${pickerMode ? ' picker-mode' : ''}${compact ? ' compact' : ''}`}>
      {isSingleLeaf ? (
        // Special case: if there's only a single leaf, show it at the branch level for better readability
        <LeafNode
          nodeKey={leaves[0][0]}
          value={leaves[0][1] as string | number | boolean | null}
          path={[..._prefix, leaves[0][0]]}
          onChange={onChange}
          onSelect={onSelect}
          pickerMode={pickerMode}
          selectedKeys={selectedKeys}
          depth={_depth}
        />
      ) : null}     
      {leaves.length > 0 && !isSingleLeaf && (
        <div className="dt-table">
          {leaves.map(([key, value]) => (
            <LeafNode
              key={key}
              nodeKey={key}
              value={value as string | number | boolean | null}
              path={[..._prefix, key]}
              onChange={onChange}
              onSelect={onSelect}
              pickerMode={pickerMode}
              selectedKeys={selectedKeys}
              depth={_depth}
            />
          ))}
        </div>
      )}
      {branches.map(([key, value]) => (
        <BranchNode
          key={key}
          nodeKey={key}
          value={value as Record<string, unknown>}
          path={[..._prefix, key]}
          onChange={onChange}
          onSelect={onSelect}
          pickerMode={pickerMode}
          selectedKeys={selectedKeys}
          depth={_depth}
        />
      ))}
    </div>
  );
}

// ---------- internal nodes ----------

function BranchNode({
  nodeKey,
  value,
  path,
  onChange,
  onSelect,
  pickerMode,
  selectedKeys,
  depth,
  isSingleLeaf,
}: {
  nodeKey: string;
  value: Record<string, unknown> | unknown[];
  path: string[];
  onChange?: (path: string[], value: unknown) => void;
  onSelect?: (path: string[]) => void;
  pickerMode?: boolean;
  selectedKeys?: Set<string>;
  depth: number;
  isSingleLeaf?: boolean;
}) {
  const [open, setOpen] = useState(false);
  const indent = depth * 16;
  const pathStr = path.filter(Boolean).join('/').replace(/\/+/g, '/');//remove any double slashes just in case
  const isSelected = !!pickerMode && !!selectedKeys && selectedKeys.has(pathStr);

  return (
    <div className={`dt-branch ${isSelected ? 'selected' : ''}`} onClick={(e) => { e.stopPropagation(); onSelect?.(path); }}>
      <div className="dt-branch-row">
        <button className="dt-toggle" onDoubleClick={() => setOpen((o) => !o)}>
          <span className="dt-toggle-key dt-key-cell" style={{ paddingLeft: indent }}>
            {!isSingleLeaf && <span className="dt-arrow">{open ? '▾' : '▸'}</span>}
            <span className="dt-key">{nodeKey}</span>
          </span>
          {!isSingleLeaf && (
            <span className="dt-type-hint">{Array.isArray(value) ? `[${value.length}]` : `{${Object.keys(value).length}}`}</span>
          )}
        </button>
        {onSelect && (
          pickerMode ? (
            <></>
          ) : (
            <button className="dt-add" onClick={(e) => { e.stopPropagation(); onSelect(path); }} title="Add this node">+</button>
          )
        )}
      </div>
      {open && (
        <div className="dt-children">
          <DataTree
            data={Array.isArray(value) ? Object.fromEntries(value.map((v, i) => [String(i), v])) : value as Record<string, unknown>}
            onChange={onChange}
            onSelect={onSelect}
            pickerMode={pickerMode}
            selectedKeys={selectedKeys}
            _prefix={path}
            _depth={depth + 1}
          />
        </div>
      )}
    </div>
  );
}

function LeafNode({
  nodeKey,
  value,
  path,
  onChange,
  onSelect,
  pickerMode,
  selectedKeys,
  depth,
}: {
  nodeKey: string;
  value: string | number | boolean | null;
  path: string[];
  onChange?: (path: string[], value: unknown) => void;
  onSelect?: (path: string[]) => void;
  pickerMode?: boolean;
  selectedKeys?: Set<string>;
  depth: number;
}) {
  const [editing, setEditing] = useState(false);
  const [raw, setRaw] = useState('');

  function beginEdit() {
    setRaw(value === null ? 'null' : String(value));
    setEditing(true);
  }

  function commit() {
    setEditing(false);
    if (!onChange) return;
    // Coerce back to the original type
    let coerced: unknown = raw;
    if (typeof value === 'number') {
      const n = Number(raw);
      coerced = isNaN(n) ? value : n;
    } else if (typeof value === 'boolean') {
      coerced = raw === 'true' || raw === '1';
    } else if (value === null) {
      try { coerced = JSON.parse(raw); } catch { coerced = raw; }
    }
    onChange(path, coerced);
  }

  function onKeyDown(e: React.KeyboardEvent) {
    if (e.key === 'Enter') commit();
    if (e.key === 'Escape') setEditing(false);
  }
  const pathStr = path.filter(Boolean).join('/').replace(/\/+/g, '/');//remove any double slashes just in case  
  const isSelected = !!pickerMode && !!selectedKeys && selectedKeys.has(pathStr);
  return (
    <div className={`dt-leaf ${isSelected ? 'selected' : ''}`} onClick={(e) => { e.stopPropagation(); onSelect?.(path); }}>
      <span className="dt-key-cell" style={{ paddingLeft: depth * 16 }}>
        <span className="dt-arrow-spacer" />
        <span className={`dt-key`}>{nodeKey}</span>
        {onSelect && (
          pickerMode ? (<>  </>
          ) : (
            <button className="dt-add" onClick={(e) => { e.stopPropagation(); onSelect(path); }} title="Add this node">+</button>
          )
        )}
      </span>
      <span className="dt-val-cell">
        {editing ? (
          <input
            className="dt-input"
            autoFocus
            value={raw}
            placeholder='-'
            title='Value'
            onChange={(e) => setRaw(e.target.value)}
            onBlur={commit}
            onKeyDown={onKeyDown}
          />
        ) : (
          <button
            className={`dt-value dt-${typeof value}`}
            onClick={onChange ? beginEdit : undefined}
            title={onChange ? 'Click to edit' : undefined}
          >
            {value === null ? 'null' : String(value)}
          </button>
        )}
      </span>
    </div>
  );
}
