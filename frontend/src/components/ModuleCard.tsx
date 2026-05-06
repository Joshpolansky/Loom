/**
 * ModuleCard — dashboard tile for a single module.
 * Shows the module's Summary section (if populated) or a default stats summary,
 * then always shows cycle stats below.
 */
import React from 'react';
import { useNavigate } from 'react-router-dom';
import type { ModuleState } from '../api/dataService';
import { MODULE_STATES } from '../types';

interface ModuleCardProps {
  moduleState: ModuleState;
}

export function ModuleCard({ moduleState }: ModuleCardProps) {
  const navigate = useNavigate();
  const { info, liveStats, liveSummary } = moduleState;
  const stats = liveStats ?? info.stats;
  const summaryEntries = Object.entries(liveSummary ?? {});
  const hasSummary = summaryEntries.length > 0;

  // Jitter is measured in the scheduler as |actual_wake - scheduled_wake|
  const jitterUs = stats?.lastJitterUs;

  return (
    <div
      className="module-card"
      onClick={() => navigate(`/module/${info.id}`)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => e.key === 'Enter' && navigate(`/module/${info.id}`)}
    >
      <h3>
        {info.name}{' '}
        <span className={`state-badge state-${info.state}`}>
          {MODULE_STATES[info.state] ?? 'Unknown'}
        </span>
      </h3>
      <div className="meta">
        <span>ID: {info.id}</span>
        <span>v{info.version}</span>
      </div>

      {/* Cycle stats — always shown */}
      <dl className="stats stats-footer">
        <dt>Cycle Time</dt>
        <dd>{stats?.lastCycleTimeUs ?? '—'} µs</dd>
        <dt>Jitter</dt>
        <dd>{jitterUs != null ? `${jitterUs} µs` : '—'}</dd>

        <dt>Cycles</dt>
        <dd>{stats?.cycleCount ?? '—'}</dd>
        <dt>Max</dt>
        <dd>{stats?.maxCycleTimeUs ?? '—'} µs</dd>
        <dt>Overruns</dt>
        <dd>{stats?.overrunCount ?? '—'}</dd>
      </dl>
      {/* Module-defined summary, or default stats summary */}
      {hasSummary ? (
        <dl className="stats">
          {summaryEntries.map(([k, v]) => (
            <React.Fragment key={k}><dt>{k}</dt><dd>{String(v)}</dd></React.Fragment>
          ))}
        </dl>
      ) : null}

    </div>
  );
}
