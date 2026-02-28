#include "csa.hpp"

#include <algorithm>
#include <iostream>
#include <ranges>

static bool canUseConnection(const Connection &conn, const CSAResult &result)
{
    // Already on this trip; no transfer needed.
    if (result.tripReached.contains(conn.tripId))
        return true;

    // Must have arrived at the departure stop with enough time to transfer.
    // Written as addition to avoid unsigned underflow.
    auto timeIt = result.earliestArrival.find(conn.departureStop);
    if (timeIt == result.earliestArrival.end())
        return false;
    return timeIt->second + kMinTransferSeconds <= conn.departureTime;
}

CSAResult csa(const std::vector<Connection> &connections,
              const std::unordered_map<StopId, std::unordered_map<StopId, uint32_t>> *transferTimes,
              StationId fromStop, AbsTime departureTime, StationId toStop)
{
    CSAResult result;
    // Seed source as if we arrived kMinTransferSeconds early, so the transfer
    // check passes for any connection departing at exactly departureTime.
    result.earliestArrival[fromStop] = departureTime - kMinTransferSeconds;

    std::size_t start = static_cast<std::size_t>(
        std::ranges::lower_bound(connections, departureTime, {}, &Connection::departureTime) - connections.begin());

    if (start == connections.size())
        return result;

    for (std::size_t i = start; i < connections.size(); ++i)
    {
        const Connection &conn = connections[i];

        // No need to scan further once every remaining connection departs after
        // we have already reached the destination.
        auto destIt = result.earliestArrival.find(toStop);
        if (destIt != result.earliestArrival.end() && conn.departureTime > destIt->second)
            break;

        if (canUseConnection(conn, result))
        {
            result.tripReached.insert(conn.tripId);
            auto arrIt = result.earliestArrival.find(conn.arrivalStop);
            if (arrIt == result.earliestArrival.end() || conn.arrivalTime < arrIt->second)
            {
                result.earliestArrival[conn.arrivalStop] = conn.arrivalTime;
                result.inConnection[conn.arrivalStop] = i;
                result.inTransfer.erase(conn.arrivalStop); // connection beats any earlier transfer
            }

            // Propagate walking transfers out of conn.arrivalStop.
            // transferTimes is keyed by StopId; StationId values share the same integer space.
            if (transferTimes)
            {
                auto tfIt = transferTimes->find(asStopId(conn.arrivalStop));
                if (tfIt != transferTimes->end())
                {
                    for (const auto &[toStopId, walkSecs] : tfIt->second)
                    {
                        const StationId toStation{static_cast<uint32_t>(toStopId)};
                        const AbsTime transferArrival = conn.arrivalTime + walkSecs;
                        auto it = result.earliestArrival.find(toStation);
                        if (it == result.earliestArrival.end() || transferArrival < it->second)
                        {
                            result.earliestArrival[toStation] = transferArrival;
                            result.inTransfer[toStation]      = conn.arrivalStop;
                            result.inConnection.erase(toStation); // transfer beats any earlier connection
                        }
                    }
                }
            }
        }
    }

    return result;
}

std::vector<PathLeg> reconstructPath(const std::vector<Connection> &connections,
                                     const CSAResult &result,
                                     StationId fromStop, StationId toStop)
{
    std::vector<PathLeg> path;
    if (!result.earliestArrival.contains(toStop))
        return path;

    for (StationId cur = toStop; cur != fromStop;)
    {
        auto inConnIt = result.inConnection.find(cur);
        if (inConnIt != result.inConnection.end())
        {
            const Connection &conn = connections[inConnIt->second];
            path.push_back(&conn);
            cur = conn.departureStop;
            continue;
        }

        auto inTrIt = result.inTransfer.find(cur);
        if (inTrIt != result.inTransfer.end())
        {
            const StationId from = inTrIt->second;
            const AbsTime arrFrom = result.earliestArrival.at(from);
            const AbsTime arrCur  = result.earliestArrival.at(cur);
            const uint32_t walkSecs = (arrCur > arrFrom) ? static_cast<uint32_t>(arrCur - arrFrom) : 0u;
            path.push_back(TransferLeg{from, cur, walkSecs});
            cur = from;
            continue;
        }

        std::cerr << "Error during path reconstruction: no incoming leg for stop "
                  << static_cast<uint32_t>(cur) << ".\n";
        return {};
    }
    std::reverse(path.begin(), path.end());
    return path;
}
