#include "gtfs/feed.hpp"
#include "realtime/realtime.hpp"
#include "routing/connections.hpp"
#include "routing/journey.hpp"
#include "routing/raptor.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#ifdef _WIN32
#include <windows.h>
#endif

// Returns subdirectories of data/ that are most likely GTFS feeds (contain stops.txt),
// sorted alphabetically.
static std::vector<std::filesystem::path> discoverFeeds()
{
    std::vector<std::filesystem::path> feeds;
    const std::filesystem::path dataDir = "data";
    if (!std::filesystem::is_directory(dataDir))
        return feeds;
    for (const auto &entry : std::filesystem::directory_iterator(dataDir))
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "stops.txt"))
            feeds.push_back(entry.path());
    std::ranges::sort(feeds);
    return feeds;
}

// If exactly one feed is found it is returned automatically.
// If multiple feeds exist the user is prompted to pick one by number.
static std::filesystem::path pickFeed()
{
    const auto feeds = discoverFeeds();
    if (feeds.empty())
        throw std::runtime_error("No GTFS feeds found in data/");
    if (feeds.size() == 1)
    {
        std::cout << "Using feed: " << feeds[0].filename().string() << '\n';
        return feeds[0];
    }

    std::cout << "Available feeds:\n";
    for (std::size_t i = 0; i < feeds.size(); ++i)
        std::cout << "  [" << i + 1 << "] " << feeds[i].filename().string() << '\n';

    while (true)
    {
        std::cout << "Select feed (1-" << feeds.size() << "): ";
        std::string line;
        std::getline(std::cin, line);
        int n{};
        const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), n);
        if (ec == std::errc{} && n >= 1 && n <= static_cast<int>(feeds.size()))
        {
            std::cout << "Using feed: " << feeds[static_cast<std::size_t>(n - 1)].filename().string() << '\n';
            return feeds[static_cast<std::size_t>(n - 1)];
        }
        std::cout << "Invalid selection.\n";
    }
}

int main()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    const auto fromDate = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const auto toDate = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now()) + std::chrono::days{1}};

    const auto feedDir = pickFeed();
    const auto realtimePath = feedDir / "realtime.pb";

    const BoundingBox bbox{47.991432, 48.309058, 11.319433, 11.820564};
    const auto feed = Feed::load(feedDir, bbox);
    std::cout << "Loaded feed with " << feed.trips.size() << " trips.\n";

    enum class Router { CSA, RAPTOR };
    Router currentRouter = Router::RAPTOR;

    std::vector<Connection> connections;
    std::optional<RealtimeOverlays> rtOverlays;
    std::optional<RaptorData> raptorData;

    // Builds RaptorData from the feed (and current realtime overlays) if not yet built.
    auto buildRaptorDataIfNeeded = [&]()
    {
        if (!raptorData)
        {
            std::cout << "Building RAPTOR data...\n";
            raptorData = buildRaptorData(feed, rtOverlays ? &*rtOverlays : nullptr);
            std::cout << "RAPTOR data built.\n";
        }
    };

    // Re-parses the realtime .pb and rebuilds connections from the static feed.
    auto reloadRealtime = [&]()
    {
        auto overlays = parseRealtimeFeed(feed, realtimePath);
        const size_t updatedTrips = static_cast<size_t>(std::ranges::count_if(
            overlays, [&](const auto &kv)
            { return kv.first < feed.tripStopTimes.size() && !feed.tripStopTimes[kv.first].empty(); }));
        std::cout << "Realtime updates: " << updatedTrips << " / " << feed.tripStopTimes.size()
                  << " area trips covered (" << overlays.size() << " total in feed).\n";
        connections = buildConnections(feed, fromDate, toDate, &overlays);
        rtOverlays = std::move(overlays);
        raptorData = std::nullopt;
        if (currentRouter == Router::RAPTOR)
            buildRaptorDataIfNeeded();
        std::cout << "Rebuilt " << connections.size() << " connections.\n";
    };

    // Print how many trips are currently en route and show the stop-time list for a sample trip.
    auto printEnRouteStats = [&]()
    {
        const AbsTime now = static_cast<AbsTime>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

        std::unordered_set<TripId> enRouteTrips;
        const Connection *sampleConn = nullptr;
        TripId sampleTripId = kNoTrip;
        std::size_t idx = 0;

        for (const auto &conn : connections)
        {
            if (conn.departureTime > now)
                break;
            if (conn.arrivalTime >= now)
            {
                enRouteTrips.insert(conn.tripId);
                if (!sampleConn && idx > 2 && rtOverlays && rtOverlays->count(conn.tripId))
                {
                    sampleConn = &conn;
                    sampleTripId = conn.tripId;
                }
                ++idx;
            }
        }

        std::size_t withRealtime = 0;
        if (rtOverlays)
            for (TripId tid : enRouteTrips)
                if (rtOverlays->count(tid))
                    ++withRealtime;

        std::cout << "En-route trips right now: " << enRouteTrips.size()
                  << " | with realtime data: " << withRealtime << '\n';

        if (sampleConn && rtOverlays)
        {
            const auto rtTimes = getRealtimeStopTimesForTrip(sampleTripId, feed, *rtOverlays);
            for (const auto &st : rtTimes)
            {
                std::cout << "  " << feed.stopName(st.stopId);
                if (st.arrivalTimeScheduled != kNoTime)
                {
                    std::cout << " arr " << fmtHhmm(st.arrivalTimeScheduled);
                    if (st.arrivalTimeRealtime != kNoTime)
                        std::cout << ' ' << fmtDelay(static_cast<int32_t>(st.arrivalTimeRealtime) - static_cast<int32_t>(st.arrivalTimeScheduled));
                }
                if (st.departureTimeScheduled != kNoTime)
                {
                    std::cout << " dep " << fmtHhmm(st.departureTimeScheduled);
                    if (st.departureTimeRealtime != kNoTime)
                        std::cout << ' ' << fmtDelay(static_cast<int32_t>(st.departureTimeRealtime) - static_cast<int32_t>(st.departureTimeScheduled));
                }
                if (st.skipped)
                    std::cout << " [SKIPPED]";
                std::cout << '\n';
            }
        }
    };

    if (std::filesystem::exists(realtimePath))
    {
        std::cout << "Realtime feed found, loading...\n";
        reloadRealtime();
        printEnRouteStats();
    }
    else
    {
        connections = buildConnections(feed, fromDate, toDate);
    }

    std::cout << "Using " << connections.size() << " connections.\n";
    std::cout << "Number of unique stops in connections: " << countUniqueStops(connections) << "\n";
    buildRaptorDataIfNeeded();

    while (true)
    {
        std::string fromStopName, toStopName;
        std::cout << "\nEnter origin stop ('reload' to refresh realtime, 'raptor'/'csa' to switch router, empty to quit): ";
        std::getline(std::cin, fromStopName);
        if (fromStopName.empty())
            break;

        if (fromStopName == "reload")
        {
            if (!std::filesystem::exists(realtimePath))
                std::cout << "No realtime feed found at " << realtimePath << ".\n";
            else
            {
                reloadRealtime();
                printEnRouteStats();
            }
            continue;
        }

        if (fromStopName == "raptor")
        {
            buildRaptorDataIfNeeded();
            currentRouter = Router::RAPTOR;
            std::cout << "Switched to RAPTOR.\n";
            continue;
        }

        if (fromStopName == "csa")
        {
            currentRouter = Router::CSA;
            std::cout << "Switched to CSA.\n";
            continue;
        }

        std::cout << "Enter destination stop (or empty to quit): ";
        std::getline(std::cin, toStopName);
        if (toStopName.empty())
            break;

        auto fromIt = feed.stopNameIdx.find(fromStopName);
        auto toIt = feed.stopNameIdx.find(toStopName);
        if (fromIt == feed.stopNameIdx.end() || fromIt->second.empty())
        {
            std::cerr << "Stop '" << fromStopName << "' not found.\n";
            continue;
        }
        if (toIt == feed.stopNameIdx.end() || toIt->second.empty())
        {
            std::cerr << "Stop '" << toStopName << "' not found.\n";
            continue;
        }
        const StationId fromStop = *fromIt->second.begin();
        const StationId toStop = *toIt->second.begin();

        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm;
#ifdef _WIN32
        localtime_s(&local_tm, &now_time_t);
#else
        localtime_r(&now_time_t, &local_tm);
#endif
        const std::chrono::year_month_day ymd{
            std::chrono::year{local_tm.tm_year + 1900},
            std::chrono::month{static_cast<unsigned>(local_tm.tm_mon + 1)},
            std::chrono::day{static_cast<unsigned>(local_tm.tm_mday)}};
        const AbsTime midnight = midnightUnix(ymd);
        const AbsTime departureTime = midnight + local_tm.tm_hour * 3600u + local_tm.tm_min * 60u + local_tm.tm_sec;

        std::cout << "Finding route from " << feed.stopName(fromStop)
                  << " to " << feed.stopName(toStop)
                  << " departing at current time"
                  << " [" << (currentRouter == Router::RAPTOR ? "rRAPTOR" : "CSA") << "].\n";

        if (currentRouter == Router::RAPTOR)
        {
            buildRaptorDataIfNeeded();
            const Time relDeparture = departureTime - midnight;
            constexpr Time kSearchWindowSeconds = 2 * 3600;
            const std::vector<RouteResult> results = rangeRaptor(fromStop, toStop, relDeparture, relDeparture + kSearchWindowSeconds,
                                                                  midnight, *raptorData, feed,
                                                                  rtOverlays ? &*rtOverlays : nullptr);
            for (const RouteResult &result : results)
                printRoute(result, feed);
        }
        else
        {
            const RouteResult result = getRoute(fromStop, toStop, departureTime,
                                                connections, feed,
                                                rtOverlays ? &*rtOverlays : nullptr);
            printRoute(result, feed);
        }
    }
}
