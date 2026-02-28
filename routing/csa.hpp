#pragma once

#include "routing/connections.hpp"

#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

//  CSA result

struct CSAResult
{
    std::unordered_map<StationId, AbsTime>    earliestArrival;
    std::unordered_set<TripId>                tripReached;
    std::unordered_map<StationId, std::size_t> inConnection; // station -> index into connections
    std::unordered_map<StationId, StationId>  inTransfer;    // station -> station we walked FROM
};

// Minimum seconds needed between arriving at a stop and boarding a new trip.
inline constexpr AbsTime kMinTransferSeconds = 2 * 60;

// A single raw leg from the path-reconstruction step: either a transit
// connection or a timed walking transfer between two root stations.
struct TransferLeg
{
    StationId from;
    StationId to;
    uint32_t  walkSeconds;
};
using PathLeg = std::variant<const Connection *, TransferLeg>;

// transferTimes may be nullptr when no transfer data is available.  When non-null,
// walking transfers are propagated and recorded during the scan so that
// reconstructPath can emit TransferLeg steps.
CSAResult csa(const std::vector<Connection> &connections,
              const std::unordered_map<StopId, std::unordered_map<StopId, uint32_t>> *transferTimes,
              StationId fromStop, AbsTime departureTime, StationId toStop);

std::vector<PathLeg> reconstructPath(const std::vector<Connection> &connections,
                                     const CSAResult &result,
                                     StationId fromStop, StationId toStop);
