#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <variant>

#include "gtfs/feed.hpp"
#include "routing/connections.hpp"
#include "routing/raptor.hpp"

struct ParentLabel
{
    StationId stop;            // board/source stop for this leg
    TripId trip;               // kNoTrip if transfer/footpath
    RouteStopIndex stopIndex;  // index of arrival stop in the route pattern (for time lookup in circular routes)
    RouteStopIndex boardIndex; // index of board stop in the route pattern
};

RaptorData buildRaptorData(const Feed &feed, const RealtimeOverlays *overlays)
{
    RaptorData data;
    std::unordered_map<RouteId, std::map<std::vector<StationId>, RaptorRouteId>> routeToRaptorRouteStops;
    RaptorRouteId nextRaptorRouteId = 0;

    for (const Trip &trip : feed.trips)
    {
        // get stop sequence for this trip, resolving child stops to root stations
        static const RealtimeOverlays emptyOverlays{};
        const auto stopTimes = getRealtimeStopTimesForTrip(trip.id, feed, overlays ? *overlays : emptyOverlays);
        if (stopTimes.empty())
            continue;
        std::vector<StationId> routeStops;
        for (const RealtimeStopTime &stopTime : stopTimes)
        {
            if (stopTime.skipped)
                continue; // skip stops that are skipped in realtime
            // RealtimeStopTime::stopId is already a resolved root-station StationId
            routeStops.push_back(stopTime.stopId);
        }

        // check if this route has already been seen with the same stop sequence
        auto &raptorRouteStopsMap = routeToRaptorRouteStops[trip.routeId];
        auto it = raptorRouteStopsMap.find(routeStops);
        RaptorRouteId raptorRouteId;
        if (it == raptorRouteStopsMap.end())
        {
            // new route pattern, assign a new RaptorRouteId
            raptorRouteId = nextRaptorRouteId++;
            raptorRouteStopsMap[routeStops] = raptorRouteId;
            data.routeStops[raptorRouteId] = routeStops;
        }
        else
        {
            // existing route pattern, reuse the RaptorRouteId
            raptorRouteId = it->second;
        }

        data.routeTrips[raptorRouteId].push_back(trip.id);
        data.tripToRoute[trip.id] = raptorRouteId;
    }

    for (auto &[raptorRouteId, trips] : data.routeTrips)
    {
        std::sort(trips.begin(), trips.end(), [&](TripId a, TripId b)
                  {
            // Sort by realtime departure time at the first stop of the route pattern.
            static const RealtimeOverlays emptyOverlays{};
            const auto &ov = overlays ? *overlays : emptyOverlays;
            const auto stA = getRealtimeStopTimesForTrip(a, feed, ov);
            const auto stB = getRealtimeStopTimesForTrip(b, feed, ov);
            Time depA = stA.empty() ? kNoTime : stA.front().getExpectedDeparture();
            Time depB = stB.empty() ? kNoTime : stB.front().getExpectedDeparture();
            return depA < depB; });
    }

    for (const auto &[routeId, raptorRouteStopsMap] : routeToRaptorRouteStops)
    {
        for (const auto &[routeStops, raptorRouteId] : raptorRouteStopsMap)
        {
            for (StationId stationId : routeStops)
            {
                auto &routes = data.stopRoutes[stationId];
                if (routes.empty() || routes.back() != raptorRouteId)
                {
                    // Prevent pushing a route twice for the same stop if it appears multiple times in the same route pattern (circular routes).
                    routes.push_back(raptorRouteId);
                }
            }
        }
    }

    data.serviceIndex = buildServiceIndex(feed);
    data.serviceExceptionsIndex = buildServiceExceptionsIndex(feed);

    return data;
}

// Includes realtime data if available in overlays, otherwise falls back to the static schedule.
struct ResolvedStopTime
{
    Time arrival;
    Time departure;
};

static ResolvedStopTime getResolvedStopTimeAtIndex(TripId tripId, RouteStopIndex index,
                                                   const Feed &feed, const RealtimeOverlays *overlays)
{
    const auto st = feed.stopTimesForTrip(tripId);
    if (index >= st.size())
        return {kNoTime, kNoTime};
    const StopTime &stopTime = st[index];
    if (overlays)
    {
        auto tripIt = overlays->find(tripId);
        if (tripIt != overlays->end())
        {
            auto stopIt = tripIt->second.find(stopTime.stopSequence);
            if (stopIt != tripIt->second.end())
            {
                const auto *ov = std::get_if<StopTimeOverride>(&stopIt->second);
                if (ov)
                {
                    if (ov->skipped)
                        return {kNoTime, kNoTime};
                    return {ov->arrival.value_or(stopTime.arrivalTime),
                            ov->departure.value_or(stopTime.departureTime)};
                }
            }
        }
    }
    return {stopTime.arrivalTime, stopTime.departureTime};
}

static Time getDepartureTimeAtIndex(TripId tripId, RouteStopIndex index,
                                    const Feed &feed, const RealtimeOverlays *overlays)
{
    return getResolvedStopTimeAtIndex(tripId, index, feed, overlays).departure;
}

static Time getArrivalTimeAtIndex(TripId tripId, RouteStopIndex index,
                                  const Feed &feed, const RealtimeOverlays *overlays)
{
    return getResolvedStopTimeAtIndex(tripId, index, feed, overlays).arrival;
}

// TODO ensure we correctly handle trips that run past midnight
std::vector<RouteResult> raptor(StationId origin, StationId target, Time departureTime, AbsTime midnight,
                                const RaptorData &data, const Feed &feed, const RealtimeOverlays *overlays)
{
    constexpr int MAX_NUM_ROUNDS = 8;
    constexpr int DEFAULT_MINIMUM_TRANSFER_TIME = 120; // 2 minutes
    // Index 0 = initial state, indices 1..MAX_NUM_ROUNDS = rounds 1..8
    std::unordered_map<StationId, Time> earliestArrivalTime[MAX_NUM_ROUNDS + 1];
    earliestArrivalTime[0][origin] = departureTime;
    std::unordered_map<StationId, Time> earliestOverallArrivalTime;
    earliestOverallArrivalTime[origin] = departureTime;
    std::unordered_map<StationId, ParentLabel> parent[MAX_NUM_ROUNDS + 1];
    std::set<StationId> markedStops;
    markedStops.insert(origin);

    const auto ymd = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::from_time_t(midnight))};

    for (int i = 1; i <= MAX_NUM_ROUNDS; i++)
    {
        // Serves both as a list of marked routes and maps them to the earliest marked stop index
        std::unordered_map<RaptorRouteId, RouteStopIndex> markedRoutesEarliestStopIndex;
        for (StationId markedStop : markedStops)
        {
            if (!data.stopRoutes.count(markedStop))
                continue;
            for (RaptorRouteId routeId : data.stopRoutes.at(markedStop))
            {
                const auto &routeStopsVec = data.routeStops.at(routeId);
                auto it = std::find(routeStopsVec.begin(), routeStopsVec.end(), markedStop);
                RouteStopIndex markedStopIndexInRoute = static_cast<RouteStopIndex>(it - routeStopsVec.begin());

                auto [mapIt, inserted] = markedRoutesEarliestStopIndex.emplace(routeId, markedStopIndexInRoute);
                if (!inserted && mapIt->second > markedStopIndexInRoute)
                    mapIt->second = markedStopIndexInRoute;
            }
        }
        markedStops.clear();

        // Traverse each route
        for (auto &[routeId, earliestStopIndex] : markedRoutesEarliestStopIndex)
        {
            TripId currentTrip{kNoTrip};
            StationId boardStop = kNoStation;
            RouteStopIndex boardStopIndex = 0;
            const std::vector<TripId> &routeTrips = data.routeTrips.at(routeId);
            const std::vector<StationId> &routeStopsVec = data.routeStops.at(routeId);

            for (RouteStopIndex stopIndex = earliestStopIndex; stopIndex < routeStopsVec.size(); stopIndex++)
            {
                StationId stopId = routeStopsVec[stopIndex];

                if (currentTrip != kNoTrip)
                {
                    // Use stopIndex directly to avoid returning the wrong occurrence
                    // for circular routes where the same station appears multiple times.
                    Time arrivalTime = getArrivalTimeAtIndex(currentTrip, stopIndex, feed, overlays);
                    if (arrivalTime != kNoTime
                        // local pruning
                        && (!earliestOverallArrivalTime.count(stopId) || arrivalTime < earliestOverallArrivalTime[stopId])
                        // target pruning
                        && (!earliestOverallArrivalTime.count(target) || arrivalTime < earliestOverallArrivalTime[target]))
                    {
                        earliestOverallArrivalTime[stopId] = arrivalTime;
                        earliestArrivalTime[i][stopId] = arrivalTime;
                        parent[i][stopId] = ParentLabel{boardStop, currentTrip, stopIndex, boardStopIndex};
                        markedStops.insert(stopId);
                    }
                }

                // Can we board an earlier trip at this stop?
                // If our arrival from the previous round is at or before the current trip's
                // departure here, a trip departing earlier might be available.
                auto prevIt = earliestArrivalTime[i - 1].find(stopId);
                if (prevIt == earliestArrivalTime[i - 1].end())
                    continue;
                Time prevArrival = prevIt->second;

                // MCT applies when the passenger arrived by transit (i >= 2).
                // Round 1 boards from the origin with no prior trip, so no MCT.
                Time mct = 0;
                if (i >= 2)
                {
                    const StopId rawStopId = asStopId(stopId);
                    auto fromIt = feed.transferTimes.find(rawStopId);
                    if (fromIt != feed.transferTimes.end())
                    {
                        auto toIt = fromIt->second.find(rawStopId);
                        if (toIt != fromIt->second.end())
                        {
                            mct = toIt->second;
                        }
                        else
                        {
                            mct = DEFAULT_MINIMUM_TRANSFER_TIME;
                        }
                    }
                    else
                    {
                        mct = DEFAULT_MINIMUM_TRANSFER_TIME;
                    }
                }

                const Time currentTripDep = currentTrip != kNoTrip
                                                ? getDepartureTimeAtIndex(currentTrip, stopIndex, feed, overlays)
                                                : kNoTime;

                if (currentTrip == kNoTrip || prevArrival + mct <= currentTripDep)
                {
                    // Find the earliest trip on this route at this stop that we can catch.
                    // routeTrips must be sorted by departure time (ascending) for this to work.
                    for (TripId tripId : routeTrips)
                    {
                        if (!isServiceActive(data.serviceIndex, data.serviceExceptionsIndex, feed.tripById(tripId)->serviceId, ymd))
                            continue;
                        Time tripStopTime = getDepartureTimeAtIndex(tripId, stopIndex, feed, overlays);
                        if (tripStopTime == kNoTime)
                            continue;
                        // We are already on this trip or an earlier (=better) one
                        if (currentTrip != kNoTrip && tripStopTime >= currentTripDep)
                            break;
                        if (tripStopTime >= prevArrival + mct)
                        {
                            currentTrip = tripId;
                            boardStop = stopId;
                            boardStopIndex = stopIndex;
                            break;
                        }
                    }
                }
            }
        }

        // Look at footpaths/transfers
        // feed.transferTimes uses raw StopId keys, so bridge via asStopId().
        for (StationId markedStop : markedStops)
        {
            const StopId markedStopId = asStopId(markedStop);
            if (!feed.transferTimes.count(markedStopId))
                continue;
            if (!earliestArrivalTime[i].count(markedStop))
                continue;

            for (const auto &[toStopId, transferTimeSeconds] : feed.transferTimes.at(markedStopId))
            {
                const Stop *toStopInfo = feed.stopById(toStopId);
                StationId toStation = toStopInfo ? toStopInfo->stationId()
                                                 : StationId{static_cast<uint32_t>(toStopId)};
                Time arrivalTime = earliestArrivalTime[i][markedStop] + transferTimeSeconds;
                if (arrivalTime != kNoTime
                    // local pruning
                    && (!earliestOverallArrivalTime.count(toStation) || arrivalTime < earliestOverallArrivalTime[toStation])
                    // target pruning
                    && (!earliestOverallArrivalTime.count(target) || arrivalTime < earliestOverallArrivalTime[target]))
                {
                    earliestOverallArrivalTime[toStation] = arrivalTime;
                    earliestArrivalTime[i][toStation] = arrivalTime;
                    parent[i][toStation] = ParentLabel{markedStop, kNoTrip, 0, 0};
                    markedStops.insert(toStation);
                }
            }
        }

        // stopping criterion
        if (markedStops.empty())
            break;
    }

    // Reconstruct the pareto-optimal journeys

    std::vector<RouteResult> results;
    if (!earliestOverallArrivalTime.count(target))
        return results;

    for (int numTransfers = 1; numTransfers <= MAX_NUM_ROUNDS; numTransfers++)
    {
        if (!parent[numTransfers].count(target))
            continue;

        RouteResult result;

        int k = numTransfers;
        StationId cur = target;
        while (k >= 1 && cur != origin)
        {
            assert(parent[k].count(cur) && "missing parent entry during journey reconstruction");
            ParentLabel label = parent[k][cur];
            Leg leg;
            leg.departureStop = label.stop;
            leg.arrivalStop = cur;
            leg.departureTimeRealTime = kNoAbsTime;
            leg.arrivalTimeRealTime = kNoAbsTime;

            if (label.trip == kNoTrip)
            {
                // Transfer leg: source was reached in the same round, don't decrement k
                leg.isWalk = true;
                leg.departureTime = midnight + earliestArrivalTime[k][label.stop];
                leg.arrivalTime = midnight + earliestArrivalTime[k][cur];
            }
            else
            {
                // Transit leg: decrement k to look for how we reached the board stop.
                // Use stored stop indices for time lookup, which is correct for circular routes
                // where the same station appears multiple times in the stop sequence.
                leg.isWalk = false;
                const Time dep = getDepartureTimeAtIndex(label.trip, label.boardIndex, feed, overlays);
                const Time arr = getArrivalTimeAtIndex(label.trip, label.stopIndex, feed, overlays);
                leg.departureTime = dep != kNoTime ? midnight + dep : kNoAbsTime;
                leg.arrivalTime = arr != kNoTime ? midnight + arr : kNoAbsTime;
                const Trip *trip = feed.tripById(label.trip);

                if (trip)
                {
                    const Route *route = feed.routeById(trip->routeId);
                    if (route)
                        leg.routeShortName = route->shortName;
                    leg.tripHeadsign = trip->headsign;

                    const auto &routeStops = data.routeStops.at(data.tripToRoute.at(label.trip));
                    for (RouteStopIndex idx = label.boardIndex + 1; idx < label.stopIndex; idx++)
                    {
                        if (idx >= routeStops.size())
                            break;
                        StationId intermediateStopId = routeStops[idx];
                        Time arrivalTimeIntermediate = getArrivalTimeAtIndex(label.trip, idx, feed, overlays);
                        Time departureTimeIntermediate = getDepartureTimeAtIndex(label.trip, idx, feed, overlays);
                        IntermediateStop intermediateStop{
                            intermediateStopId,
                            arrivalTimeIntermediate != kNoTime ? midnight + arrivalTimeIntermediate : kNoAbsTime,
                            departureTimeIntermediate != kNoTime ? midnight + departureTimeIntermediate : kNoAbsTime,
                            kNoAbsTime, kNoAbsTime, false};
                        leg.intermediateStops.push_back(intermediateStop);
                    }
                }
                k--;
            }

            result.push_back(leg);
            cur = label.stop;
        }

        std::reverse(result.begin(), result.end());
        results.push_back(std::move(result));
    }
    return results;
}
