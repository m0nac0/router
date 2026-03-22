#pragma once

#include "gtfs/feed.hpp"
#include "realtime/realtime.hpp"

#include <chrono>
#include <set>
#include <vector>

// Returns the Unix timestamp (seconds) for midnight UTC of the given date.
AbsTime midnightUnix(std::chrono::year_month_day ymd);

// A single transit leg between two consecutive stops on the same trip.
// Both stop IDs are root-station IDs (StationId), never raw child-stop IDs.
struct Connection
{
    StationId departureStop;
    AbsTime departureTime;
    StationId arrivalStop;
    AbsTime arrivalTime;
    TripId tripId;
    uint32_t departureSequence; // stop_sequence of the departure stop, for realtime lookup
};

std::unordered_map<ServiceId, std::set<CalendarEntry>> buildServiceIndex(const Feed &feed);
std::unordered_map<ServiceId, std::set<CalendarDateException>> buildServiceExceptionsIndex(const Feed &feed);
bool isServiceActive(const std::unordered_map<ServiceId, std::set<CalendarEntry>> &serviceEntries,
                     const std::unordered_map<ServiceId, std::set<CalendarDateException>> &serviceExceptions,
                     ServiceId serviceId, const std::chrono::year_month_day &date);

// Expands every active trip within [fromDate, toDate] into Connection records,
// resolves child stops to their parent station, and returns the list sorted by
// departure time (required by the CSA scan).
// When overlays is non-null, the realtime arrival/departure times are used instead of scheduled times
// and skipped stops are excluded.
std::vector<Connection> buildConnections(const Feed &feed,
                                         std::chrono::year_month_day fromDate,
                                         std::chrono::year_month_day toDate,
                                         const RealtimeOverlays *overlays = nullptr);

std::size_t countUniqueStops(const std::vector<Connection> &connections);
