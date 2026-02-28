#pragma once

#include "gtfs/feed.hpp"

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

// Per-stop override produced by a GTFS-RT TripUpdate.
// Times are seconds since midnight of the service day, same as StopTime.
struct StopTimeOverride
{
    std::optional<Time> arrival;   // nullopt = no override
    std::optional<Time> departure; // nullopt = no override
    bool skipped = false;          // true when schedule_relationship == SKIPPED
};

// Signals that the feed explicitly has no data for this stop,
// which stops delay propagation from previous stops.
struct NoDataMarker {};

// Per-stop entry in a TripUpdate: either real override data or a no-data value
using StopTimeEntry = std::variant<StopTimeOverride, NoDataMarker>;

// TripId -> (stop_sequence -> StopTimeEntry)
using TripOverrides    = std::unordered_map<uint32_t, StopTimeEntry>;
using RealtimeOverlays = std::unordered_map<TripId, TripOverrides>;

// Parses a binary GTFS-RT FeedMessage from path and returns per-trip
// stop-time overrides keyed by stop_sequence.
// Trips not found in the static feed are silently skipped.
// Returns an empty map on I/O or parse error.
[[nodiscard]] RealtimeOverlays parseRealtimeFeed(const Feed &feed,
                                                  const std::filesystem::path &path);

// Merges static stop-times with realtime overrides for one trip.
// Every static stop appears exactly once in the result; overridden stops have
// their realtime fields populated; skipped stops have skipped = true.
[[nodiscard]] std::vector<RealtimeStopTime> getRealtimeStopTimesForTrip(
    TripId tripId, const Feed &feed, const RealtimeOverlays &overlays);
