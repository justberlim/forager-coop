// Hits on world objects and their health bars: a guest's tool hit is carried out by the host (as the guest's
// puppet), the outcome comes back as health; each player's hit is shown at the others; repairs and damage the
// guest's own projectiles did go to the host.
#include "sync.h"
#include "sync_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "interiors.h"
#include "log.h"
#include "mobs.h"
#include "puppet.h"

namespace coop::sync {

namespace {

constexpr int kSlotHp = yyc::slot::hp;
constexpr int kSlotHpAlpha = 0x574;
constexpr int kSlotHpLast = 0x575;
constexpr int kSlotHpWhite = 0x577;
constexpr int kSlotDmg = 0x197;  // objPlayer.dmg: what a hit takes off an object (parObject Other_10)
constexpr int kSlotKilledByPlayer = 0x5DF;
constexpr uint32_t kYYVarRoomSpeed = 0x01AD8750;
constexpr uint32_t kScriptUseRepairKit = 0x006D6120;  // every structure near the player back to hpMax
constexpr uint32_t kEventUser = 0x016F2D00;           // event_user(n): (self, other, n)

#pragma pack(push, 1)
struct HpMsg {
    int32_t id;  // the host's instance id
    int32_t object;
    float x, y;
    float hp, alpha, last, white;  // what the health bar is drawn from
    uint8_t hitter;  // the player whose tool hit it (net::PeerId), kNoHitter for a repair
};
#pragma pack(pop)
constexpr uint8_t kNoHitter = 0xFF;

using EventUserFn = void(__cdecl*)(yyc::CInstance* self, yyc::CInstance* other, int n);
EventUserFn g_origEventUser = nullptr;
yyc::ScriptFn g_origRepairKit = nullptr;
std::unordered_map<int, net::PeerId> g_hitByGuest;  // host: instances a guest hit last, and which guest

// ---- Damage that is no tool hit ----
// Shots (arrows, rods' bolts and fireballs), explosions and fire take an object's health in their own events, at the
// player whose shot it was: the others saw the object whole, and hitting it they started from full health (a rod
// had brought it low at the other player). Fire is `burn` (parObject Step_0: counts down, takes health while above
// 0 and draws FireEffect), poison `poison` the same: only the shooter saw a tree burn.
// Each side looks at the world objects around it every few steps: the host tells any change of health (as after a
// tool hit) and a fire or poison started; a guest sends the health its own shot took (as a hit, the host does it) and
// a fire or poison it started (the host's object then burns, and its health comes back from the host). A guest's
// copy burns too, to show it: health lost to that is not sent, the host's comes instead.
constexpr uint8_t kObjectStatus = 112;  // either way (the host passes a guest's on): StatusMsg
constexpr int kSlotBurn = 0xD7;
constexpr int kSlotPoison = 0x704;
constexpr int kParObject = 80;
constexpr unsigned kWatchEverySteps = 6;
#pragma pack(push, 1)
struct StatusMsg {
    int32_t hostId;  // the host's id (-1 if the sender has none bound: then by object and spot)
    int32_t object;
    float x, y;
    float burn, poison;  // what it was set to here (both count down at every player by themselves)
};
#pragma pack(pop)
struct Seen {
    float hp, burn, poison;
};
std::unordered_map<int, Seen> g_seen;  // local id -> as last looked at, sent or applied
unsigned g_watchSteps = 0;

using yyc::CallReal;
using yyc::StackFrame;

// event_user(0) with the player as `other` is also how drops get collected, so only tool use counts as a hit.
bool CalledFromToolUse() {
    const StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr) return false;
    return strcmp(top->name, "gml_Script_UsePickaxe") == 0 || strcmp(top->name, "gml_Script_UseSword") == 0;
}

// The swing's hitbox: UseSword makes an objSwordCollision, and its collision with a rock, tree, bush... runs
// `with (other) event_user(0)` - the hit has the hitbox as `other`, not the player, and comes from its collision
// event, not from UseSword. The cosmic sword (also damages resources and plants) hits that way only: the hit was
// not shown at the other players (the pickaxe's was), they saw the tree just vanish. Only UseSword makes one, and
// only the local player runs it (puppets have no Step): every hitbox is the local player's.
constexpr int kObjSwordCollision = 300;  // decompiled/data/objects.tsv
constexpr int kParCritter = 64;

bool IsSwordHitbox(yyc::CInstance* instance) {
    return instance != nullptr &&
           static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index)) == kObjSwordCollision;
}

// Enemies and critters are streamed by mobs.cpp, which already passes on the damage the sword's hitbox does to them.
bool IsMob(yyc::CInstance* self, int object) {
    return IsEnemy(self, object) || object == kParCritter ||
           CallReal(self, "object_is_ancestor", {double(object), double(kParCritter)}) != 0;
}

// event_user(n) on `self` (a world object) with `other` is the local player's tool hit.
bool IsToolHit(yyc::CInstance* self, yyc::CInstance* other, int n) {
    if (n != 0 || other == nullptr) return false;
    if (other == g_localPlayer) return CalledFromToolUse();
    return IsSwordHitbox(other) &&
           !IsMob(self, static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)));
}

// Host: after a hit, the guest's copy shows the same health bar (and the hit itself, when `hitter` is not that guest).
void SendHp(yyc::CInstance* target, uint8_t hitter = kNoHitter) {
    if (!IsHost() || !Connected() || target == nullptr) return;
    const InstanceInfo info = Describe(target);
    const HpMsg msg{info.id, info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                    ReadVar(info.id, kSlotHp), ReadVar(info.id, kSlotHpAlpha), ReadVar(info.id, kSlotHpLast),
                    ReadVar(info.id, kSlotHpWhite), hitter};
    net::Send(kObjectHp, &msg, sizeof(msg));
    if (const auto seen = g_seen.find(info.id); seen != g_seen.end()) seen->second.hp = msg.hp;
}

void ApplyHp(yyc::CInstance* self, const HpMsg& msg) {
    if (!std::isfinite(msg.hp) || !std::isfinite(msg.alpha) || !std::isfinite(msg.last) || !std::isfinite(msg.white)) {
        return;
    }
    if (IsHost() && !IsSynced(self, msg.object)) return;  // a guest repairs world objects only
    const int local = Resolve(self, msg.id, msg.object, msg.x, msg.y);
    if (local == 0) return;
    const int slots[4] = {kSlotHp, kSlotHpAlpha, kSlotHpLast, kSlotHpWhite};
    const float values[4] = {msg.hp, msg.alpha, msg.last, msg.white};
    for (int i = 0; i < 4; ++i) {
        yyc::RValue v = yyc::Real(values[i]);
        yyc::SetVar(local, slots[i], &v);
    }
    mobs::NoteHostHp(local, msg.hp);
    if (const auto seen = g_seen.find(local); seen != g_seen.end()) seen->second.hp = msg.hp;  // not ours to send
}

// Guest: a tool hit goes to the host with the guest's own damage (the puppet's would be used there otherwise).
void SendHit(const InstanceInfo& info) {
    ObjectMsg msg{HostIdOf(info.id), info.object, static_cast<float>(info.x), static_cast<float>(info.y), 0,
                  kNoItem, kOwnerAnyone, -1, 0};
    if (g_localPlayer != nullptr) {
        msg.depth = ReadVar(static_cast<int>(yyc::GetBuiltinReal(g_localPlayer, yyc::va::YYVar_id)), kSlotDmg);
    }
    if (net::Send(kHit, &msg, sizeof(msg))) ++g_sent[kHit];
}

// Guest: the hit is also shown here - shake, flash, sound, particles - by running the object's own hit event
// with its health padded so that it cannot break, and put back afterwards. Nothing it does is reported or
// kept: no XP, no destroys, no drops (the host decides those, PLAYTEST.md #7).
void DryHit(yyc::CInstance* target, yyc::CInstance* player) {
    constexpr double kPadding = 1e6;
    const int id = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id));
    yyc::RValue hp{};
    const bool number = yyc::GetVar(id, kSlotHp, &hp) && yyc::IsNumber(hp);
    const double before = number ? yyc::ToReal(hp) : 0;
    yyc::Free(&hp);
    if (!number) return;
    yyc::RValue padded = yyc::Real(before + kPadding);
    yyc::SetVar(id, kSlotHp, &padded);
    g_dryHit = true;
    BeginSuppressSpawns();
    g_origEventUser(target, player, 0);
    EndSuppressSpawns(target);
    g_dryHit = false;
    if (yyc::FindInstance(id) == nullptr) return;
    yyc::RValue restored = yyc::Real(before);
    yyc::SetVar(id, kSlotHp, &restored);
}

// Guest: another player's tool hit (the host's, or a guest's passed on by the host) is shown here as well, as our
// own hits are. Our own come back too: we have no puppet of ourselves, so those are not shown twice.
void ShowOthersHit(yyc::CInstance* self, const HpMsg& msg) {
    if (msg.hitter == kNoHitter || IsEnemy(self, msg.object)) return;
    yyc::CInstance* hitter = puppet::Instance(msg.hitter);
    const int local = hitter != nullptr ? Resolve(self, msg.id, msg.object, msg.x, msg.y) : 0;
    yyc::CInstance* target = local != 0 ? yyc::FindInstance(local) : nullptr;
    if (target == nullptr) return;
    puppet::BeginActing(msg.hitter);
    DryHit(target, hitter);
    puppet::EndActing();
}

// Guest: a tool hit is `with (target) event_user(0)` with the player as `other`; the host decides its outcome.
// Host: its own tool hits are shown on the guest's copy (health bar).
void __cdecl EventUserHook(yyc::CInstance* self, yyc::CInstance* other, int n) {
    if (IsHost() && Connected() && IsToolHit(self, other, n)) {
        const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
        g_hitByGuest.erase(id);  // the last hit decides who gets the drop (playtest 4, bug 1)
        const bool synced = IsSynced(self, static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)));
        g_origEventUser(self, other, n);
        if (synced) SendHp(yyc::FindInstance(id), net::kHostPeer);
        return;
    }
    if (!IsGuest() || g_applyingRemote > 0 || !Connected() || !IsToolHit(self, other, n)) {
        g_origEventUser(self, other, n);
        return;
    }
    const InstanceInfo info = Describe(self);
    if (!IsSynced(self, info.object)) {
        g_origEventUser(self, other, n);
        return;
    }
    SendHit(info);
    Log("sync: hit object %d at (%.0f, %.0f) sent to the host", info.object, info.x, info.y);
    // Enemies too: their hit event is the sound, flash and squash of the hit - the guest's sword was silent on them.
    DryHit(self, other);
}

// Guest: a repair kit (UseRepairKit: every structure near the player back to hpMax) mends the host's structures,
// not only the guest's copies of them (playtest 4, bug 6).
yyc::RValue* __cdecl RepairKitHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                   yyc::RValue** args) {
    if (!IsGuest() || !Connected()) return g_origRepairKit(self, other, result, argc, args);
    constexpr int kParStructure = yyc::obj::parStructure;
    std::vector<std::pair<int, float>> before;
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(kParStructure)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {double(kParStructure), double(i)}));
        if (yyc::FindInstance(id) != nullptr) before.emplace_back(id, ReadVar(id, kSlotHp));
    }
    yyc::RValue* r = g_origRepairKit(self, other, result, argc, args);
    for (const auto& [id, hp] : before) {
        yyc::CInstance* structure = yyc::FindInstance(id);
        if (structure == nullptr || ReadVar(id, kSlotHp) == hp) continue;
        const InstanceInfo info = Describe(structure);
        const HpMsg msg{HostIdOf(id), info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                        ReadVar(id, kSlotHp), ReadVar(id, kSlotHpAlpha), ReadVar(id, kSlotHpLast),
                        ReadVar(id, kSlotHpWhite), kNoHitter};
        net::Send(kObjectHp, &msg, sizeof(msg));
        Log("sync: repaired structure %d at (%.0f, %.0f): hp %.0f -> %.0f, sent to the host", info.object, info.x,
            info.y, hp, msg.hp);
    }
    return r;
}

// `woken`: the second try, after waking up the object's kind (the host may have culled what the guest hit).
void ApplyHit(yyc::CInstance* self, net::PeerId from, const ObjectMsg& msg, bool woken = false) {
    yyc::CInstance* target = ResolveOnHost(self, msg);
    yyc::CInstance* guest = puppet::Instance(from);
    if (target == nullptr && guest != nullptr && !woken) {
        WakeThen(self, msg.object, [self, from, msg] { ApplyHit(self, from, msg, true); });
        return;
    }
    Log("sync: guest hit object %d (host id %d) at (%.0f, %.0f): %s (dmg %.1f, guest %u)", msg.object, msg.id,
        msg.x, msg.y, target == nullptr ? "not found" : guest == nullptr ? "no puppet" : "applied", msg.depth, from);
    if (target == nullptr || guest == nullptr) return;
    const int targetId = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_id));
    g_hitByGuest[targetId] = from;
    // Like UsePickaxe: marks the hit as the player's, which enemies need to reward XP when they die.
    yyc::RValue killedByPlayer = yyc::Real(yyc::GetBuiltinReal(target, kYYVarRoomSpeed));
    yyc::SetVar(targetId, kSlotKilledByPlayer, &killedByPlayer);
    // The guest's tool decides the damage: the puppet carries the guest's `dmg` for this hit (PLAYTEST.md #6).
    const int guestId = static_cast<int>(yyc::GetBuiltinReal(guest, yyc::va::YYVar_id));
    const float puppetDmg = ReadVar(guestId, kSlotDmg);
    if (msg.depth > 0) {
        yyc::RValue dmg = yyc::Real(msg.depth);
        yyc::SetVar(guestId, kSlotDmg, &dmg);
    }
    puppet::BeginActing(from);
    yyc::PerformEvent(target, guest, yyc::kEvOther, yyc::kEvUser0);
    puppet::EndActing();
    if (msg.depth > 0 && yyc::FindInstance(guestId) != nullptr) {
        yyc::RValue dmg = yyc::Real(puppetDmg);
        yyc::SetVar(guestId, kSlotDmg, &dmg);
    }
    SendHp(yyc::FindInstance(targetId), from);  // gone if the hit destroyed it
}

void SendStatus(yyc::CInstance* target, float burn, float poison) {
    const InstanceInfo info = Describe(target);
    const StatusMsg msg{HostIdOf(info.id), info.object, static_cast<float>(info.x), static_cast<float>(info.y), burn,
                        poison};
    net::Send(kObjectStatus, &msg, sizeof(msg));
}

bool ReadNumber(int id, int slot, float* out) {
    yyc::RValue value{};
    const bool number = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    if (number) *out = static_cast<float>(yyc::ToReal(value));
    yyc::Free(&value);
    return number;
}

// Every few steps: the world objects here whose health went down, or that started burning or poisoned, since the
// last look (see g_seen). Enemies and critters are mobs.cpp's.
void WatchObjects(yyc::CInstance* self) {
    if (!Connected() || ++g_watchSteps % kWatchEverySteps != 0) return;
    const int count = static_cast<int>(CallReal(self, "instance_number", {double(kParObject)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(self, "instance_find", {double(kParObject), double(i)}));
        yyc::CInstance* target = yyc::FindInstance(id);
        if (target == nullptr || interiors::IsInterior(id)) continue;
        const int object = static_cast<int>(yyc::GetBuiltinReal(target, yyc::va::YYVar_object_index));
        float hp = 0, burn = 0, poison = 0;
        if (!ReadNumber(id, kSlotHp, &hp)) continue;
        ReadNumber(id, kSlotBurn, &burn);
        ReadNumber(id, kSlotPoison, &poison);
        const auto found = g_seen.find(id);
        if (found == g_seen.end()) {
            if (!IsMob(self, object)) g_seen[id] = {hp, burn, poison};
            continue;
        }
        Seen& seen = found->second;
        // Both count down: a rise is a new fire or poison.
        const bool started = burn > seen.burn + 0.5f || poison > seen.poison + 0.5f;
        if (started) SendStatus(target, burn, poison);
        if (IsHost() && hp != seen.hp) {
            SendHp(target);  // no hitter: no hit shown, the health bar only
        } else if (IsGuest() && hp < seen.hp && !(burn > 0) && !(poison > 0)) {
            // Our own shot's: the host does it as our hit and tells the health back. By host id or, as a tool hit, by
            // spot (an object from the save may not be bound yet: SendDamage wants a host id).
            const InstanceInfo info = Describe(target);
            ObjectMsg msg{HostIdOf(id), info.object, static_cast<float>(info.x), static_cast<float>(info.y),
                          seen.hp - hp, kNoItem, kOwnerAnyone, -1, 0};
            if (net::Send(kHit, &msg, sizeof(msg))) ++g_sent[kHit];
            Log("sync: own shot's damage %.1f to object %d at (%.0f, %.0f) sent to the host", msg.depth, info.object,
                info.x, info.y);
        }
        seen = {hp, burn, poison};
    }
}

void ApplyStatus(yyc::CInstance* self, const StatusMsg& msg) {
    if (!std::isfinite(msg.x) || !std::isfinite(msg.y) || !std::isfinite(msg.burn) || !std::isfinite(msg.poison) ||
        msg.burn < 0 || msg.poison < 0 || msg.burn > 100000 || msg.poison > 100000 || !IsSynced(self, msg.object)) {
        return;
    }
    const int local = Resolve(self, msg.hostId, msg.object, msg.x, msg.y);
    if (local == 0) return;
    const int slots[2] = {kSlotBurn, kSlotPoison};
    const float values[2] = {msg.burn, msg.poison};
    for (int i = 0; i < 2; ++i) {
        float now = 0;
        if (!ReadNumber(local, slots[i], &now) || now >= values[i]) continue;  // never a variable it has not got
        yyc::RValue v = yyc::Real(values[i]);
        yyc::SetVar(local, slots[i], &v);
    }
    if (const auto seen = g_seen.find(local); seen != g_seen.end()) {  // not news to send back
        seen->second.burn = (std::max)(seen->second.burn, msg.burn);
        seen->second.poison = (std::max)(seen->second.poison, msg.poison);
    }
}

}  // namespace

namespace internal {

void Hit(yyc::CInstance* localPlayer, yyc::CInstance* target) {
    const InstanceInfo info = sync::Describe(target);
    SendHit(info);
    DryHit(target, localPlayer);  // as a real tool hit shows it
}

}  // namespace internal

bool InstallHitHooks() {
    bool ok = Hook(kScriptUseRepairKit, reinterpret_cast<void*>(&RepairKitHook), &g_origRepairKit);
    ok = Hook(kEventUser, reinterpret_cast<void*>(&EventUserHook), &g_origEventUser) && ok;
    return ok;
}

void WatchHits(yyc::CInstance* localPlayer) { WatchObjects(localPlayer); }

bool HandleHits(const net::Message& message, yyc::CInstance* localPlayer) {
    const size_t size = message.payload.size();
    if (message.type == kObjectStatus) {
        if (size == sizeof(StatusMsg)) {
            StatusMsg msg{};
            memcpy(&msg, message.payload.data(), sizeof(msg));
            ApplyStatus(localPlayer, msg);
        }
        return true;
    }
    if (message.type == kHit) {
        if (size == sizeof(ObjectMsg) && IsHost()) {
            ObjectMsg msg{};
            memcpy(&msg, message.payload.data(), sizeof(msg));
            ApplyHit(localPlayer, message.from, msg);
        }
        return true;
    }
    if (message.type != kObjectHp) return false;
    if (size == sizeof(HpMsg)) {
        HpMsg msg{};
        memcpy(&msg, message.payload.data(), sizeof(msg));
        if (IsGuest()) ShowOthersHit(localPlayer, msg);
        ApplyHp(localPlayer, msg);
        if (IsHost()) {  // a guest's repair: the other guests see it too
            const int local = Resolve(localPlayer, msg.id, msg.object, msg.x, msg.y);
            if (local != 0) SendHp(yyc::FindInstance(local));
        }
    }
    return true;
}

bool TakeGuestHitter(int id, net::PeerId* guest) {
    const auto found = g_hitByGuest.find(id);
    if (found == g_hitByGuest.end()) return false;
    *guest = found->second;
    g_hitByGuest.erase(found);
    return true;
}

void ForgetHits() {
    g_hitByGuest.clear();
    g_seen.clear();
    g_watchSteps = 0;
}

void SendDamage(int localId, float amount) {
    yyc::CInstance* target = yyc::FindInstance(localId);
    const int hostId = HostIdOf(localId);
    if (!IsGuest() || target == nullptr || hostId < 0 || !(amount > 0) || !Connected()) return;
    const InstanceInfo info = Describe(target);
    ObjectMsg msg{hostId, info.object, static_cast<float>(info.x), static_cast<float>(info.y), amount,
                  kNoItem, kOwnerAnyone, -1, 0};
    if (net::Send(kHit, &msg, sizeof(msg))) ++g_sent[kHit];
    Log("sync: own damage %.1f to object %d at (%.0f, %.0f) sent to the host", amount, info.object, info.x, info.y);
}

}  // namespace coop::sync
