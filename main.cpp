#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  static void closeSocket(socket_t s) { closesocket(s); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  using socket_t = int;
  static void closeSocket(socket_t s) { close(s); }
#endif

namespace {
constexpr uint64_t HIGHWAY_PERIOD = 104;
constexpr int HIGHWAY_CYCLES_REQUIRED = 20;
constexpr uint64_t HASH_POLY = 0x9e3779b185ebca87ULL;
constexpr uint64_t HASH_BASE_AX = 0xd6e8feb86659fd93ULL;
constexpr uint64_t HASH_BASE_BY = 0xa5a3564e27f8862bULL;
constexpr uint64_t HASH_BASE_CX = 0x9e3779b185ebca87ULL;
constexpr uint64_t HASH_BASE_DY = 0xc2b2ae3d27d4eb4fULL;

struct Coord {
    int64_t x = 0, y = 0;
    bool operator==(const Coord& o) const noexcept { return x == o.x && y == o.y; }
};
struct CoordHash {
    size_t operator()(const Coord& c) const noexcept {
        uint64_t x = static_cast<uint64_t>(c.x);
        uint64_t y = static_cast<uint64_t>(c.y);
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL; x ^= x >> 27; x *= 0x94d049bb133111ebULL; x ^= x >> 31;
        y ^= y >> 30; y *= 0xbf58476d1ce4e5b9ULL; y ^= y >> 27; y *= 0x94d049bb133111ebULL; y ^= y >> 31;
        return static_cast<size_t>(x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2)));
    }
};

static uint64_t powMod(uint64_t base, uint64_t exp) {
    uint64_t r = 1;
    while (exp) { if (exp & 1) r *= base; base *= base; exp >>= 1; }
    return r;
}
static uint64_t inverseMod2_64(uint64_t a) {
    uint64_t x = a;
    for (int i = 0; i < 6; ++i) x *= 2 - a * x;
    return x;
}
static uint64_t signedPow(uint64_t base, int64_t exp) {
    if (exp >= 0) return powMod(base, static_cast<uint64_t>(exp));
    uint64_t mag = static_cast<uint64_t>(-(exp + 1)) + 1;
    return powMod(inverseMod2_64(base), mag);
}

struct AxisPowers { uint64_t a, ai, c, ci; };
struct PairPowers { uint64_t b, bi, d, di; };

struct Snapshot {
    Coord p1, p2;
    int d1 = 0, d2 = 0;
};
struct HighwayTrack {
    bool candidate = false;
    bool confirmed = false;
    uint64_t lastMatchLocal = 0;
    uint64_t candidateStartLocal = 0;
    int streakCycles = 0;
    uint64_t startLocal = 0;
    uint64_t startGlobal = 0;
    uint64_t confirmedGlobal = 0;
};
struct LoopKey {
    int64_t relx = 0, rely = 0;
    int d1 = 0, d2 = 0;
    uint64_t h1 = 0, h2 = 0;
    bool operator==(const LoopKey& o) const noexcept {
        return relx == o.relx && rely == o.rely && d1 == o.d1 && d2 == o.d2 && h1 == o.h1 && h2 == o.h2;
    }
};
struct LoopKeyHash {
    size_t operator()(const LoopKey& k) const noexcept {
        uint64_t h = k.h1 ^ (k.h2 + 0x9e3779b97f4a7c15ULL + (k.h1 << 6) + (k.h1 >> 2));
        auto mix = [&h](uint64_t v) { v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL; v ^= v >> 27; v *= 0x94d049bb133111ebULL; v ^= v >> 31; h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(static_cast<uint64_t>(k.relx)); mix(static_cast<uint64_t>(k.rely)); mix(static_cast<uint64_t>(k.d1)); mix(static_cast<uint64_t>(k.d2));
        return static_cast<size_t>(h);
    }
};

class Simulation {
public:
    Coord ant[2]{};
    int dir[2] = {0, 0}; // 0 up, 1 right, 2 down, 3 left
    int spacing = 10;
    uint64_t totalSteps = 0;
    uint64_t epochStartStep = 0;

    std::unordered_set<Coord, CoordHash> blackCells;
    std::unordered_set<Coord, CoordHash> visitedCells;
    std::unordered_set<Coord, CoordHash> highwayCells;
    std::unordered_map<int64_t, std::unordered_set<int64_t>> blackRows;
    std::unordered_map<int64_t, std::unordered_set<int64_t>> highwayRows;
    uint64_t highwayBlackCount = 0;
    uint64_t boardHash1 = 0, boardHash2 = 0;

    std::unordered_map<int64_t, AxisPowers> xPowers;
    std::unordered_map<int64_t, PairPowers> yPowers;

    std::vector<Snapshot> snapshots;
    std::array<std::vector<uint8_t>, 2> turns;
    std::array<std::vector<uint64_t>, 2> turnPrefix;
    std::vector<uint64_t> hashPowers{1};
    std::array<HighwayTrack, 2> hw{};
    std::unordered_map<LoopKey, uint64_t, LoopKeyHash> seenStates;
    bool loopFound = false;
    uint64_t loopFirstStep = 0, loopDetectedStep = 0, loopPeriod = 0;

    Simulation() { reset(10, 0, 0); }

    AxisPowers xPow(int64_t x) {
        auto it = xPowers.find(x); if (it != xPowers.end()) return it->second;
        AxisPowers p{signedPow(HASH_BASE_AX, x), signedPow(HASH_BASE_AX, -x), signedPow(HASH_BASE_CX, x), signedPow(HASH_BASE_CX, -x)};
        xPowers.emplace(x, p); return p;
    }
    PairPowers yPow(int64_t y) {
        auto it = yPowers.find(y); if (it != yPowers.end()) return it->second;
        PairPowers p{signedPow(HASH_BASE_BY, y), signedPow(HASH_BASE_BY, -y), signedPow(HASH_BASE_DY, y), signedPow(HASH_BASE_DY, -y)};
        yPowers.emplace(y, p); return p;
    }
    std::pair<uint64_t,uint64_t> cellTokens(Coord p) {
        auto xp = xPow(p.x); auto yp = yPow(p.y);
        return {xp.a * yp.b, xp.c * yp.d};
    }
    bool isBlack(Coord p) const { return blackCells.find(p) != blackCells.end(); }
    bool isHighway(Coord p) const { return highwayCells.find(p) != highwayCells.end(); }

    void setBlack(Coord p, bool black) {
        bool old = isBlack(p); if (old == black) return;
        auto tok = cellTokens(p);
        if (black) {
            blackCells.insert(p); blackRows[p.y].insert(p.x);
            boardHash1 += tok.first; boardHash2 += tok.second;
            if (isHighway(p)) ++highwayBlackCount;
        } else {
            blackCells.erase(p);
            auto row = blackRows.find(p.y); if (row != blackRows.end()) { row->second.erase(p.x); if (row->second.empty()) blackRows.erase(row); }
            boardHash1 -= tok.first; boardHash2 -= tok.second;
            if (isHighway(p) && highwayBlackCount) --highwayBlackCount;
        }
    }
    void addHighwayCell(Coord p) {
        if (highwayCells.insert(p).second) {
            highwayRows[p.y].insert(p.x);
            if (isBlack(p)) ++highwayBlackCount;
        }
    }
    void clearHighwayCells() { highwayCells.clear(); highwayRows.clear(); highwayBlackCount = 0; }

    void reset(int newSpacing, int d1, int d2) {
        blackCells.clear(); visitedCells.clear(); clearHighwayCells(); blackRows.clear();
        boardHash1 = boardHash2 = 0; totalSteps = 0; spacing = std::clamp(newSpacing, 1, 1000000);
        dir[0] = ((d1 % 4) + 4) % 4; dir[1] = ((d2 % 4) + 4) % 4;
        int64_t x1 = -(static_cast<int64_t>(spacing) / 2);
        ant[0] = {x1, 0}; ant[1] = {x1 + spacing, 0};
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        xPowers.clear(); yPowers.clear();
        startNewAnalysisEpoch();
    }

    void repositionSpacing(int newSpacing) {
        spacing = std::clamp(newSpacing, 1, 1000000);
        int64_t x1 = -(static_cast<int64_t>(spacing) / 2);
        ant[0] = {x1, 0}; ant[1] = {x1 + spacing, 0};
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        startNewAnalysisEpoch();
    }
    void changeDirection(int idx, int d) {
        if (idx < 0 || idx > 1) return;
        dir[idx] = ((d % 4) + 4) % 4;
        startNewAnalysisEpoch();
    }

    void startNewAnalysisEpoch() {
        epochStartStep = totalSteps;
        snapshots.clear(); turns[0].clear(); turns[1].clear();
        turnPrefix[0].assign(1, 0); turnPrefix[1].assign(1, 0);
        hashPowers.assign(1, 1);
        hw = {}; seenStates.clear(); loopFound = false; loopFirstStep = loopDetectedStep = loopPeriod = 0;
        snapshots.push_back({ant[0], ant[1], dir[0], dir[1]});
        rememberLoopState();
    }

    void ensureHashPower(size_t n) {
        while (hashPowers.size() <= n) hashPowers.push_back(hashPowers.back() * HASH_POLY);
    }
    uint64_t substringHash(int idx, size_t l, size_t r) {
        ensureHashPower(r-l);
        return turnPrefix[idx][r] - turnPrefix[idx][l] * hashPowers[r-l];
    }

    bool repeatPairStart(int idx, size_t s) {
        const size_t p = HIGHWAY_PERIOD;
        if (s + 2*p > turns[idx].size()) return false;
        if (substringHash(idx, s, s+p) != substringHash(idx, s+p, s+2*p)) return false;
        for (size_t j = 0; j < p; ++j) if (turns[idx][s+j] != turns[idx][s+p+j]) return false;
        const Snapshot& a = snapshots[s];
        const Snapshot& b = snapshots[s+p];
        const Snapshot& c = snapshots[s+2*p];
        Coord pa = idx == 0 ? a.p1 : a.p2;
        Coord pb = idx == 0 ? b.p1 : b.p2;
        Coord pc = idx == 0 ? c.p1 : c.p2;
        int da = idx == 0 ? a.d1 : a.d2;
        int db = idx == 0 ? b.d1 : b.d2;
        int dc = idx == 0 ? c.d1 : c.d2;
        const int64_t stepDx = pb.x - pa.x;
        const int64_t stepDy = pb.y - pa.y;
        return (stepDx != 0 || stepDy != 0) && (stepDx == pc.x-pb.x) && (stepDy == pc.y-pb.y) && (da == db) && (db == dc);
    }

    void analyseHighways(size_t localStep) {
        for (int idx = 0; idx < 2; ++idx) {
            auto& h = hw[idx];
            if (h.confirmed) { addHighwayCell(idx == 0 ? snapshots[localStep].p1 : snapshots[localStep].p2); continue; }
            if (localStep < 2*HIGHWAY_PERIOD) continue;
            if (h.candidate) {
                if (localStep == h.lastMatchLocal + HIGHWAY_PERIOD) {
                    if (repeatPairStart(idx, localStep - 2*HIGHWAY_PERIOD)) {
                        ++h.streakCycles; h.lastMatchLocal = localStep;
                        if (h.streakCycles >= HIGHWAY_CYCLES_REQUIRED) {
                            h.confirmed = true;
                            size_t start = static_cast<size_t>(h.candidateStartLocal);
                            while (start > 0 && repeatPairStart(idx, start-1)) --start;
                            h.startLocal = start;
                            h.startGlobal = epochStartStep + start;
                            h.confirmedGlobal = totalSteps;
                            for (size_t j = start; j <= localStep; ++j) addHighwayCell(idx == 0 ? snapshots[j].p1 : snapshots[j].p2);
                        }
                    } else {
                        h.candidate = false; h.streakCycles = 0;
                    }
                }
            } else if (repeatPairStart(idx, localStep - 2*HIGHWAY_PERIOD)) {
                h.candidate = true; h.streakCycles = 2; h.lastMatchLocal = localStep;
                h.candidateStartLocal = localStep - 2*HIGHWAY_PERIOD;
            }
        }
    }

    LoopKey currentLoopKey() {
        auto xp = xPow(ant[0].x); auto yp = yPow(ant[0].y);
        return {ant[1].x - ant[0].x, ant[1].y - ant[0].y, dir[0], dir[1], boardHash1 * xp.ai * yp.bi, boardHash2 * xp.ci * yp.di};
    }
    void rememberLoopState() {
        if (loopFound) return;
        LoopKey key = currentLoopKey();
        auto it = seenStates.find(key);
        if (it != seenStates.end()) {
            loopFound = true; loopFirstStep = it->second; loopDetectedStep = totalSteps; loopPeriod = totalSteps - it->second;
        } else seenStates.emplace(key, totalSteps);
    }

    void stepOne() {
        // 両方とも盤面更新前の色を読み、向きと反転対象を決めてから、同時に反転・移動する。
        const bool black0 = isBlack(ant[0]);
        const bool black1 = isBlack(ant[1]);
        const Coord old0 = ant[0], old1 = ant[1];
        const uint8_t t0 = black0 ? 1 : 0; // 1=左折、0=右折
        const uint8_t t1 = black1 ? 1 : 0;
        dir[0] = (dir[0] + (black0 ? 3 : 1)) % 4;
        dir[1] = (dir[1] + (black1 ? 3 : 1)) % 4;
        if (old0 == old1) setBlack(old0, !black0);
        else { setBlack(old0, !black0); setBlack(old1, !black1); }
        static const int dx[4] = {0, 1, 0, -1};
        static const int dy[4] = {-1, 0, 1, 0};
        ant[0].x += dx[dir[0]]; ant[0].y += dy[dir[0]];
        ant[1].x += dx[dir[1]]; ant[1].y += dy[dir[1]];
        ++totalSteps;
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        turns[0].push_back(t0); turns[1].push_back(t1);
        turnPrefix[0].push_back(turnPrefix[0].back() * HASH_POLY + static_cast<uint64_t>(t0 + 1));
        turnPrefix[1].push_back(turnPrefix[1].back() * HASH_POLY + static_cast<uint64_t>(t1 + 1));
        snapshots.push_back({ant[0], ant[1], dir[0], dir[1]});
        ensureHashPower(turns[0].size());
        analyseHighways(snapshots.size()-1);
        rememberLoopState();
    }

    void runSteps(uint64_t n) { n = std::min<uint64_t>(n, 5000); for (uint64_t i=0; i<n; ++i) stepOne(); }

    uint64_t nonHighwayBlackCount() const { return static_cast<uint64_t>(blackCells.size()) - highwayBlackCount; }
    static std::string dirName(int d) { static const char* names[] = {"上", "右", "下", "左"}; return names[(d%4+4)%4]; }
    static std::string jsonEscape(const std::string& s) {
        std::ostringstream o; for (unsigned char c : s) { if (c=='"') o << "\\\""; else if (c=='\\') o << "\\\\"; else if (c=='\n') o << "\\n"; else if (c=='\r') o << "\\r"; else if (c=='\t') o << "\\t"; else if (c < 32) o << ' '; else o << c; } return o.str();
    }
    std::string stateJson() const {
        std::ostringstream o;
        o << "{\"steps\":" << totalSteps
          << ",\"spacing\":" << spacing
          << ",\"ants\":["
          << "{\"x\":" << ant[0].x << ",\"y\":" << ant[0].y << ",\"dir\":" << dir[0] << ",\"highway\":" << (hw[0].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[0].confirmed?std::to_string(hw[0].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[0].confirmed?std::to_string(hw[0].confirmedGlobal):"null") << "},"
          << "{\"x\":" << ant[1].x << ",\"y\":" << ant[1].y << ",\"dir\":" << dir[1] << ",\"highway\":" << (hw[1].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[1].confirmed?std::to_string(hw[1].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[1].confirmed?std::to_string(hw[1].confirmedGlobal):"null") << "}],"
          << "\"blackCells\":" << blackCells.size() << ",\"visitedCells\":" << visitedCells.size()
          << ",\"highwayCells\":" << highwayCells.size() << ",\"nonHighwayBlackCells\":" << nonHighwayBlackCount()
          << ",\"bothHighway\":" << ((hw[0].confirmed && hw[1].confirmed)?"true":"false")
          << ",\"loop\":{\"found\":" << (loopFound?"true":"false") << ",\"firstStep\":" << (loopFound?std::to_string(loopFirstStep):"null") << ",\"detectedStep\":" << (loopFound?std::to_string(loopDetectedStep):"null") << ",\"period\":" << (loopFound?std::to_string(loopPeriod):"null") << "}"
          << ",\"epochStartStep\":" << epochStartStep << "}";
        return o.str();
    }

    std::string visibleJson(int64_t x0, int64_t y0, int64_t w, int64_t h) const {
        w = std::clamp<int64_t>(w, 1, 400); h = std::clamp<int64_t>(h, 1, 300);
        std::ostringstream o; o << "{\"black\":["; bool first=true;
        for (int64_t y=y0; y<y0+h; ++y) { auto it=blackRows.find(y); if (it==blackRows.end()) continue; for (int64_t x:it->second) if (x>=x0 && x<x0+w) { if(!first)o<<','; first=false; o<<'['<<x<<','<<y<<']'; } }
        o << "],\"highway\":["; first=true;
        for (int64_t y=y0; y<y0+h; ++y) { auto it=highwayRows.find(y); if (it==highwayRows.end()) continue; for (int64_t x:it->second) if (x>=x0 && x<x0+w) { if(!first)o<<','; first=false; o<<'['<<x<<','<<y<<']'; } }
        o << "]}"; return o.str();
    }
};

Simulation sim;
std::mutex simMutex;

static std::string readFile(const std::string& name) {
    std::ifstream f(name, std::ios::binary); if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}
static std::string getQueryValue(const std::string& query, const std::string& key, const std::string& fallback="0") {
    std::istringstream ss(query); std::string item;
    while (std::getline(ss, item, '&')) { auto p=item.find('='); if (p!=std::string::npos && item.substr(0,p)==key) return item.substr(p+1); }
    return fallback;
}
static long long numQuery(const std::string& q, const std::string& key, long long fallback=0) {
    try { auto v=getQueryValue(q,key,std::to_string(fallback)); size_t n=0; long long x=std::stoll(v,&n); return n?x:fallback; } catch(...) { return fallback; }
}
static long long numJson(const std::string& body, const std::string& key, long long fallback=0) {
    std::string tag="\""+key+"\""; auto p=body.find(tag); if(p==std::string::npos) return fallback; p=body.find(':',p+tag.size()); if(p==std::string::npos) return fallback; ++p; while(p<body.size()&&std::isspace(static_cast<unsigned char>(body[p])))++p;
    try { size_t n=0; long long v=std::stoll(body.substr(p), &n); return n?v:fallback; } catch(...) { return fallback; }
}
static std::string contentType(const std::string& p) {
    if (p.size()>=5 && p.substr(p.size()-5)==".html") return "text/html; charset=utf-8";
    if (p.size()>=4 && p.substr(p.size()-4)==".css") return "text/css; charset=utf-8";
    if (p.size()>=3 && p.substr(p.size()-3)==".js") return "application/javascript; charset=utf-8";
    return "text/plain; charset=utf-8";
}
static void sendResponse(socket_t s, int status, const std::string& type, const std::string& body) {
    std::string reason = status==200?"OK":status==400?"Bad Request":status==404?"Not Found":status==405?"Method Not Allowed":"Internal Server Error";
    std::ostringstream h; h << "HTTP/1.1 "<<status<<' '<<reason<<"\r\nContent-Type: "<<type<<"\r\nContent-Length: "<<body.size()<<"\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    std::string head=h.str();
    auto sendBytes=[&](const char* data, size_t len) { size_t sent=0; while(sent<len) { int r=send(s,data+sent,static_cast<int>(std::min<size_t>(len-sent, 1<<20)),0); if(r<=0)break; sent+=static_cast<size_t>(r); } };
    sendBytes(head.data(),head.size()); sendBytes(body.data(),body.size());
}
static std::string mimeSafePath(const std::string& route) {
    if (route=="/" || route=="/index.html") return "index.html";
    if (route=="/style.css") return "style.css";
    if (route=="/app.js") return "app.js";
    return {};
}
static void handleClient(socket_t s) {
    std::string req; req.reserve(8192); char buf[4096]; size_t headerEnd=std::string::npos; size_t expected=0;
    while (req.size() < 2*1024*1024) {
        int n=recv(s,buf,sizeof(buf),0); if(n<=0) { closeSocket(s); return; }
        req.append(buf,static_cast<size_t>(n));
        if (headerEnd==std::string::npos) {
            headerEnd=req.find("\r\n\r\n");
            if (headerEnd!=std::string::npos) {
                std::string header=req.substr(0,headerEnd); auto cp=header.find("Content-Length:");
                if(cp==std::string::npos) cp=header.find("content-length:");
                if(cp!=std::string::npos) { cp=header.find(':',cp)+1; while(cp<header.size()&&std::isspace(static_cast<unsigned char>(header[cp])))++cp; try{expected=static_cast<size_t>(std::stoull(header.substr(cp)));}catch(...){expected=0;} }
            }
        }
        if(headerEnd!=std::string::npos && req.size()>=headerEnd+4+expected) break;
    }
    if(headerEnd==std::string::npos) { sendResponse(s,400,"text/plain; charset=utf-8","Bad request"); closeSocket(s); return; }
    std::string header=req.substr(0,headerEnd); std::string body=req.substr(headerEnd+4, expected);
    std::istringstream hs(header); std::string method,target,version; hs>>method>>target>>version;
    auto qp=target.find('?'); std::string route=target.substr(0,qp), query=qp==std::string::npos?"":target.substr(qp+1);
    if(method=="GET" && route.rfind("/api/state",0)==0) {
        std::lock_guard<std::mutex> lock(simMutex); sendResponse(s,200,"application/json; charset=utf-8",sim.stateJson());
    } else if(method=="GET" && route=="/api/visible") {
        int64_t x0=numQuery(query,"x0"),y0=numQuery(query,"y0"),w=numQuery(query,"w",80),h=numQuery(query,"h",45);
        std::lock_guard<std::mutex> lock(simMutex); sendResponse(s,200,"application/json; charset=utf-8",sim.visibleJson(x0,y0,w,h));
    } else if(method=="POST" && route=="/api/step") {
        uint64_t n=static_cast<uint64_t>(std::clamp<long long>(numJson(body,"count",1),1,5000));
        std::lock_guard<std::mutex> lock(simMutex); sim.runSteps(n); sendResponse(s,200,"application/json; charset=utf-8",sim.stateJson());
    } else if(method=="POST" && route=="/api/reset") {
        int sp=static_cast<int>(std::clamp<long long>(numJson(body,"spacing",10),1,1000000)); int d1=static_cast<int>(numJson(body,"dir1",0)), d2=static_cast<int>(numJson(body,"dir2",0));
        std::lock_guard<std::mutex> lock(simMutex); sim.reset(sp,d1,d2); sendResponse(s,200,"application/json; charset=utf-8",sim.stateJson());
    } else if(method=="POST" && route=="/api/spacing") {
        int sp=static_cast<int>(std::clamp<long long>(numJson(body,"spacing",10),1,1000000));
        std::lock_guard<std::mutex> lock(simMutex); sim.repositionSpacing(sp); sendResponse(s,200,"application/json; charset=utf-8",sim.stateJson());
    } else if(method=="POST" && route=="/api/direction") {
        int idx=static_cast<int>(numJson(body,"ant",0)), d=static_cast<int>(numJson(body,"dir",0));
        std::lock_guard<std::mutex> lock(simMutex); sim.changeDirection(idx,d); sendResponse(s,200,"application/json; charset=utf-8",sim.stateJson());
    } else if(method=="GET") {
        std::string path=mimeSafePath(route); if(path.empty()) sendResponse(s,404,"text/plain; charset=utf-8","Not found"); else { auto file=readFile(path); if(file.empty()) sendResponse(s,404,"text/plain; charset=utf-8","File not found. Run the server from its project folder."); else sendResponse(s,200,contentType(path),file); }
    } else sendResponse(s,405,"text/plain; charset=utf-8","Method not allowed");
    closeSocket(s);
}
}

int main(int argc, char** argv) {
    // Local development defaults to loopback. Hosted platforms can set PORT and HOST.
    int port = 8080;
    if (const char* envPort = std::getenv("PORT")) {
        try { port = std::clamp(std::stoi(envPort), 1, 65535); } catch(...) {}
    }
    if (argc > 1) { try { port=std::clamp(std::stoi(argv[1]),1,65535); } catch(...){} }
    std::string bindHost = "127.0.0.1";
    if (const char* envHost = std::getenv("HOST")) {
        if (*envHost) bindHost = envHost;
    }
#ifdef _WIN32
    WSADATA wsa; if (WSAStartup(MAKEWORD(2,2), &wsa)!=0) { std::cerr << "Winsock の初期化に失敗しました。\n"; return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    socket_t server=socket(AF_INET,SOCK_STREAM,0);
#ifdef _WIN32
    if(server==INVALID_SOCKET) { std::cerr << "ソケットを作成できません。\n"; WSACleanup(); return 1; }
#else
    if(server<0) { std::cerr << "ソケットを作成できません。\n"; return 1; }
#endif
    int yes=1;
#ifdef _WIN32
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&yes),sizeof(yes));
#else
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
#endif
    sockaddr_in addr{}; addr.sin_family=AF_INET;
    if (bindHost == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bindHost.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "HOSTにはIPv4アドレスを指定してください（例: 127.0.0.1 または 0.0.0.0）。\n";
        closeSocket(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    addr.sin_port=htons(static_cast<uint16_t>(port));
    if(bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0 || listen(server,32)<0) {
        std::cerr << "ポート "<<port<<" を使用できません。別のポート番号を指定してください。\n"; closeSocket(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "Langton's Ant C++ サーバーを起動しました。\n待受アドレス: "<<bindHost<<":"<<port<<"\n";
    if (bindHost == "127.0.0.1") std::cout << "ブラウザーで http://127.0.0.1:"<<port<<" を開いてください。\n";
    std::cout << "終了するには Ctrl+C を押してください。\n" << std::flush;
    while(true) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int clientLen=sizeof(clientAddr);
#else
        socklen_t clientLen=sizeof(clientAddr);
#endif
        socket_t client=accept(server,reinterpret_cast<sockaddr*>(&clientAddr),&clientLen);
#ifdef _WIN32
        if(client==INVALID_SOCKET) continue;
#else
        if(client<0) continue;
#endif
        std::thread(handleClient,client).detach();
    }
    closeSocket(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
#include <emscripten.h>
#include <vector>

// 蟻の状態や盤面（簡易例）
struct Ant {
    int x = 0;
    int y = 0;
    int dir = 0; // 0:上, 1:右, 2:下, 3:左
};

static Ant ant1;
static long long total_steps = 0;

extern "C" {

// 初期化関数
EMSCRIPTEN_KEEPALIVE
void init_simulation() {
    ant1.x = 0;
    ant1.y = 0;
    ant1.dir = 0;
    total_steps = 0;
}

// N歩一気に進める（JSからの通信オーバーヘッドを減らすためまとめて実行）
EMSCRIPTEN_KEEPALIVE
void step_n(int n) {
    for (int i = 0; i < n; ++i) {
        // --- ここにラングトンのアリの移動・盤面更新ロジックを書く ---
        total_steps++;
    }
}

// 状態取得用
EMSCRIPTEN_KEEPALIVE
int get_ant1_x() { return ant1.x; }

EMSCRIPTEN_KEEPALIVE
int get_ant1_y() { return ant1.y; }

EMSCRIPTEN_KEEPALIVE
long long get_total_steps() { return total_steps; }

}