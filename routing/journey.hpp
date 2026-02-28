#pragma once

#include "gtfs/feed.hpp"
#include "realtime/realtime.hpp"
#include "routing/csa.hpp"

#include <string>
#include <vector>

// A single stop within a transit leg, between the board and alight stops.
struct IntermediateStop
{
    StationId stopId;
    AbsTime   arrivalTime;
    AbsTime   departureTime;
    AbsTime   arrivalTimeRealTime;
    AbsTime   departureTimeRealTime;
    bool      skipped;
};

// One leg of a journey: either a transit ride or a walking transfer.
struct Leg
{
    StationId   departureStop;
    StationId   arrivalStop;
    AbsTime     departureTime;
    AbsTime     arrivalTime;
    AbsTime     departureTimeRealTime;
    AbsTime     arrivalTimeRealTime;
    std::string routeShortName;
    std::string tripHeadsign;
    std::vector<IntermediateStop> intermediateStops;
    bool isWalk = false;
};

using RouteResult = std::vector<Leg>;

std::string fmtTime(AbsTime t);
std::string fmtHhmm(Time t);
std::string fmtDelay(int32_t s);

RouteResult getRoute(StationId fromStop, StationId toStop, AbsTime departureTime,
                     const std::vector<Connection> &connections,
                     const Feed &feed,
                     const RealtimeOverlays *rtOverlays = nullptr);

void printRoute(const RouteResult &route, const Feed &feed);
