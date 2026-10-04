/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "pickupdrop.h"

#include "character.h"

#include <generated/protocol.h>

#include <game/gamecore.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

static constexpr float PICKUPDROP_COLLECT_RADIUS = 32.0f;

CPickupDrop::CPickupDrop(CGameWorld *pGameWorld, int Subtype, vec2 Pos, vec2 Vel, int DroppedByCid, int LifetimeTicks) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_PICKUP, Pos, (int)(CCharacterCore::PhysicalSize() * 0.5f))
{
	m_StartTick = Server()->Tick();
	m_Lifetime = LifetimeTicks;
	m_PickupDelay = (int)(Server()->TickSpeed() * 1.5f);
	m_Subtype = Subtype;
	m_OwnerCid = DroppedByCid;
	m_Vel = Vel;

	GameWorld()->InsertEntity(this);
}

void CPickupDrop::Reset()
{
	m_MarkedForDestroy = true;
}

void CPickupDrop::Tick()
{
	if(m_MarkedForDestroy)
		return;

	if(m_Lifetime > 0)
		m_Lifetime--;
	if(m_Lifetime == 0)
	{
		Reset();
		return;
	}

	if(Collect())
		return;

	Move();
}

void CPickupDrop::TickPaused()
{
}

bool CPickupDrop::Collect()
{
	if(m_PickupDelay > 0)
	{
		m_PickupDelay--;
		return false;
	}

	CEntity *apEnts[MAX_CLIENTS];
	int Num = GameWorld()->FindEntities(m_Pos, PICKUPDROP_COLLECT_RADIUS, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		auto *pChr = static_cast<CCharacter *>(apEnts[i]);
		if(!pChr || !pChr->IsAlive())
			continue;
		if(pChr->GetWeaponGot(m_Subtype))
			continue;

		pChr->GiveWeapon(m_Subtype);
		pChr->SetActiveWeapon(m_Subtype);
		int Sound = SOUND_PICKUP_HEALTH;
		if(m_Subtype == WEAPON_GRENADE)
			Sound = SOUND_PICKUP_GRENADE;
		else if(m_Subtype == WEAPON_SHOTGUN)
			Sound = SOUND_PICKUP_SHOTGUN;
		else if(m_Subtype == WEAPON_LASER)
			Sound = SOUND_PICKUP_SHOTGUN;
		GameServer()->CreateSound(pChr->m_Pos, Sound, pChr->TeamMask());
		if(pChr->GetPlayer())
			GameServer()->SendWeaponPickup(pChr->GetPlayer()->GetCid(), m_Subtype);

		Reset();
		return true;
	}
	return false;
}

void CPickupDrop::Move()
{
	m_Vel.y += GlobalTuning()->m_Gravity;

	bool Grounded = false;
	Collision()->MoveBox(&m_Pos, &m_Vel, CCharacterCore::PhysicalSizeVec2(), vec2(0.5f, 0.5f), &Grounded);
	if(Grounded)
		m_Vel.x *= 0.88f;
	m_Vel.x *= 0.98f;
}

void CPickupDrop::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	const int Tick = Server()->Tick() - m_StartTick;
	// Blink when about to disappear (last 10 seconds)
	if(m_Lifetime < Server()->TickSpeed() * 10 && (Tick / (Server()->TickSpeed() / 4)) % 2 == 0)
		return;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	bool Sixup = Server()->IsSixup(SnappingClient);

	GameServer()->SnapPickup(CSnapContext(SnappingClientVersion, Sixup, SnappingClient), GetId(), m_Pos, POWERUP_WEAPON, m_Subtype, 0, PICKUPFLAG_NO_PREDICT);
}
