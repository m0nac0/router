#include "feed.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace
{

    //  CSV parser

    // Strips a UTF-8 BOM (EF BB BF) from the start of a line if present.
    std::string_view strip_bom(std::string_view s)
    {
        constexpr std::string_view kBom = "\xEF\xBB\xBF";
        if (s.starts_with(kBom))
            return s.substr(kBom.size());
        return s;
    }


        // Strip surrounding double-quotes from a raw field view produced by
    // split_csv_fields.  Handles only the simple `"value"` form, which should be  sufficient
    // for GTFS identifier and numeric fields.
    std::string_view unquote(std::string_view sv) noexcept
    {
        if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"')
            return sv.substr(1, sv.size() - 2);
        return sv;
    }

    // Splits a CSV line into at most max_n fields stored as string_views into the
    // line buffer.  Tracks quoted sections so commas inside quotes don't split
    // fields.  The string_views include the surrounding
    // quote characters for quoted fields. Strips a trailing bare \r.
    int split_csv_fields(std::string_view line, std::string_view *out, int max_n) noexcept
    {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const char *p = line.data(), *e = p + line.size();
        int n = 0;
        while (n < max_n)
        {
            const char *s = p;
            bool in_quotes = false;
            while (p < e)
            {
                if (*p == '"')
                {
                    in_quotes = !in_quotes;
                    ++p;
                }
                else if (!in_quotes && *p == ',')
                    break;
                else
                    ++p;
            }
            out[n++] = {s, static_cast<std::size_t>(p - s)};
            if (p >= e)
                break;
            ++p; // skip comma
        }
        return n;
    };

    static constexpr int kMaxCsvColumns = 64;

    // Streaming CSV reader: parses the header once on open(), then yields rows
    // one at a time without buffering the whole file.
    //
    // col() / require_col() must be called before the first next() to declare which columns will be accessed.
    struct CsvReader
    {
        // Factory: opens path, parses header line, strips BOM. Throws on failure.
        static CsvReader open(const std::filesystem::path &path)
        {
            CsvReader r;
            constexpr std::size_t kBufSize = 1 << 22; // 4 MB read buffer
            r._buf.resize(kBufSize);
            r._file.rdbuf()->pubsetbuf(r._buf.data(), kBufSize);
            r._file.open(path);
            if (!r._file.is_open())
                throw std::runtime_error("Cannot open GTFS file: " + path.string());
            std::getline(r._file, r._line);
            std::string_view fields[kMaxCsvColumns];
            int n = split_csv_fields(strip_bom(r._line), fields, kMaxCsvColumns);
            for (int i = 0; i < n; ++i)
                r._headers.emplace_back(unquote(fields[i]));
            return r;
        }

        // Return column index, or -1 if absent.
        int col(std::string_view name) noexcept
        {
            for (int i = 0; i < static_cast<int>(_headers.size()); ++i)
            {
                if (_headers[i] == name)
                {
                    _need_cols = std::max(_need_cols, i + 1);
                    return i;
                }
            }
            return -1;
        }

        // Like col(), but throws if the column is missing.
        int require_col(std::string_view name)
        {
            int c = col(name);
            if (c < 0)
                throw std::runtime_error("Missing required column: " + std::string(name));
            return c;
        }

        // Advance to the next non-empty row. Returns false at EOF.
        bool next()
        {
            while (std::getline(_file, _line))
            {
                if (_line.empty() || _line == "\r")
                    continue;
                _field_count = split_csv_fields(_line, _fields, _need_cols);
                return true;
            }
            return false;
        }

        // Field value at column index, unquoted.
        // Valid only until the next call to next(). Asserts col_idx is valid.
        std::string_view get(int col_idx) const noexcept
        {
            assert(col_idx >= 0 && col_idx < _field_count);
            return unquote(_fields[col_idx]);
        }

    private:
        std::vector<char> _buf;    // I/O read buffer, must outlive _file
        std::ifstream _file;
        std::string _line;
        std::vector<std::string> _headers;
        std::string_view _fields[kMaxCsvColumns];
        int _field_count = 0;
        int _need_cols = 0;
    };

    //  Scalar parsing helpers

    // Parses GTFS time "H:MM:SS" or "HH:MM:SS" (hours may exceed 23).
    // Returns kNoTime for empty or malformed input.
    Time parse_time(std::string_view s)
    {
        if (s.empty())
            return kNoTime;
        const char *p = s.data(), *end = p + s.size();
        uint32_t h{}, m{}, sec{};
        auto [p1, ec1] = std::from_chars(p, end, h);
        if (ec1 != std::errc{} || p1 == end || *p1 != ':')
            return kNoTime;
        auto [p2, ec2] = std::from_chars(p1 + 1, end, m);
        if (ec2 != std::errc{} || p2 == end || *p2 != ':')
            return kNoTime;
        auto [p3, ec3] = std::from_chars(p2 + 1, end, sec);
        if (ec3 != std::errc{})
            return kNoTime;
        return h * 3600u + m * 60u + sec;
    }

    // Parses an integer; returns default_val for empty or malformed input.
    int parse_int(std::string_view s, int default_val = 0)
    {
        if (s.empty())
            return default_val;
        int v{};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        return ec == std::errc{} ? v : default_val;
    }

    // Parses a floating-point coordinate.
    double parse_double(std::string_view s)
    {
        if (s.empty())
            return 0.0;
        return std::stod(std::string(s));
    }

    // Parses "YYYYMMDD" to chrono::year_month_day.
    std::chrono::year_month_day parseGtfsDate(std::string_view s)
    {
        if (s.size() != 8)
            throw std::invalid_argument("Invalid GTFS date: " + std::string(s));
        int y = parse_int(s.substr(0, 4));
        int m = parse_int(s.substr(4, 2));
        int d = parse_int(s.substr(6, 2));
        return std::chrono::year{y} / std::chrono::month{static_cast<unsigned>(m)} / std::chrono::day{static_cast<unsigned>(d)};
    }

    //  Per-file loaders

    std::vector<Agency> loadAgencies(const std::filesystem::path &dir)
    {
        auto csv = CsvReader::open(dir / "agency.txt");
        const int ci_id = csv.col("agency_id");
        const int ci_name = csv.require_col("agency_name");
        const int ci_url = csv.require_col("agency_url");
        const int ci_tz = csv.require_col("agency_timezone");

        std::vector<Agency> result;
        while (csv.next())
        {
            result.push_back({
                ci_id >= 0 ? std::string(csv.get(ci_id)) : "",
                std::string(csv.get(ci_name)),
                std::string(csv.get(ci_url)),
                std::string(csv.get(ci_tz)),
            });
        }
        std::cout << "Loaded " << result.size() << " agencies.\n";
        return result;
    }

    std::vector<Stop> loadStops(const std::filesystem::path &dir,
                                std::unordered_map<std::string, StopId, StringHash, std::equal_to<>> &stopStrIdx, std::optional<BoundingBox> bbox = std::nullopt)
    {
        auto csv = CsvReader::open(dir / "stops.txt");
        const int ci_id = csv.require_col("stop_id");
        const int ci_lat = csv.require_col("stop_lat");
        const int ci_lon = csv.require_col("stop_lon");
        const int ci_name = csv.require_col("stop_name");
        const int ci_type = csv.col("location_type");
        const int ci_parent = csv.col("parent_station");

        std::vector<Stop> result;

        // Track stops whose parent wasn't loaded yet at the time of their row.
        // Stored as (index into result, parent string id).
        std::vector<std::pair<std::size_t, std::string>> pendingParents;

        while (csv.next())
        {
            Stop stop;
            stop.lat = parse_double(csv.get(ci_lat));
            stop.lon = parse_double(csv.get(ci_lon));
            if (bbox && !bbox->contains(stop.lat, stop.lon))
                continue;

            stop.locationType = ci_type >= 0 ? parse_int(csv.get(ci_type)) : 0;
            if (stop.locationType > 1)
                continue; // we only care about stops and stations, not entrances / boarding areas
            const auto strId = std::string(csv.get(ci_id));
            if (stopStrIdx.contains(strId))
                continue; // duplicate stop_id; skip

            // Use result.size() so IDs are always 0, 1, 2, ... without gaps.
            stop.id = StopId{static_cast<uint32_t>(result.size())};
            stopStrIdx[strId] = stop.id;

            stop.name = std::string(csv.get(ci_name));

            if (ci_parent >= 0)
            {
                const auto parentStr = std::string(csv.get(ci_parent));
                if (!parentStr.empty())
                {
                    auto it = stopStrIdx.find(parentStr);
                    if (it != stopStrIdx.end())
                        stop.parentStation = it->second;
                    else
                        pendingParents.emplace_back(result.size(), parentStr);
                }
            }
            result.push_back(std::move(stop));
        }

        // Second pass: resolve parent links for stops whose parent row appeared
        // after the child row (or was filtered out by the bbox).
        for (auto &[idx, parentStr] : pendingParents)
        {
            auto it = stopStrIdx.find(parentStr);
            if (it != stopStrIdx.end())
                result[idx].parentStation = it->second;
            // else: parent outside bbox or absent. In that case, leave as kNoParentStation
        }

        std::cout << "Loaded " << result.size() << " stops.\n";
        return result;
    }

    std::vector<Route> loadRoutes(const std::filesystem::path &dir,
                                  std::unordered_map<std::string, RouteId, StringHash, std::equal_to<>> &routeStrIdx)
    {
        std::vector<Route> result;
        auto csv = CsvReader::open(dir / "routes.txt");
        const int ci_route = csv.require_col("route_id");
        const int ci_type = csv.require_col("route_type");
        const int ci_long_name = csv.col("route_long_name");
        const int ci_short_name = csv.col("route_short_name");

        while (csv.next())
        {
            const auto strId = std::string(csv.get(ci_route));
            if (routeStrIdx.contains(strId))
            {
                std::cerr << "Duplicate route_id: " << strId << '\n';
                continue;
            }
            Route route;
            route.id = static_cast<RouteId>(routeStrIdx.size());
            routeStrIdx[strId] = route.id;
            route.shortName = ci_short_name >= 0 ? std::string(csv.get(ci_short_name)) : "";
            route.longName = ci_long_name >= 0 ? std::string(csv.get(ci_long_name)) : "";
            route.type = static_cast<RouteType>(parse_int(csv.get(ci_type), 3));
            result.push_back(std::move(route));
        }
        std::cout << "Loaded " << result.size() << " routes.\n";
        return result;
    }



    std::vector<Trip> loadTrips(const std::filesystem::path &dir,
                                std::unordered_map<std::string, TripId, StringHash, std::equal_to<>> &tripStrIdx,
                                std::unordered_map<std::string, RouteId, StringHash, std::equal_to<>> &routeStrIdx,
                                std::unordered_map<std::string, ServiceId, StringHash, std::equal_to<>> &serviceStrIdx)
    {
        auto csv = CsvReader::open(dir / "trips.txt");
        const int ci_route     = csv.require_col("route_id");
        const int ci_service   = csv.require_col("service_id");
        const int ci_trip      = csv.require_col("trip_id");
        const int ci_headsign  = csv.col("trip_headsign");
        const int ci_direction = csv.col("direction_id");

        // tripStrIdx is already populated by loadStopTimes with in-bbox trips only.
        std::vector<Trip> result;
        result.reserve(tripStrIdx.size());
        while (csv.next())
        {
            auto it_trip = tripStrIdx.find(csv.get(ci_trip));
            if (it_trip == tripStrIdx.end())
                continue;

            auto it_route = routeStrIdx.find(csv.get(ci_route));
            if (it_route == routeStrIdx.end())
                continue;

            auto it_service = serviceStrIdx.find(csv.get(ci_service));
            if (it_service == serviceStrIdx.end())
                continue;

            result.push_back(Trip{
                .id = it_trip->second,
                .routeId = it_route->second,
                .serviceId = it_service->second,
                .headsign = ci_headsign >= 0 ? std::string(csv.get(ci_headsign)) : "",
                .directionId = ci_direction >= 0 ? parse_int(csv.get(ci_direction), 0) : -1,
            });
        }
        std::cout << "Loaded " << result.size() << " trips.\n";
        return result;
    }

    // stop_times.txt is streamed line-by-line to avoid holding millions of string
    // rows in memory at once. Returns stop-times indexed directly by TripId.
    // Also populates tripStrIdx: only trips that have at least one in-bbox stop
    // receive a TripId, so the returned vector only covers in-bbox trips.
    // The caller is responsible for sorting each trip's times by stopSequence.
    std::vector<std::vector<StopTime>> loadStopTimes(
        const std::filesystem::path &dir,
        std::unordered_map<std::string, TripId, StringHash, std::equal_to<>> &tripStrIdx,
        std::unordered_map<std::string, StopId, StringHash, std::equal_to<>> &stopStrIdx)
    {
        auto csv = CsvReader::open(dir / "stop_times.txt");
        const int ci_trip = csv.require_col("trip_id");
        const int ci_stop = csv.require_col("stop_id");
        const int ci_arr  = csv.require_col("arrival_time");
        const int ci_dep  = csv.require_col("departure_time");
        const int ci_seq  = csv.require_col("stop_sequence");

        // Sized lazily: only in-bbox trips get an entry.
        std::vector<std::vector<StopTime>> result;
        std::size_t total = 0;

        // Cache the last-seen trip to avoid a hash-map lookup on every row,
        // exploiting the common case where stop_times.txt is sorted by trip_id.
        std::string  lastTripStr;
        std::size_t  lastTripIdx = SIZE_MAX; // index into result; SIZE_MAX = unknown/out-of-bbox

        while (csv.next())
        {
            // Check stop_id first: the bbox filter makes it the most selective.
            auto it_stop = stopStrIdx.find(csv.get(ci_stop));
            if (it_stop == stopStrIdx.end())
                continue;

            // Trip lookup with single-entry cache.
            const auto tripSv = csv.get(ci_trip);
            if (tripSv != lastTripStr)
            {
                lastTripStr = tripSv; // copy into owned string before next()
                auto it_trip = tripStrIdx.find(std::string_view(lastTripStr));
                if (it_trip != tripStrIdx.end())
                {
                    lastTripIdx = static_cast<std::size_t>(it_trip->second);
                }
                else
                {
                    // First in-bbox stop for this trip: assign a new dense TripId.
                    lastTripIdx = result.size();
                    tripStrIdx.emplace(lastTripStr, static_cast<TripId>(lastTripIdx));
                    result.emplace_back();
                }
            }

            result[lastTripIdx].push_back({
                it_stop->second,
                parse_time(csv.get(ci_arr)),
                parse_time(csv.get(ci_dep)),
                parse_int(csv.get(ci_seq)),
            });
            ++total;
        }
        std::cout << "Loaded " << total << " stop times across " << result.size() << " trips.\n";
        return result;
    }

    void loadTransfers(
        const std::filesystem::path &dir,
        std::unordered_map<std::string, StopId, StringHash, std::equal_to<>> &stopStrIdx,
        std::unordered_map<StopId, std::unordered_map<StopId, uint32_t>> &transferTimes)
    {
        const auto path = dir / "transfers.txt";
        if (!std::filesystem::exists(path))
        {
            std::cerr << "No transfers.txt found; skipping transfer times.\n";
            return;
        }

        auto csv = CsvReader::open(path);
        // TODO: read more specific transfer types for routes/trips
        const int ci_from = csv.require_col("from_stop_id");
        const int ci_to   = csv.require_col("to_stop_id");
        const int ci_type = csv.require_col("transfer_type");
        const int ci_time = csv.col("min_transfer_time");

        std::size_t total = 0;
        while (csv.next())
        {
            auto it_from = stopStrIdx.find(csv.get(ci_from));
            if (it_from == stopStrIdx.end())
                continue;

            auto it_to = stopStrIdx.find(csv.get(ci_to));
            if (it_to == stopStrIdx.end())
                continue;

            auto type = parse_int(csv.get(ci_type));
            if (type == 3)
                continue; // transfer impossible, so we skip entirely

            auto time = ci_time >= 0 ? parse_int(csv.get(ci_time), -1) : -1;
            if (time < 0)
            {
                if (type == 1 || type == 4 || type == 5)
                    time = 0; // timed / same-vehicle transfer: no walk time
                else
                    continue; // TODO: calculate footpath time from coordinates
            }
            transferTimes[it_from->second][it_to->second] = static_cast<uint32_t>(time);
            ++total;
        }
        std::cout << "Loaded " << total << " transfer times.\n";
    }

    std::vector<CalendarEntry> loadCalendarEntries(const std::filesystem::path &dir,
                                                   std::unordered_map<std::string, ServiceId, StringHash, std::equal_to<>> &serviceStrIdx)
    {
        if (!std::filesystem::exists(dir / "calendar.txt"))
            return {};
        auto csv = CsvReader::open(dir / "calendar.txt");
        const int ci_service = csv.require_col("service_id");
        const int ci_mon = csv.require_col("monday");
        const int ci_tue = csv.require_col("tuesday");
        const int ci_wed = csv.require_col("wednesday");
        const int ci_thu = csv.require_col("thursday");
        const int ci_fri = csv.require_col("friday");
        const int ci_sat = csv.require_col("saturday");
        const int ci_sun = csv.require_col("sunday");
        const int ci_start = csv.require_col("start_date");
        const int ci_end = csv.require_col("end_date");

        std::vector<CalendarEntry> result;
        while (csv.next())
        {
            try
            {
                const auto strServiceId = std::string(csv.get(ci_service));
                if (serviceStrIdx.contains(strServiceId))
                    throw std::runtime_error("Duplicate service_id in calendar.txt: " + strServiceId);

                CalendarEntry entry;
                entry.serviceId = static_cast<ServiceId>(serviceStrIdx.size());
                serviceStrIdx[strServiceId] = entry.serviceId;

                entry.daysOfWeek[0] = parse_int(csv.get(ci_mon)) != 0;
                entry.daysOfWeek[1] = parse_int(csv.get(ci_tue)) != 0;
                entry.daysOfWeek[2] = parse_int(csv.get(ci_wed)) != 0;
                entry.daysOfWeek[3] = parse_int(csv.get(ci_thu)) != 0;
                entry.daysOfWeek[4] = parse_int(csv.get(ci_fri)) != 0;
                entry.daysOfWeek[5] = parse_int(csv.get(ci_sat)) != 0;
                entry.daysOfWeek[6] = parse_int(csv.get(ci_sun)) != 0;
                entry.startDate = parseGtfsDate(csv.get(ci_start));
                entry.endDate = parseGtfsDate(csv.get(ci_end));
                result.push_back(std::move(entry));
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error parsing calendar.txt row: " << e.what() << '\n';
            }
        }
        std::cout << "Loaded " << result.size() << " calendar entries.\n";
        return result;
    }

    std::vector<CalendarDateException> loadCalendarDateExceptions(
        const std::filesystem::path &dir,
        std::unordered_map<std::string, ServiceId, StringHash, std::equal_to<>> &serviceStrIdx)
    {
        if (!std::filesystem::exists(dir / "calendar_dates.txt"))
            return {};
        auto csv = CsvReader::open(dir / "calendar_dates.txt");
        const int ci_service = csv.require_col("service_id");
        const int ci_date = csv.require_col("date");
        const int ci_type = csv.require_col("exception_type");

        std::vector<CalendarDateException> result;
        while (csv.next())
        {
            try
            {
                CalendarDateException ex;
                const auto strServiceId = std::string(csv.get(ci_service));
                auto serviceIt = serviceStrIdx.find(strServiceId);
                if (serviceIt == serviceStrIdx.end())
                {
                    ex.serviceId = static_cast<ServiceId>(serviceStrIdx.size());
                    serviceStrIdx[strServiceId] = ex.serviceId;
                }
                else
                {
                    ex.serviceId = serviceIt->second;
                }
                ex.date = parseGtfsDate(csv.get(ci_date));
                ex.exceptionType = parse_int(csv.get(ci_type));
                result.push_back(ex);
            }
            catch (const std::exception &e)
            {
                std::cerr << "Error parsing calendar_dates.txt row: " << e.what() << '\n';
            }
        }
        std::cout << "Loaded " << result.size() << " calendar date exceptions.\n";
        return result;
    }

} // namespace

//  Feed::load

Feed Feed::load(const std::filesystem::path &dir, std::optional<BoundingBox> bbox)
{
    Feed feed;

    feed.agencies = loadAgencies(dir);
    feed.stops = loadStops(dir, feed.stopStrIdx, bbox);
    feed.routes = loadRoutes(dir, feed.routeStrIdx);
    feed.calendar = loadCalendarEntries(dir, feed.serviceStrIdx);
    feed.calendarDates = loadCalendarDateExceptions(dir, feed.serviceStrIdx);
    // loadStopTimes must run first: it assigns TripIds and populates tripStrIdx
    // for in-bbox trips only. loadTrips then reads those IDs.
    feed.tripStopTimes = loadStopTimes(dir, feed.tripStrIdx, feed.stopStrIdx);
    feed.trips = loadTrips(dir, feed.tripStrIdx, feed.routeStrIdx, feed.serviceStrIdx);
    loadTransfers(dir, feed.stopStrIdx, feed.transferTimes);

    if (feed.calendar.empty() && feed.calendarDates.empty())
        throw std::runtime_error("GTFS feed must contain calendar.txt or calendar_dates.txt");

    // Sort each trip's stop-times by stopSequence.
    for (auto &times : feed.tripStopTimes)
        if (!times.empty() && !std::ranges::is_sorted(times, {}, &StopTime::stopSequence))
            std::ranges::sort(times, {}, &StopTime::stopSequence);

    // Build stop name -> id index for root stops (no parent station).
    // These are the stops callers refer to by name; child platform stops are
    // resolved to their parent during connection building.
    feed.stopNameIdx.reserve(feed.stops.size());
    for (const auto &stop : feed.stops)
    {
        if (stop.parentStation == kNoParentStation)
            feed.stopNameIdx[stop.name].insert(stop.stationId());
    }
    
    feed.tripIdx.reserve(feed.trips.size());
    for (std::size_t i = 0; i < feed.trips.size(); ++i)
    {
        feed.tripIdx[feed.trips[i].id] = i;
    }


    return feed;
}

//  Accessors ─

const Stop *Feed::stopById(const StopId &id) const noexcept
{
    const auto idx = static_cast<uint32_t>(id);
    return idx < stops.size() ? &stops[idx] : nullptr;
}

const Route *Feed::routeById(const RouteId &id) const noexcept
{
    return id < routes.size() ? &routes[id] : nullptr;
}

const Trip *Feed::tripById(const TripId &id) const noexcept
{
    auto it = tripIdx.find(id);
    return it != tripIdx.end() ? &trips[it->second] : nullptr;
}

const std::string &Feed::stopName(StationId id) const noexcept
{
    const Stop *s = stopById(asStopId(id));
    static const std::string kEmpty;
    return s ? s->name : kEmpty;
}

const Stop *Feed::stopByName(const std::string &name) const noexcept
{
    auto it = stopNameIdx.find(name);
    if (it == stopNameIdx.end() || it->second.empty())
        return nullptr;
    return stopById(asStopId(*it->second.begin()));
}

std::span<const StopTime> Feed::stopTimesForTrip(const TripId &id) const noexcept
{
    if (id >= tripStopTimes.size())
        return {};
    return tripStopTimes[id];
}

const StopTime *Feed::stopTimeBySequence(const TripId &id, uint32_t seq) const noexcept
{
    const auto span = stopTimesForTrip(id);
    const auto it = std::lower_bound(span.begin(), span.end(), seq,
                                     [](const StopTime &st, uint32_t s)
                                     { return static_cast<uint32_t>(st.stopSequence) < s; });
    if (it == span.end() || static_cast<uint32_t>(it->stopSequence) != seq)
        return nullptr;
    return &*it;
}
