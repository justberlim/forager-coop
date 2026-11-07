#include "shots.h"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include "bombs.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::shots {

namespace {

constexpr uint8_t kShot = 58;   // either way (the host passes a guest's on): ShotMsg
constexpr uint8_t kBlast = 59;  // either way: BlastMsg, a fireball went off (ExplosionCreate in its collision)
constexpr uint8_t kBurn = 61;   // either way: BurnEntry list, world objects a blast set on fire
constexpr int kObjArrow = 303, kObjFireball = 301;  // decompiled/data/objects.tsv
// decompiled/code/yyvar_map.tsv
constexpr uint32_t kYYVarDirection = 0x01AD87A0, kYYVarSpeed = 0x01AD8C10, kYYVarImageAngle = 0x01AD8728;
// objFireball.owner: UseSword sets it to the player; Create_0 leaves noone. Its Step_0 starts with
// `if (!instance_exists(owner)) { instance_destroy(); exit }` - a copy (no owner) was gone at its first step, so the
// sword's fireballs were never seen by the other players.
constexpr int kSlotFireballOwner = 0x6CA;
// ExplosionCreate(x, y, range, dmgPlayer) makes the objExplosion of a fireball's hit (its collision events call it;
// bombs make theirs themselves, bombs.cpp) and returns nothing. A copy hits nothing, so the other players saw no blast.
constexpr uint32_t kScriptExplosionCreate = 0x0086D1E0;
constexpr int kBlastArgs = 4;
constexpr int kObjExplosion = 295;   // decompiled/data/objects.tsv
constexpr int kSlotRange = 0x744;    // objExplosion.range: ExplosionCreate's third argument
// parObject.burn: set by a blast (objExplosion Step_0); while it is on, parObject Step_0 takes 0.02 hp a step and
// draws the fire (FireEffect). A blast copy is harmless (it sets nothing on fire), and an object's health goes to the
// others only on tool hits: the other players saw no fire and no health going down, only the object vanish.
constexpr int kSlotBurn = 0xD7;
constexpr int kBurnParents[] = {80, 81};  // parObject, parHerb (decompiled/data/objects.tsv)
constexpr double kBurnMargin = 16;        // past the blast's range: what its edge reached
constexpr size_t kMaxBurns = 64;
constexpr unsigned kBurnUpdateSteps = 10;  // host: a burning object's health to the guests, 6 times a second
constexpr int kSlotHp = 0x573, kSlotHpMax = 0x576, kSlotDmg = 0x197, kSlotDmgPlayer = 0x198;
constexpr uint32_t kObjectStep = 0x00B5CF40;  // parObject Step_0 (children call it as their inherited step)
constexpr float kBlastMatchDistance = 48;  // a copy flies a little behind its fireball: the one that went off

// What a copy must not do (decompiled/code/index.tsv): hit (the collision events deal the damage) and, for an arrow,
// Destroy_0 (it drops the arrow item where it lands).
constexpr uint32_t kHarmful[] = {
    0x01052DB0, 0x01055320, 0x01057130, 0x010573A0,  // objArrow Collision_80, _23, _271, _64
    0x01051960,                                      // objArrow Destroy_0
    0x01049800, 0x0104A020, 0x0104B5A0,              // objFireball Collision_80, _20, _271
};
constexpr size_t kHookCount = sizeof(kHarmful) / sizeof(kHarmful[0]);

#pragma pack(push, 1)
struct ShotMsg {
    int32_t object;
    float x, y, depth;
    float direction, speed, angle;
    float xscale, yscale;
    float sprite;
};
struct BurnEntry {
    int32_t hostId;  // the host's id of it (sync::HostIdOf), found with its spot at the others (sync::Resolve)
    int32_t object;
    float x, y;
    float burn;
};
struct BlastMsg {
    float args[kBlastArgs];  // ExplosionCreate's: x, y, range, dmgPlayer
    float depth;
    float dmg;  // the shooter's blast damage (objExplosion.dmg), 0 if unknown
};
#pragma pack(pop)

std::vector<int> g_fired;              // ours, made since the last step
std::unordered_set<int> g_copies;      // other players' shots shown here
int g_showing = 0;                     // inside our own instance_create of a copy
yyc::EventFn g_orig[kHookCount] = {};
yyc::ScriptFn g_origExplosionCreate = nullptr;

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

bool IsShot(int object) { return object == kObjArrow || object == kObjFireball; }

// The local player fired it: made in its own step or Use script (an enemy's fireball is made by the enemy).
bool FiredByLocalPlayer(yyc::CInstance* self) {
    return self != nullptr && static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)) ==
                                  yyc::obj::player &&
           !puppet::IsPuppet(self);
}

template <size_t N>
void __cdecl HarmfulHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (!g_copies.empty() && g_copies.count(IdOf(self)) > 0) return;
    g_orig[N](self, other);
}

template <size_t... N>
struct HookTable {
    static constexpr yyc::EventFn kHooks[] = {&HarmfulHook<N>...};
};
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7>;
static_assert(sizeof(Hooks::kHooks) / sizeof(Hooks::kHooks[0]) == kHookCount, "one hook per harmful event");

ShotMsg Describe(yyc::CInstance* shot, int object) {
    auto get = [shot](uint32_t var) { return static_cast<float>(yyc::GetBuiltinReal(shot, var)); };
    return ShotMsg{object,
                   get(yyc::va::YYVar_x),
                   get(yyc::va::YYVar_y),
                   get(yyc::va::YYVar_depth),
                   get(kYYVarDirection),
                   get(kYYVarSpeed),
                   get(kYYVarImageAngle),
                   get(yyc::va::YYVar_image_xscale),
                   get(yyc::va::YYVar_image_yscale),
                   get(yyc::va::YYVar_sprite_index)};
}

// A shot from the network: finite, and nothing the drawing could fail on (a sprite index past the last sprite) or
// that would swamp the screen.
bool IsSane(const ShotMsg& m) {
    for (const float v : {m.x, m.y, m.depth, m.direction, m.speed, m.angle, m.xscale, m.yscale, m.sprite}) {
        if (!std::isfinite(v)) return false;
    }
    constexpr float kSpriteCount = 1388, kMaxSpeed = 64, kMaxScale = 8;  // decompiled/data/sprites.tsv
    return m.sprite < kSpriteCount && std::fabs(m.speed) <= kMaxSpeed && std::fabs(m.xscale) <= kMaxScale &&
           std::fabs(m.yscale) <= kMaxScale;
}

void Show(yyc::CInstance* self, net::PeerId from, const ShotMsg& msg) {
    ++g_showing;
    const int id = static_cast<int>(
        yyc::CallReal(self, "instance_create_depth", {msg.x, msg.y, msg.depth, double(msg.object)}));
    --g_showing;
    yyc::CInstance* copy = yyc::FindInstance(id);
    if (copy == nullptr) return;
    g_copies.insert(id);
    // Its Create aimed it from our player: it flies as the shooter's did.
    yyc::SetBuiltinReal(copy, kYYVarDirection, msg.direction);
    yyc::SetBuiltinReal(copy, kYYVarSpeed, msg.speed);
    yyc::SetBuiltinReal(copy, kYYVarImageAngle, msg.angle);
    yyc::SetBuiltinReal(copy, yyc::va::YYVar_image_xscale, msg.xscale);
    yyc::SetBuiltinReal(copy, yyc::va::YYVar_image_yscale, msg.yscale);
    if (msg.sprite >= 0) yyc::SetBuiltinReal(copy, yyc::va::YYVar_sprite_index, msg.sprite);
    if (msg.object == kObjFireball) {
        // The shooter's puppet owns it (a guest hears every shot from the host: then the host's); any player does.
        yyc::CInstance* shooter = puppet::Instance(from);
        yyc::RValue owner = yyc::Real(IdOf(shooter != nullptr ? shooter : self));
        yyc::SetVar(id, kSlotFireballOwner, &owner);
    }
    // Shots end by themselves (lifetime, outside the room): forget the ids that are gone.
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        it = yyc::FindInstance(*it) == nullptr ? g_copies.erase(it) : std::next(it);
    }
}

// The host decides what a fireball's blast does, as it decides every world object's health:
//  - a guest's own blast is harmless at the guest (only shown); the host makes the real one, as the guest's (what it
//    breaks drops for the guest), from the blast message;
//  - the host's blast is real at the host and harmless at the guests;
//  - what a real blast hurts and sets on fire (`burn`): its health goes to the guests at once, and while it burns, every
//    few steps; at a guest a burning copy shows the fire but loses no health of its own (parObject Step_0's 0.02 a
//    step), so it never breaks before the host's - the guest's copy broke first and the drop went to the guest, even
//    for the host's own hit.
// Before, each blast was real at its shooter only: the health of the same tree differed between the players.

bool g_makingOwnBlast = false;  // guest: inside our fireball's ExplosionCreate (its objExplosion: harmless here)
int g_ownBlastId = 0;
std::unordered_map<int, net::PeerId> g_blastOwners;  // host: a guest's blast made here -> that guest
std::unordered_set<int> g_burningAtHost;            // host: world objects a real blast set on fire
std::unordered_map<yyc::CInstance*, int> g_burningCopies;  // guest: copies burning (instance -> id)
unsigned g_ticks = 0;
yyc::EventFn g_origObjectStep = nullptr;

double ReadReal(int id, int slot, bool* ok) {
    yyc::RValue value{};
    *ok = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    const double result = *ok ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return result;
}

void WriteReal(int id, int slot, double value) {
    yyc::RValue v = yyc::Real(value);
    yyc::SetVar(id, slot, &v);
}

// One of our own fireballs went off: the others see its blast too (and lose their copy of it there).
yyc::RValue* __cdecl ExplosionCreateHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                         yyc::RValue** args) {
    const bool ours = self != nullptr && net::IsConnected() && argc >= 2 && args != nullptr &&
                      static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)) == kObjFireball &&
                      g_copies.count(IdOf(self)) == 0;  // a copy never collides; to be safe all the same
    if (!ours) return g_origExplosionCreate(self, other, result, argc, args);
    const bool guest = sync::IsGuest();
    g_makingOwnBlast = guest;
    g_ownBlastId = 0;
    yyc::RValue* r = g_origExplosionCreate(self, other, result, argc, args);
    g_makingOwnBlast = false;
    BlastMsg msg{};
    for (int i = 0; i < kBlastArgs; ++i) {
        const yyc::RValue* a = i < argc ? args[i] : nullptr;
        msg.args[i] = a != nullptr && yyc::IsNumber(*a) ? static_cast<float>(yyc::ToReal(*a)) : 0.f;
    }
    msg.depth = static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_depth));
    bool ok = false;
    // The explosion takes its damage from its maker (objExplosion Create_0): the host's copy of a guest's blast needs
    // the guest's, not the host's.
    if (g_ownBlastId != 0) msg.dmg = static_cast<float>(ReadReal(g_ownBlastId, kSlotDmg, &ok));
    net::Send(kBlast, &msg, sizeof(msg));
    return r;
}

// Another player's fireball went off: its copy here (the nearest) goes, and the same blast shows - at a guest
// harmless (bombs.cpp makes it so), at the host the real one of a guest's blast.
void ShowBlast(yyc::CInstance* self, net::PeerId from, const BlastMsg& msg) {
    const float x = msg.args[0], y = msg.args[1];
    int nearest = 0;
    double best = double(kBlastMatchDistance) * kBlastMatchDistance;
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        yyc::CInstance* copy = yyc::FindInstance(*it);
        if (copy == nullptr) {
            it = g_copies.erase(it);
            continue;
        }
        const double dx = yyc::GetBuiltinReal(copy, yyc::va::YYVar_x) - x;
        const double dy = yyc::GetBuiltinReal(copy, yyc::va::YYVar_y) - y;
        if (static_cast<int>(yyc::GetBuiltinReal(copy, yyc::va::YYVar_object_index)) == kObjFireball &&
            dx * dx + dy * dy <= best) {
            best = dx * dx + dy * dy;
            nearest = *it;
        }
        ++it;
    }
    if (nearest != 0) {
        g_copies.erase(nearest);
        yyc::CallReal(self, "instance_destroy", {double(nearest), 0});
    }
    // Made as ExplosionCreate makes it, but here: ExplosionCreate returns nothing, so its blast could not be told
    // apart (review of PR #4). As bombs::Blast does: the explosion directly, its range set.
    ++g_showing;
    const int blast = static_cast<int>(
        yyc::CallReal(self, "instance_create_depth", {double(x), double(y), double(msg.depth), double(kObjExplosion)}));
    --g_showing;
    if (yyc::FindInstance(blast) == nullptr) return;
    WriteReal(blast, kSlotRange, msg.args[2]);
    if (sync::IsHost() && from != net::kHostPeer) {
        // A guest's blast: the real one is ours, with the guest's damage, and what it breaks is the guest's.
        if (msg.dmg > 0) WriteReal(blast, kSlotDmg, msg.dmg);
        WriteReal(blast, kSlotDmgPlayer, msg.args[3]);
        g_blastOwners[blast] = from;
        for (auto it = g_blastOwners.begin(); it != g_blastOwners.end();) {
            it = yyc::FindInstance(it->first) == nullptr ? g_blastOwners.erase(it) : std::next(it);
        }
        return;
    }
    bombs::MakeHarmless(blast);  // the harmless step zeroes dmg and dmgPlayer
}

void ApplyBurns(yyc::CInstance* self, const net::Message& message) {
    if (!sync::IsGuest()) return;  // the host's world objects burn from its own blasts only
    const size_t count = message.payload.size() / sizeof(BurnEntry);
    if (message.payload.size() % sizeof(BurnEntry) != 0 || count > kMaxBurns) return;
    for (size_t i = 0; i < count; ++i) {
        BurnEntry entry{};
        memcpy(&entry, message.payload.data() + i * sizeof(entry), sizeof(entry));
        if (!std::isfinite(entry.x) || !std::isfinite(entry.y) || !std::isfinite(entry.burn)) continue;
        const int local = sync::Resolve(self, entry.hostId, entry.object, entry.x, entry.y);
        bool ok = false;
        ReadReal(local, kSlotBurn, &ok);
        yyc::CInstance* copy = local != 0 ? yyc::FindInstance(local) : nullptr;
        if (copy == nullptr || !ok) continue;  // never turn a string (or nothing) into a number
        WriteReal(local, kSlotBurn, entry.burn);
        g_burningCopies[copy] = local;
    }
}

// Guest: a burning copy's step shows the fire, but its health is the host's (it comes with the host's updates).
void __cdecl ObjectStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    if (g_burningCopies.empty()) {
        g_origObjectStep(self, other);
        return;
    }
    const auto found = g_burningCopies.find(self);
    if (found == g_burningCopies.end()) {
        g_origObjectStep(self, other);
        return;
    }
    const int id = found->second;
    bool ok = false;
    const double hp = yyc::FindInstance(id) == self ? ReadReal(id, kSlotHp, &ok) : 0;
    // Padded for the step, as a dry hit does: the fire's 0.02 must not break it here (the host decides that).
    constexpr double kPadding = 1e6;
    if (ok) WriteReal(id, kSlotHp, hp + kPadding);
    g_origObjectStep(self, other);
    if (yyc::FindInstance(id) != self) {
        g_burningCopies.erase(self);
        return;
    }
    if (ok) WriteReal(id, kSlotHp, hp);
    if (ReadReal(id, kSlotBurn, &ok) <= 0 || !ok) g_burningCopies.erase(self);
}

}  // namespace

bool BlastOwner(int explosionId, net::PeerId* owner) {
    const auto found = g_blastOwners.find(explosionId);
    if (found == g_blastOwners.end()) return false;
    *owner = found->second;
    return true;
}

void AfterBlast(yyc::CInstance* explosion) {
    if (explosion == nullptr || !net::IsConnected() || !sync::IsHost()) return;
    const int blastId = IdOf(explosion);
    net::PeerId owner = net::kHostPeer;
    BlastOwner(blastId, &owner);
    bool ok = false;
    const double range = ReadReal(blastId, kSlotRange, &ok);
    if (!ok) return;
    const double bx = yyc::GetBuiltinReal(explosion, yyc::va::YYVar_x);
    const double by = yyc::GetBuiltinReal(explosion, yyc::va::YYVar_y);
    const double reach = (range + kBurnMargin) * (range + kBurnMargin);
    std::vector<BurnEntry> burns;
    for (const int parent : kBurnParents) {
        const int count = static_cast<int>(yyc::CallReal(explosion, "instance_number", {double(parent)}));
        for (int i = 0; i < count && burns.size() < kMaxBurns; ++i) {
            const int id = static_cast<int>(yyc::CallReal(explosion, "instance_find", {double(parent), double(i)}));
            yyc::CInstance* target = yyc::FindInstance(id);
            if (target == nullptr) continue;
            const double x = yyc::GetBuiltinReal(target, yyc::va::YYVar_x);
            const double y = yyc::GetBuiltinReal(target, yyc::va::YYVar_y);
            if ((x - bx) * (x - bx) + (y - by) * (y - by) > reach) continue;
            const double hp = ReadReal(id, kSlotHp, &ok);
            const double hpMax = ok ? ReadReal(id, kSlotHpMax, &ok) : 0;
            const double burn = ReadReal(id, kSlotBurn, &ok);
            const bool burning = ok && burn > 0;
            if (!burning && !(hp < hpMax)) continue;  // the blast did nothing to it
            sync::NoteHitter(id, owner);   // what breaking it gives goes to the blast's shooter
            sync::SendObjectHp(target);    // the blast's damage, at the guests too
            if (!burning) continue;
            g_burningAtHost.insert(id);
            const int object = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_object_index));
            burns.push_back({id, object, static_cast<float>(x), static_cast<float>(y), static_cast<float>(burn)});
        }
    }
    if (burns.empty()) return;
    net::Send(kBurn, burns.data(), burns.size() * sizeof(BurnEntry));
    Log("shots: a blast set %u objects on fire - sent", static_cast<unsigned>(burns.size()));
}

bool InstallHooks() {
    bool ok = true;
    for (size_t i = 0; i < kHookCount; ++i) {
        ok = MH_CreateHook(yyc::At<void*>(kHarmful[i]), reinterpret_cast<void*>(Hooks::kHooks[i]),
                           reinterpret_cast<void**>(&g_orig[i])) == MH_OK && ok;
    }
    ok = MH_CreateHook(yyc::At<void*>(kScriptExplosionCreate), reinterpret_cast<void*>(&ExplosionCreateHook),
                       reinterpret_cast<void**>(&g_origExplosionCreate)) == MH_OK && ok;
    ok = MH_CreateHook(yyc::At<void*>(kObjectStep), reinterpret_cast<void*>(&ObjectStepHook),
                       reinterpret_cast<void**>(&g_origObjectStep)) == MH_OK && ok;
    if (!ok) Log("shots: hooks failed");
    return ok;
}

void OnCreated(yyc::CInstance* self, int createdId, int object) {
    if (g_makingOwnBlast && object == kObjExplosion) {  // a guest's own blast: shown only, the host's is real
        g_ownBlastId = createdId;
        bombs::MakeHarmless(createdId);
        return;
    }
    if (!IsShot(object) || g_showing > 0 || !net::IsConnected() || !FiredByLocalPlayer(self)) return;
    g_fired.push_back(createdId);
}

void Tick() {
    // Host: what burns loses health every step; the guests' copies follow it.
    if (!g_burningAtHost.empty() && ++g_ticks % kBurnUpdateSteps == 0) {
        for (auto it = g_burningAtHost.begin(); it != g_burningAtHost.end();) {
            yyc::CInstance* target = yyc::FindInstance(*it);
            if (target == nullptr) {
                it = g_burningAtHost.erase(it);
                continue;
            }
            sync::SendObjectHp(target);
            bool ok = false;
            it = ReadReal(*it, kSlotBurn, &ok) > 0 && ok ? std::next(it) : g_burningAtHost.erase(it);
        }
    }
    if (g_fired.empty()) return;
    for (const int id : g_fired) {
        yyc::CInstance* shot = yyc::FindInstance(id);
        if (shot == nullptr) continue;  // gone within its first step (hit at point-blank range)
        const int object = static_cast<int>(yyc::GetBuiltinReal(shot, yyc::va::YYVar_object_index));
        if (!IsShot(object)) continue;
        const ShotMsg msg = Describe(shot, object);
        net::Send(kShot, &msg, sizeof(msg));
    }
    g_fired.clear();
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kBurn) {
        ApplyBurns(localPlayer, message);
        return true;
    }
    if (message.type == kBlast) {
        if (message.payload.size() != sizeof(BlastMsg)) return true;
        BlastMsg msg{};
        memcpy(&msg, message.payload.data(), sizeof(msg));
        for (const float v : msg.args) {
            if (!std::isfinite(v)) return true;
        }
        if (!std::isfinite(msg.depth) || !std::isfinite(msg.dmg)) return true;
        ShowBlast(localPlayer, message.from, msg);
        return true;
    }
    if (message.type != kShot) return false;
    if (message.payload.size() != sizeof(ShotMsg)) return true;
    ShotMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!IsShot(msg.object) || !IsSane(msg)) return true;
    Show(localPlayer, message.from, msg);
    return true;
}

void Reset() {
    g_fired.clear();
    g_copies.clear();
    g_showing = 0;
    g_makingOwnBlast = false;
    g_blastOwners.clear();
    g_burningAtHost.clear();
    g_burningCopies.clear();
}

}  // namespace coop::shots
