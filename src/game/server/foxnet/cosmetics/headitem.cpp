#include "headitem.h"

#include <base/log.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

#include "cosmetic_types.h"

CHeadItem::CHeadItem(CGameWorld *pGameWorld, int Owner, vec2 Pos, int Type, vec2 Offset) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_HEADITEM, Pos)
{
	m_Pos = Pos;
	m_Type = Type;
	m_Offset = Offset;
	m_SnapId = Server()->SnapNewId();
	GameWorld()->InsertEntity(this);
}

void CHeadItem::Reset()
{
	if(m_MarkedForDestroy)
		return;
	Server()->SnapFreeId(m_SnapId);
	m_MarkedForDestroy = true;
}

void CHeadItem::Tick()
{
	if(m_MarkedForDestroy)
		return;

	CPlayer *pPl = GetPlayer();
	CCharacter *pChr = GetCharacter();
	if(!pPl || !pChr)
	{
		Reset();
		return;
	}

	if(m_Type == HEADITEM_COSMETIC)
	{
		if(pPl->m_CosmeticHatType == HATTYPE_NONE)
		{
			Reset();
			return;
		}
	}

	m_Pos = pChr->GetPos();
}

void CHeadItem::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(!CanSnapEntity(SnappingClient))
		return;

	CPlayer *pPl = GetPlayer();
	CCharacter *pChr = GetCharacter();
	if(!pPl || !pChr)
		return;

	const int SnapVer = Server()->GetClientVersion(SnappingClient);
	const bool SixUp = Server()->IsSixup(SnappingClient);

	int Type = POWERUP_WEAPON;
	int SubType = 0;
	if(m_Type == HEADITEM_COSMETIC)
	{
		SubType = pPl->m_CosmeticHatType > 0 ? (pPl->m_CosmeticHatType - 1) : 0;
	}

	vec2 Pos = pChr->GetPos() + m_Offset;
	GameServer()->SnapPickup(CSnapContext(SnapVer, SixUp, SnappingClient), m_SnapId, Pos, Type, SubType, -1, PICKUPFLAG_NO_PREDICT);
}
