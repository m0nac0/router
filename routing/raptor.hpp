#pragma once

#include "gtfs/feed.hpp"
#include "routing/connections.hpp"
#include "routing/journey.hpp"

#include <set>
#include <unordered_map>
#include <vector>

using RaptorRouteId = uint32_t;
using RouteStopIndex = uint32_t;

struct RaptorData
{
    std::unordered_map<RaptorRouteId, std::vector<StationId>> routeStops;
    std::unordered_map<StationId, std::vector<RaptorRouteId>> stopRoutes;
    // Must be sorted by departure time for each route to allow efficient scanning in raptor()
    std::unordered_map<RaptorRouteId, std::vector<TripId>> routeTrips;
    std::unordered_map<TripId, RaptorRouteId> tripToRoute;
    std::unordered_map<ServiceId, std::set<CalendarEntry>> serviceIndex;
    std::unordered_map<ServiceId, std::set<CalendarDateException>> serviceExceptionsIndex;
};

RaptorData buildRaptorData(const Feed &feed, const RealtimeOverlays *overlays = nullptr);

constexpr int MAX_NUM_ROUNDS = 8;

// Runs the RAPTOR algorithm and returns a reconstructed journey.
// departureTime: seconds since midnight of the service day.
// midnight: Unix timestamp of midnight on that service day (for AbsTime output).
// earliestArrivalTime: optional inherited label arrays for rRAPTOR; pass nullptr for standalone use.
std::vector<RouteResult> raptor(StationId origin, StationId target, Time departureTime, AbsTime midnight,
                                const RaptorData &data, const Feed &feed,
                                const RealtimeOverlays *overlays = nullptr,
                                std::unordered_map<StationId, Time> *earliestArrivalTime = nullptr);

// Runs rRAPTOR over all departures in [departureTimeMin, departureTimeMax] and returns
// the Pareto-optimal journeys across departure time, arrival time, and number of transfers.
std::vector<RouteResult> rangeRaptor(StationId origin, StationId target, Time departureTimeMin, Time departureTimeMax,
                                     AbsTime midnight, const RaptorData &data, const Feed &feed,
                                     const RealtimeOverlays *overlays = nullptr);
