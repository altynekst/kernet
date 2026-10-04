#include "halo.h"

#include <base/log.h>
#include <base/math.h>

#include <algorithm>

#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

CHalo::CHalo(CGameWorld *pGameWorld, int Owner, vec2 Pos) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_HALO, Pos)
{
	m_Pos = Pos;
	m_StartTick = Server()->Tick();
	for(auto &S : m_aSnap)
		S.m_Id = Server()->SnapNewId();
	std::sort(std::begin(m_aSnap), std::end(m_aSnap), [](const SSnap &a, const SSnap &b) { return a.m_Id < b.m_Id; });
	GameWorld()->InsertEntity(this);
}

void CHalo::Reset()
{
	if(m_MarkedForDestroy)
		return;
	for(auto &S : m_aSnap)
		Server()->SnapFreeId(S.m_Id);
	m_MarkedForDestroy = true;
}

void CHalo::Tick()
{
	if(m_MarkedForDestroy)
		return;

	CPlayer *pPl = GetPlayer();
	if(!pPl || !pPl->m_CosmeticHalo)
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
	SetData();
}

void CHalo::SetData()
{
	const int Tick = Server()->Tick() - m_StartTick;
	const float L = 40.0f;
	const vec2 Center(0.0f, -64.0f);

	for(int i = 0; i < NUM_IDS; i++)
	{
		m_aSnap[i].m_Pos = Center;
		m_aSnap[i].m_Pos.x = sinf(Tick * 0.025f + (float)i) * L;
		const float OffsetY = sinf(Tick * 0.1f + (float)i) * 8.0f;
		m_aSnap[i].m_Pos.y += OffsetY;
		const float TiltRad = sinf(Tick * 0.03f) * 0.15f;
		const float TiltDeg = TiltRad * 180.0f / pi;
		const vec2 Pivot = Center + vec2(0.0f, OffsetY);
		m_aSnap[i].m_Pos = Pivot + rotate(m_aSnap[i].m_Pos - Pivot, TiltDeg);
	}
}

void CHalo::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(!CanSnapEntity(SnappingClient))
		return;

	const int SnapVer = Server()->GetClientVersion(SnappingClient);
	const bool SixUp = Server()->IsSixup(SnappingClient);

	for(const auto &S : m_aSnap)
	{
		vec2 From = m_Pos + S.m_Pos;
		vec2 To = m_Pos + S.m_Pos;
		GameServer()->SnapLaserObject(CSnapContext(SnapVer, SixUp, SnappingClient), S.m_Id, To, From, Server()->Tick(), m_Owner, LASERTYPE_GUN, -1, -1);
	}
}
