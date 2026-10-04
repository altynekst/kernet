#include "lovely.h"

#include <base/log.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/collision.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

CLovely::CLovely(CGameWorld *pGameWorld, int Owner, vec2 Pos) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_LOVELY, Pos)
{
	m_SpawnDelay = 0;
	for(auto &H : m_aData)
	{
		H.m_Id = Server()->SnapNewId();
		H.m_Lifespan = -1;
		H.m_Pos = vec2(0, 0);
	}
	GameWorld()->InsertEntity(this);
}

void CLovely::Reset()
{
	if(m_MarkedForDestroy)
		return;
	for(auto &H : m_aData)
		Server()->SnapFreeId(H.m_Id);
	m_MarkedForDestroy = true;
}

void CLovely::Tick()
{
	if(m_MarkedForDestroy)
		return;

	CPlayer *pPl = GetPlayer();
	if(!pPl || !pPl->m_CosmeticLovely)
	{
		Reset();
		return;
	}
	CCharacter *pChr = GetCharacter();
	if(!pChr)
	{
		Reset();
		return;
	}

	m_Pos = pChr->GetPos();

	m_SpawnDelay--;
	if(m_SpawnDelay <= 0)
	{
		SpawnNewHeart();
		const int SpawnTime = 45;
		m_SpawnDelay = Server()->TickSpeed() - (rand() % (SpawnTime - (SpawnTime - 10) + 1) + (SpawnTime - 10));
	}

	for(auto &H : m_aData)
	{
		if(H.m_Lifespan == -1)
			continue;
		H.m_Lifespan--;
		H.m_Pos.y -= 2.4f;
		if(H.m_Lifespan == 0 || GetCollision()->TestBox(H.m_Pos, vec2(14.f, 14.f)))
			H.m_Lifespan = -1;
	}
}

void CLovely::SpawnNewHeart()
{
	for(auto &H : m_aData)
	{
		if(H.m_Lifespan > 0)
			continue;
		CCharacter *pOwner = GetCharacter();
		if(!pOwner)
			return;
		H.m_Lifespan = (int)(Server()->TickSpeed() / 2.5f);
		H.m_Pos = vec2(pOwner->GetPos().x + (rand() % 50 - 25), pOwner->GetPos().y - 30);
		pOwner->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
		break;
	}
}

void CLovely::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(!CanSnapEntity(SnappingClient))
		return;

	const int SnapVer = Server()->GetClientVersion(SnappingClient);
	const bool SixUp = Server()->IsSixup(SnappingClient);

	for(const auto &H : m_aData)
	{
		if(H.m_Lifespan == -1)
			continue;
		GameServer()->SnapPickup(CSnapContext(SnapVer, SixUp, SnappingClient), H.m_Id, H.m_Pos, POWERUP_HEALTH, -1, -1, PICKUPFLAG_NO_PREDICT);
	}
}
