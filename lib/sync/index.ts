// Greylock — sync barrel
// =============================================================================
// AGENT-SYNC (Phase 3). Single import surface for the orchestrator, snapshot
// writer, and the keybridge bridge.
// =============================================================================

export {
  createSyncOrchestrator,
  type SyncOrchestratorDeps,
  type OrchestratorKeybridge,
  type OrchestratorBorrowedDek,
} from './orchestrator.js';
export {
  createSnapshotWriter,
  type SnapshotWriterDeps,
  type SnapshotWriterError,
  type WriteSnapshotInput,
  type WriteSnapshotResult,
} from './snapshot-writer.js';
export {
  decryptItemTokenWithBorrowedDek,
  useBorrowedDek,
  type DecryptWithBorrowedDekInput,
  type BorrowedDecryptError,
} from './keybridge-client-bridge.js';
