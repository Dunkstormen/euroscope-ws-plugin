// Plain data captured from EuroScope on its own thread, plus JSON encoding.
// Deliberately free of any EuroScope types so it can be tested anywhere.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Bumped when a message changes in a way clients may need to know about.
const int kProtocolVersion = 2;

struct FlightPlanInfo {
    bool valid = false;
    // Filed flight plan
    std::string origin, destination, alternate;
    std::string aircraftType;     // e.g. "B738"
    std::string aircraftInfo;     // full ICAO type/equipment string, e.g. "B738/M-SDE2E3FGHIRWY/LB1"
    std::string wakeCategory;     // "L", "M", "H", "J"
    std::string engineType;       // "J" jet, "T" turboprop, "P" piston, ...
    int engineCount = 0;
    std::string capabilities;     // equipment suffix letter, e.g. "L"
    std::string flightRules;      // "I" or "V"
    std::string route;
    std::string remarks;
    std::string sid, star, departureRunway, arrivalRunway;
    int trueAirspeed = 0;         // kt, filed
    std::string estimatedDeparture, actualDeparture;  // "HHMM" as filed
    std::string enrouteTime, fuelTime;                // "H:MM"
    std::string pilotName;
    int requestedAltitude = 0;    // ft, final/requested (controller override if set)
    // Controller assigned
    int clearedAltitude = 0;      // ft, controller-assigned (0 = none)
    std::string assignedSquawk;
    int assignedHeading = 0;      // 0 = none
    int assignedSpeed = 0;        // kt, 0 = none
    int assignedMach = 0;         // Mach x 100, 0 = none
    int assignedRate = 0;         // ft/min, 0 = none
    std::string directTo;
    std::string scratchpad;
    std::string communicationType;  // "V" voice, "R" receive only, "T" text, "" unknown
    std::vector<std::string> annotations;  // flight strip annotations 0-8
    bool clearance = false;       // clearance received flag
    std::string groundState;      // e.g. "PUSH", "TAXI", "DEPA" (if set)
    // Coordination
    std::string trackingController;
    bool trackedByMe = false;
    std::string handoffTarget;    // controller a handoff is offered to
    std::string nextController;   // coordinated next controller
    std::string state;            // "notConcerned", "notified", "coordinated", "transferToMe", "transferFromMe", "assumed", "redundant"
    int sectorEntryMinutes = -1, sectorExitMinutes = -1;  // -1 = not applicable
    std::string entryPoint, exitPoint;
    double distanceToDestination = 0, distanceFromOrigin = 0;  // nm
};

struct AircraftState {
    std::string callsign;
    double latitude = 0, longitude = 0;
    int altitude = 0;             // ft, pressure altitude
    int flightLevel = 0;          // ft, standard-pressure altitude
    int groundSpeed = 0;          // kt
    int heading = 0;              // deg, reported (magnetic)
    int verticalSpeed = 0;        // ft/min
    std::string squawk;
    bool modeC = false;
    bool ident = false;
    FlightPlanInfo fp;
};

struct ControllerInfo {
    std::string callsign, name;
    double frequency = 0;         // MHz, 199.998 = none
    std::string facility;         // "OBS", "FSS", "DEL", "GND", "TWR", "APP", "CTR"
    std::string rating;           // "S1".."ADM"
    std::string positionId;       // ESE position id, e.g. "CH" (empty if not identified)
    std::string sectorFile;       // sector file name, e.g. "EKCH_APP"
    double latitude = 0, longitude = 0;
    int range = 0;                // nm, visibility range
    bool isController = false;    // false for observers
    bool isMe = false;
};

struct MeInfo {
    ControllerInfo controller;
    std::string connectionType;   // "offline", "direct", "proxy", "simulatorServer", "playback", "simulatorClient", "sweatbox"
    std::string asel;             // callsign of the selected aircraft, empty if none
};

struct AirportInfo {
    std::string icao;
    std::string metar;
    std::vector<std::string> departureRunways, arrivalRunways;
};

struct RoutePoint {
    std::string name, airway;
    double latitude = 0, longitude = 0;
    int minutes = 0;              // minutes from now, -1 if unknown
    int altitude = 0;             // ft, calculated profile
    bool passed = false;
};

struct PredictedPosition {
    double latitude = 0, longitude = 0;
    int altitude = 0;
};

struct RouteInfo {
    std::string callsign;
    std::vector<RoutePoint> points;
    std::vector<PredictedPosition> predictions;  // one per minute ahead
};

namespace json {

inline void AppendString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else if (c >= 0x80) out += '?';  // EuroScope strings are ANSI; keep output valid UTF-8
                else out += char(c);
        }
    }
    out += '"';
}

inline void Key(std::string& out, const char* k) { out += '"'; out += k; out += "\":"; }
inline void Str(std::string& out, const char* k, const std::string& v) { Key(out, k); AppendString(out, v); out += ','; }
inline void Int(std::string& out, const char* k, long long v) { Key(out, k); out += std::to_string(v); out += ','; }
inline void Bool(std::string& out, const char* k, bool v) { Key(out, k); out += v ? "true," : "false,"; }
inline void Num(std::string& out, const char* k, double v, int decimals = 6) {
    char b[48]; snprintf(b, sizeof(b), "%.*f", decimals, v);
    Key(out, k); out += b; out += ',';
}
inline void StrArray(std::string& out, const char* k, const std::vector<std::string>& v) {
    Key(out, k);
    out += '[';
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += ','; AppendString(out, v[i]); }
    out += "],";
}
inline void CloseObject(std::string& out) {
    if (!out.empty() && out.back() == ',') out.back() = '}'; else out += '}';
}
// Starts a top-level message: {"type":...,"time":...,
inline std::string Begin(const char* type, long long unixMillis) {
    std::string out = "{";
    Str(out, "type", type);
    Int(out, "time", unixMillis);
    return out;
}

}  // namespace json

// Appends the flightPlan value (object or null) followed by a comma.
inline void EncodeFlightPlan(std::string& out, const FlightPlanInfo& f) {
    if (!f.valid) { out += "null,"; return; }
    out += '{';
    json::Str(out, "origin", f.origin);
    json::Str(out, "destination", f.destination);
    json::Str(out, "alternate", f.alternate);
    json::Str(out, "aircraftType", f.aircraftType);
    json::Str(out, "aircraftInfo", f.aircraftInfo);
    json::Str(out, "wakeCategory", f.wakeCategory);
    json::Str(out, "engineType", f.engineType);
    json::Int(out, "engineCount", f.engineCount);
    json::Str(out, "capabilities", f.capabilities);
    json::Str(out, "flightRules", f.flightRules);
    json::Str(out, "route", f.route);
    json::Str(out, "remarks", f.remarks);
    json::Str(out, "sid", f.sid);
    json::Str(out, "star", f.star);
    json::Str(out, "departureRunway", f.departureRunway);
    json::Str(out, "arrivalRunway", f.arrivalRunway);
    json::Int(out, "trueAirspeed", f.trueAirspeed);
    json::Str(out, "estimatedDeparture", f.estimatedDeparture);
    json::Str(out, "actualDeparture", f.actualDeparture);
    json::Str(out, "enrouteTime", f.enrouteTime);
    json::Str(out, "fuelTime", f.fuelTime);
    json::Str(out, "pilotName", f.pilotName);
    json::Int(out, "requestedAltitude", f.requestedAltitude);
    json::Int(out, "clearedAltitude", f.clearedAltitude);
    json::Str(out, "assignedSquawk", f.assignedSquawk);
    json::Int(out, "assignedHeading", f.assignedHeading);
    json::Int(out, "assignedSpeed", f.assignedSpeed);
    json::Num(out, "assignedMach", f.assignedMach / 100.0, 2);
    json::Int(out, "assignedRate", f.assignedRate);
    json::Str(out, "directTo", f.directTo);
    json::Str(out, "scratchpad", f.scratchpad);
    json::Str(out, "communicationType", f.communicationType);
    json::StrArray(out, "annotations", f.annotations);
    json::Bool(out, "clearance", f.clearance);
    json::Str(out, "groundState", f.groundState);
    json::Str(out, "trackingController", f.trackingController);
    json::Bool(out, "trackedByMe", f.trackedByMe);
    json::Str(out, "handoffTarget", f.handoffTarget);
    json::Str(out, "nextController", f.nextController);
    json::Str(out, "state", f.state);
    json::Int(out, "sectorEntryMinutes", f.sectorEntryMinutes);
    json::Int(out, "sectorExitMinutes", f.sectorExitMinutes);
    json::Str(out, "entryPoint", f.entryPoint);
    json::Str(out, "exitPoint", f.exitPoint);
    json::Num(out, "distanceToDestination", f.distanceToDestination, 1);
    json::Num(out, "distanceFromOrigin", f.distanceFromOrigin, 1);
    json::CloseObject(out);
    out += ',';
}

inline void EncodeAircraft(std::string& out, const AircraftState& a) {
    out += '{';
    json::Str(out, "callsign", a.callsign);
    json::Num(out, "lat", a.latitude);
    json::Num(out, "lon", a.longitude);
    json::Int(out, "altitude", a.altitude);
    json::Int(out, "flightLevel", a.flightLevel);
    json::Int(out, "groundSpeed", a.groundSpeed);
    json::Int(out, "heading", a.heading);
    json::Int(out, "verticalSpeed", a.verticalSpeed);
    json::Str(out, "squawk", a.squawk);
    json::Bool(out, "modeC", a.modeC);
    json::Bool(out, "ident", a.ident);
    json::Key(out, "flightPlan");
    EncodeFlightPlan(out, a.fp);
    json::CloseObject(out);
}

// {"type":"aircraft","time":...,"source":...,"count":N,"aircraft":[...]}
inline std::string EncodeSnapshot(const std::vector<AircraftState>& list, long long unixMillis, const std::string& source) {
    std::string out = json::Begin("aircraft", unixMillis);
    out.reserve(256 + list.size() * 1024);
    json::Str(out, "source", source);
    json::Int(out, "count", (long long)list.size());
    json::Key(out, "aircraft");
    out += '[';
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ',';
        EncodeAircraft(out, list[i]);
    }
    out += "]}";
    return out;
}

inline void EncodeController(std::string& out, const ControllerInfo& c) {
    out += '{';
    json::Str(out, "callsign", c.callsign);
    json::Str(out, "name", c.name);
    json::Num(out, "frequency", c.frequency, 3);
    json::Str(out, "facility", c.facility);
    json::Str(out, "rating", c.rating);
    json::Str(out, "positionId", c.positionId);
    json::Str(out, "sectorFile", c.sectorFile);
    json::Num(out, "lat", c.latitude);
    json::Num(out, "lon", c.longitude);
    json::Int(out, "range", c.range);
    json::Bool(out, "isController", c.isController);
    json::Bool(out, "isMe", c.isMe);
    json::CloseObject(out);
}

// {"type":"controllers","time":...,"me":{...},"count":N,"controllers":[...]}
inline std::string EncodeControllers(const MeInfo& me, const std::vector<ControllerInfo>& list, long long unixMillis) {
    std::string out = json::Begin("controllers", unixMillis);
    json::Key(out, "me");
    out += '{';
    json::Str(out, "callsign", me.controller.callsign);
    json::Str(out, "connectionType", me.connectionType);
    json::Str(out, "asel", me.asel);
    json::Key(out, "controller");
    EncodeController(out, me.controller);
    json::CloseObject(out);
    out += ',';
    json::Int(out, "count", (long long)list.size());
    json::Key(out, "controllers");
    out += '[';
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ',';
        EncodeController(out, list[i]);
    }
    out += "]}";
    return out;
}

// {"type":"airports","time":...,"airports":[{"icao":...,"metar":...,"departureRunways":[...],"arrivalRunways":[...]}]}
inline std::string EncodeAirports(const std::vector<AirportInfo>& list, long long unixMillis) {
    std::string out = json::Begin("airports", unixMillis);
    json::Int(out, "count", (long long)list.size());
    json::Key(out, "airports");
    out += '[';
    for (size_t i = 0; i < list.size(); ++i) {
        const AirportInfo& a = list[i];
        if (i) out += ',';
        out += '{';
        json::Str(out, "icao", a.icao);
        json::Str(out, "metar", a.metar);
        json::StrArray(out, "departureRunways", a.departureRunways);
        json::StrArray(out, "arrivalRunways", a.arrivalRunways);
        json::CloseObject(out);
    }
    out += "]}";
    return out;
}

// {"type":"routes","time":...,"routes":[{"callsign":...,"points":[...],"predictions":[...]}]}
inline std::string EncodeRoutes(const std::vector<RouteInfo>& list, long long unixMillis) {
    std::string out = json::Begin("routes", unixMillis);
    json::Int(out, "count", (long long)list.size());
    json::Key(out, "routes");
    out += '[';
    for (size_t i = 0; i < list.size(); ++i) {
        const RouteInfo& r = list[i];
        if (i) out += ',';
        out += '{';
        json::Str(out, "callsign", r.callsign);
        json::Key(out, "points");
        out += '[';
        for (size_t j = 0; j < r.points.size(); ++j) {
            const RoutePoint& p = r.points[j];
            if (j) out += ',';
            out += '{';
            json::Str(out, "name", p.name);
            json::Str(out, "airway", p.airway);
            json::Num(out, "lat", p.latitude);
            json::Num(out, "lon", p.longitude);
            json::Int(out, "minutes", p.minutes);
            json::Int(out, "altitude", p.altitude);
            json::Bool(out, "passed", p.passed);
            json::CloseObject(out);
        }
        out += "],";
        json::Key(out, "predictions");
        out += '[';
        for (size_t j = 0; j < r.predictions.size(); ++j) {
            const PredictedPosition& p = r.predictions[j];
            if (j) out += ',';
            out += '{';
            json::Num(out, "lat", p.latitude);
            json::Num(out, "lon", p.longitude);
            json::Int(out, "altitude", p.altitude);
            json::CloseObject(out);
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

// {"type":"hello","time":...,"protocol":2,"plugin":...,"version":...,"source":...,"topics":[...],
//  "defaultTopics":[...],"commands":bool,"tokenRequired":bool}
inline std::string EncodeHello(const std::string& plugin, const std::string& version, const std::string& source,
                               const std::vector<std::string>& topics, const std::vector<std::string>& defaultTopics,
                               bool commandsEnabled, bool tokenRequired, long long unixMillis) {
    std::string out = json::Begin("hello", unixMillis);
    json::Int(out, "protocol", kProtocolVersion);
    json::Str(out, "plugin", plugin);
    json::Str(out, "version", version);
    json::Str(out, "source", source);
    json::StrArray(out, "topics", topics);
    json::StrArray(out, "defaultTopics", defaultTopics);
    json::Bool(out, "commands", commandsEnabled);
    json::Bool(out, "tokenRequired", tokenRequired);
    json::CloseObject(out);
    return out;
}
