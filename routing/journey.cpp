#include "journey.hpp"

#include <format>
#include <iostream>

std::string fmtTime(AbsTime t)
{
    return std::format("{:%Y-%m-%d %H:%M}", std::chrono::sys_seconds{std::chrono::seconds{t}});
}

std::string fmtHhmm(Time t)
{
    return std::format("{:02d}:{:02d}", t / 3600, (t % 3600) / 60);
}

std::string fmtDelay(int32_t s)
{
    if (s == 0)
        return "on time";
    const char *sign = s > 0 ? "+" : "-";
    s = std::abs(s);
    return s < 60 ? std::format("{}{}s", sign, s)
                  : std::format("{}{}m{}s", sign, s / 60, s % 60);
}

RouteResult getRoute(StationId fromStop, StationId toStop, AbsTime departureTime,
                     const std::vector<Connection> &connections,
                     const Feed &feed,
                     const RealtimeOverlays *rtOverlays)
{
    auto csaResult = csa(connections, &feed.transferTimes, fromStop, departureTime, toStop);
    if (!csaResult.earliestArrival.contains(toStop))
    {
        std::cout << "No route found from " << feed.stopName(fromStop)
                  << " to " << feed.stopName(toStop) << ".\n";
        return {};
    }

    auto path = reconstructPath(connections, csaResult, fromStop, toStop);
    TripId prevTrip = kNoTrip;
    RouteResult route;
    std::optional<Leg> currentLeg;
    std::vector<RealtimeStopTime> rtTimes;
    std::optional<int32_t> currentStopArrivalDelay;
    const Connection *lastConn = nullptr;

    // Pushes the open transit leg (if any) to route, using lastConn for arrival info.
    auto finalizeCurrentLeg = [&]()
    {
        if (currentLeg && lastConn)
        {
            currentLeg->arrivalStop = lastConn->arrivalStop;
            currentLeg->arrivalTime = lastConn->arrivalTime;
            if (currentStopArrivalDelay)
                currentLeg->arrivalTimeRealTime = lastConn->arrivalTime + *currentStopArrivalDelay;
            route.push_back(*currentLeg);
            currentLeg.reset();
            currentStopArrivalDelay.reset();
        }
    };

    for (const auto &step : path)
    {
        if (const auto *tl = std::get_if<TransferLeg>(&step))
        {
            finalizeCurrentLeg();
            prevTrip = kNoTrip;
            lastConn = nullptr;
            const AbsTime walkDep = csaResult.earliestArrival.at(tl->from);
            route.push_back(Leg{
                .departureStop         = tl->from,
                .arrivalStop           = tl->to,
                .departureTime         = walkDep,
                .arrivalTime           = walkDep + tl->walkSeconds,
                .departureTimeRealTime = kNoAbsTime,
                .arrivalTimeRealTime   = kNoAbsTime,
                .routeShortName        = "Walk",
                .tripHeadsign          = "",
                .intermediateStops     = {},
                .isWalk                = true});
            continue;
        }

        const Connection *conn   = std::get<const Connection *>(step);
        const Trip       *trip   = feed.tripById(conn->tripId);
        const Route      *route_ = trip ? feed.routeById(trip->routeId) : nullptr;

        // New leg when the trip changes AND it's not the same route+headsign (through service).
        bool newLeg = (conn->tripId != prevTrip);
        if (newLeg && prevTrip != kNoTrip)
        {
            const Trip *prev = feed.tripById(prevTrip);
            if (trip && prev && trip->routeId == prev->routeId && trip->headsign == prev->headsign)
                newLeg = false;
        }

        if (newLeg)
        {
            if (rtOverlays)
                rtTimes = getRealtimeStopTimesForTrip(conn->tripId, feed, *rtOverlays);
            else
                rtTimes.clear();
        }

        const auto rtIt = std::ranges::find_if(rtTimes, [&](const RealtimeStopTime &rt)
            { return static_cast<uint32_t>(rt.stopSequence) == conn->departureSequence; });

        std::optional<int32_t> nextStopArrivalDelay;
        bool currentStopSkipped = false;
        std::optional<int32_t> departureDelay;

        if (rtIt != rtTimes.end())
        {
            if (rtIt->skipped)
            {
                currentStopSkipped = true;
            }
            else if (rtIt->departureTimeRealtime != kNoTime)
            {
                departureDelay       = static_cast<int32_t>(rtIt->departureTimeRealtime)
                                     - static_cast<int32_t>(rtIt->departureTimeScheduled);
                nextStopArrivalDelay = static_cast<int32_t>(rtIt->arrivalTimeRealtime)
                                     - static_cast<int32_t>(rtIt->arrivalTimeScheduled);
            }
        }

        const AbsTime arrAtThisStop = lastConn ? lastConn->arrivalTime : conn->departureTime;
        const IntermediateStop currentStop{
            .stopId                = conn->departureStop,
            .arrivalTime           = arrAtThisStop,
            .departureTime         = conn->departureTime,
            .arrivalTimeRealTime   = currentStopArrivalDelay
                                         ? conn->arrivalTime + *currentStopArrivalDelay : kNoAbsTime,
            .departureTimeRealTime = departureDelay
                                         ? conn->departureTime + *departureDelay : kNoAbsTime,
            .skipped               = currentStopSkipped};

        if (newLeg)
        {
            finalizeCurrentLeg();
            currentLeg = Leg{
                .departureStop         = currentStop.stopId,
                .arrivalStop           = kNoStation,
                .departureTime         = currentStop.departureTime,
                .arrivalTime           = kNoAbsTime,
                .departureTimeRealTime = departureDelay
                                             ? currentStop.departureTime + *departureDelay : kNoAbsTime,
                .arrivalTimeRealTime   = kNoAbsTime,
                .routeShortName        = route_ ? route_->shortName : "",
                .tripHeadsign          = trip   ? trip->headsign    : "",
                .intermediateStops     = {}};
        }
        else
        {
            currentLeg->intermediateStops.push_back(currentStop);
        }

        currentStopArrivalDelay = nextStopArrivalDelay;
        prevTrip = conn->tripId;
        lastConn = conn;
    }

    finalizeCurrentLeg();
    return route;
}

void printRoute(const RouteResult &route, const Feed &feed)
{
    for (const Leg &leg : route)
    {
        if (leg.isWalk)
        {
            std::cout << "\n[Walk " << (leg.arrivalTime - leg.departureTime) / 60 << " min]\n";
            std::cout << "  " << feed.stopName(leg.departureStop)
                      << " -> " << feed.stopName(leg.arrivalStop) << '\n';
            continue;
        }
        std::cout << "\n[" << leg.routeShortName << "] towards " << leg.tripHeadsign << "\n";

        std::cout << "  " << feed.stopName(leg.departureStop)
                  << " dep " << fmtTime(leg.departureTime);
        if (leg.departureTimeRealTime != kNoAbsTime)
            std::cout << " " << fmtDelay(static_cast<int32_t>(leg.departureTimeRealTime)
                                         - static_cast<int32_t>(leg.departureTime));
        std::cout << " -> " << feed.stopName(leg.arrivalStop)
                  << " arr " << fmtTime(leg.arrivalTime);
        if (leg.arrivalTimeRealTime != kNoAbsTime)
            std::cout << " " << fmtDelay(static_cast<int32_t>(leg.arrivalTimeRealTime)
                                         - static_cast<int32_t>(leg.arrivalTime));
        std::cout << '\n';

        for (const auto &stop : leg.intermediateStops)
        {
            std::cout << "    Intermediate stop: " << feed.stopName(stop.stopId)
                      << " arr " << fmtTime(stop.arrivalTime);
            if (stop.arrivalTimeRealTime != kNoAbsTime)
                std::cout << " " << fmtDelay(static_cast<int32_t>(stop.arrivalTimeRealTime)
                                             - static_cast<int32_t>(stop.arrivalTime));
            std::cout << ", dep " << fmtTime(stop.departureTime);
            if (stop.departureTimeRealTime != kNoAbsTime)
                std::cout << " " << fmtDelay(static_cast<int32_t>(stop.departureTimeRealTime)
                                             - static_cast<int32_t>(stop.departureTime));
            if (stop.skipped)
                std::cout << " [SKIPPED]";
            std::cout << '\n';
        }
    }
}
