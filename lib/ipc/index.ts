// Greylock — IPC barrel
// =============================================================================
// AGENT-SYNC (Phase 3). Single import surface for the keybridge protocol +
// the server/client implementations.
// =============================================================================

export * from './keybridge-protocol.js';
export {
  createKeybridgeServer,
  type KeybridgeServerOptions,
  type DekProvider,
} from './keybridge-server.js';
export {
  createKeybridgeClient,
  type KeybridgeClientOptions,
  type BorrowedDek,
  type KeybridgeClient as KeybridgeClientType,
} from './keybridge-client.js';
export { peerUidMatchesOurs, readPeerCred, type PeerCred } from './peer-cred.js';
