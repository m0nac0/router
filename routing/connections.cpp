#include "connections.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <unordered_set>

AbsTime midnightUnix(std::chrono::year_month_day ymd)
{
    auto dp = std::chrono::sys_days{ymd};
    return static_cast<AbsTime>(dp.time_since_epoch().count() * 86400u);
}

namespace
{

std::unordered_map<ServiceId, std::set<CalendarEntry>> buildServiceIndex(const Feed &feed)
{
    std::unordered_map<ServiceId, std::set<CalendarEntry>> index;
    for (const auto &entry : feed.calendar)
        index[entry.serviceId].insert(entry);
    return index;
}

std::unordered_map<ServiceId, std::set<CalendarDateException>> buildServiceExceptionsIndex(const Feed &feed)
{
    std::unordered_map<ServiceId, std::set<CalendarDateException>> index;
    for (const auto &ex : feed.calendarDates)
        index[ex.serviceId].insert(ex);
    return index;
}

bool isServiceActive(const std::unordered_map<ServiceId, std::set<CalendarEntry>> &serviceEntries,
                     const std::unordered_map<ServiceId, std::set<CalendarDateException>> &serviceExceptions,
                     ServiceId serviceId, const std::chrono::year_month_day &date)
{
    auto exIt = serviceExceptions.find(serviceId);
    if (exIt != serviceExceptions.end())
    {
        CalendarDateException key;
        key.date = date;
        auto it = exIt->second.lower_bound(key);
        if (it != exIt->second.end() && it->date == date)
            return it->exceptionType == 1; // 1 = added, 2 = removed
    }

    auto entryIt = serviceEntries.find(serviceId);
    if (entryIt != serviceEntries.end())
    {
        for (const auto &entry : entryIt->second)
        {
            if (entry.startDate <= date && date <= entry.endDate)
            {
                // iso_encoding() returns 1=Mon ... 7=Sun; subtract 1 to get 0=Mon ... 6=Sun,
                // matching the daysOfWeek array layout.
                auto weekday = std::chrono::weekday{std::chrono::sys_days{date}}.iso_encoding() - 1;
                return entry.daysOfWeek[weekday];
            }
        }
    }

    return false;
}

} // namespace

std::vector<Connection> buildConnections(const Feed &feed,
                                         std::chrono::year_month_day fromDate,
                                         std::chrono::year_month_day toDate,
                                         const RealtimeOverlays *overlays)
{
    std::vector<Connection> connections;
    auto serviceEntries    = buildServiceIndex(feed);
    auto serviceExceptions = buildServiceExceptionsIndex(feed);

    for (auto d = std::chrono::sys_days{fromDate};
         d <= std::chrono::sys_days{toDate};
         d += std::chrono::days{1})
    {
        auto date = std::chrono::year_month_day{d};
        AbsTime midnight = midnightUnix(date);

        for (const auto &trip : feed.trips)
        {
            if (!isServiceActive(serviceEntries, serviceExceptions, trip.serviceId, date))
                continue;

            if (overlays && overlays->contains(trip.id))
            {
                // Realtime path: merge static times with overrides, bridge over skipped stops.
                const auto rtTimes = getRealtimeStopTimesForTrip(trip.id, feed, *overlays);
                const RealtimeStopTime *prev = nullptr;
                for (const auto &rt : rtTimes)
                {
                    if (rt.skipped)
                        continue; // skipped stop: prev stays at the last non-skipped stop
                    if (prev)
                    {
                        const Time depTime = prev->getExpectedDeparture();
                        const Time arrTime = rt.getExpectedArrival();
                        if (depTime != kNoTime && arrTime != kNoTime)
                        {
                            connections.push_back({prev->stopId, depTime + midnight,
                                                   rt.stopId,   arrTime + midnight,
                                                   trip.id,
                                                   static_cast<uint32_t>(prev->stopSequence)});
                        }
                    }
                    prev = &rt;
                }
            }
            else
            {
                // Static path: resolve each child stop to its root station via Stop::stationId().
                auto stopTimes = feed.stopTimesForTrip(trip.id);
                for (std::size_t i = 0; i + 1 < stopTimes.size(); ++i)
                {
                    const auto &st1 = stopTimes[i];
                    const auto &st2 = stopTimes[i + 1];
                    auto stop1 = feed.stopById(st1.stopId);
                    auto stop2 = feed.stopById(st2.stopId);
                    if (!stop1 || !stop2)
                        continue;
                    connections.push_back({stop1->stationId(), st1.departureTime + midnight,
                                           stop2->stationId(), st2.arrivalTime   + midnight,
                                           trip.id,
                                           static_cast<uint32_t>(st1.stopSequence)});
                }
            }
        }
    }

    std::ranges::sort(connections, {}, &Connection::departureTime);
    return connections;
}

std::size_t countUniqueStops(const std::vector<Connection> &connections)
{
    std::unordered_set<StationId> unique;
    for (const auto &conn : connections)
    {
        unique.insert(conn.departureStop);
        unique.insert(conn.arrivalStop);
    }
    return unique.size();
}
