// Plain data captured from EuroScope on its own thread, plus JSON encoding.
// Deliberately free of any EuroScope types so it can be tested anywhere.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct FlightPlanInfo {
    bool valid = false;
    std::string origin, destination, alternate;
    std::string aircraftType;     // e.g. "B738"
    std::string wakeCategory;     // "L", "M", "H", "J"
    std::string flightRules;      // "I" or "V"
    std::string route;
    int requestedAltitude = 0;    // ft, final/requested
    int clearedAltitude = 0;      // ft, controller-assigned (0 = none)
    std::string assignedSquawk;
    int assignedHeading = 0;      // 0 = none
    int assignedSpeed = 0;        // kt, 0 = none
    std::string trackingController;
    std::string groundState;      // e.g. "PUSH", "TAXI", "DEPA" (if set)
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
inline void Num(std::string& out, const char* k, double v) {
    char b[32]; snprintf(b, sizeof(b), "%.6f", v);
    Key(out, k); out += b; out += ',';
}
inline void CloseObject(std::string& out) {
    if (!out.empty() && out.back() == ',') out.back() = '}'; else out += '}';
}

}  // namespace json

// {"type":"aircraft","time":...,"source":...,"count":N,"aircraft":[...]}
inline std::string EncodeSnapshot(const std::vector<AircraftState>& list, long long unixMillis, const std::string& source) {
    std::string out;
    out.reserve(256 + list.size() * 512);
    out += '{';
    json::Str(out, "type", "aircraft");
    json::Int(out, "time", unixMillis);
    json::Str(out, "source", source);
    json::Int(out, "count", (long long)list.size());
    json::Key(out, "aircraft");
    out += '[';
    for (size_t i = 0; i < list.size(); ++i) {
        const AircraftState& a = list[i];
        if (i) out += ',';
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
        if (!a.fp.valid) {
            out += "null,";
        } else {
            const FlightPlanInfo& f = a.fp;
            out += '{';
            json::Str(out, "origin", f.origin);
            json::Str(out, "destination", f.destination);
            json::Str(out, "alternate", f.alternate);
            json::Str(out, "aircraftType", f.aircraftType);
            json::Str(out, "wakeCategory", f.wakeCategory);
            json::Str(out, "flightRules", f.flightRules);
            json::Str(out, "route", f.route);
            json::Int(out, "requestedAltitude", f.requestedAltitude);
            json::Int(out, "clearedAltitude", f.clearedAltitude);
            json::Str(out, "assignedSquawk", f.assignedSquawk);
            json::Int(out, "assignedHeading", f.assignedHeading);
            json::Int(out, "assignedSpeed", f.assignedSpeed);
            json::Str(out, "trackingController", f.trackingController);
            json::Str(out, "groundState", f.groundState);
            json::CloseObject(out);
            out += ',';
        }
        json::CloseObject(out);
    }
    out += "]}";
    return out;
}
