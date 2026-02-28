#include "realtime.hpp"

#include "realtime/build/gen/gtfs-realtime.pb.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{

    // Seconds since Unix epoch at local midnight for the given date and IANA timezone.
    // This matches the reference used by the static feed (GTFS times are
    // seconds since local midnight), so realtime absolute timestamps converted with
    // this base land in the same space as scheduled times.
    uint32_t localMidnightUnix(std::chrono::year_month_day ymd, const std::string &tzName)
    {
        try
        {
            const auto zone = std::chrono::locate_zone(tzName);
            const auto sysMidnight = std::chrono::zoned_time{zone, std::chrono::local_days{ymd}}.get_sys_time();
            return static_cast<uint32_t>(sysMidnight.time_since_epoch().count());
        }
        catch (const std::exception &)
        {
            // Unknown timezone, so we fall back to UTC midnight (wrong offset but consistent).
            return static_cast<uint32_t>(
                std::chrono::sys_days{ymd}.time_since_epoch().count() * 86400u);
        }
    }

    // Parse an 8-character YYYYMMDD string to year_month_day.
    std::optional<std::chrono::year_month_day> parseYMD(const std::string &s)
    {
        if (s.size() != 8)
            return std::nullopt;
        int y{}, m{}, d{};
        if (std::from_chars(s.data(),     s.data() + 4, y).ec != std::errc{}) return std::nullopt;
        if (std::from_chars(s.data() + 4, s.data() + 6, m).ec != std::errc{}) return std::nullopt;
        if (std::from_chars(s.data() + 6, s.data() + 8, d).ec != std::errc{}) return std::nullopt;
        auto ymd = std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(m)}
                                        / std::chrono::day{static_cast<unsigned>(d)};
        if (!ymd.ok())
            return std::nullopt;
        return ymd;
    }

} // namespace

RealtimeOverlays parseRealtimeFeed(const Feed &feed, const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        std::cerr << "realtime: cannot open " << path << '\n';
        return {};
    }
    const std::string data(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{});

    transit_realtime::FeedMessage msg;
    if (!msg.ParseFromString(data))
    {
        std::cerr << "realtime: failed to parse FeedMessage from " << path << '\n';
        return {};
    }

    RealtimeOverlays overlays;

    for (const auto &entity : msg.entity())
    {
        if (!entity.has_trip_update())
            continue;

        const auto &tu = entity.trip_update();
        const auto &td = tu.trip();

        // Resolve trip_id string -> internal TripId.
        auto tripIt = feed.tripStrIdx.find(td.trip_id());
        if (tripIt == feed.tripStrIdx.end())
            continue;
        const TripId tripId = tripIt->second;

        // Determine midnight of the service day for delay-relative calculations.
        // Use local midnight (agency timezone) so converted times land in the
        // same "seconds since local midnight" space as the static scheduled times.

        uint32_t midnight = 0;
        bool haveMidnight = false;
        if (td.has_start_date())
        {
            if (auto ymd = parseYMD(td.start_date()))
            {
                // TODO: look up the timezone per trip via trip->route->agency instead of assuming
            // the first agency's timezone; requires a route->agency lookup structure in Feed.
            const std::string &tz = feed.agencies.empty() ? "UTC" : feed.agencies[0].timezone;
                midnight = localMidnightUnix(*ymd, tz);
                haveMidnight = true;
            }
        }

        // Static stop-times for this trip (used for delay-relative overrides).
        const auto staticTimes = feed.stopTimesForTrip(tripId);

        TripOverrides &tripOverrides = overlays[tripId];

        for (const auto &stu : tu.stop_time_update())
        {
            // Identify the target stop by sequence (preferred) or by stop_id.
            uint32_t seq = 0;
            bool haveSeq = false;

            if (stu.has_stop_sequence())
            {
                seq = stu.stop_sequence();
                haveSeq = true;
            }
            else if (stu.has_stop_id())
            {
                // Fall back: scan the static span once to find the stop_sequence
                // for this stop_id
                auto stopIt = feed.stopStrIdx.find(stu.stop_id());
                if (stopIt != feed.stopStrIdx.end())
                {
                    const StopId sid = stopIt->second;
                    for (const auto &st : staticTimes)
                    {
                        if (st.stopId == sid)
                        {
                            seq = static_cast<uint32_t>(st.stopSequence);
                            haveSeq = true;
                            break;
                        }
                    }
                }
            }

            if (!haveSeq)
                continue;

            // Direct binary-search lookup for delay calculations.
            const StopTime *staticSt = feed.stopTimeBySequence(tripId, seq);

            using SR = transit_realtime::TripUpdate_StopTimeUpdate_ScheduleRelationship;
            if (stu.schedule_relationship() == SR::TripUpdate_StopTimeUpdate_ScheduleRelationship_SKIPPED)
            {
                tripOverrides[seq] = StopTimeOverride{.skipped = true};
            }
            else if (stu.schedule_relationship() == SR::TripUpdate_StopTimeUpdate_ScheduleRelationship_NO_DATA)
            {
                tripOverrides[seq] = NoDataMarker{};
            }
            else if (stu.schedule_relationship() == SR::TripUpdate_StopTimeUpdate_ScheduleRelationship_SCHEDULED)
            {
                StopTimeOverride ov;

                if (stu.has_arrival())
                {
                    const auto &arr = stu.arrival();
                    if (arr.has_time() && haveMidnight)
                    {
                        // Convert absolute Unix timestamp into seconds since midnight.
                        ov.arrival = static_cast<Time>(arr.time() - static_cast<int64_t>(midnight));
                    }
                    else if (arr.has_delay() && staticSt && staticSt->arrivalTime != kNoTime)
                    {
                        ov.arrival = static_cast<Time>(
                            static_cast<int64_t>(staticSt->arrivalTime) + arr.delay());
                    }
                }

                if (stu.has_departure())
                {
                    const auto &dep = stu.departure();
                    if (dep.has_time() && haveMidnight)
                    {
                        ov.departure = static_cast<Time>(dep.time() - static_cast<int64_t>(midnight));
                    }
                    else if (dep.has_delay() && staticSt && staticSt->departureTime != kNoTime)
                    {
                        ov.departure = static_cast<Time>(
                            static_cast<int64_t>(staticSt->departureTime) + dep.delay());
                    }
                }

                tripOverrides[seq] = ov;
            }
        }
    }

    return overlays;
}

std::vector<RealtimeStopTime> getRealtimeStopTimesForTrip(
    TripId tripId, const Feed &feed, const RealtimeOverlays &overlays)
{
    const auto staticTimes = feed.stopTimesForTrip(tripId);

    std::vector<RealtimeStopTime> result;
    result.reserve(staticTimes.size());

    const TripOverrides *tripOv = nullptr;
    if (auto it = overlays.find(tripId); it != overlays.end())
        tripOv = &it->second;

    // Per the GTFS-RT spec, the delay of the last explicit StopTimeUpdate
    // propagates forward to all subsequent stops that have no update of their own.
    // We track the last known delay (in seconds) as we walk through the stops.
    int32_t propagatedDelay = 0; // 0 until we see the first realtime update
    bool havePropagated = false;

    for (const auto &st : staticTimes)
    {
        RealtimeStopTime rtst;
        rtst.tripId = tripId;
        // Resolve child stop to its root station (same as Connection).
        const Stop *s = feed.stopById(st.stopId);
        rtst.stopId = s ? s->stationId() : StationId{static_cast<uint32_t>(st.stopId)};
        rtst.arrivalTimeScheduled = st.arrivalTime;
        rtst.departureTimeScheduled = st.departureTime;
        rtst.stopSequence = st.stopSequence;

        if (tripOv)
        {
            if (auto ovIt = tripOv->find(static_cast<uint32_t>(st.stopSequence));
                ovIt != tripOv->end())
            {
                std::visit([&](const auto &entry)
                           {
                    using T = std::decay_t<decltype(entry)>;
                    if constexpr (std::is_same_v<T, NoDataMarker>)
                    {
                        havePropagated = false; // explicit no-data: stop delay propagation
                    }
                    else // StopTimeOverride
                    {
                        rtst.skipped = entry.skipped;
                        if (!entry.skipped)
                        {
                            if (entry.arrival)   rtst.arrivalTimeRealtime   = *entry.arrival;
                            if (entry.departure) rtst.departureTimeRealtime = *entry.departure;
                        }

                        // Update the propagated delay from this explicit update.
                        // Prefer departure delay; fall back to arrival delay.
                        if (entry.departure && st.departureTime != kNoTime)
                        {
                            propagatedDelay = static_cast<int32_t>(*entry.departure)
                                            - static_cast<int32_t>(st.departureTime);
                            havePropagated = true;
                        }
                        else if (entry.arrival && st.arrivalTime != kNoTime)
                        {
                            propagatedDelay = static_cast<int32_t>(*entry.arrival)
                                            - static_cast<int32_t>(st.arrivalTime);
                            havePropagated = true;
                        }
                    } }, ovIt->second);
            }
            else if (havePropagated && !rtst.skipped)
            {
                // No explicit update for this stop, so we propagate the last known delay.
                if (st.arrivalTime != kNoTime)
                    rtst.arrivalTimeRealtime = static_cast<Time>(
                        static_cast<int32_t>(st.arrivalTime) + propagatedDelay);
                if (st.departureTime != kNoTime)
                    rtst.departureTimeRealtime = static_cast<Time>(
                        static_cast<int32_t>(st.departureTime) + propagatedDelay);
            }
        }

        result.push_back(rtst);
    }

    return result;
}
