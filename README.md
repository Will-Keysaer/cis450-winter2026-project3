# cis450-winter2026-project3

Traffic Intersection Simulation

This project simulates a four-way stop-sign intersection using threads in C. Each car is represented by a thread that arrives, waits, crosses the intersection, and exits

Features
- Multi-threaded simulation using pthread
- Synchronization using mutexes and semaphores
- Head-of-line queuing for each traffic lane
- Intersection divided into four zones (NW, NE, SW, SE)
- Safe concurrent crossing when paths do not conflict
- Arrival-based priority handling (stop-sign behavior)

How It Works

- Each car thread goes through three steps:

- Arrive – waits at the stop sign and joins its lane queue
- Cross – acquires required intersection zones and crosses
- Exit – releases resources and leaves the intersection

Build & run on Ubuntu Linux using the following commands:

gcc tc.c -o tc -lpthread

./tc
