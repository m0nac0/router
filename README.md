# router
A simple public transit routing engine written in C++.
It reads public transit schedules in GTFS format and computes optimal routes between stops, using the Connection Scan Algorithm (CSA).

A focus is on efficiently reading large GTFS datasets, especially the fact that reading large datasets (e.g. nationwide) and filtering them to a specific region does not consume significantly more memory than reading a smaller dataset for that region only.
GTFS realtime data is also supported.


main.cpp contains a simple CLI for testing the feed loading and routing, with a hardcoded bounding box of the Munich metropolitan region.
Datasets should be placed in the data directory. Datasets covering Germany for testing can be obtained for example from gtfs.de or DELFI.

For a real-world deployment I would recommend using a more mature routing engine, such as MOTIS or OpenTripPlanner.