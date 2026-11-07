// Runs the plugin's WebSocket server and protocol with simulated data, so clients can be
// developed and tested without EuroScope. Usage: fake_feed [port] [count] [radarSeconds]
// Like EuroScope, it publishes every second but positions only change every radarSeconds
// (default 5). Commands are always enabled (no token) and change the simulated aircraft.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "Protocol.h"
#include "Snapshot.h"
#include "WsServer.h"

static volatile std::sig_atomic_t g_stop = 0;

namespace {

const char* kMe = "EKCH_APP";
const double kPi = 3.14159265358979;

long long NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

AircraftState* Find(std::vector<AircraftState>& fleet, const std::string& cs) {
    for (AircraftState& a : fleet)
        if (a.callsign == cs) return &a;
    return nullptr;
}

void SetTracking(FlightPlanInfo& f, const std::string& controller) {
    f.trackingController = controller;
    f.trackedByMe = controller == kMe;
    f.state = controller.empty() ? "notConcerned" : (f.trackedByMe ? "assumed" : "notConcerned");
}

RouteInfo MakeRoute(const AircraftState& a) {
    RouteInfo r;
    r.callsign = a.callsign;
    double hdg = a.heading * kPi / 180.0;
    for (int i = 0; i < 5; ++i) {
        RoutePoint p;
        char name[8]; std::snprintf(name, sizeof(name), "FIX%02d", i);
        p.name = name;
        p.airway = i ? "DCT" : "";
        double nm = (i - 1) * 20.0;
        p.latitude = a.latitude + nm * std::cos(hdg) / 60.0;
        p.longitude = a.longitude + nm * std::sin(hdg) / (60.0 * std::cos(a.latitude * kPi / 180.0));
        p.minutes = a.groundSpeed ? int(nm * 60 / a.groundSpeed) : -1;
        p.altitude = a.altitude;
        p.passed = i == 0;
        r.points.push_back(p);
    }
    for (int m = 1; m <= 5; ++m) {
        PredictedPosition p;
        double nm = a.groundSpeed * m / 60.0;
        p.latitude = a.latitude + nm * std::cos(hdg) / 60.0;
        p.longitude = a.longitude + nm * std::sin(hdg) / (60.0 * std::cos(a.latitude * kPi / 180.0));
        p.altitude = a.altitude;
        r.predictions.push_back(p);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 8765;
    int count = argc > 2 ? std::atoi(argv[2]) : 8;
    int radarSec = argc > 3 ? std::max(1, std::atoi(argv[3])) : 5;
    std::signal(SIGINT, [](int) { g_stop = 1; });

    WsServer server;
    ClientHub hub(server);
    std::string err;
    if (!server.Start("127.0.0.1", uint16_t(port), &err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("fake feed on ws://127.0.0.1:%d with %d aircraft (Ctrl+C to stop)\n", port, count);

    server.Publish(topic::kHello,
                   EncodeHello("WebSocket Data (fake feed)", "dev", kMe, AllTopics(), DefaultTopics(), true, false,
                               NowMillis()),
                   true);

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
        FlightPlanInfo& f = a.fp;
        f.valid = i % 4 != 3;  // some targets have no flight plan
        f.origin = "EKCH";
        f.destination = dests[i % 6];
        f.aircraftType = types[i % 6];
        f.aircraftInfo = std::string(types[i % 6]) + "/M-SDE2E3FGHIRWY/LB1";
        f.wakeCategory = "M";
        f.engineType = "J";
        f.engineCount = 2;
        f.capabilities = "L";
        f.flightRules = "I";
        f.route = "SIMEG DCT \"QUOTED\" TEST";
        f.remarks = "PBN/A1B1C1D1 /V/";
        f.sid = "SIMEG1A";
        f.departureRunway = "22R";
        f.trueAirspeed = 450;
        f.estimatedDeparture = "1200";
        f.enrouteTime = "1:15";
        f.fuelTime = "3:30";
        f.pilotName = "Fake Pilot " + std::to_string(i);
        f.requestedAltitude = 36000;
        f.clearedAltitude = a.altitude + 2000;
        f.assignedSquawk = sq;
        f.communicationType = "V";
        f.annotations.assign(9, std::string());
        SetTracking(f, i % 2 ? kMe : "");
        f.sectorExitMinutes = 10 + i;
        f.exitPoint = "SIMEG";
        f.distanceToDestination = 300 + i * 10;
        f.distanceFromOrigin = 20 + i * 5;
    }

    std::vector<ControllerInfo> controllers;
    {
        ControllerInfo c;
        c.callsign = "EKCH_TWR"; c.name = "Fake Tower"; c.frequency = 118.105; c.facility = "TWR"; c.rating = "S2";
        c.positionId = "CT"; c.sectorFile = "EKCH_TWR"; c.latitude = 55.618; c.longitude = 12.656; c.range = 50;
        c.isController = true;
        controllers.push_back(c);
        c.callsign = "EKDK_CTR"; c.name = "Fake Control"; c.frequency = 136.485; c.facility = "CTR"; c.rating = "C1";
        c.positionId = "KC"; c.sectorFile = "EKDK_CTR"; c.latitude = 55.8; c.longitude = 10.5; c.range = 300;
        controllers.push_back(c);
    }
    MeInfo me;
    me.controller.callsign = kMe; me.controller.name = "Fake Feed"; me.controller.frequency = 119.805;
    me.controller.facility = "APP"; me.controller.rating = "C1"; me.controller.positionId = "CA";
    me.controller.sectorFile = kMe; me.controller.latitude = 55.618; me.controller.longitude = 12.656;
    me.controller.range = 150; me.controller.isController = true; me.controller.isMe = true;
    me.connectionType = "playback";

    std::vector<AirportInfo> airports(1);
    airports[0].icao = "EKCH";
    airports[0].metar = "EKCH 071820Z 24012KT 9999 FEW030 08/04 Q1012 NOSIG";
    airports[0].departureRunways = {"22R"};
    airports[0].arrivalRunways = {"22L"};
    server.Publish(topic::kAirports, EncodeAirports(airports, NowMillis()), true);

    auto event = [&](const char* name, const std::function<void(std::string&)>& fields) {
        server.Publish(topic::kEvents, EncodeEvent(name, NowMillis(), fields), false);
    };
    auto changed = [&](AircraftState& a, const char* field) {
        event("assignedDataChanged", [&](std::string& out) {
            json::Str(out, "callsign", a.callsign);
            json::Str(out, "field", field);
            json::Key(out, "flightPlan");
            EncodeFlightPlan(out, a.fp);
        });
    };

    // Simulated command handling: enough to exercise clients, no EuroScope rules.
    auto exec = [&](const Command& c) -> std::string {
        AircraftState* a = Find(fleet, c.callsign);
        if (!a || !a->fp.valid) return "no flight plan for " + c.callsign;
        FlightPlanInfo& f = a->fp;
        double v = 0;
        if (c.name == "select") { me.asel = a->callsign; return ""; }
        if (c.name == "assume" || c.name == "acceptHandoff") {
            std::string from = f.trackingController;
            SetTracking(f, kMe);
            f.handoffTarget.clear();
            event("trackingChanged", [&](std::string& out) {
                json::Str(out, "callsign", a->callsign); json::Str(out, "from", from); json::Str(out, "to", kMe);
            });
            return "";
        }
        if (c.name == "drop") {
            if (!f.trackedByMe) return "not tracked by me";
            SetTracking(f, "");
            event("trackingChanged", [&](std::string& out) {
                json::Str(out, "callsign", a->callsign); json::Str(out, "from", kMe); json::Str(out, "to", "");
            });
            return "";
        }
        if (c.name == "handoff") {
            f.handoffTarget = c.Text("to");
            if (f.handoffTarget.empty()) return "\"to\" (controller callsign) required";
            event("handoffInitiated", [&](std::string& out) {
                json::Str(out, "callsign", a->callsign); json::Str(out, "from", f.trackingController);
                json::Str(out, "to", f.handoffTarget);
            });
            return "";
        }
        if (c.name == "setClearedAltitude") {
            if (!c.Number(&v) || v < 0 || v > 99999) return "value out of range";
            f.clearedAltitude = int(v); changed(*a, "clearedAltitude"); return "";
        }
        if (c.name == "setHeading") {
            if (!c.Number(&v) || v < 0 || v > 360) return "value out of range";
            f.assignedHeading = int(v); changed(*a, "heading"); return "";
        }
        if (c.name == "setSpeed") {
            if (!c.Number(&v) || v < 0 || v > 999) return "value out of range";
            f.assignedSpeed = int(v); changed(*a, "speed"); return "";
        }
        if (c.name == "setSquawk") { f.assignedSquawk = c.Text(); changed(*a, "squawk"); return ""; }
        if (c.name == "setScratchpad") { f.scratchpad = c.Text(); changed(*a, "scratchpad"); return ""; }
        if (c.name == "setDirectTo") { f.directTo = c.Text(); changed(*a, "directTo"); return ""; }
        return "unknown command: " + c.name;
    };

    auto last = std::chrono::steady_clock::now();
    for (int tick = 1; !g_stop; ++tick) {
        // Handle client messages every 100 ms so command replies are quick.
        for (int i = 0; i < 10 && !g_stop; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            hub.Process(true, "", exec);
        }
        auto now = std::chrono::steady_clock::now();
        if (tick % radarSec == 0) {
            double dt = std::chrono::duration<double>(now - last).count();
            last = now;
            for (AircraftState& a : fleet) {
                double nm = a.groundSpeed * dt / 3600.0;
                double hdg = a.heading * kPi / 180.0;
                a.latitude += nm * std::cos(hdg) / 60.0;
                a.longitude += nm * std::sin(hdg) / (60.0 * std::cos(a.latitude * kPi / 180.0));
                a.heading = (a.heading + 3 * radarSec) % 360;
            }
        }
        long long ms = NowMillis();
        hub.PublishAircraft(fleet, ms, kMe);
        server.Publish(topic::kControllers, EncodeControllers(me, controllers, ms), true);
        if (tick % 5 == 1 && hub.WantsRoutes()) {
            std::vector<RouteInfo> routes;
            for (const AircraftState& a : fleet)
                if (a.fp.valid) routes.push_back(MakeRoute(a));
            hub.PublishRoutes(routes, fleet, ms);
        }
        std::printf("\rclients: %zu   ", server.ClientCount());
        std::fflush(stdout);
    }
    server.Stop();
    std::printf("\nstopped\n");
    return 0;
}
