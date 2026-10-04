#include "rotating_ball.h"

#include <base/log.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>
#include <game/server/player.h>

CRotatingBall::CRotatingBall(CGameWorld *pGameWorld, int Owner, vec2 Pos) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_ROTATING_BALL, Pos)
{
	m_Pos = Pos;
	m_LaserDirAngle = 0;
	m_LaserPos = vec2(0, 0);
	m_ProjPos = vec2(0, 0);
	m_ProjId = Server()->SnapNewId();
	GameWorld()->InsertEntity(this);
}

void CRotatingBall::Reset()
{
	if(m_MarkedForDestroy)
		return;
	Server()->SnapFreeId(m_ProjId);
	m_MarkedForDestroy = true;
}

void CRotatingBall::Tick()
{
	if(m_MarkedForDestroy)
		return;

	CPlayer *pPl = GetPlayer();
	if(!pPl || !pPl->m_CosmeticRotatingBall)
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
	m_LaserDirAngle = (m_LaserDirAngle + 4) % 360;
	m_LaserPos.x = 65 * sin(m_LaserDirAngle * pi / 180.0f);
	m_LaserPos.y = 65 * cos(m_LaserDirAngle * pi / 180.0f);
	m_ProjPos.x = m_LaserPos.x + 22 * sin(Server()->Tick() * 13 * pi / 180.0f);
	m_ProjPos.y = m_LaserPos.y + 22 * cos(Server()->Tick() * 13 * pi / 180.0f);
}

void CRotatingBall::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(!CanSnapEntity(SnappingClient))
		return;

	const int SnapVer = Server()->GetClientVersion(SnappingClient);
	const bool SixUp = Server()->IsSixup(SnappingClient);

	vec2 LaserPos = m_Pos + m_LaserPos;
	GameServer()->SnapLaserObject(CSnapContext(SnapVer, SixUp, SnappingClient), GetId(), LaserPos, LaserPos, Server()->Tick(), m_Owner, LASERTYPE_GUN, -1, -1);

	CNetObj_DDNetProjectile *pProj = Server()->SnapNewItem<CNetObj_DDNetProjectile>(m_ProjId);
	if(!pProj)
		return;

	vec2 ProjPos = m_Pos + m_ProjPos;
	pProj->m_X = round_to_int(ProjPos.x * 100.0f);
	pProj->m_Y = round_to_int(ProjPos.y * 100.0f);
	pProj->m_Type = WEAPON_HAMMER;
	pProj->m_Owner = m_Owner;
	pProj->m_StartTick = 0;
	pProj->m_VelX = 0;
	pProj->m_VelY = 0;
}
