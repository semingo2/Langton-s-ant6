#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <emscripten.h>

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
    int dir[2] = {0, 0};
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

    void setSpacing(int newSpacing) {
        spacing = std::clamp(newSpacing, 1, 1000000);
        int64_t x1 = -(static_cast<int64_t>(spacing) / 2);
        ant[0] = {x1, 0}; ant[1] = {x1 + spacing, 0};
        visitedCells.insert(ant[0]); visitedCells.insert(ant[1]);
        startNewAnalysisEpoch();
    }
    void setDirection(int idx, int d) {
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
        const bool black0 = isBlack(ant[0]);
        const bool black1 = isBlack(ant[1]);
        const Coord old0 = ant[0], old1 = ant[1];
        const uint8_t t0 = black0 ? 1 : 0;
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

    std::string stateJson() const {
        std::ostringstream o;
        o << "{\"steps\":" << totalSteps
          << ",\"spacing\":" << spacing
          << ",\"ants\":["
          << "{\"x\":" << ant[0].x << ",\"y\":" << ant[0].y << ",\"dir\":" << dir[0] << ",\"highway\":" << (hw[0].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[0].confirmed?std::to_string(hw[0].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[0].confirmed?std::to_string(hw[0].confirmedGlobal):"null") << "},"
          << "{\"x\":" << ant[1].x << ",\"y\":" << ant[1].y << ",\"dir\":" << dir[1] << ",\"highway\":" << (hw[1].confirmed?"true":"false") << ",\"highwayStart\":" << (hw[1].confirmed?std::to_string(hw[1].startGlobal):"null") << ",\"highwayConfirmedAt\":" << (hw[1].confirmed?std::to_string(hw[1].confirmedGlobal):"null") << "}],"
          << ",\"blackCells\":" << blackCells.size() << ",\"visitedCells\":" << visitedCells.size()
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

} // namespace

// WebAssembly (JavaScript) 連携用 C関数群
extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* wasm_get_state() {
    static std::string res;
    res = sim.stateJson();
    return res.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_get_visible(int x0, int y0, int w, int h) {
    static std::string res;
    res = sim.visibleJson(x0, y0, w, h);
    return res.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_step(int count) {
    sim.runSteps(count);
    return wasm_get_state();
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_reset(int spacing, int dir1, int dir2) {
    sim.reset(spacing, dir1, dir2);
    return wasm_get_state();
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_set_spacing(int spacing) {
    sim.setSpacing(spacing);
    return wasm_get_state();
}

EMSCRIPTEN_KEEPALIVE
const char* wasm_set_direction(int ant, int dir) {
    sim.setDirection(ant, dir);
    return wasm_get_state();
}

}

int main() {
    // Wasmビルド用のメイン関数（サーバー起動処理などは行いません）
    return 0;
}
