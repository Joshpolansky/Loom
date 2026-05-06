/**
 * SectionPanel — shows a single module data section (config/recipe/runtime)
 * as a DataTree with live inline editing.
 */
import { DataTree } from './DataTree';
import type { DataSection } from '../types';

interface SectionPanelProps {
  title: string;
  data: Record<string, unknown>;
  section: DataSection;
  /** If provided, fields are editable. */
  onWrite?: (path: string[], value: unknown) => void;
}

const SECTION_DESCRIPTIONS: Record<DataSection, string> = {
  config: 'Persisted device parameters',
  recipe: 'Product / batch parameters',
  runtime: 'Variables',
  summary: 'Key state indicators shown on the dashboard',
};

export function SectionPanel({ title, data, section, onWrite }: SectionPanelProps) {
  return (
    <div className="section-panel">
      <div className="section-panel-header">
        <span className="section-panel-title">{title}</span>
        <span className="section-panel-desc">{SECTION_DESCRIPTIONS[section]}</span>
        {onWrite && <span className="section-panel-badge live">live</span>}
        {!onWrite && <span className="section-panel-badge readonly">read-only</span>}
      </div>
      {Object.keys(data).length === 0 ? (
        <p className="section-panel-empty">No fields</p>
      ) : (
        <DataTree data={data} onChange={onWrite} />
      )}
    </div>
  );
}
