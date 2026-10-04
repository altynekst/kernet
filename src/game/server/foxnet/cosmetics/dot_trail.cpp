#include "dot_trail.h"

#include <base/log.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

#include "cosmetic_types.h"

CDotTrail::CDotTrail(CGameWorld *pGameWorld, int Owner, vec2 Pos) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_DOT_TRAIL, Pos)
{
	m_Pos = Pos;
	m_LastPos = Pos;
	GameWorld()->InsertEntity(this);
}

void CDotTrail::Reset()
{
	if(m_MarkedForDestroy)
		return;
	m_MarkedForDestroy = true;
}

void CDotTrail::Tick()
{
	if(m_MarkedForDestroy)
		return;

	CPlayer *pPl = GetPlayer();
	if(!pPl || pPl->m_CosmeticTrailType != TRAILTYPE_DOT)
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

	m_LastPos = m_Pos;
	m_Pos = pChr->GetPos();
}

void CDotTrail::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	if(!CanSnapEntity(SnappingClient))
		return;

	if(m_LastPos == m_Pos)
		return;

	CNetObj_DDNetProjectile *pProj = Server()->SnapNewItem<CNetObj_DDNetProjectile>(GetId());
	if(!pProj)
		return;

	pProj->m_X = round_to_int(m_Pos.x * 100.0f);
	pProj->m_Y = round_to_int(m_Pos.y * 100.0f);
	pProj->m_Type = WEAPON_HAMMER;
	pProj->m_Owner = m_Owner;
	pProj->m_StartTick = 0;
	pProj->m_VelX = 0;
	pProj->m_VelY = 0;
}
