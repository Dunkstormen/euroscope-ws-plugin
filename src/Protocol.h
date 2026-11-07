// What clients can send and how the server answers: topics, subscriptions, per-client
// filters, authentication and commands. Shared by the plugin and fake_feed so both speak
// exactly the same protocol. Free of EuroScope types.
//
// Threading: the WsServer callbacks (server thread) only queue messages here. Everything
// else, including running commands, happens in Process() / Publish*() on the caller's
// thread, which in the plugin is EuroScope's own thread.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "JsonParse.h"
#include "Snapshot.h"
#include "WsServer.h"

namespace topic {
const char* const kHello = "hello";
const char* const kAircraft = "aircraft";
const char* const kControllers = "controllers";
const char* const kAirports = "airports";
const char* const kEvents = "events";
const char* const kRoutes = "routes";
}  // namespace topic

// Topics a client may subscribe to, and those it gets without asking. "routes" is opt-in
// because it is large. "hello" is always on.
inline const std::vector<std::string>& AllTopics() {
    static const std::vector<std::string> t = {topic::kAircraft, topic::kControllers, topic::kAirports,
                                               topic::kEvents, topic::kRoutes};
    return t;
}
inline const std::vector<std::string>& DefaultTopics() {
    static const std::vector<std::string> t = {topic::kAircraft, topic::kControllers, topic::kAirports,
                                               topic::kEvents};
    return t;
}
inline std::set<std::string> DefaultTopicSet() {
    std::set<std::string> s(DefaultTopics().begin(), DefaultTopics().end());
    s.insert(topic::kHello);
    return s;
}

// Narrows what one client gets for "aircraft" or "routes".
struct AircraftFilter {
    bool active = false;
    std::string tracked;               // "" all, "me" tracked by me, "any" tracked by someone
    bool hasArea = false;
    double minLat = 0, minLon = 0, maxLat = 0, maxLon = 0;
    std::set<std::string> callsigns;   // empty = all

    bool Matches(const AircraftState& a) const {
        if (!active) return true;
        if (!callsigns.empty() && !callsigns.count(a.callsign)) return false;
        if (tracked == "me" && !(a.fp.valid && a.fp.trackedByMe)) return false;
        if (tracked == "any" && !(a.fp.valid && !a.fp.trackingController.empty())) return false;
        if (hasArea && (a.latitude < minLat || a.latitude > maxLat || a.longitude < minLon || a.longitude > maxLon))
            return false;
        return true;
    }
};

// A client command that acts on EuroScope data, handed to the executor.
struct Command {
    WsServer::ClientId client = 0;
    std::string name;       // e.g. "setClearedAltitude"
    std::string callsign;   // aircraft it applies to (may be empty for some commands)
    const jsonr::Value* msg = nullptr;  // the whole message, for command-specific fields

    std::string Text(const char* key = "value") const { return msg ? msg->GetString(key) : std::string(); }
    bool Number(double* out, const char* key = "value") const { return msg && msg->GetNumber(key, out); }
};

// Encodes {"type":"event","event":name,"time":...,<fields>}. `fields` appends "key":value, pairs.
inline std::string EncodeEvent(const char* name, long long unixMillis, const std::function<void(std::string&)>& fields) {
    std::string out = json::Begin("event", unixMillis);
    json::Str(out, "event", name);
    if (fields) fields(out);
    json::CloseObject(out);
    return out;
}

class ClientHub {
public:
    // Returns "" on success or an error message for the client.
    using Executor = std::function<std::string(const Command&)>;

    explicit ClientHub(WsServer& server) : server_(server) {
        server_.SetDefaultTopics(DefaultTopicSet());
        server_.SetMessageHandler([this](WsServer::ClientId id, const std::string& text) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inbox_.size() < 1000) inbox_.emplace_back(id, text);
        });
        server_.SetCloseHandler([this](WsServer::ClientId id) {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_.push_back(id);
        });
    }
    ~ClientHub() {
        server_.SetMessageHandler(nullptr);
        server_.SetCloseHandler(nullptr);
    }

    // Handle everything clients sent since the last call. Aircraft commands go to `exec`, and only
    // when `commandsEnabled`; if `token` is not empty the client must have authenticated with it.
    void Process(bool commandsEnabled, const std::string& token, const Executor& exec) {
        std::vector<std::pair<WsServer::ClientId, std::string>> inbox;
        std::vector<WsServer::ClientId> closed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox.swap(inbox_);
            closed.swap(closed_);
        }
        for (const auto& m : inbox) Handle(m.first, m.second, commandsEnabled, token, exec);
        // After handling, so a message that arrived just before the close doesn't recreate state.
        for (WsServer::ClientId id : closed) clients_.erase(id);
    }

    // Publish the aircraft snapshot: everyone gets the full list, filtered clients their subset.
    void PublishAircraft(const std::vector<AircraftState>& list, long long t, const std::string& source) {
        server_.Publish(topic::kAircraft, EncodeSnapshot(list, t, source), true);
        for (auto& c : clients_) {
            auto f = c.second.filters.find(topic::kAircraft);
            if (f == c.second.filters.end() || !c.second.topics.count(topic::kAircraft)) continue;
            std::vector<AircraftState> subset;
            for (const AircraftState& a : list)
                if (f->second.Matches(a)) subset.push_back(a);
            server_.SendTo(c.first, EncodeSnapshot(subset, t, source));
        }
    }

    // Publish routes; filtered clients get the routes of aircraft matching their "routes" filter.
    void PublishRoutes(const std::vector<RouteInfo>& routes, const std::vector<AircraftState>& aircraft, long long t) {
        server_.Publish(topic::kRoutes, EncodeRoutes(routes, t), true);
        std::map<std::string, const AircraftState*> byCallsign;
        for (const AircraftState& a : aircraft) byCallsign[a.callsign] = &a;
        for (auto& c : clients_) {
            auto f = c.second.filters.find(topic::kRoutes);
            if (f == c.second.filters.end() || !c.second.topics.count(topic::kRoutes)) continue;
            std::vector<RouteInfo> subset;
            for (const RouteInfo& r : routes) {
                auto a = byCallsign.find(r.callsign);
                if (a != byCallsign.end() && f->second.Matches(*a->second)) subset.push_back(r);
            }
            server_.SendTo(c.first, EncodeRoutes(subset, t));
        }
    }

    bool WantsRoutes() { return server_.HasSubscribers(topic::kRoutes) || AnyFiltered(topic::kRoutes); }

private:
    struct ClientState {
        std::set<std::string> topics = DefaultTopicSet();
        std::map<std::string, AircraftFilter> filters;  // topic -> filter (only "aircraft", "routes")
        bool authed = false;
    };

    bool AnyFiltered(const char* t) const {
        for (const auto& c : clients_)
            if (c.second.topics.count(t) && c.second.filters.count(t)) return true;
        return false;
    }

    // Tell the server what to broadcast to this client. Filtered topics are left out of the
    // broadcast because the client gets its own copy via SendTo.
    void ApplyTopics(WsServer::ClientId id, const ClientState& st) {
        std::set<std::string> server = st.topics;
        for (const auto& f : st.filters) server.erase(f.first);
        server_.Subscribe(id, server);
    }

    static void AppendId(std::string& out, const jsonr::Value* id) {
        if (!id) return;
        if (id->type == jsonr::Value::Number) json::Num(out, "id", id->n, id->n == double((long long)id->n) ? 0 : 6);
        else if (id->type == jsonr::Value::String) json::Str(out, "id", id->s);
    }

    void Reply(WsServer::ClientId id, const jsonr::Value* msgId, const std::string& cmd, const std::string& error) {
        std::string out = "{";
        json::Str(out, "type", "result");
        AppendId(out, msgId);
        json::Str(out, "cmd", cmd);
        json::Bool(out, "ok", error.empty());
        if (!error.empty()) json::Str(out, "error", error);
        json::CloseObject(out);
        server_.SendTo(id, out);
    }

    static bool KnownTopic(const std::string& t) {
        for (const std::string& k : AllTopics())
            if (k == t) return true;
        return false;
    }

    std::string Subscribe(WsServer::ClientId id, ClientState& st, const jsonr::Value& msg) {
        const jsonr::Value* list = msg.Get("topics");
        if (!list || list->type != jsonr::Value::Array) return "topics must be an array";
        std::set<std::string> topics = {topic::kHello};
        for (const jsonr::Value& v : list->a) {
            if (v.type != jsonr::Value::String || !KnownTopic(v.s)) return "unknown topic: " + v.s;
            topics.insert(v.s);
        }
        st.topics = topics;
        ApplyTopics(id, st);
        return std::string();
    }

    std::string Filter(WsServer::ClientId id, ClientState& st, const jsonr::Value& msg) {
        std::string t = msg.GetString("topic", topic::kAircraft);
        if (t != topic::kAircraft && t != topic::kRoutes) return "filters apply to aircraft or routes";
        AircraftFilter f;
        f.tracked = msg.GetString("tracked");
        if (!f.tracked.empty() && f.tracked != "me" && f.tracked != "any") return "tracked must be \"me\" or \"any\"";
        if (const jsonr::Value* area = msg.Get("area")) {
            if (area->type != jsonr::Value::Array || area->a.size() != 4) return "area must be [minLat, minLon, maxLat, maxLon]";
            for (const jsonr::Value& v : area->a)
                if (v.type != jsonr::Value::Number) return "area must be [minLat, minLon, maxLat, maxLon]";
            f.hasArea = true;
            f.minLat = area->a[0].n; f.minLon = area->a[1].n; f.maxLat = area->a[2].n; f.maxLon = area->a[3].n;
        }
        if (const jsonr::Value* cs = msg.Get("callsigns")) {
            if (cs->type != jsonr::Value::Array) return "callsigns must be an array";
            for (const jsonr::Value& v : cs->a)
                if (v.type == jsonr::Value::String) f.callsigns.insert(v.s);
        }
        f.active = !f.tracked.empty() || f.hasArea || !f.callsigns.empty();
        if (f.active) st.filters[t] = f;
        else st.filters.erase(t);
        ApplyTopics(id, st);
        return std::string();
    }

    void Handle(WsServer::ClientId id, const std::string& text, bool commandsEnabled, const std::string& token,
                const Executor& exec) {
        jsonr::Value msg;
        std::string err;
        if (!jsonr::Parse(text, msg, &err)) { Reply(id, nullptr, "", "invalid JSON: " + err); return; }
        const jsonr::Value* msgId = msg.Get("id");
        std::string cmd = msg.GetString("cmd");
        if (msg.type != jsonr::Value::Object || cmd.empty()) { Reply(id, msgId, "", "expected {\"cmd\":...}"); return; }
        ClientState& st = clients_[id];

        if (!token.empty() && msg.GetString("token") == token) st.authed = true;

        if (cmd == "subscribe") { Reply(id, msgId, cmd, Subscribe(id, st, msg)); return; }
        if (cmd == "filter") { Reply(id, msgId, cmd, Filter(id, st, msg)); return; }
        if (cmd == "auth") {
            Reply(id, msgId, cmd, token.empty() || st.authed ? std::string() : "wrong token");
            return;
        }
        if (cmd == "ping") { Reply(id, msgId, cmd, std::string()); return; }

        if (!commandsEnabled) { Reply(id, msgId, cmd, "commands are disabled (enable with .wsdata commands on)"); return; }
        if (!token.empty() && !st.authed) { Reply(id, msgId, cmd, "not authenticated (send {\"cmd\":\"auth\",\"token\":...})"); return; }
        Command c;
        c.client = id;
        c.name = cmd;
        c.callsign = msg.GetString("callsign");
        c.msg = &msg;
        Reply(id, msgId, cmd, exec ? exec(c) : std::string("not supported"));
    }

    WsServer& server_;
    std::map<WsServer::ClientId, ClientState> clients_;  // caller's thread only

    std::mutex mutex_;  // guards the queues filled by the server thread
    std::vector<std::pair<WsServer::ClientId, std::string>> inbox_;
    std::vector<WsServer::ClientId> closed_;
};
