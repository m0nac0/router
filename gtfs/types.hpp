#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

//  ID type aliases ─

using AgencyId  = std::string;
using RouteId   = uint32_t;
using TripId    = uint32_t;
using ServiceId = uint32_t;

// Raw GTFS stop ID. May be a child platform/quay or a root station.
// All data below the connection-building layer (StopTime, RealtimeStopTime,
// Feed internals) uses StopId.
enum class StopId : uint32_t {};

// Resolved root-station ID. Always refers to a stop with no parent
// (or IS the parent of child platforms).  All routing-layer types
// (Connection, CSA) use StationId so accidental mixing
// with raw child-stop IDs is a compile error.
enum class StationId : uint32_t {};

// Recovers the underlying StopId from a StationId.
// Safe because every StationId is a valid StopId (root stops are stops too).
inline StopId asStopId(StationId sid) noexcept
{
    return StopId{static_cast<uint32_t>(sid)};
}

//  std::hash specialisations for unordered containers

namespace std
{
    template<> struct hash<StopId>
    {
        size_t operator()(StopId id) const noexcept
        {
            return hash<uint32_t>{}(static_cast<uint32_t>(id));
        }
    };
    template<> struct hash<StationId>
    {
        size_t operator()(StationId id) const noexcept
        {
            return hash<uint32_t>{}(static_cast<uint32_t>(id));
        }
    };
} // namespace std

//  Scalar types

// Seconds since midnight of the service day.  May exceed 86 400 for trips
// that run past midnight (e.g. 25:30:00 == 91 800).
using Time = uint32_t;

// Seconds since Unix epoch.
using AbsTime = uint32_t;

inline constexpr Time      kNoTime         = UINT32_MAX;
inline constexpr AbsTime   kNoAbsTime      = UINT32_MAX;
inline constexpr StopId    kNoParentStation{UINT32_MAX};
inline constexpr StationId kNoStation      {UINT32_MAX};
inline constexpr TripId    kNoTrip         = UINT32_MAX;

//  Enumerations

enum class RouteType : int
{
    Tram       = 0,
    Subway     = 1,
    Rail       = 2,
    Bus        = 3,
    Ferry      = 4,
    CableTram  = 5,
    AerialLift = 6,
    Funicular  = 7,
    Trolleybus = 11,
    Monorail   = 12,
};

//  Data structures

struct Agency
{
    AgencyId    id;
    std::string name;
    std::string url;
    std::string timezone;
};

struct Stop
{
    StopId      id{};
    std::string name;
    double      lat{};
    double      lon{};
    int         locationType{};
    StopId      parentStation{kNoParentStation};

    // Returns the resolved root-station ID: the parent if one exists,
    // otherwise this stop itself.  Always valid after Feed::load completes.
    StationId stationId() const noexcept
    {
        return StationId{static_cast<uint32_t>(
            parentStation != kNoParentStation ? parentStation : id)};
    }
};

struct Route
{
    RouteId     id{};
    std::string shortName;
    std::string longName;
    RouteType   type{RouteType::Bus};
};

struct Trip
{
    TripId      id{};
    RouteId     routeId{};
    ServiceId   serviceId{};
    std::string headsign;
    int         directionId{};
};

struct StopTime
{
    StopId stopId{};          // raw child-stop ID
    Time   arrivalTime{kNoTime};
    Time   departureTime{kNoTime};
    int    stopSequence{};
    // tripId is intentionally absent: stop-times are stored per-trip in Feed,
    // so the trip is implicit from the container.
};

struct RealtimeStopTime
{
    TripId    tripId{};
    StationId stopId{};        // resolved root-station ID
    Time      arrivalTimeScheduled{kNoTime};
    Time      departureTimeScheduled{kNoTime};
    Time      arrivalTimeRealtime{kNoTime};
    Time      departureTimeRealtime{kNoTime};
    bool      skipped{false};  // whether the trip will skip this stop entirely
    int       stopSequence{};

    Time getExpectedArrival() const
    {
        if (skipped)
            return kNoTime;
        return arrivalTimeRealtime != kNoTime ? arrivalTimeRealtime : arrivalTimeScheduled;
    }

    Time getExpectedDeparture() const
    {
        if (skipped)
            return kNoTime;
        return departureTimeRealtime != kNoTime ? departureTimeRealtime : departureTimeScheduled;
    }
};

struct CalendarEntry
{
    ServiceId                   serviceId{};
    std::array<bool, 7>         daysOfWeek{}; // [0]=Mon ... [6]=Sun
    std::chrono::year_month_day startDate;
    std::chrono::year_month_day endDate;

    bool operator<(const CalendarEntry &other) const { return startDate < other.startDate; }
};

struct CalendarDateException
{
    ServiceId                   serviceId{};
    std::chrono::year_month_day date;
    int                         exceptionType{}; // 1 = service added, 2 = service removed

    bool operator<(const CalendarDateException &other) const { return date < other.date; }
};
