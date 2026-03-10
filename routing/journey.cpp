#include "journey.hpp"

#include <algorithm>
#include <format>
#include <iostream>

std::string fmtTime(AbsTime t)
{
    return std::format("{:%Y-%m-%d %H:%M}", std::chrono::sys_seconds{std::chrono::seconds{t}});
}

static std::string fmtAbsHhmm(AbsTime t)
{
    return std::format("{:%H:%M}", std::chrono::sys_seconds{std::chrono::seconds{t}});
}

std::string fmtHhmm(Time t)
{
    return std::format("{:02d}:{:02d}", t / 3600, (t % 3600) / 60);
}

std::string fmtDelay(int32_t s)
{
    if (s == 0)
        return "(on time)";
    const char *sign = s > 0 ? "+" : "-";
    s = std::abs(s);
    std::string inner = s < 60  ? std::format("{}{}s", sign, s)
                      : s % 60  ? std::format("{}{}m{}s", sign, s / 60, s % 60)
                                : std::format("{}{}m", sign, s / 60);
    return '(' + inner + ')';
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
    if (route.empty())
        return;

    const unsigned totalMin = (route.back().arrivalTime - route.front().departureTime) / 60;
    std::cout << '\n'
              << feed.stopName(route.front().departureStop) << " \u2192 "
              << feed.stopName(route.back().arrivalStop)
              << "  (" << totalMin << " min)\n";

    for (const Leg &leg : route)
    {
        std::cout << '\n';
        if (leg.isWalk)
        {
            const unsigned min = (leg.arrivalTime - leg.departureTime) / 60;
            std::cout << "  \u2195 " << min << " min walk\n";
            continue;
        }

        std::cout << "  [" << leg.routeShortName << "] " << leg.tripHeadsign << '\n';

        // Column width: widest stop name in this leg + 2 padding.
        std::size_t nameWidth = std::max(feed.stopName(leg.departureStop).size(),
                                         feed.stopName(leg.arrivalStop).size());
        for (const auto &s : leg.intermediateStops)
            if (!s.skipped)
                nameWidth = std::max(nameWidth, feed.stopName(s.stopId).size());
        nameWidth += 2;

        auto printStop = [&](std::string_view prefix, const std::string &name, AbsTime scheduled,
                             AbsTime realtime)
        {
            std::cout << std::format("  {} {:<{}} {}", prefix, name, nameWidth,
                                     fmtAbsHhmm(scheduled));
            if (realtime != kNoAbsTime)
                std::cout << ' ' << fmtDelay(static_cast<int32_t>(realtime)
                                             - static_cast<int32_t>(scheduled));
            std::cout << '\n';
        };

        printStop("\u25cf", feed.stopName(leg.departureStop),
                  leg.departureTime, leg.departureTimeRealTime);

        for (const auto &stop : leg.intermediateStops)
        {
            if (stop.skipped)
                continue;
            printStop("\u2502", feed.stopName(stop.stopId),
                      stop.arrivalTime, stop.arrivalTimeRealTime);
        }

        printStop("\u25cf", feed.stopName(leg.arrivalStop),
                  leg.arrivalTime, leg.arrivalTimeRealTime);
    }
}
