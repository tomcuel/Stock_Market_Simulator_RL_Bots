//=======================================================================
// Periodic and shutdown-time persistence for the exchange server: 
// - accounts (as their already one-way-hashed credentials, never the plaintext password)
// - portfolios (cash + holdings)
// - each symbol's last traded price are written to a plain-text snapshot file and restored on the next startup
// Deliberately a simple line-based format rather than JSON/a binary format, so no extra dependency is needed to read or write it (consistent with the rest of Src_Simulation)
//
// Known scope limitation, stated plainly: resting orders in the live book and the waiting registry are not persisted
// A restart loses in-flight orders but keeps every account's money and positions: the part that actually matters for a client not to lose everything on a restart
// Persisting live orders too would mean reconstructing OrderBook's internal state (and re-deriving which orders were mid-partial-fill) 
// rather than just replaying a few admin-style engine calls, which is a larger, riskier change than this pass covers
//=======================================================================
#pragma once

#include <string>

#include "matching_engine.hpp"
#include "net/auth.hpp"

namespace sim::net {

class PersistenceStore {
public:
    // Writes a full snapshot to `path` (a temp file is written and renamed into place, so a crash mid-write can never leave a half-written, unreadable snapshot behind) (Returns false on a file I/O error)
    static bool save(const std::string& path, const ClientDirectory& directory, const MatchingEngine& engine);

    // Loads a snapshot written by save() and restores it into `directory`/`engine`, intended to be called once at startup, before the server starts accepting connections
    // Returns false if the file doesn't exist or couldn't be read: callers should treat that as "start fresh", not as fatal, since a missing snapshot on first-ever startup is the normal case
    static bool load(const std::string& path, ClientDirectory& directory, MatchingEngine& engine);
};

} // namespace sim::net
