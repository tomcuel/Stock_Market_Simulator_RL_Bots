//=======================================================================
// Periodic and shutdown-time persistence for the exchange server: 
// - accounts (as their already one-way-hashed credentials, never the plaintext password)
// - portfolios (cash + holdings)
// - each symbol's last traded price (the official close after a closing auction) are written to a plain-text snapshot file and restored on the next startup
//   Deliberately a simple line-based format rather than JSON/a binary format, so no extra dependency is needed to read or write it (consistent with the rest of Src_Simulation)
// - the open orders that outlive the day: GTC orders, and orders with their own expiry date still ahead (time in force, see types.hpp)
//   DAY orders expire at the close (sim_server.x --closing-auction) and are not saved
// Each saved order keeps its id, its place in the queue (book orders are written in priority order) and the trading time it has left, counted at the close: its clock stops while the market is closed
// At load, the reservations are rebuilt from the restored orders (they are never saved), and sim_server.x opens the session with an auction in case restored orders cross
// Persisting live orders too would mean reconstructing OrderBook's internal state (and re-deriving which orders were mid-partial-fill) rather than just replaying a few admin-style engine calls, which is a larger, riskier change than this pass covers
//=======================================================================
#ifndef NET_PERSISTENCE_HPP
#define NET_PERSISTENCE_HPP

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

#endif // NET_PERSISTENCE_HPP