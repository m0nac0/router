# router
A simple public transit routing engine written in C++.
It reads public transit schedules in GTFS format and computes optimal routes between stops, using the RAPTOR or Connection Scan (CSA) algorithms.

A focus is on efficiently reading large GTFS datasets, especially the fact that reading large datasets (e.g. nationwide) and filtering them to a specific region does not consume significantly more memory than reading a smaller dataset for that region only.



main.cpp contains a simple CLI for testing the feed loading and routing, with a hardcoded bounding box of the Munich metropolitan region.

Example output:
```cli
Enter origin stop ('reload' to refresh realtime, empty to quit): Marienplatz
Enter destination stop (or empty to quit): Giesing
Finding route from Marienplatz to Giesing departing at current time.

Marienplatz → Giesing  (13 min)

  [U6] Klinikum Großhadern
  ● Marienplatz      18:52 (+2m)
  ● Sendlinger Tor   18:53 (+2m)

  [U1] Mangfallplatz
  ● Sendlinger Tor      18:56 (+1m30s)
  │ Fraunhoferstraße    18:57 (+1m30s)
  ● Kolumbusplatz       18:59 (+1m30s)

  [U2] Messestadt Ost
  ● Kolumbusplatz       19:02
  │ Silberhornstraße    19:03
  │ Untersbergstraße    19:04
  ● Giesing             19:05
```

Datasets should be unzipped and placed into a subfolder of the `data` directory.
GTFS realtime data is also supported. Just place the realtime file named `realtime.pb` into the feed data directory and it will be loaded automatically and can be reloaded interactively.
Datasets for testing that cover Germany and support realtime data can be obtained for example from gtfs.de or DELFI.

For a real-world deployment I would recommend using a more mature routing engine, such as MOTIS or OpenTripPlanner.