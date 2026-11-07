// EuroScope plugin: streams EuroScope data to WebSocket clients as JSON and, if enabled,
// accepts commands from them.
//
// Threading model:
//   EuroScope thread  -> OnTimer() and the On... callbacks read the SDK, build JSON, call
//                        server.Publish()/SendTo(); ClientHub::Process() runs client commands
//   Server thread     -> owns all sockets, sends queued JSON, queues what clients send
// The SDK is never touched from the server thread.

// EuroScopePlugIn.h uses Windows types (POINT, RECT, HDC, COLORREF) without including
// <windows.h> itself, so it must come first. winsock2.h goes before windows.h to avoid
// clashing with the old winsock.h.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "EuroScopePlugIn.h"
#include "Protocol.h"
#include "Snapshot.h"
#include "WsServer.h"

namespace ES = EuroScopePlugIn;

namespace {

const char* kName = "WebSocket Data";
#ifndef WSDATA_VERSION
#define WSDATA_VERSION "0.1.0"
#endif
const char* kVersion = WSDATA_VERSION;
const char* kAuthor = "Benjamin";
const char* kCommand = ".wsdata";

const char* kDefaultBind = "127.0.0.1";
const int kDefaultPort = 8765;
const int kDefaultInterval = 1;     // seconds between pushes
const int kRoutesInterval = 5;      // routes are big; push at most this often
const int kRunwayCheckInterval = 30;  // seconds between active runway re-checks

std::string Safe(const char* s) { return s ? s : ""; }

// SDK getters return a mix of const char*, char and int for text-like fields; these
// overloads turn any of them into a string so the capture code doesn't care which.
std::string Text(const char* s) { return Safe(s); }
std::string Text(char c) { return c > ' ' ? std::string(1, c) : std::string(); }
std::string Text(int v) { return v ? std::to_string(v) : std::string(); }

// "H:MM" from separate hour and minute fields, "" if neither is set.
template <class H, class M>
std::string HoursMinutes(H hours, M minutes) {
    std::string h = Text(hours), m = Text(minutes);
    if (h.empty() && m.empty()) return std::string();
    if (m.size() < 2) m.insert(0, 2 - m.size(), '0');
    return (h.empty() ? "0" : h) + ":" + m;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// Calls an SDK action and treats a void return as success, so it works whichever way the
// header declares it.
template <class F>
bool Call(F f) {
    if constexpr (std::is_void_v<decltype(f())>) { f(); return true; }
    else return bool(f());
}

long long NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* FacilityName(int f) {
    static const char* names[] = {"OBS", "FSS", "DEL", "GND", "TWR", "APP", "CTR"};
    return f >= 0 && f < 7 ? names[f] : "";
}

std::string RatingName(int r) {
    static const char* names[] = {"", "OBS", "S1", "S2", "S3", "C1", "C2", "C3", "I1", "I2", "I3", "SUP", "ADM"};
    return r >= 1 && r <= 12 ? names[r] : (r ? std::to_string(r) : std::string());
}

const char* FlightPlanStateName(int s) {
    switch (s) {
        case ES::FLIGHT_PLAN_STATE_NON_CONCERNED: return "notConcerned";
        case ES::FLIGHT_PLAN_STATE_NOTIFIED: return "notified";
        case ES::FLIGHT_PLAN_STATE_COORDINATED: return "coordinated";
        case ES::FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED: return "transferToMe";
        case ES::FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED: return "transferFromMe";
        case ES::FLIGHT_PLAN_STATE_ASSUMED: return "assumed";
        case ES::FLIGHT_PLAN_STATE_REDUNDANT: return "redundant";
        default: return "";
    }
}

const char* ConnectionTypeName(int t) {
    switch (t) {
        case ES::CONNECTION_TYPE_NO: return "offline";
        case ES::CONNECTION_TYPE_DIRECT: return "direct";
        case ES::CONNECTION_TYPE_VIA_PROXY: return "proxy";
        case ES::CONNECTION_TYPE_SIMULATOR_SERVER: return "simulatorServer";
        case ES::CONNECTION_TYPE_PLAYBACK: return "playback";
        case ES::CONNECTION_TYPE_SIMULATOR_CLIENT: return "simulatorClient";
        case ES::CONNECTION_TYPE_SWEATBOX: return "sweatbox";
        default: return "unknown";
    }
}

// Field name used in "assignedDataChanged" events.
const char* AssignedDataName(int type) {
    switch (type) {
        case ES::CTR_DATA_TYPE_SQUAWK: return "squawk";
        case ES::CTR_DATA_TYPE_FINAL_ALTITUDE: return "finalAltitude";
        case ES::CTR_DATA_TYPE_TEMPORARY_ALTITUDE: return "clearedAltitude";
        case ES::CTR_DATA_TYPE_COMMUNICATION_TYPE: return "communicationType";
        case ES::CTR_DATA_TYPE_SCRATCH_PAD_STRING: return "scratchpad";
        case ES::CTR_DATA_TYPE_GROUND_STATE: return "groundState";
        case ES::CTR_DATA_TYPE_CLEARENCE_FLAG: return "clearance";
        case ES::CTR_DATA_TYPE_DEPARTURE_SEQUENCE: return "departureSequence";
        case ES::CTR_DATA_TYPE_SPEED: return "speed";
        case ES::CTR_DATA_TYPE_MACH: return "mach";
        case ES::CTR_DATA_TYPE_RATE: return "rate";
        case ES::CTR_DATA_TYPE_HEADING: return "heading";
        case ES::CTR_DATA_TYPE_DIRECT_TO: return "directTo";
        default: return "other";
    }
}

void FillFlightPlan(ES::CFlightPlan& fp, FlightPlanInfo& f) {
    ES::CFlightPlanData d = fp.GetFlightPlanData();
    ES::CFlightPlanControllerAssignedData c = fp.GetControllerAssignedData();
    f.valid = true;
    f.origin = Text(d.GetOrigin());
    f.destination = Text(d.GetDestination());
    f.alternate = Text(d.GetAlternate());
    f.aircraftType = Text(d.GetAircraftFPType());
    f.aircraftInfo = Text(d.GetAircraftInfo());
    f.wakeCategory = Text(d.GetAircraftWtc());
    f.engineType = Text(d.GetEngineType());
    f.engineCount = d.GetEngineNumber();
    f.capabilities = Text(d.GetCapibilities());
    f.flightRules = Text(d.GetPlanType());
    f.route = Text(d.GetRoute());
    f.remarks = Text(d.GetRemarks());
    f.sid = Text(d.GetSidName());
    f.star = Text(d.GetStarName());
    f.departureRunway = Text(d.GetDepartureRwy());
    f.arrivalRunway = Text(d.GetArrivalRwy());
    f.trueAirspeed = d.GetTrueAirspeed();
    f.estimatedDeparture = Text(d.GetEstimatedDepartureTime());
    f.actualDeparture = Text(d.GetActualDepartureTime());
    f.enrouteTime = HoursMinutes(d.GetEnrouteHours(), d.GetEnrouteMinutes());
    f.fuelTime = HoursMinutes(d.GetFuelHours(), d.GetFuelMinutes());
    f.pilotName = Text(fp.GetPilotName());
    f.requestedAltitude = fp.GetFinalAltitude();

    f.clearedAltitude = c.GetClearedAltitude();
    f.assignedSquawk = Text(c.GetSquawk());
    f.assignedHeading = c.GetAssignedHeading();
    f.assignedSpeed = c.GetAssignedSpeed();
    f.assignedMach = c.GetAssignedMach();
    f.assignedRate = c.GetAssignedRate();
    f.directTo = Text(c.GetDirectToPointName());
    f.scratchpad = Text(c.GetScratchPadString());
    f.communicationType = Text(c.GetCommunicationType());
    if (f.communicationType.empty()) f.communicationType = Text(d.GetCommunicationType());
    f.annotations.clear();
    for (int i = 0; i < 9; ++i) f.annotations.push_back(Text(c.GetFlightStripAnnotation(i)));
    f.clearance = fp.GetClearenceFlag();
    f.groundState = Text(fp.GetGroundState());

    f.trackingController = Text(fp.GetTrackingControllerCallsign());
    f.trackedByMe = fp.GetTrackingControllerIsMe();
    f.handoffTarget = Text(fp.GetHandoffTargetControllerCallsign());
    f.nextController = Text(fp.GetCoordinatedNextController());
    f.state = FlightPlanStateName(fp.GetState());
    f.sectorEntryMinutes = fp.GetSectorEntryMinutes();
    f.sectorExitMinutes = fp.GetSectorExitMinutes();
    f.entryPoint = Text(fp.GetEntryCoordinationPointName());
    f.exitPoint = Text(fp.GetExitCoordinationPointName());
    f.distanceToDestination = fp.GetDistanceToDestination();
    f.distanceFromOrigin = fp.GetDistanceFromOrigin();
}

void FillController(ES::CController& c, const std::string& myCallsign, ControllerInfo& out) {
    out.callsign = Text(c.GetCallsign());
    out.name = Text(c.GetFullName());
    out.frequency = c.GetPrimaryFrequency();
    out.facility = FacilityName(c.GetFacility());
    out.rating = RatingName(c.GetRating());
    out.positionId = Text(c.GetPositionId());
    out.sectorFile = Text(c.GetSectorFileName());
    ES::CPosition p = c.GetPosition();
    out.latitude = p.m_Latitude;
    out.longitude = p.m_Longitude;
    out.range = c.GetRange();
    out.isController = c.IsController();
    out.isMe = !out.callsign.empty() && out.callsign == myCallsign;
}

void FillRoute(ES::CFlightPlan& fp, RouteInfo& r) {
    r.callsign = Text(fp.GetCallsign());
    ES::CFlightPlanExtractedRoute route = fp.GetExtractedRoute();
    int n = route.GetPointsNumber();
    int next = route.GetPointsCalculatedIndex();
    for (int i = 0; i < n; ++i) {
        RoutePoint p;
        p.name = Text(route.GetPointName(i));
        p.airway = Text(route.GetPointAirwayName(i));
        ES::CPosition pos = route.GetPointPosition(i);
        p.latitude = pos.m_Latitude;
        p.longitude = pos.m_Longitude;
        p.minutes = route.GetPointDistanceInMinutes(i);
        p.altitude = route.GetPointCalculatedProfileAltitude(i);
        p.passed = next >= 0 && i < next;
        r.points.push_back(p);
    }
    ES::CFlightPlanPositionPredictions pred = fp.GetPositionPredictions();
    int m = pred.GetPointsNumber();
    for (int i = 0; i < m; ++i) {
        PredictedPosition p;
        ES::CPosition pos = pred.GetPosition(i);
        p.latitude = pos.m_Latitude;
        p.longitude = pos.m_Longitude;
        p.altitude = pred.GetAltitude(i);
        r.predictions.push_back(p);
    }
}

}  // namespace

class WsDataPlugin : public ES::CPlugIn {
public:
    WsDataPlugin()
        : CPlugIn(ES::COMPATIBILITY_CODE, kName, kVersion, kAuthor, "MIT"), hub_(server_) {
        LoadSettings();
        StartServer();
    }

    ~WsDataPlugin() { server_.Stop(); }

    void OnTimer(int counter) override {
        if (!server_.IsRunning()) return;
        // Client messages are handled every second, independent of the push interval.
        hub_.Process(commandsEnabled_, token_, [this](const Command& c) { return Execute(c); });

        std::string me = Safe(ControllerMyself().GetCallsign());
        if (me != helloSource_) PublishHello();
        if (counter - lastRunwayCheck_ >= kRunwayCheckInterval || lastRunwayCheck_ < 0) {
            lastRunwayCheck_ = counter;
            RefreshRunways();
        }

        if (interval_ <= 0 || counter % interval_ != 0) return;
        // Nobody listening: skip the work. New clients still get the last snapshot on connect.
        if (server_.ClientCount() == 0 && !forceCapture_) {
            tracking_.clear();  // don't report stale changes when someone connects later
            return;
        }
        forceCapture_ = false;

        long long now = NowMillis();
        bool routes = counter - lastRoutes_ >= std::max(kRoutesInterval, interval_) && hub_.WantsRoutes();
        std::vector<RouteInfo> routeList;
        std::vector<AircraftState> list = CaptureAircraft(routes ? &routeList : nullptr);
        hub_.PublishAircraft(list, now, me);
        if (routes) {
            lastRoutes_ = counter;
            hub_.PublishRoutes(routeList, list, now);
        }
        PublishControllers(me, now);
        DetectTrackingChanges(list, now);
    }

    bool OnCompileCommand(const char* line) override {
        std::string cmd = Safe(line);
        if (cmd.compare(0, strlen(kCommand), kCommand) != 0) return false;
        std::vector<std::string> args;
        size_t pos = 0;
        while ((pos = cmd.find_first_not_of(' ', pos)) != std::string::npos) {
            size_t end = cmd.find(' ', pos);
            args.push_back(cmd.substr(pos, end - pos));
            pos = end;
        }

        if (args.size() >= 3 && args[1] == "port") {
            int p = std::atoi(args[2].c_str());
            if (p <= 0 || p > 65535) { Say("Invalid port"); return true; }
            port_ = p;
            SaveDataToSettings("port", "WebSocket port", std::to_string(p).c_str());
            Restart();
        } else if (args.size() >= 3 && args[1] == "bind") {
            bind_ = args[2];
            SaveDataToSettings("bind", "WebSocket bind address", bind_.c_str());
            Restart();
        } else if (args.size() >= 3 && args[1] == "interval") {
            int s = std::atoi(args[2].c_str());
            if (s <= 0) { Say("Interval must be at least 1 second"); return true; }
            interval_ = s;
            SaveDataToSettings("interval", "Seconds between pushes", std::to_string(s).c_str());
            Say("Pushing every " + std::to_string(s) + " s");
        } else if (args.size() >= 3 && args[1] == "commands" && (args[2] == "on" || args[2] == "off")) {
            commandsEnabled_ = args[2] == "on";
            SaveDataToSettings("commands", "Accept commands from clients", commandsEnabled_ ? "1" : "0");
            Say(std::string("Client commands ") + (commandsEnabled_ ? "enabled" : "disabled"));
            if (commandsEnabled_ && token_.empty() && bind_ != "127.0.0.1")
                Say("Warning: no token set and the server is reachable from the network. Use .wsdata token <secret>");
            PublishHello();
        } else if (args.size() >= 3 && args[1] == "token") {
            token_ = args[2] == "off" ? std::string() : args[2];
            SaveDataToSettings("token", "Token clients need for commands", token_.c_str());
            Say(token_.empty() ? "Token removed" : "Token set");
            PublishHello();
        } else if (args.size() >= 2 && args[1] == "restart") {
            Restart();
        } else {
            Status();
            Say("Usage: .wsdata [status|port <n>|bind <addr>|interval <sec>|commands on|off|token <secret>|off|restart]");
        }
        return true;
    }

    // ---- events ----------------------------------------------------------------

    void OnFlightPlanFlightPlanDataUpdate(ES::CFlightPlan fp) override {
        if (!Listening()) return;
        FlightPlanInfo f;
        FillFlightPlan(fp, f);
        std::string cs = Text(fp.GetCallsign());
        PublishEvent("flightPlanUpdated", [&](std::string& out) {
            json::Str(out, "callsign", cs);
            json::Key(out, "flightPlan");
            EncodeFlightPlan(out, f);
        });
    }

    void OnFlightPlanControllerAssignedDataUpdate(ES::CFlightPlan fp, int dataType) override {
        if (!Listening()) return;
        FlightPlanInfo f;
        FillFlightPlan(fp, f);
        std::string cs = Text(fp.GetCallsign());
        PublishEvent("assignedDataChanged", [&](std::string& out) {
            json::Str(out, "callsign", cs);
            json::Str(out, "field", AssignedDataName(dataType));
            json::Key(out, "flightPlan");
            EncodeFlightPlan(out, f);
        });
    }

    void OnFlightPlanDisconnect(ES::CFlightPlan fp) override {
        std::string cs = Text(fp.GetCallsign());
        tracking_.erase(cs);
        if (!Listening()) return;
        PublishEvent("aircraftDisconnected", [&](std::string& out) { json::Str(out, "callsign", cs); });
    }

    void OnFlightPlanFlightStripPushed(ES::CFlightPlan fp, const char* sender, const char* target) override {
        if (!Listening()) return;
        std::string cs = Text(fp.GetCallsign());
        PublishEvent("stripPushed", [&](std::string& out) {
            json::Str(out, "callsign", cs);
            json::Str(out, "from", Safe(sender));
            json::Str(out, "to", Safe(target));
        });
    }

    void OnControllerPositionUpdate(ES::CController c) override {
        std::string cs = Text(c.GetCallsign());
        if (cs.empty() || !knownControllers_.insert(cs).second) return;
        if (!Listening()) return;
        ControllerInfo info;
        FillController(c, Safe(ControllerMyself().GetCallsign()), info);
        PublishEvent("controllerConnected", [&](std::string& out) {
            json::Key(out, "controller");
            EncodeController(out, info);
            out += ',';
        });
    }

    void OnControllerDisconnect(ES::CController c) override {
        std::string cs = Text(c.GetCallsign());
        knownControllers_.erase(cs);
        if (!Listening()) return;
        PublishEvent("controllerDisconnected", [&](std::string& out) { json::Str(out, "callsign", cs); });
    }

    void OnNewMetarReceived(const char* station, const char* fullMetar) override {
        std::string icao = Trim(Safe(station)), metar = Trim(Safe(fullMetar));
        if (icao.empty() || metars_[icao] == metar) return;
        metars_[icao] = metar;
        PublishAirports();
        if (!Listening()) return;
        PublishEvent("metarUpdated", [&](std::string& out) {
            json::Str(out, "icao", icao);
            json::Str(out, "metar", metar);
        });
    }

    void OnAirportRunwayActivityChanged() override {
        if (RefreshRunways() && Listening())
            PublishEvent("runwaysChanged", nullptr);
    }

private:
    bool Listening() const { return server_.IsRunning() && server_.ClientCount() > 0; }

    void PublishEvent(const char* name, const std::function<void(std::string&)>& fields) {
        server_.Publish(topic::kEvents, EncodeEvent(name, NowMillis(), fields), false);
    }

    void PublishHello() {
        helloSource_ = Safe(ControllerMyself().GetCallsign());
        server_.Publish(topic::kHello,
                        EncodeHello(kName, kVersion, helloSource_, AllTopics(), DefaultTopics(), commandsEnabled_,
                                    !token_.empty(), NowMillis()),
                        true);
    }

    std::vector<AircraftState> CaptureAircraft(std::vector<RouteInfo>* routes) {
        std::vector<AircraftState> list;
        for (ES::CRadarTarget rt = RadarTargetSelectFirst(); rt.IsValid(); rt = RadarTargetSelectNext(rt)) {
            ES::CRadarTargetPositionData pos = rt.GetPosition();
            if (!pos.IsValid()) continue;

            AircraftState a;
            a.callsign = Safe(rt.GetCallsign());
            ES::CPosition p = pos.GetPosition();
            a.latitude = p.m_Latitude;
            a.longitude = p.m_Longitude;
            a.altitude = pos.GetPressureAltitude();
            a.flightLevel = pos.GetFlightLevel();
            a.groundSpeed = rt.GetGS();
            a.heading = pos.GetReportedHeading();
            a.verticalSpeed = rt.GetVerticalSpeed();
            a.squawk = Safe(pos.GetSquawk());
            a.modeC = pos.GetTransponderC();
            a.ident = pos.GetTransponderI();

            ES::CFlightPlan fp = rt.GetCorrelatedFlightPlan();
            if (fp.IsValid()) {
                FillFlightPlan(fp, a.fp);
                if (routes) {
                    routes->emplace_back();
                    FillRoute(fp, routes->back());
                }
            }
            list.push_back(std::move(a));
        }
        return list;
    }

    void PublishControllers(const std::string& myCallsign, long long now) {
        MeInfo me;
        ES::CController self = ControllerMyself();
        if (self.IsValid()) FillController(self, myCallsign, me.controller);
        me.controller.callsign = myCallsign;
        me.controller.isMe = true;
        me.connectionType = ConnectionTypeName(GetConnectionType());
        ES::CFlightPlan asel = FlightPlanSelectASEL();
        if (asel.IsValid()) me.asel = Text(asel.GetCallsign());

        std::vector<ControllerInfo> list;
        for (ES::CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c)) {
            ControllerInfo info;
            FillController(c, myCallsign, info);
            if (!info.callsign.empty()) list.push_back(info);
        }
        std::sort(list.begin(), list.end(),
                  [](const ControllerInfo& a, const ControllerInfo& b) { return a.callsign < b.callsign; });
        server_.Publish(topic::kControllers, EncodeControllers(me, list, now), true);
    }

    // EuroScope has no callback for tracking or handoff changes, so compare with the last tick.
    void DetectTrackingChanges(const std::vector<AircraftState>& list, long long now) {
        std::map<std::string, std::pair<std::string, std::string>> current;  // callsign -> (tracking, handoff target)
        for (const AircraftState& a : list)
            if (a.fp.valid) current[a.callsign] = {a.fp.trackingController, a.fp.handoffTarget};
        bool first = tracking_.empty();
        for (const auto& c : current) {
            if (first) break;
            auto old = tracking_.find(c.first);
            if (old == tracking_.end()) continue;
            const std::string& cs = c.first;
            const auto& before = old->second;
            const auto& after = c.second;
            if (before.first != after.first) {
                server_.Publish(topic::kEvents, EncodeEvent("trackingChanged", now, [&](std::string& out) {
                    json::Str(out, "callsign", cs);
                    json::Str(out, "from", before.first);
                    json::Str(out, "to", after.first);
                }), false);
            }
            if (before.second != after.second && !after.second.empty()) {
                server_.Publish(topic::kEvents, EncodeEvent("handoffInitiated", now, [&](std::string& out) {
                    json::Str(out, "callsign", cs);
                    json::Str(out, "from", after.first);
                    json::Str(out, "to", after.second);
                }), false);
            } else if (before.second != after.second) {
                bool accepted = after.first == before.second;
                server_.Publish(topic::kEvents, EncodeEvent("handoffEnded", now, [&](std::string& out) {
                    json::Str(out, "callsign", cs);
                    json::Str(out, "to", before.second);
                    json::Bool(out, "accepted", accepted);
                }), false);
            }
        }
        tracking_.swap(current);
    }

    // Reads active runways from the sector file. Returns true (and publishes) if they changed.
    bool RefreshRunways() {
        std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> runways;
        SelectActiveSectorfile();
        for (ES::CSectorElement e = SectorFileElementSelectFirst(ES::SECTOR_ELEMENT_RUNWAY); e.IsValid();
             e = SectorFileElementSelectNext(e, ES::SECTOR_ELEMENT_RUNWAY)) {
            // The airport field is "ICAO Name"; keep the code.
            std::string airport = Trim(Safe(e.GetAirportName()));
            airport = airport.substr(0, airport.find(' '));
            if (airport.empty()) continue;
            for (int i = 0; i < 2; ++i) {
                std::string name = Trim(Safe(e.GetRunwayName(i)));
                if (name.empty()) continue;
                if (e.IsElementActive(true, i)) runways[airport].first.push_back(name);
                if (e.IsElementActive(false, i)) runways[airport].second.push_back(name);
            }
        }
        for (auto it = runways.begin(); it != runways.end();)
            it = it->second.first.empty() && it->second.second.empty() ? runways.erase(it) : std::next(it);
        if (runways == runways_) return false;
        runways_.swap(runways);
        PublishAirports();
        return true;
    }

    void PublishAirports() {
        std::set<std::string> icaos;
        for (const auto& m : metars_) icaos.insert(m.first);
        for (const auto& r : runways_) icaos.insert(r.first);
        std::vector<AirportInfo> list;
        for (const std::string& icao : icaos) {
            AirportInfo a;
            a.icao = icao;
            auto m = metars_.find(icao);
            if (m != metars_.end()) a.metar = m->second;
            auto r = runways_.find(icao);
            if (r != runways_.end()) { a.departureRunways = r->second.first; a.arrivalRunways = r->second.second; }
            list.push_back(a);
        }
        server_.Publish(topic::kAirports, EncodeAirports(list, NowMillis()), true);
    }

    // ---- client commands -------------------------------------------------------

    std::string Execute(const Command& c) {
        if (c.callsign.empty()) return "callsign required";
        ES::CFlightPlan fp = FlightPlanSelect(c.callsign.c_str());
        if (!fp.IsValid()) return "no flight plan for " + c.callsign;
        ES::CFlightPlanControllerAssignedData cad = fp.GetControllerAssignedData();
        const std::string rejected = "EuroScope rejected the change (is the aircraft tracked by someone else?)";
        auto ok = [&](bool done) { return done ? std::string() : rejected; };
        double num = 0;
        auto needInt = [&](int lo, int hi, int* out) -> bool {
            if (!c.Number(&num) || num < lo || num > hi) return false;
            *out = int(std::lround(num));
            return true;
        };
        std::string range = "value out of range";
        int v = 0;

        if (c.name == "select") { SetASELAircraft(fp); return std::string(); }
        if (c.name == "assume") return ok(Call([&] { return fp.StartTracking(); }));
        if (c.name == "drop") return ok(Call([&] { return fp.EndTracking(); }));
        if (c.name == "handoff") {
            std::string to = c.Text("to");
            if (to.empty()) return "\"to\" (controller callsign) required";
            return ok(Call([&] { return fp.InitiateHandoff(to.c_str()); }));
        }
        if (c.name == "acceptHandoff") return ok(Call([&] { return fp.AcceptHandoff(); }));
        if (c.name == "refuseHandoff") return ok(Call([&] { return fp.RefuseHandoff(); }));
        if (c.name == "setClearedAltitude") {
            if (!needInt(0, 99999, &v)) return range;
            return ok(cad.SetClearedAltitude(v));
        }
        if (c.name == "setFinalAltitude") {
            if (!needInt(0, 99999, &v)) return range;
            return ok(cad.SetFinalAltitude(v));
        }
        if (c.name == "setHeading") {
            if (!needInt(0, 360, &v)) return range;
            return ok(cad.SetAssignedHeading(v));
        }
        if (c.name == "setSpeed") {
            if (!needInt(0, 999, &v)) return range;
            return ok(cad.SetAssignedSpeed(v));
        }
        if (c.name == "setMach") {
            if (!c.Number(&num) || num < 0 || num > 100) return range;
            int mach = int(std::lround(num < 5 ? num * 100 : num));  // accept 0.78 or 78
            return ok(cad.SetAssignedMach(mach));
        }
        if (c.name == "setRate") {
            if (!needInt(-99999, 99999, &v)) return range;
            return ok(cad.SetAssignedRate(v));
        }
        if (c.name == "setSquawk") {
            std::string sq = c.Text();
            if (sq.size() != 4 || sq.find_first_not_of("01234567") != std::string::npos) return "squawk must be 4 octal digits";
            return ok(cad.SetSquawk(sq.c_str()));
        }
        if (c.name == "setScratchpad") return ok(cad.SetScratchPadString(c.Text().c_str()));
        if (c.name == "setDirectTo") return ok(cad.SetDirectToPointName(c.Text().c_str()));
        if (c.name == "setCommunicationType") {
            std::string t = c.Text();
            if (t.size() != 1 || std::string("vrtVRT").find(t[0]) == std::string::npos) return "value must be v, r or t";
            return ok(cad.SetCommunicationType(char(std::toupper(static_cast<unsigned char>(t[0])))));
        }
        if (c.name == "setAnnotation") {
            double idx = -1;
            if (!c.Number(&idx, "index") || idx < 0 || idx > 8) return "index must be 0-8";
            return ok(cad.SetFlightStripAnnotation(int(idx), c.Text().c_str()));
        }
        if (c.name == "setRoute") {
            std::string route = c.Text();
            if (route.empty()) return "value (route) required";
            ES::CFlightPlanData d = fp.GetFlightPlanData();
            if (!d.SetRoute(route.c_str())) return rejected;
            return ok(d.AmendFlightPlan());
        }
        return "unknown command: " + c.name;
    }

    // ---- settings / server -----------------------------------------------------

    void LoadSettings() {
        bind_ = kDefaultBind;
        port_ = kDefaultPort;
        interval_ = kDefaultInterval;
        if (const char* v = GetDataFromSettings("bind")) if (*v) bind_ = v;
        if (const char* v = GetDataFromSettings("port")) if (std::atoi(v) > 0) port_ = std::atoi(v);
        if (const char* v = GetDataFromSettings("interval")) if (std::atoi(v) > 0) interval_ = std::atoi(v);
        if (const char* v = GetDataFromSettings("commands")) commandsEnabled_ = std::atoi(v) == 1;
        if (const char* v = GetDataFromSettings("token")) token_ = v;
    }

    void StartServer() {
        std::string err;
        if (server_.Start(bind_, uint16_t(port_), &err)) {
            Say("Streaming on ws://" + bind_ + ":" + std::to_string(port_));
            PublishHello();
            PublishAirports();
        } else {
            Say("Could not start WebSocket server: " + err);
        }
    }

    void Restart() {
        server_.Stop();
        server_.ClearRetained();
        forceCapture_ = true;
        StartServer();
    }

    void Status() {
        Say(std::string(server_.IsRunning() ? "Running" : "Stopped") + " on ws://" + bind_ + ":" +
            std::to_string(port_) + ", " + std::to_string(server_.ClientCount()) + " client(s), every " +
            std::to_string(interval_) + " s, commands " + (commandsEnabled_ ? "on" : "off") +
            (token_.empty() ? "" : " (token set)"));
    }

    void Say(const std::string& msg) {
        DisplayUserMessage("Message", kName, msg.c_str(), true, true, false, false, false);
    }

    WsServer server_;
    ClientHub hub_;
    std::string bind_;
    int port_ = kDefaultPort;
    int interval_ = kDefaultInterval;
    bool commandsEnabled_ = false;
    std::string token_;
    bool forceCapture_ = true;  // one capture at start so the first client gets data straight away
    std::string helloSource_;
    int lastRoutes_ = -1000;
    int lastRunwayCheck_ = -1;
    std::map<std::string, std::pair<std::string, std::string>> tracking_;  // callsign -> (tracking, handoff target)
    std::set<std::string> knownControllers_;
    std::map<std::string, std::string> metars_;
    std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> runways_;
};

// ---------------------------------------------------------------------------
// EuroScope entry points

static std::unique_ptr<WsDataPlugin> g_plugin;

void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance) {
    g_plugin.reset(new WsDataPlugin());
    *ppPlugInInstance = g_plugin.get();
}

void __declspec(dllexport) EuroScopePlugInExit(void) {
    g_plugin.reset();
}
