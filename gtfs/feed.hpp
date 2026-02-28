#pragma once

#include "types.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Transparent hash for std::unordered_map to support heterogeneous lookup
// (i.e. find(string_view) without constructing a std::string).
struct StringHash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
};

struct BoundingBox
{
    double minLat = std::numeric_limits<double>::max();
    double maxLat = std::numeric_limits<double>::lowest();
    double minLon = std::numeric_limits<double>::max();
    double maxLon = std::numeric_limits<double>::lowest();

    bool contains(double lat, double lon) const
    {
        return lat >= minLat && lat <= maxLat && lon >= minLon && lon <= maxLon;
    }
};

// Holds a fully-loaded GTFS feed.
struct Feed
{
    std::vector<Agency> agencies;
    // internal id -> stop
    std::vector<Stop> stops;
    std::vector<Route> routes;
    std::vector<Trip> trips;
    std::vector<CalendarEntry> calendar;
    std::vector<CalendarDateException> calendarDates;

    // GTFS id -> internal id
    std::unordered_map<std::string, StopId, StringHash, std::equal_to<>> stopStrIdx;
    std::unordered_map<std::string, RouteId, StringHash, std::equal_to<>> routeStrIdx;
    std::unordered_map<std::string, TripId, StringHash, std::equal_to<>> tripStrIdx;
    std::unordered_map<std::string, ServiceId, StringHash, std::equal_to<>> serviceStrIdx;

    // from stopId -> to stopId -> min transfer time in seconds
    std::unordered_map<StopId, std::unordered_map<StopId, uint32_t>> transferTimes;

    // Stop name -> root-station id (for user-friendly lookup; only root stops indexed)
    std::unordered_map<std::string, std::unordered_set<StationId>, StringHash, std::equal_to<>> stopNameIdx;

    // TripId -> stop-times for that trip, sorted by stopSequence.
    std::vector<std::vector<StopTime>> tripStopTimes;

    // TripId -> index in trips vector.
    // Needed because TripIds are assigned in stop_times.txt discovery order
    // while trips are stored in trips.txt order, so the two orderings differ.
    std::unordered_map<TripId, std::size_t> tripIdx;

    // Load a feed from a directory of unzipped GTFS .txt files.
    // Throws std::runtime_error if a required file is missing or malformed.
    [[nodiscard]] static Feed load(const std::filesystem::path &dir, std::optional<BoundingBox> bbox = std::nullopt);

    // O(1) look-ups; return nullptr when the id is not found.
    [[nodiscard]] const Stop *stopById(const StopId &) const noexcept;
    [[nodiscard]] const Route *routeById(const RouteId &) const noexcept;
    [[nodiscard]] const Trip *tripById(const TripId &) const noexcept;

    [[nodiscard]] const Stop *stopByName(const std::string &) const noexcept;
    // Returns the name of the station with the given id, or an empty string.
    [[nodiscard]] const std::string &stopName(StationId id) const noexcept;

    // Returns the stop-times for a trip as a contiguous, pre-sorted span.
    // Returns an empty span when the trip is not found.
    [[nodiscard]] std::span<const StopTime> stopTimesForTrip(const TripId &) const noexcept;

    // Returns the single StopTime whose stopSequence equals seq, or nullptr.
    // Uses binary search on the pre-sorted span. O(log n).
    [[nodiscard]] const StopTime *stopTimeBySequence(const TripId &, uint32_t seq) const noexcept;
};
