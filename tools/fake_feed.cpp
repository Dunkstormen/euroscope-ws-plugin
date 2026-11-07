// Runs the plugin's WebSocket server with simulated aircraft, so clients can be
// developed and tested without EuroScope. Usage: fake_feed [port] [count] [radarSeconds]
// Like EuroScope, it broadcasts every second but positions only change every radarSeconds (default 5).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "Snapshot.h"
#include "WsServer.h"

static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8765;
    int count = argc > 2 ? std::atoi(argv[2]) : 8;
    int radarSec = argc > 3 ? std::max(1, std::atoi(argv[3])) : 5;
    std::signal(SIGINT, [](int) { g_stop = 1; });

    WsServer server;
    server.SetMessageHandler([](const std::string& m) { std::printf("client says: %s\n", m.c_str()); });
    std::string err;
    if (!server.Start("127.0.0.1", uint16_t(port), &err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("fake feed on ws://127.0.0.1:%d with %d aircraft (Ctrl+C to stop)\n", port, count);

    const char* types[] = {"A320", "B738", "A21N", "B77W", "DH8D", "E190"};
    const char* dests[] = {"EKCH", "ESSA", "ENGM", "EDDF", "EGLL", "EHAM"};
    std::vector<AircraftState> fleet(count);
    for (int i = 0; i < count; ++i) {
        AircraftState& a = fleet[i];
        char cs[16]; std::snprintf(cs, sizeof(cs), "SAS%03d", 100 + i * 7);
        a.callsign = cs;
        a.latitude = 55.6 + std::sin(i) * 0.8;
        a.longitude = 12.6 + std::cos(i) * 1.2;
        a.altitude = a.flightLevel = 5000 + i * 3000;
        a.groundSpeed = 250 + i * 20;
        a.heading = (i * 45) % 360;
        char sq[8]; std::snprintf(sq, sizeof(sq), "%04o", 01000 + i);
        a.squawk = sq;
        a.modeC = true;
        a.fp.valid = i % 4 != 3;  // some targets have no flight plan
        a.fp.origin = "EKCH";
        a.fp.destination = dests[i % 6];
        a.fp.aircraftType = types[i % 6];
        a.fp.wakeCategory = "M";
        a.fp.flightRules = "I";
        a.fp.route = "SIMEG DCT \"QUOTED\" TEST";
        a.fp.requestedAltitude = 36000;
        a.fp.clearedAltitude = a.altitude + 2000;
        a.fp.assignedSquawk = sq;
        a.fp.trackingController = i % 2 ? "EKCH_APP" : "";
    }

    auto last = std::chrono::steady_clock::now();
    for (int tick = 1; !g_stop; ++tick) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        if (tick % radarSec == 0) {
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            for (AircraftState& a : fleet) {
                double nm = a.groundSpeed * dt / 3600.0;
                double hdg = a.heading * 3.14159265358979 / 180.0;
                a.latitude += nm * std::cos(hdg) / 60.0;
                a.longitude += nm * std::sin(hdg) / (60.0 * std::cos(a.latitude * 3.14159265358979 / 180.0));
                a.heading = (a.heading + 3 * radarSec) % 360;
            }
        }
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
        server.Broadcast(EncodeSnapshot(fleet, ms, "FAKE_FEED"));
        std::printf("\rclients: %zu   ", server.ClientCount());
        std::fflush(stdout);
    }
    server.Stop();
    std::printf("\nstopped\n");
    return 0;
}
