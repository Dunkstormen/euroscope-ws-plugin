// EuroScope plugin: streams aircraft data to WebSocket clients as JSON.
//
// Threading model:
//   EuroScope thread  -> OnTimer() reads the SDK, builds a JSON string, calls server.Broadcast()
//   Server thread     -> owns all sockets, sends the queued JSON to clients
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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "EuroScopePlugIn.h"
#include "Snapshot.h"
#include "WsServer.h"

namespace {

const char* kName = "WebSocket Data";
const char* kVersion = "0.1.0";
const char* kAuthor = "Benjamin";
const char* kCommand = ".wsdata";

const char* kDefaultBind = "127.0.0.1";
const int kDefaultPort = 8765;
const int kDefaultInterval = 1;  // seconds between pushes

std::string Safe(const char* s) { return s ? s : ""; }

long long NowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

class WsDataPlugin : public EuroScopePlugIn::CPlugIn {
public:
    WsDataPlugin()
        : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, kName, kVersion, kAuthor, "MIT") {
        LoadSettings();
        StartServer();
    }

    ~WsDataPlugin() { server_.Stop(); }

    void OnTimer(int counter) override {
        if (!server_.IsRunning() || interval_ <= 0 || counter % interval_ != 0) return;
        // Nobody listening: skip the work. New clients still get the last snapshot on connect.
        if (server_.ClientCount() == 0 && !lastSnapshotEmpty_) return;

        std::vector<AircraftState> list = Capture();
        lastSnapshotEmpty_ = false;
        server_.Broadcast(EncodeSnapshot(list, NowMillis(), Safe(ControllerMyself().GetCallsign())));
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
        } else if (args.size() >= 2 && args[1] == "restart") {
            Restart();
        } else {
            Status();
            Say("Usage: .wsdata [status|port <n>|bind <addr>|interval <sec>|restart]");
        }
        return true;
    }

private:
    std::vector<AircraftState> Capture() {
        std::vector<AircraftState> list;
        for (EuroScopePlugIn::CRadarTarget rt = RadarTargetSelectFirst(); rt.IsValid();
             rt = RadarTargetSelectNext(rt)) {
            EuroScopePlugIn::CRadarTargetPositionData pos = rt.GetPosition();
            if (!pos.IsValid()) continue;

            AircraftState a;
            a.callsign = Safe(rt.GetCallsign());
            EuroScopePlugIn::CPosition p = pos.GetPosition();
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

            EuroScopePlugIn::CFlightPlan fp = rt.GetCorrelatedFlightPlan();
            if (fp.IsValid()) {
                EuroScopePlugIn::CFlightPlanData d = fp.GetFlightPlanData();
                EuroScopePlugIn::CFlightPlanControllerAssignedData c = fp.GetControllerAssignedData();
                FlightPlanInfo& f = a.fp;
                f.valid = true;
                f.origin = Safe(d.GetOrigin());
                f.destination = Safe(d.GetDestination());
                f.alternate = Safe(d.GetAlternate());
                f.aircraftType = Safe(d.GetAircraftFPType());
                char wtc = d.GetAircraftWtc();
                if (wtc > ' ') f.wakeCategory = std::string(1, wtc);
                f.flightRules = Safe(d.GetPlanType());
                f.route = Safe(d.GetRoute());
                f.requestedAltitude = d.GetFinalAltitude();
                f.clearedAltitude = c.GetClearedAltitude();
                f.assignedSquawk = Safe(c.GetSquawk());
                f.assignedHeading = c.GetAssignedHeading();
                f.assignedSpeed = c.GetAssignedSpeed();
                f.trackingController = Safe(fp.GetTrackingControllerCallsign());
                f.groundState = Safe(fp.GetGroundState());
            }
            list.push_back(std::move(a));
        }
        return list;
    }

    void LoadSettings() {
        bind_ = kDefaultBind;
        port_ = kDefaultPort;
        interval_ = kDefaultInterval;
        if (const char* v = GetDataFromSettings("bind")) if (*v) bind_ = v;
        if (const char* v = GetDataFromSettings("port")) if (std::atoi(v) > 0) port_ = std::atoi(v);
        if (const char* v = GetDataFromSettings("interval")) if (std::atoi(v) > 0) interval_ = std::atoi(v);
    }

    void StartServer() {
        std::string err;
        if (server_.Start(bind_, uint16_t(port_), &err))
            Say("Streaming on ws://" + bind_ + ":" + std::to_string(port_));
        else
            Say("Could not start WebSocket server: " + err);
    }

    void Restart() {
        server_.Stop();
        lastSnapshotEmpty_ = true;
        StartServer();
    }

    void Status() {
        Say(std::string(server_.IsRunning() ? "Running" : "Stopped") + " on ws://" + bind_ + ":" +
            std::to_string(port_) + ", " + std::to_string(server_.ClientCount()) + " client(s), every " +
            std::to_string(interval_) + " s");
    }

    void Say(const std::string& msg) {
        DisplayUserMessage("Message", kName, msg.c_str(), true, true, false, false, false);
    }

    WsServer server_;
    std::string bind_;
    int port_ = kDefaultPort;
    int interval_ = kDefaultInterval;
    bool lastSnapshotEmpty_ = true;  // forces one capture so late joiners get data
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
