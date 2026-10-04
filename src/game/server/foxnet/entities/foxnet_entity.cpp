#include "foxnet_entity.h"

#include <base/dbg.h>

#include <game/collision.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

CEntityOwned::CEntityOwned(CGameWorld *pGameWorld, int Owner, int Objtype, vec2 Pos, int ProximityRadius) :
	CEntity(pGameWorld, Objtype, Pos, ProximityRadius)
{
	m_Owner = Owner;
	if(GetCharacter())
		m_StartTeamMask = TeamMask();
}

bool CEntityOwned::CanSnapEntity(int SnappingClient, CPlayer **ppSnapPlayer)
{
	if(m_MarkedForDestroy)
		return false;
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return true;

	CPlayer *pSnapPlayer = GameServer()->m_apPlayers[SnappingClient];
	if(!pSnapPlayer || !GetCharacter())
		return false;
	if(pSnapPlayer->m_HideOthersCosmetics && SnappingClient != m_Owner)
		return false;
	if(GetCharacter()->IsPaused())
		return false;
	if(!TeamMask().test(SnappingClient))
		return false;
	if(pSnapPlayer->GetCharacter() && GetCharacter())
		if(!GetCharacter()->CanSnapCharacter(SnappingClient))
			return false;

	if(ppSnapPlayer)
		*ppSnapPlayer = pSnapPlayer;
	return true;
}

CPlayer *CEntityOwned::GetPlayer()
{
	dbg_assert(m_Owner >= 0 && m_Owner < MAX_CLIENTS, "invalid owner id %d", m_Owner);
	if(Server()->ClientSlotEmpty(m_Owner))
		return nullptr;
	return GameServer()->m_apPlayers[m_Owner];
}

CCharacter *CEntityOwned::GetCharacter()
{
	dbg_assert(m_Owner >= 0 && m_Owner < MAX_CLIENTS, "invalid owner id %d", m_Owner);
	if(Server()->ClientSlotEmpty(m_Owner))
		return nullptr;
	CPlayer *pPlayer = GetPlayer();
	if(!pPlayer)
		return nullptr;
	return pPlayer->GetCharacter();
}

CCollision *CEntityOwned::GetCollision()
{
	dbg_assert(m_Owner >= 0 && m_Owner < MAX_CLIENTS, "invalid owner id %d", m_Owner);
	if(Server()->ClientSlotEmpty(m_Owner))
		return Collision();
	return Collision();
}

CClientMask CEntityOwned::TeamMask()
{
	CCharacter *pCharacter = GetCharacter();
	if(!pCharacter)
		return CClientMask().set();
	return pCharacter->TeamMask();
}
