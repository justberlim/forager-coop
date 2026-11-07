// A player's arrows (objArrow) and the nuclear sword's fireballs (objFireball) are no world objects: they were seen
// by the shooter only. Each shot goes to the other players, who show a harmless copy of it - it flies the same way,
// but hits nothing and drops nothing (the shooter's own shot does the damage, as before).
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::shots {

// Creates the hooks (enabled by the caller).
bool InstallHooks();

// From the instance_create hook: `createdId` (an `object`) was just made by `self`.
void OnCreated(yyc::CInstance* self, int createdId, int object);

// Game thread, every local player step while connected: sends the shots made since the last step (by now they
// fly: their direction and speed are set).
void Tick();

// Host, from bombs.cpp: a real objExplosion here has just dealt its damage. What it hurt goes to the guests with its
// health, and what it set on fire (`burn`, the void/nuclear sword's fireballs) burns there too - shown only: the
// guests' copies follow the host's health while they burn.
void AfterBlast(yyc::CInstance* explosion);

// Host: explosion `explosionId` is the real one of a guest's fireball blast (made from its message): `owner` is that
// guest. Its step runs as the guest.
bool BlastOwner(int explosionId, net::PeerId* owner);

// Game thread: applies a shot message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// The session is over: forget the shots.
void Reset();

}  // namespace coop::shots
