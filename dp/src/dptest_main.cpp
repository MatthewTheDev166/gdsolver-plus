// dptest -- the header core's rules that can be decided without a level.
//
// The suite (py/quick_regress.py) compares whole sections against a corpus, which is the right
// instrument for a change that moves the model -- and the wrong one for a boundary that the
// corpus happens not to contain. The recording overlay is such a boundary: the corpus'
// recordings predate the `end` trailer, so quick_regress cannot see the rule at all.
//
//   dptest            runs every case, prints one line each, exit 1 on the first failure
//   dptest --groups-corpus <file>...
//                     reads each --groups recording with parseGroupTimeline and with the sscanf
//                     reader it replaced, and says whether they agree bit for bit
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "dp/cli.hpp"
#include "dp/band_rows.hpp"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) g_fail = 1;
}

std::string writeTmp(const std::string& name, const std::string& body) {
    const std::string p =
        (std::filesystem::temp_directory_path() / name).string();
    std::ofstream f(p, std::ios::trunc);
    f << body;
    return p;
}

// Does the merged timeline hold a row at this tick with this cy?
bool has(const dp::GroupTimeline& g, int uid, int t, float cy) {
    const auto it = g.find(uid);
    if (it == g.end()) return false;
    for (const dp::DynSample& s : it->second)
        if (s.t == t && s.cy == cy) return true;
    return false;
}

// THE OVERLAY'S REACH. `over` is the newer recording: it wins for every tick its RUN covered,
// which is its `end` trailer when it has one and its last row when it does not. The rows here
// are far enough apart that the bootstrap re-timing finds no fit, so the base keeps its own
// ticks and the only thing under test is the reach.
void overlayReach() {
    const std::string over = writeTmp("dptest_over.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "10,7,0,200,30,3,1,0\n"
        "end,50\n");
    const std::string base = writeTmp("dptest_base.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "5,7,0,300,30,3,1,0\n"
        "20,7,0,400,30,3,1,0\n"
        "50,7,0,500,30,3,1,0\n"
        "60,7,0,600,30,3,1,0\n");

    long long end = -1;
    dp::GroupTimeline o = dp::loadGroupTimeline(over, &end);
    check(end == 50, "the `end` trailer is read back as the run's end");
    check(o[7].size() == 2, "the trailer is not taken for a row");

    dp::GroupTimeline g = dp::loadGroupTimeline(base);
    dp::overlayGroupTimeline(g, o, false, end);
    check(has(g, 7, 10, 200.f), "the newer recording's own rows are kept");
    check(!has(g, 7, 20, 400.f),
          "a base row after the newer file's last row but inside its run is dropped");
    check(!has(g, 7, 50, 500.f), "a base row at t == end is dropped (the run covers it)");
    check(has(g, 7, 60, 600.f), "a base row past the end is kept");

    long long none = -1;
    dp::GroupTimeline o2 = dp::loadGroupTimeline(writeTmp("dptest_over2.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "10,7,0,200,30,3,1,0\n"), &none);
    check(none == -1, "a recording without the trailer says so");
    dp::GroupTimeline g2 = dp::loadGroupTimeline(base);
    dp::overlayGroupTimeline(g2, o2, false, none);
    check(has(g2, 7, 20, 400.f) && has(g2, 7, 50, 500.f) && has(g2, 7, 60, 600.f),
          "without a trailer the reach is the last row, as before");
}

// THE t=0 SNAPSHOT. The mod writes `init,<uid>,<on>` right after the game's reset; the reader
// hands them out on request and otherwise skips them, and they are never taken for a row.
void initLines() {
    const std::string p = writeTmp("dptest_init.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "init,7,1\n"
        "init,8,0\n"
        "1,7,0,100,30,3,0,0\n"
        "5,8,0,200,30,3,1,0\n"
        "end,9\n");
    std::unordered_map<int, uint8_t> init;
    long long end = -1;
    const dp::GroupTimeline g = dp::loadGroupTimeline(p, &end, &init);
    check(init.size() == 2 && init.at(7) == 1 && init.at(8) == 0,
          "each init line gives its object's on at t=0");
    check(g.at(7).size() == 1 && g.at(8).size() == 1 && end == 9,
          "the init lines are not rows, and the trailer still reads");
    const dp::GroupTimeline g0 = dp::loadGroupTimeline(p);
    check(g0.at(7).size() == 1 && g0.at(8).size() == 1,
          "a caller that does not ask gets the same rows as before");
}

// The objrects header as the mod writes it (src/solver/solver.hpp). `nocol` is the 50th column,
// so a fixture that put it anywhere else would test a layout no dump has.
const char* const kObjHeader =
    "id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0,"
    "tpy,tpg,tpix,tpiy,tw,zoom,zdur,zease,zrate,mvdir,gnddir,"
    "optp1,optp2,flipx,flipy,nofx,notouch,tpex,tpey,dis,"
    "editvel,vmodx,vmody,ovrvel,force,free,touch,spawn,chan,axis,exstat,rev,"
    "nocol";

// One 30x30 static solid. Every column past `rot` is empty (what an older dump leaves there)
// except `nocol`, which is written when `withNocol` says the dump has that column at all.
std::string solidRow(int id, int uid, double cx, bool withNocol, const char* nocol) {
    std::string r = std::to_string(id) + ",0," + std::to_string(cx) + ",100,30,30,0,"
                    + std::to_string(uid) + ",0,0";
    if (withNocol) {
        for (int i = 10; i < 49; ++i) r += ",";
        r += ",";
        r += nocol;
    }
    return r + "\n";
}

bool keeps(const dp::Level& L, int uid) {
    for (const dp::Obj& o : L.objs)
        if (o.uid == uid) return true;
    return false;
}

// WHICH OBJECTS THE MODEL DOES NOT COLLIDE WITH. GD skips an object whose byte [obj+0x515] is
// set, whatever its id; the model used to stand in for that byte with id 1910. With the `nocol`
// column the byte decides, and without it the id rule stands. The corpus cannot test the new
// branch: every stored dump predates the column, so all 1,116 sections take the fallback.
void noCollideColumn() {
    {
        std::istringstream in(std::string(kObjHeader) + "\n"
                              + solidRow(1910, 11, 100, true, "0")
                              + solidRow(1, 12, 200, true, "1")
                              + solidRow(1, 13, 300, true, "0")
                              + solidRow(1910, 14, 400, true, ""));
        const dp::Level L = dp::loadLevelFrom(in);
        check(keeps(L, 11), "with the column, an id-1910 object whose byte is 0 stays solid");
        check(!keeps(L, 12), "with the column, an id-1 object whose byte is 1 is dropped");
        check(keeps(L, 13), "with the column, an id-1 object whose byte is 0 stays solid");
        check(!keeps(L, 14), "an empty cell falls back to the id rule for that row");
    }
    {
        std::istringstream in(std::string(kObjHeader).substr(0, std::string(kObjHeader).size()
                                                                    - std::string(",nocol").size())
                              + "\n"
                              + solidRow(1910, 21, 100, false, "")
                              + solidRow(1, 22, 200, false, ""));
        const dp::Level L = dp::loadLevelFrom(in);
        check(!keeps(L, 21), "without the column, id 1910 is dropped as before");
        check(keeps(L, 22), "without the column, any other id stays solid");
    }
}

// THE LEVEL TEXT'S PASS, KEPT FROM CALL TO CALL (CsvRows, level_loader.hpp). A call used to read the whole text into
// rows, take each row's uid and place, drop GD's anti-cheat spike row and write the rest out again before it read a
// row; it now reads its rows from the text a first call has made that pass over. The same level through both must be
// the same level, and the rows a call is handed the rows the old pass would have handed it.
void keptTextPass() {
    // (1) The rows. getline over the text the old pass wrote (every kept row, '\n' ended) against nextCsvRow over the
    // kept pass's text: a last row without a newline, empty rows, a '\r' before the '\n', the empty text.
    const char* const texts[] = {"", "h", "h\n", "h\nr1", "h\nr1\n", "h\n\nr1\n\n", "h\r\nr1\r\n", "a\nb\n\nc", "\n", "\n\n"};
    for (const char* t : texts) {
        const std::string text = t;
        std::vector<std::string> rows;
        {
            std::istringstream in(text);
            std::string row;
            while (std::getline(in, row)) rows.push_back(row);
        }
        std::string all;
        for (const std::string& r : rows) { all += r; all.push_back('\n'); }
        std::vector<std::string> slow;
        {
            std::istringstream in(all);
            std::string row;
            while (std::getline(in, row)) slow.push_back(row);
        }
        const dp::CsvRows kept = dp::buildCsvRows(text);
        const std::string_view src = kept.noDrop ? std::string_view(text) : std::string_view(kept.filtered);
        std::vector<std::string> fast;
        size_t pos = 0;
        std::string row;
        while (dp::nextCsvRow(src, pos, row)) fast.push_back(row);
        check(fast == slow, "the kept pass hands a call the rows the old pass wrote, edge texts included");
    }
    // (2) A level with the anti-cheat spike (the highest uid, id 8 at (0,105), 6x12, 30x30 unscaled), a row with too
    // few fields for the uid pass, and solids: dropped, skipped, kept.
    auto joined = [](const std::vector<std::string>& f) {
        std::string s;
        for (size_t i = 0; i < f.size(); ++i) s += (i ? "," : "") + f[i];
        return s;
    };
    std::vector<std::string> spike(17, "");
    spike[0] = "8"; spike[1] = "0"; spike[2] = "0"; spike[3] = "105"; spike[4] = "6"; spike[5] = "12";
    spike[6] = "0"; spike[7] = "99"; spike[8] = "0"; spike[9] = "0"; spike[15] = "30"; spike[16] = "30";
    const std::string text = std::string(kObjHeader) + "\n" + solidRow(1, 11, 100, false, "") + "1,0,50,100\n"
                             + solidRow(1, 12, 200, false, "") + solidRow(1, 13, 300, false, "") + joined(spike) + "\n";
    const dp::CsvRows kept = dp::buildCsvRows(text);
    check(!kept.noDrop && kept.maxUid == 99, "the spike is the highest uid and is dropped");
    check(kept.places.size() == 4 && kept.places[0].uid == 11 && kept.places[0].cx == 100.0
              && kept.places[1].uid == 12 && kept.places[3].uid == 99,
          "the places are the rows with eight fields or more, the short one left out");
    check(kept.filtered.find(joined(spike)) == std::string::npos && kept.filtered.find("\n1,0,50,100\n") != std::string::npos,
          "the filtered text has no spike row and keeps the short one");
    std::istringstream in(text);
    const dp::Level slowL = dp::loadLevelFrom(in);
    std::istringstream none;
    const dp::Level fastL = dp::loadLevelFrom(none, nullptr, nullptr, nullptr, &kept, &text);
    bool same = slowL.objs.size() == fastL.objs.size();
    for (size_t i = 0; same && i < slowL.objs.size(); ++i) {
        const dp::Obj& a = slowL.objs[i];
        const dp::Obj& b = fastL.objs[i];
        same = a.uid == b.uid && a.cx == b.cx && a.cy == b.cy && a.hw == b.hw && a.hh == b.hh && a.id == b.id;
    }
    check(same && keeps(fastL, 11) && keeps(fastL, 13) && !keeps(fastL, 99),
          "a level read from the kept pass is the level the old pass gave, spike gone");
    // (3) No spike at all: the text itself is what the rows are read from.
    const std::string plain = std::string(kObjHeader) + "\n" + solidRow(1, 11, 100, false, "");
    check(dp::buildCsvRows(plain).noDrop, "nothing dropped: the kept pass keeps no copy of the text");
}

// AN OBJECT'S ON/OFF AS A SEQUENCE OF SWITCHES (five boundaries). A touch mask
// cannot carry any of these -- it only ever gains bits -- so each case is one the mask form gets
// wrong.
void onOffEvents() {
    uint16_t fireB[dp::kTouchBits] = {};
    auto on = [&](bool init, const std::vector<dp::OnEvent>& ev, dp::TouchMask mask, int t) {
        return dp::resolveOn(init, ev.data(), ev.size(), mask, fireB, t);
    };
    auto boxEv = [](int box, bool isOn, double delay) {
        dp::OnEvent e;
        e.box = box;
        e.on = isOn ? 1 : 0;
        e.delay = delay;
        return e;
    };
    const dp::TouchMask b0 = dp::touchBit(0), b1 = dp::touchBit(1);

    // (1) a spawn delay: the switch lands `delay` ticks after the box, not with it
    fireB[0] = 100;
    const std::vector<dp::OnEvent> delayed{boxEv(0, true, 10.0)};
    check(!on(false, delayed, b0, 109), "(1) one tick before the delay runs out it is still off");
    check(on(false, delayed, b0, 110), "(1) on the tick the delay runs out it is on");
    check(!on(false, delayed, dp::TouchMask{}, 500), "(1) a state that never entered the box: off");

    // (2) a Toggle that switches OFF (which a mask cannot say at all)
    const std::vector<dp::OnEvent> off{boxEv(0, false, 0.0)};
    check(on(true, off, b0, 99), "(2) before the box it is on");
    check(!on(true, off, b0, 100), "(2) from the box on it is off");

    // (3) two chains on one object: the later switch wins, whichever box it came from
    fireB[0] = 100;
    fireB[1] = 150;
    const std::vector<dp::OnEvent> onThenOff{boxEv(0, true, 0.0), boxEv(1, false, 0.0)};
    check(on(false, onThenOff, b0 | b1, 120), "(3) on at 100, off at 150: on at 120");
    check(!on(false, onThenOff, b0 | b1, 160), "(3) on at 100, off at 150: off at 160");
    const std::vector<dp::OnEvent> offThenOn{boxEv(0, false, 0.0), boxEv(1, true, 0.0)};
    check(on(true, offThenOn, b0 | b1, 160), "(3) off at 100, on at 150: on at 160");
    // ...and it is the EFFECTIVE tick that orders them: the box entered first can land last
    const std::vector<dp::OnEvent> slowFirst{boxEv(0, true, 100.0), boxEv(1, false, 0.0)};
    check(!on(false, slowFirst, b0 | b1, 190), "(3) off lands at 150, on at 200: off at 190");
    check(on(false, slowFirst, b0 | b1, 210), "(3) off lands at 150, on at 200: on at 210");
    // ...and a box the state did not enter takes no part
    check(on(false, onThenOff, b0, 160), "(3) without the second box it stays on");

    // (4) and (5): which roots give switches at all. A Tap and a Count set their bit on the
    // first press or item, so their chain's Toggle cannot be read off the bit; an Item Compare
    // (a Count root with cmode 3) has its own gate for coins in cli.hpp.
    std::vector<dp::TouchTrig> boxes(4);
    for (dp::TouchTrig& T : boxes) T.togEv.push_back({42, 1, 0.0});
    boxes[1].tap = true;
    boxes[2].count = 3;
    boxes[3].count = 6;
    boxes[3].cmode = 3;
    const auto ev = dp::boxOnEvents(boxes);
    const auto it = ev.find(42);
    check(it != ev.end() && it->second.size() == 1 && it->second[0].box == 0,
          "(4)(5) only the plain box gives a switch -- not the Tap, the Count or the Item Compare");
}

// A pickup row as the mod dumps it (SubZero 4002's key uid 6166, with uid / x / target / actgrp
// swapped in). actgrp is the 41st column.
std::string pickupRow(int uid, double cx, int target, int actgrp, int id = 1275,
                      int touch = 0) {
    return std::to_string(uid) + "," + std::to_string(id) + "," + std::to_string(cx)
           + ",985,25,20," + std::to_string(target) + ",0," + std::to_string(touch)
           + ",0,0.5,0,0,0,2,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,-,0,0,-1,0,"
           + std::to_string(actgrp) + ",0,0,0,-1,-1,-1,-1,0,0,-1,-1,-1,0,-1,-1,-1,-1\n";
}

const dp::TouchTrig* boxOf(const std::vector<dp::TouchTrig>& v, int uid) {
    for (const dp::TouchTrig& t : v)
        if (t.uid == uid) return &t;
    return nullptr;
}

// WHICH PICKUPS BECOME BOXES (--activators). Only one that switches its group ON, and only when
// everything in the group is something to stand on, collect or press: custom levels have pickups
// that bring hazards in or take coins away, and those are left out rather than guessed at.
void activatorRoots() {
    const std::string trig = writeTmp("dptest_act_trig.txt",
        "uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,ease,erate,lockx,locky,grav,"
        "gravmod,deg,ord,chan,sord,sordd,t360,lockrot,sdelay,mvtgt,mvaxis,tmodctr,dirsnap,"
        "dirdist,dynmode,silent,togon,remap,item,item2,count,subcount,actgrp,thold,ttog,tdual,"
        "cmode,i1mode,i2mode,tgtmode,mod1,mod2,res1,res2,res3,tol,rnd1,rnd2,sgn1,sgn2\n"
        + pickupRow(101, 1000, 10, 1)    // two solids
        + pickupRow(102, 2000, 20, 1)    // a solid and a hazard
        + pickupRow(103, 3000, 30, 1)    // an orb
        + pickupRow(104, 4000, 40, 0)    // switches OFF
        + pickupRow(105, 5000, 50, 1, 1594, 1));   // a toggle block (the dump says touch=1)
    const std::string grp = writeTmp("dptest_act_grp.txt",
        "uid,groups\n1001 10\n1002 10\n2001 20\n2002 20\n3001 30\n4001 40\n5001 50\n");
    const std::unordered_map<int, int> types{
        {101, 30}, {102, 30}, {103, 30}, {104, 30}, {105, 36},
        {1001, 0}, {1002, 0}, {2001, 0}, {2002, 2}, {3001, 35}, {4001, 0}, {5001, 35}};

    const auto on = dp::loadTouchTriggers(trig, grp, -1e18, &types);
    const dp::TouchTrig* k = boxOf(on, 101);
    bool both = k && k->activator && k->togEv.size() == 2;
    for (size_t i = 0; both && i < k->togEv.size(); ++i)
        both = k->togEv[i].on == 1 && k->togEv[i].delay == 0.0
               && (k->togEv[i].uid == 1001 || k->togEv[i].uid == 1002);
    check(both, "a pickup that switches two solids on is a box that switches both on");
    check(k && k->cx == 1000 && k->hw == 12.5 && k->hh == 10.0,
          "its box is the pickup's own rect");
    check(!boxOf(on, 102), "a pickup whose group holds a hazard is left out");
    check(boxOf(on, 103) != nullptr, "a pickup that switches an orb on is kept");
    check(!boxOf(on, 104), "a pickup that switches its group OFF is left out");
    const dp::TouchTrig* tb = boxOf(on, 105);
    check(tb && tb->activator && tb->press && !k->press,
          "a toggle block is an activator that needs the button; a pickup does not");
}

// A trigger row for the chain walks: the columns up to togon matter here (sdelay is the 27th,
// togon the 35th).
std::string trigRow(int uid, int id, double cx, int target, int spawn, int togon, double sdelay,
                    double ox) {
    std::string r = std::to_string(uid) + "," + std::to_string(id) + "," + std::to_string(cx)
                    + ",315,30,30," + std::to_string(target) + ",0,0," + std::to_string(spawn)
                    + ",0.5," + std::to_string(ox) + ",0,0,2,0,0,1,0,0,0,0,0,0,0,0,"
                    + std::to_string(sdelay) + ",0,0,0,0,0,0,0," + std::to_string(togon)
                    + ",-,0,0,-1,0,-1,0,0,0,-1,-1,-1,-1,0,0,-1,-1,-1,0,-1,-1,-1,-1\n";
    return r;
}

// THE x-CROSSING SWITCHES, and the whole of SubZero 4003's hidden ring through the resolver: on
// at the start, off when the player crosses x=6,225 (Toggle uid 3027), on again when the box
// (id 1594) fires. The crossing's tick is a level property the search resolves as it goes, so the
// event must read it then, not when it was built.
void autoSwitches() {
    const std::string trig = writeTmp("dptest_auto_trig.txt",
        "uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,ease,erate,lockx,locky,grav,"
        "gravmod,deg,ord,chan,sord,sordd,t360,lockrot,sdelay,mvtgt,mvaxis,tmodctr,dirsnap,"
        "dirdist,dynmode,silent,togon,remap,item,item2,count,subcount,actgrp,thold,ttog,tdual,"
        "cmode,i1mode,i2mode,tgtmode,mod1,mod2,res1,res2,res3,tol,rnd1,rnd2,sgn1,sgn2\n"
        + trigRow(3027, 1049, 6225, 145, 0, 0, 0.0, 0.0)     // x-crossing Toggle, off
        + trigRow(900, 901, 9000, 50, 0, -1, 0.0, 30.0)      // x-crossing Move, whose group
        + trigRow(910, 1049, 9001, 60, 1, 1, 0.5, 0.0));     // holds a spawn-only Toggle
    const std::string grp = writeTmp("dptest_auto_grp.txt",
        "uid,groups\n4256 145\n500 50\n910 50\n600 60\n");
    std::vector<dp::AutoTrig> autos = dp::loadAutoTriggers(trig, grp);
    std::sort(autos.begin(), autos.end(),
              [](const dp::AutoTrig& a, const dp::AutoTrig& b) { return a.cx < b.cx; });
    const auto ev = dp::autoOnEvents(autos);
    const auto ring = ev.find(4256);
    check(ring != ev.end() && ring->second.size() == 1 && ring->second[0].on == 0
              && ring->second[0].autoIdx == 0 && ring->second[0].delay == 0.0,
          "an x-crossing Toggle switches its group off, on its own crossing");
    // A Move root spawns nothing: the spawn-triggered Toggle in its target group is carried as an
    // object and waits for a Spawn or a box (triggers.hpp, the Move-root rule in loadAutoTriggers,
    // measured on a custom level's group 171). This check used to expect the Toggle's switch 120
    // ticks after the crossing, the rule that measurement overturned.
    check(ev.find(600) == ev.end(),
          "a Move a crossing fires does not spawn the Toggle in its target group");

    // The ring: the crossing's switch plus the box's, resolved the way the model will.
    std::vector<dp::OnEvent> r = ring->second;
    dp::OnEvent box;
    box.box = 0;
    box.on = 1;
    r.push_back(box);
    uint16_t fireB[dp::kTouchBits] = {};
    fireB[0] = 7750;
    const dp::TouchMask b0 = dp::touchBit(0);
    dp::g_autoTrig = autos;
    auto on = [&](dp::TouchMask m, int t) {
        return dp::resolveOn(true, r.data(), r.size(), m, fireB, t);
    };
    check(on(b0, 7000), "before the crossing is resolved the ring is as the reset left it (on)");
    dp::g_autoTrig[0].fireT = 6995;
    check(!on(b0, 7000), "once the crossing is at 6,995 the ring is off after it");
    check(on(b0, 6994), "...and still on the tick before");
    check(!on(dp::TouchMask{}, 8000), "a state that never pressed in the box: off for good");
    check(on(b0, 7760) && !on(b0, 7749), "a state whose box fired at 7,750: on from then");
    dp::g_autoTrig.clear();
}

// THE KEY HOLDS A DELAYED SWITCH. Two states with the same mask that fired a
// box at different ticks are in different worlds until the later one's switch has landed, so
// the dedupe key must keep them apart for that long -- and may merge them afterwards.
void keyHoldsDelayedSwitch() {
    const std::vector<dp::TouchTrig> saved = dp::g_touch;
    dp::g_touch.assign(1, dp::TouchTrig{});
    dp::g_touch[0].togEv.push_back({42, 1, 100.0});   // on, 100 ticks after the box
    dp::State a{};
    a.trig = dp::touchBit(0);
    a.fireB[0] = 100;
    dp::State b = a;
    b.fireB[0] = 150;
    // a's switch lands at 200, b's at 250: at 220 one world has it and the other does not.
    dp::buildTouchMoveTicks();
    check(dp::keyOf(a, 220) != dp::keyOf(b, 220),
          "while the later switch has not landed, the two states keep different keys");
    check(dp::keyOf(a, 400) == dp::keyOf(b, 400),
          "once both have landed they merge again");
    dp::g_touch = saved;
    dp::buildTouchMoveTicks();
}

}  // namespace

// splitCommaFields against the stream loop it replaced in loadLevelFrom, field by field, on the
// shapes a row can take: empty, leading/trailing/repeated commas, a CR left by the line read, and
// rows at and past the loader's column bound.
static void commaFields() {
    std::vector<std::string> rows{"", ",", "a", "a,", ",a", "a,,b", "a,b,", ",,,", "a\r",
                                  "a,b\r", " a , b ", "1,2,3"};
    std::string wide;
    for (int i = 0; i < dp::kObjFields + 6; ++i) wide += std::to_string(i) + ",";
    rows.push_back(wide);
    rows.push_back(wide.substr(0, wide.size() - 1));
    std::string exact;
    for (int i = 0; i < dp::kObjFields; ++i) exact += (i ? "," : "") + std::to_string(i);
    rows.push_back(exact);
    rows.push_back(exact + ",");
    bool same = true;
    for (const std::string& line : rows) {
        std::string a[dp::kObjFields], b[dp::kObjFields];
        std::stringstream ss(line);
        for (int i = 0; i < dp::kObjFields && std::getline(ss, a[i], ','); ++i) {}
        dp::splitCommaFields(line, b, dp::kObjFields);
        for (int i = 0; i < dp::kObjFields; ++i) same = same && a[i] == b[i];
    }
    check(same, "splitCommaFields fills the same fields as the getline loop it replaced");
}

// The reader loadGroupTimeline was until 2026-09-30 -- getline on a text-mode stream, sscanf per
// line -- kept as the reference parseGroupTimeline is held to.
static dp::GroupTimeline groupsByScanf(const std::string& path, long long* endOut,
                                       std::unordered_map<int, uint8_t>* initOut) {
    *endOut = -1;
    initOut->clear();
    dp::GroupTimeline g;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        int t = 0, uid = 0, on = 1, env = 0;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        if (line.rfind("end,", 0) == 0) {
            *endOut = std::atoll(line.c_str() + 4);
            continue;
        }
        if (line.rfind("init,", 0) == 0) {
            int u = 0, o = 1;
            if (std::sscanf(line.c_str() + 5, "%d,%d", &u, &o) == 2) (*initOut)[u] = o ? 1 : 0;
            continue;
        }
        const int n = std::sscanf(line.c_str(), "%d,%d,%f,%f,%f,%f,%d,%f,%d", &t, &uid, &cx, &cy,
                                  &w, &h, &on, &rot, &env);
        if (n < 6) continue;
        g[uid].push_back({t, cx, cy, w * 0.5f, h * 0.5f, (uint8_t)(on ? 1 : 0), rot,
                          (uint8_t)(env ? 1 : 0)});
    }
    for (auto& kv : g)
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const dp::DynSample& a, const dp::DynSample& b) { return a.t < b.t; });
    return g;
}

static bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// Empty when the two hold the same samples, bit for bit, AND iterate in the same order: the loader
// walks the timeline, so the order is part of what a reader hands on.
static std::string timelineDiff(const dp::GroupTimeline& a, const dp::GroupTimeline& b) {
    if (a.size() != b.size())
        return "objects " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (ia->first != ib->first) return "order differs at uid " + std::to_string(ia->first);
        const auto& va = ia->second;
        const auto& vb = ib->second;
        if (va.size() != vb.size()) return "uid " + std::to_string(ia->first) + " row count";
        for (size_t k = 0; k < va.size(); ++k) {
            const dp::DynSample& x = va[k];
            const dp::DynSample& y = vb[k];
            if (x.t != y.t || x.on != y.on || x.env != y.env || !sameBits(x.cx, y.cx)
                || !sameBits(x.cy, y.cy) || !sameBits(x.hw, y.hw) || !sameBits(x.hh, y.hh)
                || !sameBits(x.rot, y.rot))
                return "uid " + std::to_string(ia->first) + " t=" + std::to_string(x.t);
        }
    }
    return "";
}

// One file through both readers: the rows, the trailer and the init lines. Empty when they agree.
static std::string groupsReadersDiff(const std::string& path, long long* rows = nullptr) {
    long long endA = -1, endB = -1;
    std::unordered_map<int, uint8_t> initA, initB;
    const dp::GroupTimeline a = groupsByScanf(path, &endA, &initA);
    const dp::GroupTimeline b = dp::loadGroupTimeline(path, &endB, &initB);
    if (rows) {
        *rows = 0;
        for (const auto& kv : a) *rows += (long long)kv.second.size();
    }
    const std::string d = timelineDiff(a, b);
    if (!d.empty()) return d;
    if (endA != endB) return "end " + std::to_string(endA) + " vs " + std::to_string(endB);
    if (initA != initB) return "init lines";
    return "";
}

// parseGroupTimeline against the sscanf reader it replaced, on the shapes a line can take: a
// line cut off mid-write, blanks, signs, a CR, an exponent, inf/nan, hex, a trailing comma, more
// columns than the format, more digits than the plain path takes, float rounding midpoints -- and
// 20,000 rows of %.3f the way the recorder writes them, from small to 1e7. Once through a text
// stream (CRLF, as the mod writes) and once written as bytes (LF only).
static void groupsParser() {
    std::string body =
        "tick,uid,cx,cy,w,h,on,rot\n"
        "1,7,0,100,30,3,1,0\n"
        "2,7,0.125,100.5,30.000,3.000,1,45.500\n"
        "3,8,-0.000,-12.345,1.5,2.5,0,-90.000,1\n"
        "4,8,1e3,2E-2,3,4\n"
        "5,9, 7.5, 8.25,9,10,1,0\n"
        "6,9,.5,1.,+2.5,-.25,0,0\n"
        "7,10,1.5,2.5,3.5\n"
        "8,10,1.5,2.5,3.5,4.5,\n"
        "9,10,1.5,2.5,3.5,4.\n"
        "10,11,nan,inf,-inf,1,1,0\n"
        "11,11,123456.789,0.001,99999.999,0.0005,1,359.999\n"
        "12,11,1.2.3,4,5,6\n"
        "\n"
        "13,12,1,2,3,4,5,6,7,8\n"
        "14,12,0x1p3,2,3,4\n"
        "init,12,0\n"
        "init,13,1\n"
        "15,13,3.14159265358979,2.718281828,1,1\n"
        "16,13,16777217.000,33554433.5,1,1\n"
        "17,13,2097152.125,4194303.875,1,1\n"
        "18,13,0.1,0.2,0.3,0.7,1,0.9\n"
        "19,14,-5,6,7,8,1,0\r\n"
        "20,14,  -7.5  ,1,1,1\n"
        "21 ,15,1,1,1,1\n"
        "-22,15,1,1,1,1\n"
        "23,16,1,1,1,1,1,1,1\n"
        "24,16,1,1,1,1,0,0,0\n"
        "25,16,-,1,1,1\n"
        "26,16,1,.,1,1\n"
        "end,77\n";
    unsigned long long s = 0x9E3779B97F4A7C15ULL;
    auto next = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (double)(s >> 11) / 9007199254740992.0;
    };
    const double scales[] = {1.0, 30.0, 360.0, 5000.0, 40000.0, 2.1e6, 1.0e7};
    for (int i = 0; i < 20000; ++i) {
        const double sc = scales[i % 7];
        char b[256];
        std::snprintf(b, sizeof b, "%d,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f%s\n", 30 + i, 100 + i % 97,
                      (float)((next() - 0.5) * sc), (float)(next() * sc), (float)(next() * 60.0),
                      (float)(next() * 60.0), i % 2, (float)((next() - 0.5) * 720.0),
                      i % 5 == 0 ? ",1" : "");
        body += b;
    }
    body += "99999,17,1,2,3,4";   // the last line with no newline
    const std::string text = writeTmp("dptest_groups_text.txt", body);
    const std::string bin = (std::filesystem::temp_directory_path() / "dptest_groups_lf.txt").string();
    {
        std::ofstream f(bin, std::ios::binary | std::ios::trunc);
        f << body;
    }
    const std::string d1 = groupsReadersDiff(text);
    const std::string d2 = groupsReadersDiff(bin);
    if (!d1.empty()) std::printf("  text stream: %s\n", d1.c_str());
    if (!d2.empty()) std::printf("  bytes: %s\n", d2.c_str());
    check(d1.empty() && d2.empty(),
          "parseGroupTimeline reads every line the way the sscanf reader it replaced did");

    // groupLayersFor's first layer is a COPY of a file's parse (the parse stays cached for the
    // next call), where it used to be the parse itself: the copy has to iterate the same way, and
    // an overlay laid over it has to come out as it did over the original.
    long long end = -1;
    std::unordered_map<int, uint8_t> init;
    dp::GroupTimeline base = dp::loadGroupTimeline(bin, &end, &init);
    const dp::GroupTimeline over = dp::loadGroupTimeline(writeTmp("dptest_groups_over.txt",
        "tick,uid,cx,cy,w,h,on,rot\n"
        "5,7,1,2,3,4,1,0\n"
        "6,50000,1,2,3,4,1,0\n"
        "7,50001,1,2,3,4,1,0\n"
        "8,150,1,2,3,4,1,0\n"
        "end,20\n"), &end, &init);
    dp::GroupTimeline copy = base;
    const std::string dc = timelineDiff(base, copy);
    dp::overlayGroupTimeline(copy, over, false, end);
    dp::overlayGroupTimeline(base, over, false, end);
    const std::string doverlay = timelineDiff(base, copy);
    if (!dc.empty()) std::printf("  copy: %s\n", dc.c_str());
    if (!doverlay.empty()) std::printf("  overlay: %s\n", doverlay.c_str());
    check(dc.empty() && doverlay.empty(),
          "a copied parse iterates as the parse does, and so does an overlay laid over it");
}

// dptest --groups-corpus <file>...: the same comparison on real recordings.
static int groupsCorpus(int argc, char** argv) {
    int bad = 0;
    for (int i = 2; i < argc; ++i) {
        long long rows = 0;
        const std::string d = groupsReadersDiff(argv[i], &rows);
        std::printf("%s: %lld rows %s%s\n", d.empty() ? "ok" : "FAIL", rows, argv[i],
                    d.empty() ? "" : (" -- " + d).c_str());
        bad |= !d.empty();
    }
    return bad;
}

// THE RUN (search_dp.hpp SearchRun). A run is one level's solve: begin refuses while one is open --
// the carry-over a one-session cold run exists to catch -- and end closes it. A verification says
// whether the plan flown was the one this core emitted last (its edges from that plan's anchor on).
void runLifecycle() {
    dp::g_run = dp::SearchRun{};
    std::string why;
    check(dp::runBegin(4, 7, why), "a run begins when none is open");
    check(dp::g_run.open && dp::g_run.level == 4 && dp::g_run.seed == 7,
          "begin records the level and the seed");
    std::string why2;
    check(!dp::runBegin(5, 0, why2), "begin refuses while a run is open");
    check(why2.find("level 4") != std::string::npos && why2.find("level 5") != std::string::npos,
          "the refusal names the open run's level and the level that asked");
    check(dp::g_run.level == 4, "a refused begin leaves the open run as it was");
    dp::g_run.lastT0 = 100;
    dp::g_run.lastEdges = {{101, 1}, {110, 0}};
    const std::vector<std::pair<long long, int>> spliced = {{20, 1}, {30, 0}, {101, 1}, {110, 0}};
    const std::vector<std::pair<long long, int>> other = {{20, 1}, {30, 0}, {102, 1}, {110, 0}};
    check(dp::runOwnPlan(spliced), "a flight of the emitted tail on another prefix is the run's own");
    check(!dp::runOwnPlan(other), "a flight whose tail differs is not");
    // the seam: an emitted edge a tick before the anchor that changes nothing, which the splice drops
    dp::g_run.lastEdges = {{99, 1}, {101, 1}, {110, 0}};
    check(dp::runOwnPlan({{20, 1}, {101, 1}, {110, 0}}),
          "an emitted edge that changes nothing need not be flown");
    check(!dp::runOwnPlan({{20, 1}, {30, 0}, {101, 1}, {110, 0}}),
          "...but a level the emitted plan holds through the seam must be held there");
    dp::g_run.lastEdges = {{101, 1}, {110, 0}};
    const std::string line = dp::runVerified(spliced, 500, 1234.5, false, 480);
    check(line.find("own=1") != std::string::npos && dp::g_run.verified == 1 && dp::g_run.own == 1,
          "runVerified counts the flight and says it was the run's own");
    check(line.find("via=") == std::string::npos, "...and a flight of the run's own plan carries no via");
    // the section search splices another tail from t=200: a flight of it is counted apart
    const std::vector<std::pair<long long, int>> sec = {{20, 1}, {30, 0}, {205, 1}, {230, 0}};
    dp::runNoteSplice(sec, 200);
    const std::string ls = dp::runVerified(sec, 260, 2000.0, false, -1);
    check(ls.find("own=0") != std::string::npos && ls.find(" via=secsolve") != std::string::npos
              && dp::g_run.viaSplice == 1,
          "a flight of the spliced plan is the section search's (via=secsolve)");
    const std::string lo = dp::runVerified(other, 300, 2100.0, false, -1);
    check(lo.find("via=") == std::string::npos && dp::g_run.viaSplice == 1,
          "a flight of neither plan carries no via");
    const std::string e = dp::runEnd();
    check(e.find("level=4") != std::string::npos && e.find(" via-secsolve=1") != std::string::npos
              && !dp::g_run.open,
          "end closes the run and counts the section search's flights");
    const std::string pf = writeTmp("dptest_plan.txt", "input=101,1\ninput=105,0\n");
    check(dp::runNotePlanFile(pf) && dp::g_run.lastEdges.size() == 2
              && dp::g_run.lastEdges[1] == std::make_pair(105LL, 0),
          "the plan a call answered with is read back from its file");
    check(!dp::runNotePlanFile(pf + ".missing") && dp::g_run.lastEdges.size() == 2,
          "a plan file that cannot be read leaves the record as it was");
    check(dp::runBegin(5, 0, why), "the next level's run begins once the last has ended");
    dp::runEnd();
    check(dp::runEnd().find("no run open") != std::string::npos, "an end with no run open says so");
}

// --bandowntrack: when the recording counts as having seen a portal's band. A portal-set track (four
// on-grid rows, too few to call a camera) sees a band on a row within 3 ticks that carries it, or
// a band equal to the one in force (the track records changes only); a camera-driven one (eight
// off-grid rows) always sees, its rows being pans and not portal writes.
void bandOwnSeen() {
    const auto savedTrack = dp::g_bandTrack;
    const auto savedIdx = dp::g_bandTrackIntervals;
    const int savedCam = dp::g_bandTrackCam;
    auto setTrack = [](std::vector<dp::BandTrackRow> rows) {
        dp::g_bandTrack = std::move(rows);
        dp::g_bandTrackIntervals.clear();
        for (size_t i = 0; i < dp::g_bandTrack.size(); ++i)
            if (dp::g_bandTrack[i].kind == dp::BandKind::Interval)
                dp::g_bandTrackIntervals.push_back(i);
        dp::g_bandTrackCam = -1;
    };
    using K = dp::BandKind;
    setTrack({{2873, K::Degenerate, 90, 90}, {2874, K::Interval, 150, 450},
              {6132, K::Interval, 270, 570}, {9322, K::Interval, 630, 930}});
    check(dp::bandTrackSawBand(9322, 630, 930) && dp::bandTrackSawBand(9325, 630, 930)
              && dp::bandTrackSawBand(9319, 630.5, 929.5),
          "a portal-set track sees a band on its own row, up to 3 ticks off and within 1 px");
    check(dp::bandTrackSawBand(9326, 630, 930),
          "...and further on while that band is still in force (the track records changes only)");
    check(!dp::bandTrackSawBand(9318, 630, 930),
          "...but not 4 ticks before its row, where another band is in force");
    check(!dp::bandTrackSawBand(9322, 840, 1140),
          "a row within the window that carries another band is another portal's");
    check(!dp::bandTrackSawBand(17765, 840, 1140),
          "a portal far from every row with a band not in force is unseen (never fired on the route)");
    check(dp::bandTrackSawBand(17765, 630, 930),
          "a portal far from every row that rewrote the band in force is seen");
    std::vector<dp::BandTrackRow> cam;
    for (int i = 0; i < 8; ++i) cam.push_back({1000 + i, K::Interval, 90.f + 1.437f * i, 360.f + 1.437f * i});
    setTrack(cam);
    check(dp::bandTrackIsCamera() && dp::bandTrackSawBand(1003, 0, 1)
              && dp::bandTrackSawBand(1020, 0, 1),
          "a camera-driven track always sees: its rows are the camera's pans, not portal writes");
    dp::g_bandTrack = savedTrack;
    dp::g_bandTrackIntervals = savedIdx;
    dp::g_bandTrackCam = savedCam;
}

// --rotboost: a 2900 that swaps the axes stores the velocity-limit exemption (+0x952) as 1, then as
// 0 when the trigger has Dont Slide (rotateGameplay 0x399faa / 0x39a00d). Both are stores, so the
// byte the body arrived with does not survive the swap either way; with the flag off the model
// leaves it alone. One trigger at x=100, a ship crossing it in frame 0.
void rotBoostDontSlide() {
    const auto savedTrig = dp::g_rotTrig;
    const bool savedFlag = dp::g_rotBoost;
    auto run = [](bool flag, int boostIn, int dontSlide) {
        dp::g_rotTrig.clear();
        dp::RotTrig rt{100.0, 0.0, 3};
        rt.setRev = 0;
        rt.dontSlide = (uint8_t)dontSlide;
        dp::g_rotTrig.push_back(rt);
        dp::g_rotBoost = flag;
        dp::State c{};
        c.mode = 1;
        c.xAbs = 101.f;
        c.boost = (uint8_t)boostIn;
        const int nf = dp::applyRotation(c, 99.0, 1.95, 1);
        return nf == 3 && c.frame == 3 ? (int)c.boost : -1;
    };
    for (int b = 0; b <= 1; ++b) {
        check(run(true, b, 0) == 1, b ? "--rotboost: latched in, Dont Slide 0 -> latched"
                                       : "--rotboost: unlatched in, Dont Slide 0 -> latched");
        check(run(true, b, 1) == 0, b ? "--rotboost: latched in, Dont Slide 1 -> unlatched"
                                       : "--rotboost: unlatched in, Dont Slide 1 -> unlatched");
        check(run(false, b, 0) == b && run(false, b, 1) == b,
              b ? "flag off: a latched body keeps its byte through the swap"
                : "flag off: an unlatched body keeps its byte through the swap");
    }
    dp::g_rotTrig = savedTrig;
    dp::g_rotBoost = savedFlag;
}

// --warpfly: a ship's step inside a 0.36 time warp, as GD stepped it on a custom level's warp
// (modifiers.hpp, g_warpFly). Every rung scales, thrust included; the switch speed does not, so a
// body released at +0.82 still takes the weak rung.
void warpFlyShipStep() {
    const gdapprox::ShipParams& p = gdapprox::ShipParams::normal();
    auto step = [&](double vy, bool held, double t) {
        return dp::qVy(gdapprox::ShipModel::stepVy(vy, held, dp::shipParamsWarp(p, t)));
    };
    check(step(-0.361, false, 0.36) == -0.386, "--warpfly: released at -0.361 steps -0.025");
    check(step(-1.061, true, 0.36) == -1.022, "--warpfly: held at -1.061 steps +0.039");
    check(step(0.82, false, 0.36) == 0.795,
          "--warpfly: released at +0.82 keeps the weak rung (the switch speed is not scaled)");
    check(&dp::shipParamsWarp(p, 1.0) == &p && step(-0.361, false, 1.0) == -0.43,
          "--warpfly: outside a warp the parameters are the caller's own");
}

// --moverextend's data form needs the Move's start: a run of constant-velocity rows that goes back to
// the object's OWN first row has nothing before it that dates the Move. Object A stands still at t=0 (a row), then
// moves from t=90 (the gap dates the start): extended for the Move's remaining 50 - 12 ticks (12 steps since t=90). Object B is first
// recorded at t=100 with 3 rows: its first row is not shown to be the Move's start, so it is NOT extended, although
// another object's rows begin at t=0 (a recording-wide minimum would have called B's start known). Object C is B
// with one earlier row at another velocity: the change dates it, so it is extended (2 steps, cap 48).
void moverExtendStart() {
    dp::Dynamics d;
    d.objs.resize(3);
    d.samples.assign(3, {});
    auto row = [](int t, float cx) { dp::DynSample s{}; s.t = t; s.cx = cx; s.cy = 0.f; s.hw = s.hh = 5.f; s.on = 1; return s; };
    d.samples[0].push_back(row(0, 0.f));
    for (int t = 90; t <= 102; ++t) d.samples[0].push_back(row(t, 2.f * (float)(t - 90)));
    for (int t = 100; t <= 102; ++t) d.samples[1].push_back(row(t, 500.f + 2.f * (float)(t - 100)));
    d.samples[2].push_back(row(99, 800.f));
    for (int t = 100; t <= 102; ++t) d.samples[2].push_back(row(t, 805.f + 2.f * (float)(t - 100)));
    dp::Dynamics::AutoPart mv{};
    mv.dx = 100.f; mv.dy = 0.f; mv.dur = 50.0; mv.ease = 0;
    d.partsAny.assign(3, std::vector<dp::Dynamics::AutoPart>{mv});
    d.prepareExtend();
    check(d.extOk[0] == 1 && d.extCap[0] == 38, "--moverextend: stood still, then moved: start dated by the gap -> extended (cap 38)");
    check(d.extOk[1] == 0, "--moverextend: first recorded mid-Move at t=100 while another object has rows from t=0 -> not extended");
    check(d.extOk[2] == 1 && d.extCap[2] == 48, "--moverextend: a velocity change dates the start -> extended (cap 48)");
}
// --slambox: the toward-the-player steps an object takes from the boxes the state touched,
// each part decided the way dynamics.hpp's slam loop decides it -- skipped iff slamBoxRecorded says the
// recorded attempt fired that same box on the state's own entry tick. `fire[b]` is the state's entry into box b
// (-1 = not touched), `rec` the recorded attempt's (--rectouch). Returns the summed dy the state adds.
struct SlamPart { int box; float dy; };
float slamSum(const std::vector<SlamPart>& parts, const std::array<int, dp::kTouchBits>& fire,
              const std::array<int, dp::kTouchBits>& rec) {
    float sum = 0.f;
    for (const SlamPart& p : parts) {
        if (p.dy >= 0.f) continue;
        const int F = fire[(size_t)p.box];
        if (F < 0) continue;
        if (dp::slamBoxRecorded(rec, p.box, F)) continue;
        sum += p.dy;
    }
    return sum;
}
void slamBoxIdentity() {
    auto none = [] { std::array<int, dp::kTouchBits> a{}; a.fill(-1); return a; };
    const int A = 1, B = 2;
    {   // two candidate boxes; the recording holds A; the state enters A on its tick and B later
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100; fire[B] = 120;
        check(slamSum({{A, -9.f}, {B, -9.f}}, fire, rec) == -9.f,
              "--slambox: A recorded and entered on its tick -> skipped; B, not recorded -> kept");
    }
    {   // the old rule's hole: the recording holds A, the state touches only B, on A's tick
        auto fire = none(), rec = none();
        rec[A] = 100; fire[B] = 100;
        check(slamSum({{A, -9.f}, {B, -9.f}}, fire, rec) == -9.f,
              "--slambox: only B touched (the recording's touch is A) -> B keeps its step");
    }
    {   // two boxes hit on the same tick, only A in the recording
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100; fire[B] = 100;
        check(slamSum({{A, -9.f}, {B, -4.f}}, fire, rec) == -4.f,
              "--slambox: A and B on the same tick, only A recorded -> A skipped, B kept");
    }
    {   // one box, two slam parts
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 100;
        check(slamSum({{A, -9.f}, {A, -3.f}}, fire, rec) == 0.f,
              "--slambox: one recorded box with two parts -> both skipped");
    }
    {   // the same box entered on another tick is another activation
        auto fire = none(), rec = none();
        rec[A] = 100; fire[A] = 101;
        check(slamSum({{A, -9.f}}, fire, rec) == -9.f,
              "--slambox: the recorded box entered a tick later -> kept (delta 0 only)");
    }
    {   // the witness's shape: -9 / 0 / +300 boxes, the recording fired -9 and +300, the state likewise
        auto fire = none(), rec = none();
        const int b19 = 19, b20 = 20, b23 = 23;
        rec[b19] = 5119; rec[b23] = 5132; fire[b19] = 5119; fire[b23] = 5132;
        check(slamSum({{b19, -9.f}, {b20, 0.f}, {b23, 300.f}}, fire, rec) == 0.f,
              "--slambox: the plate's three boxes, the -9 recorded on its tick -> no second -9");
    }
}
// The recorded band's source through the writer and the reader (band_rows.hpp, bands.hpp loadBandTrack).
// A band whose source goes camera -> portal -> camera with the same numbers used to write one row, and the
// lookup gave the first row's source for all three ticks. The writer now adds an S row for a change of
// source alone; the reader keeps S rows out of the track, so every reader of the rows sees the rows a
// file without them gives.
namespace {
void clearBandTrack() {
    dp::g_bandTrack.clear();
    dp::g_bandTrackIntervals.clear();
    dp::g_bandSrcRows.clear();
    dp::g_bandTrackCam = -1;
    dp::g_bandTrackEnd = -1;
}
struct BandSeen { int kind; double fl, ce; long long age; int src; bool ok; };
std::vector<BandSeen> bandSeen(int t0, int t1) {
    std::vector<BandSeen> v;
    for (int t = t0; t <= t1; ++t) {
        BandSeen s{};
        dp::BandKind k{};
        s.ok = dp::bandTrackKindAt(t, k, s.fl, s.ce, s.age, &s.src);
        s.kind = (int)k;
        v.push_back(s);
    }
    return v;
}
}  // namespace
void bandSourceRows() {
    std::vector<dp::BandSample> s;
    auto add = [&](long long t, float lo, float hi, int cam) {
        dp::BandSample b;
        b.t = t; b.written = true; b.pmin = lo; b.pmax = hi; b.cam = cam;
        s.push_back(b);
    };
    add(1, 600.f, 900.f, 1);
    add(2, 600.f, 900.f, 0);
    add(3, 600.f, 900.f, 1);
    add(4, 630.f, 930.f, 0);
    add(5, 630.f, 930.f, 0);
    long long rows = 0;
    const std::string body = dp::bandRows(s, &rows);
    check(body == "1,I,600,900,1\n2,S,0\n3,S,1\n4,I,630,930,0\n" && rows == 4,
          "band rows: a source change with the same numbers is an S row, a value change an I row");
    clearBandTrack();
    dp::loadBandTrack(writeTmp("dptest_band_s.txt", body).c_str());
    const auto withS = bandSeen(1, 6);
    const int want[6] = {1, 0, 1, 0, 0, 0};
    bool srcOk = true;
    for (int i = 0; i < 6; ++i) srcOk = srcOk && withS[(size_t)i].ok && withS[(size_t)i].src == want[i];
    check(srcOk, "band rows: the lookup reads the source tick by tick (1, 0, 1, then the next row's 0)");
    check(dp::g_bandTrack.size() == 2 && dp::g_bandTrackIntervals.size() == 2 && dp::g_bandSrcRows.size() == 2,
          "band rows: S rows stay out of the track (2 rows, 2 intervals, 2 source changes)");
    // Every reader but the source's sees the file without its S rows.
    clearBandTrack();
    dp::loadBandTrack(writeTmp("dptest_band_nos.txt", "1,I,600,900,1\n4,I,630,930,0\n").c_str());
    const auto noS = bandSeen(1, 6);
    bool same = dp::g_bandTrack.size() == 2;
    for (int i = 0; i < 6; ++i) {
        const BandSeen& a = withS[(size_t)i];
        const BandSeen& b = noS[(size_t)i];
        same = same && a.ok == b.ok && a.kind == b.kind && a.fl == b.fl && a.ce == b.ce && a.age == b.age;
    }
    check(same, "band rows: kind, values and age are the same with and without the S rows");
    // A four-column file (before the source) reads as it did: no source anywhere.
    clearBandTrack();
    dp::loadBandTrack(writeTmp("dptest_band_4.txt", "1,I,600,900\n4,I,630,930\n").c_str());
    bool none = true;
    for (const BandSeen& b : bandSeen(1, 6)) none = none && b.ok && b.src == -1;
    check(none, "band rows: a four-column file has no source (-1) on every tick");
    // ...and an S row is no row to the older shapes: not I/D/N/U, and not three numbers.
    int t = 0;
    float fl = 0.f, ce = 0.f;
    check(std::sscanf("2,S,0", "%d,%f,%f", &t, &fl, &ce) != 3,
          "band rows: an S row is not a `t,floor,ceil` row, so an older reader skips it");
    clearBandTrack();
}
// --formularec reads the recording only when it is the anchor's own attempt, up to the tick it reached
// (--recprov). The same tick and the same box from another attempt is not that attempt's world.
void formulaRecProvenance() {
    auto with = [](bool given, long long rec, long long anc, long long reach, long long t) {
        dp::g_recProvGiven = given;
        dp::g_recProvRec = rec;
        dp::g_recProvAnchor = anc;
        dp::g_recProvReach = reach;
        return dp::formulaRecProvOk(t);
    };
    check(with(true, 41, 41, 3000, 2500), "--recprov: one attempt, before its reach -> read");
    check(!with(true, 47, 41, 3000, 2500), "--recprov: the recording is another attempt's -> not read");
    check(!with(true, 41, 41, 3000, 3001), "--recprov: past the tick the recording reached -> not read");
    check(!with(false, -1, -1, -1, 2500), "--recprov: not given (an older mod) -> not read");
    check(!with(true, -1, -1, 3000, 2500), "--recprov: unknown attempts on both sides -> not read");
    with(false, -1, -1, -1, 0);
}
void twoBodyMergeHeldMask() {
    using Edges = std::vector<std::pair<long long, int>>;
    {
        const auto m = dp::mergeTwoBodyEdges({}, {{50, 1}, {70, 0}}, 1, 0);
        check(m == (Edges{{50, 3}, {70, 1}}),
              "2p merge: P1 held across anchor stays pressed (bit 0 = 1) when only P2 emits edges");
    }
    {
        const auto m = dp::mergeTwoBodyEdges({{50, 1}, {80, 0}}, {{60, 0}}, 0, 1);
        check(m == (Edges{{50, 3}, {60, 1}, {80, 0}}),
              "2p merge: P2 held across anchor stays pressed (bit 1 = 1) until P2 releases");
    }
    {
        const auto m = dp::mergeTwoBodyEdges({{40, 0}}, {{40, 1}}, 1, 0);
        check(m == (Edges{{40, 2}}),
              "2p merge: same-tick P1 release and P2 press combine into one mask line");
    }
    {
        const auto m = dp::mergeTwoBodyEdges({}, {}, 1, 1);
        check(m.empty(), "2p merge: no edges -> empty plan regardless of initial holds");
    }
}
void humanGapJumpTiming() {
    auto mkSolid = [](int uid, double cx, double cy) {
        dp::Obj o{};
        o.uid = uid;
        o.type = 0;
        o.cx = cx;
        o.cy = cy;
        o.hw = 15.0;
        o.hh = 15.0;
        return o;
    };
    const std::vector<const dp::Obj*> noSlps, noPads;
    const double half = 15.0;

    // 1. Real drop-off ahead (10-block platform [0..300]): early jumps at 60*sc, 90*sc, 140*sc must never
    // drop to 0 penalty, while natural edge jumps near 17*sc stay near 0 across 1x/2x/3x speeds.
    {
        std::vector<dp::Obj> platBlocks;
        for (int i = 0; i < 10; ++i) platBlocks.push_back(mkSolid(100 + i, 15.0 + 30.0 * i, 15.0));
        dp::XSlice slice(platBlocks);
        for (double sc : {1.0, 1.24, 1.5}) {
            auto evalAt = [&](double edgeDist) {
                const double px = 300.0 - edgeDist;
                std::vector<const dp::Obj*> near;
                for (const auto& o : platBlocks)
                    if (o.cx - o.hw <= px + 82.0) near.push_back(&o);
                return dp::groundJumpTimingPenalty(near, noSlps, noPads, px, 45.0, half, sc,
                                                   false, false, &slice, nullptr);
            };
            const int pEdge = evalAt(17.0 * sc);
            const int pEarly60 = evalAt(60.0 * sc);
            const int pEarly90 = evalAt(90.0 * sc);
            const int pEarly140 = evalAt(140.0 * sc);
            check(pEdge <= 2 && pEarly60 > 50 && pEarly90 > pEarly60 && pEarly140 > pEarly90,
                  "human gap jump: penalty rises monotonically across 60..140*sc instead of dropping to 0");
        }
    }

    // 2. Reverse & inverted gravity: platform extends in reverse [0..360] with near trimmed to [240..360]
    // and gap at x=0; early jump at edgeDist = 70*sc is still penalized via XSlice extension.
    {
        std::vector<dp::Obj> platBlocks;
        for (int i = 0; i < 12; ++i) platBlocks.push_back(mkSolid(200 + i, 15.0 + 30.0 * i, 150.0));
        dp::XSlice slice(platBlocks);
        const double sc = 1.5;
        const double px = 70.0 * sc; // moving left toward fwdEdge = 0
        std::vector<const dp::Obj*> near;
        for (const auto& o : platBlocks)
            if (o.cx >= px - 70.0 && o.cx <= px + 40.0) near.push_back(&o);
        const int pEdge = dp::groundJumpTimingPenalty(near, noSlps, noPads, 17.0 * sc, 120.0, half, sc,
                                                      true, true, &slice, nullptr);
        const int pEarly = dp::groundJumpTimingPenalty(near, noSlps, noPads, px, 120.0, half, sc,
                                                       true, true, &slice, nullptr);
        check(pEdge <= 2 && pEarly > 80,
              "human gap jump: reverse + flipped gravity extends fwdEdge via XSlice and penalizes early jump");
    }

    // 3. Continuous flat platform (> 220*sc past px): truncated `near` must NOT invent a false drop-off.
    {
        std::vector<dp::Obj> flatBlocks;
        for (int i = 0; i < 16; ++i) flatBlocks.push_back(mkSolid(300 + i, 15.0 + 30.0 * i, 15.0));
        dp::XSlice slice(flatBlocks);
        const double px = 60.0;
        std::vector<const dp::Obj*> near;
        for (int i = 0; i < 5; ++i) near.push_back(&flatBlocks[(size_t)i]); // stops at x=150 inside `near`
        const int pFlat = dp::groundJumpTimingPenalty(near, noSlps, noPads, px, 45.0, half, 1.5,
                                                      false, false, &slice, nullptr);
        check(pFlat == 0,
              "human gap jump: continuous flat platform past 220*sc returns 0 even when near truncates earlier");
    }
}

void humanPortalEnforcement() {
    auto mkPort = [](int uid, int type, double cx, double cy, int id = 0) {
        dp::Obj o{};
        o.uid = uid;
        o.type = type;
        o.id = id;
        o.cx = cx;
        o.cy = cy;
        o.hw = 17.0;
        o.hh = 37.5;
        return o;
    };
    const std::vector<const dp::Obj*> noSpds;
    const std::vector<const dp::Obj*> noPorts;
    const double waveHalf = dp::playerHalf(4, false);

    // 1. All actual mode/size/gravity/dual/teleport portal types are enforced even past 135 px gapY
    //    (and Wave/Mini-Wave pays double surcharge so skipping into open sections never wins over normal play).
    {
        const int types[] = {3, 4, 5, 6, 16, 17, 18, 19, 23, 24, 26, 27, 28, 33, 41, 42};
        for (int pt : types) {
            dp::Obj p = mkPort(100 + pt, pt, 500.0, 200.0);
            dp::State s{};
            s.xAbs = 499.0f;
            s.mode = (pt == 6) ? 4 : 0;   // ensure pt==6 (Cube) changes from Wave; others differ from default 0
            if (pt == 17) s.mini = 1;
            if (pt == 4) s.flip = 1;
            if (pt == 24) s.dual = 1;
            dp::State kid = s;
            kid.xAbs = 500.5f;
            kid.y = 390.0f; // gapY = |390-200| - (37.5+5) = 147.5..157.5 (> old 135 px cutoff)
            const std::vector<const dp::Obj*> ports{&p};
            const int pen = dp::humanPortalSkipPenalty(s, kid, ports, noSpds, dp::playerHalf(kid.mode, kid.mini != 0));
            const int expect = (pt == 6) ? 100000 : 50000;
            check(pen == expect, "human portal: dodging state-changing portal charges 50k (100k in wave)");
        }

        // Tall wave room (>340 px away from portal inside active band) is still penalized, not 0!
        dp::Obj cubePort = mkPort(199, 6, 500.0, 120.0);
        dp::State sTall{};
        sTall.xAbs = 499.0f;
        sTall.mode = 4;
        sTall.mini = 1;
        sTall.bandFloor = 0.0f;
        sTall.bandCeil = 600.0f;
        dp::State kidTall = sTall;
        kidTall.xAbs = 500.5f;
        kidTall.y = 540.0f; // gapY = 420 - 40.5 = 379.5 (> old 340 cutoff)
        const std::vector<const dp::Obj*> tallPorts{&cubePort};
        check(dp::humanPortalSkipPenalty(sTall, kidTall, tallPorts, noSpds, dp::playerHalf(4, true)) == 100000,
              "human portal: tall active band mini-wave skip >340px still charges 100k");

        // Pre-portal rapid spam toggle vertically outside portal corridor is penalized (mini-wave & big-wave),
        // while lining up inside the portal's vertical span or normal-rate strokes (age >= 12) pay 0.
        dp::State sPre = sTall;
        sPre.xAbs = 440.0f;
        sPre.action = 0;
        sPre.edgeAge = 2; // rapid burst (< 4 ticks)
        dp::State kidOutside = sPre;
        kidOutside.xAbs = 441.5f;
        kidOutside.y = 260.0f; // outside portal [82.5, 157.5] + half
        check(dp::humanWavePortalSpamPenalty(sPre, kidOutside, 1, tallPorts, dp::playerHalf(4, true)) > 2400,
              "human portal: mini-wave rapid toggle outside upcoming portal Y corridor charges spam surcharge");

        dp::State kidInside = sPre;
        kidInside.xAbs = 441.5f;
        kidInside.y = 120.0f; // aligned with portal cy=120
        check(dp::humanWavePortalSpamPenalty(sPre, kidInside, 1, tallPorts, dp::playerHalf(4, true)) == 0,
              "human portal: toggling aligned inside upcoming portal Y span charges 0");

        dp::State sSlow = sPre;
        sSlow.edgeAge = 14; // relaxed stroke
        check(dp::humanWavePortalSpamPenalty(sSlow, kidOutside, 1, tallPorts, dp::playerHalf(4, true)) == 0,
              "human portal: relaxed wave stroke (edgeAge >= 12) charges 0 pre-portal spam");
    }

    // 2. Speed portals (type 20, id 200..203/1334) charge +15000 (+30000 in Wave) when skipped cleanly.
    {
        dp::Obj sp = mkPort(250, 20, 500.0, 200.0, 202); // 1.3x (2x speed)
        dp::State s{};
        s.xAbs = 499.0f;
        s.dx = 1.298f;
        s.mode = 4;
        dp::State kid = s;
        kid.xAbs = 500.5f;
        kid.y = 380.0f;
        const std::vector<const dp::Obj*> spds{&sp};
        check(dp::humanPortalSkipPenalty(s, kid, noPorts, spds, waveHalf) == 30000,
              "human portal: dodging speed portal in wave charges +30000");

        kid.spdFired[0] = 250;
        check(dp::humanPortalSkipPenalty(s, kid, noPorts, spds, waveHalf) == 0,
              "human portal: speed portal already fired in spdFired charges 0");
    }

    // 3. Redundant portal (same mode), already-seen portal (portSeen), and teleport-jumped portal charge 0.
    {
        dp::Obj cubePort = mkPort(301, 6, 500.0, 200.0);
        const std::vector<const dp::Obj*> ports{&cubePort};
        dp::State sCube{};
        sCube.xAbs = 499.0f;
        sCube.mode = 0; // already cube
        dp::State kidCube = sCube;
        kidCube.xAbs = 500.5f;
        kidCube.y = 380.0f;
        check(dp::humanPortalSkipPenalty(sCube, kidCube, ports, noSpds, dp::playerHalf(0, false)) == 0,
              "human portal: redundant portal (already cube) charges 0");

        dp::State sWave{};
        sWave.xAbs = 499.0f;
        sWave.mode = 4;
        dp::State kidTaken = sWave;
        kidTaken.xAbs = 500.5f;
        kidTaken.y = 380.0f;
        kidTaken.portSeen[0] = 301;
        check(dp::humanPortalSkipPenalty(sWave, kidTaken, ports, noSpds, waveHalf) == 0,
              "human portal: portal recorded in portSeen charges 0");

        dp::State kidTp = sWave;
        kidTp.xAbs = 580.0f; // > 25 px teleport jump across portal
        kidTp.y = 380.0f;
        check(dp::humanPortalSkipPenalty(sWave, kidTp, ports, noSpds, waveHalf) == 0,
              "human portal: teleport jump past portal charges 0");
    }

    // 4. End-to-end DP comparison: Deadlocked-style Wave->Cube corridor takes the Cube portal cleanly,
    // while Portel-style lethal fake Cube portal still dodges and survives.
    {
        auto objRow = [](int id, int type, double cx, double cy, double w, double h, int uid) {
            std::ostringstream os;
            os << id << "," << type << "," << cx << "," << cy << "," << w << "," << h << ",0," << uid << ",0,0\n";
            return os.str();
        };
        auto runScenario = [&](bool fakeTrap, bool miniWave) -> std::pair<bool, int> {
            std::string csv = std::string(kObjHeader) + "\n";
            // Wave/Mini-Wave band [30, 300], cube portal (id=12, type=6) at x=200, y=150 (h=75 -> y=[112.5, 187.5]).
            // Open air above y=187.5 up to 300 allows dodging over the portal by waving/spamming above it.
            csv += objRow(12, 6, 200.0, 150.0, 34.0, 75.0, 9001);
            int nextUid = 1000;
            // Solid floor at y=0..30 (top=30) so Cube lands safely unless fakeTrap blocks the low lane.
            for (double x = 15.0; x <= 615.0; x += 30.0) {
                csv += objRow(1, 0, x, 15.0, 30.0, 30.0, nextUid++);
            }
            if (fakeTrap) {
                // Solid wall at x=280 covering y=0..200: a Cube coming out of the portal at y<=187.5
                // cannot climb above y=200 and crashes, whereas a Wave/Mini-Wave dodging at y=240 clears it.
                csv += objRow(1, 0, 280.0, 100.0, 30.0, 200.0, 2001);
            } else if (miniWave) {
                // Realistic Cube spikes downstream so the Cube path spends normal jump/timing tight,
                // proving Mini-Wave spam over the portal into open sky cannot outscore entering the portal.
                csv += objRow(8, 2, 360.0, 45.0, 30.0, 30.0, 2002);
                csv += objRow(8, 2, 510.0, 45.0, 30.0, 30.0, 2003);
            }
            const std::string tag = std::string(fakeTrap ? "fake_" : "intended_") + (miniWave ? "mini" : "big");
            const std::string lvPath = writeTmp("dptest_human_portal_" + tag + ".csv", csv);
            const std::string outPath =
                (std::filesystem::temp_directory_path() / ("dptest_human_portal_" + tag + "_plan.txt")).string();
            const std::string tracePath = outPath + ".trace.csv";
            std::remove(outPath.c_str());
            std::remove(tracePath.c_str());

            const std::string startArg = miniWave ? "0,30,150,0,4,0,0,0,1" : "0,30,150,0,4,0,0";
            std::vector<std::string> argvStr = {
                "leveldp", lvPath, "--out", outPath, "--profile", "3",
                "--start", startArg, "--startband", "30,300"
            };
            std::vector<char*> argvPtr;
            for (std::string& s : argvStr) argvPtr.push_back(s.data());

            const int rc = dp::cliMain((int)argvPtr.size(), argvPtr.data());
            const bool solved = (rc == 0 && dp::g_outcome.verdict == dp::VerdictSolved);
            int endMode = -1;
            {
                std::ifstream tf(tracePath);
                std::string ln, lastLn;
                std::getline(tf, ln); // header
                while (std::getline(tf, ln)) {
                    if (!ln.empty()) lastLn = ln;
                }
                if (!lastLn.empty()) {
                    // tick,x,y,vy,mode,...
                    std::istringstream is(lastLn);
                    std::string col;
                    for (int c = 0; c <= 4 && std::getline(is, col, ','); ++c) {
                        if (c == 4) endMode = std::atoi(col.c_str());
                    }
                }
            }
            std::remove(outPath.c_str());
            std::remove(tracePath.c_str());
            std::remove(lvPath.c_str());
            return {solved, endMode};
        };

        const auto intendedBig = runScenario(false, false);
        check(intendedBig.first && intendedBig.second == 0,
              "human portal E2E: survivable big-Wave->Cube corridor enters intended Cube portal instead of dodging");

        const auto fakeBig = runScenario(true, false);
        check(fakeBig.first && fakeBig.second == 4,
              "human portal E2E: lethal fake Cube portal still dodges as big-Wave and clears level");

        const auto intendedMini = runScenario(false, true);
        check(intendedMini.first && intendedMini.second == 0,
              "human portal E2E: survivable Mini-Wave->Cube corridor enters Cube portal instead of spamming past");

        const auto fakeMini = runScenario(true, true);
        check(fakeMini.first && fakeMini.second == 4,
              "human portal E2E: lethal fake Cube portal still dodges as Mini-Wave and clears level");
    }

    // 5. Portal-center aiming unit test + rotated-portal --glitchportal E2E test.
    {
        dp::State sPre{};
        sPre.xAbs = 195.0f; sPre.y = 150.0f; sPre.mode = 4;
        dp::State kidCenter = sPre, kidRim = sPre;
        kidCenter.xAbs = 200.0f; kidCenter.y = 152.0f; kidCenter.portSeen[0] = 9101;
        kidRim.xAbs    = 200.0f; kidRim.y    = 186.0f; kidRim.portSeen[0]    = 9101;
        dp::Obj cp{};
        cp.uid = 9101; cp.type = 6; cp.cx = 200.0; cp.cy = 150.0; cp.hw = 17.0; cp.hh = 37.5;
        std::vector<const dp::Obj*> cPorts{&cp};
        const double wh = dp::playerHalf(4, false);
        check(dp::humanPortalCenterPenalty(sPre, kidCenter, cPorts, wh, wh) == 0,
              "human portal center: centered portal entry charges 0");
        check(dp::humanPortalCenterPenalty(sPre, kidRim, cPorts, wh, wh) > 0,
              "human portal center: outer-rim portal entry charges > 0");

        auto rotRow = [](int id, int type, double cx, double cy, double w, double h, int uid, double rot) {
            std::ostringstream os;
            os << id << "," << type << "," << cx << "," << cy << "," << w << "," << h
               << ",0," << uid << ",0," << rot << "\n";
            return os.str();
        };
        auto runRotPortal = [&](double gp) -> bool {
            std::string csv = std::string(kObjHeader) + "\n";
            csv += rotRow(12, 6, 200.0, 150.0, 34.0, 75.0, 9201, 25.0);
            int uid = 9300;
            for (double x = 15.0; x <= 515.0; x += 30.0) {
                csv += rotRow(1, 0, x, 15.0, 30.0, 30.0, uid++, 0.0);
            }
            // Wall at x=280 blocks any Cube exiting low; ceiling above y=198 forces Wave within <15px of tilted portal.
            csv += rotRow(1, 0, 280.0, 95.0, 30.0, 160.0, uid++, 0.0);
            csv += rotRow(1, 0, 200.0, 250.0, 120.0, 100.0, uid++, 0.0);
            const std::string lvPath = writeTmp("dptest_rot_glitchportal.csv", csv);
            const std::string outPath =
                (std::filesystem::temp_directory_path() / "dptest_rot_glitchportal_plan.txt").string();
            std::remove(outPath.c_str());
            std::vector<std::string> args = {
                "leveldp", lvPath, "--out", outPath,
                "--start", "0,30,150,0,4,0,0", "--startband", "30,300"
            };
            if (gp > 0.0) {
                args.push_back("--glitchportal");
                args.push_back(std::to_string(gp));
            }
            std::vector<char*> ptrs;
            for (auto& a : args) ptrs.push_back(a.data());
            const int rc = dp::cliMain((int)ptrs.size(), ptrs.data());
            const bool solved = (rc == 0 && dp::g_outcome.verdict == dp::VerdictSolved);
            std::remove(outPath.c_str());
            std::remove((outPath + ".trace.csv").c_str());
            std::remove(lvPath.c_str());
            return solved;
        };
        check(runRotPortal(0.0),
              "glitchportal E2E: baseline (0 margin) allows squeezing over tilted cube portal");
        check(!runRotPortal(15.0),
              "glitchportal E2E: --glitchportal 15 blocks tight wave squeeze over tilted cube portal");
    }
}

void profileDedupePromotion() {
    auto objRow = [](int id, int type, double cx, double cy, double w, double h, int uid) {
        std::ostringstream os;
        os << id << "," << type << "," << cx << "," << cy << "," << w << "," << h << ",0," << uid << ",0,0\n";
        return os.str();
    };
    auto countPlanLines = [](const std::string& planPath) {
        std::ifstream f(planPath);
        std::string ln;
        int n = 0;
        while (std::getline(f, ln)) {
            if (ln.rfind("input=", 0) == 0) ++n;
        }
        return n;
    };

    // 1. betterProfileState tie-breaking respects Max Clicks (smaller edgeAge) vs other profiles (larger edgeAge).
    {
        dp::State a{}, b{};
        a.mode = 1; b.mode = 1;
        a.tight = 10; b.tight = 10;
        a.edgeAge = 2; b.edgeAge = 10;
        const int prev = dp::g_solveProfile;
        dp::g_solveProfile = 2;
        check(dp::betterProfileState(a, b) && !dp::betterProfileState(b, a),
              "profile tie-break: Max Clicks prefers fresher toggle (smaller edgeAge)");
        dp::g_solveProfile = 1;
        check(dp::betterProfileState(b, a) && !dp::betterProfileState(a, b),
              "profile tie-break: Min Clicks prefers steadier hold (larger edgeAge)");
        dp::g_solveProfile = prev;
    }

    // 2. End-to-end flat corridor: Min Clicks emits 0 inputs, Max Clicks emits active spam, Centerline clears centered.
    std::string csv = std::string(kObjHeader) + "\n";
    int uid = 5000;
    for (double x = 15.0; x <= 615.0; x += 30.0) {
        csv += objRow(1, 0, x, 15.0, 30.0, 30.0, uid++);
        csv += objRow(1, 0, x, 315.0, 30.0, 30.0, uid++);
    }
    const std::string lvPath = writeTmp("dptest_profile_corridor.csv", csv);

    auto runProf = [&](int prof, const char* startArg, const char* tag) {
        const std::string outPath =
            (std::filesystem::temp_directory_path() / (std::string("dptest_prof_") + tag + "_plan.txt")).string();
        const std::string tracePath = outPath + ".trace.csv";
        std::remove(outPath.c_str());
        std::remove(tracePath.c_str());
        std::string pStr = std::to_string(prof);
        std::vector<std::string> argvStr = {
            "leveldp", lvPath, "--out", outPath, "--profile", pStr,
            "--start", startArg, "--startband", "30,300"
        };
        std::vector<char*> argvPtr;
        for (std::string& s : argvStr) argvPtr.push_back(s.data());
        const int rc = dp::cliMain((int)argvPtr.size(), argvPtr.data());
        const bool solved = (rc == 0 && dp::g_outcome.verdict == dp::VerdictSolved);
        const int inputs = countPlanLines(outPath);
        double maxOff = 0.0;
        {
            std::ifstream tf(tracePath);
            std::string ln;
            std::getline(tf, ln);
            while (std::getline(tf, ln)) {
                if (ln.empty()) continue;
                std::istringstream is(ln);
                std::string col;
                double y = 165.0;
                for (int c = 0; c <= 2 && std::getline(is, col, ','); ++c) {
                    if (c == 2) y = std::atof(col.c_str());
                }
                maxOff = std::max(maxOff, std::fabs(y - 165.0));
            }
        }
        std::remove(outPath.c_str());
        std::remove(tracePath.c_str());
        return std::tuple<bool, int, double>{solved, inputs, maxOff};
    };

    const auto [minOk, minInputs, minOff] = runProf(1, "0,30,45,0,0,0,0", "min");
    (void)minOff;
    check(minOk && minInputs == 0, "profile E2E: Min Clicks clears flat floor with 0 clicks");

    const auto [maxOk, maxInputs, maxOff] = runProf(2, "0,30,45,0,0,0,0", "max");
    (void)maxOff;
    check(maxOk && maxInputs >= 6, "profile E2E: Max Clicks clears flat floor with active clicking");

    const auto [ctrOk, ctrInputs, ctrOff] = runProf(4, "0,30,165,0,1,0,0", "ctr");
    (void)ctrInputs;
    check(ctrOk && ctrOff < 75.0, "profile E2E: Centerline Ship keeps route near corridor center");

    // 3. --goalx: stops cleanly before a downstream lethal wall when custom goalX precedes it.
    {
        std::string gcsv = std::string(kObjHeader) + "\n";
        int guid = 6000;
        for (double x = 15.0; x <= 815.0; x += 30.0) {
            gcsv += objRow(1, 0, x, 15.0, 30.0, 30.0, guid++);
        }
        // Unpassable spike wall at x=500 across the entire corridor.
        for (double y = 45.0; y <= 345.0; y += 30.0) {
            gcsv += objRow(8, 2, 500.0, y, 30.0, 30.0, guid++);
        }
        const std::string gLv = writeTmp("dptest_goalx.csv", gcsv);
        const std::string gOut = (std::filesystem::temp_directory_path() / "dptest_goalx_plan.txt").string();
        std::remove(gOut.c_str());
        std::remove((gOut + ".trace.csv").c_str());
        std::vector<std::string> argvStr = {
            "leveldp", gLv, "--out", gOut, "--goalx", "300", "--start", "0,30,45,0,0,0,0"
        };
        std::vector<char*> argvPtr;
        for (std::string& s : argvStr) argvPtr.push_back(s.data());
        const int rc = dp::cliMain((int)argvPtr.size(), argvPtr.data());
        check(rc == 0 && dp::g_outcome.verdict == dp::VerdictSolved && dp::g_outcome.deepX < 400.0,
              "goalx E2E: custom goalX=300 clears before downstream wall at x=500");
        std::remove(gOut.c_str());
        std::remove((gOut + ".trace.csv").c_str());
        std::remove(gLv.c_str());
    }

    std::remove(lvPath.c_str());
}

int main(int argc, char** argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--groups-corpus")) return groupsCorpus(argc, argv);
    humanGapJumpTiming();
    humanPortalEnforcement();
    profileDedupePromotion();
    bandSourceRows();
    formulaRecProvenance();
    twoBodyMergeHeldMask();
    slamBoxIdentity();
    warpFlyShipStep();
    rotBoostDontSlide();
    bandOwnSeen();
    runLifecycle();
    groupsParser();
    commaFields();
    overlayReach();
    initLines();
    noCollideColumn();
    keptTextPass();
    onOffEvents();
    activatorRoots();
    autoSwitches();
    keyHoldsDelayedSwitch();
    moverExtendStart();
    std::printf(g_fail ? "FAILED\n" : "all ok\n");
    return g_fail;
}
