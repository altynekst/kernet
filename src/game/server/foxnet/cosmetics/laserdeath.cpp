#include "laserdeath.h"

#include <base/log.h>

#include <base/math.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/server/gamecontext.h>
#include <game/server/gameworld.h>

#include <random>

CLaserDeath::CLaserDeath(CGameWorld *pGameWorld, int Owner, vec2 Pos, CClientMask Mask) :
	CEntityOwned(pGameWorld, Owner, CGameWorld::ENTTYPE_LASERDEATH, Pos)
{
	m_Pos = Pos;
	m_Mask = Mask;

	std::random_device rd;
	std::uniform_int_distribution<long> dist(5, 50);
	for(int i = 0; i < MAX_PARTICLES; i++)
	{
		m_aIds[i] = Server()->SnapNewId();
		const long Random = dist(rd) + i;
		m_aStartTick[i] = Server()->Tick() + TICKDELAY * i;
		m_aPos[i] = m_Pos + random_direction() * (float)Random;
	}
	m_EndTick = Server()->Tick() + TICKDELAY * MAX_PARTICLES;
	GameWorld()->InsertEntity(this);
}

void CLaserDeath::Reset()
{
	if(m_MarkedForDestroy)
		return;
	for(int i = 0; i < MAX_PARTICLES; i++)
		Server()->SnapFreeId(m_aIds[i]);
	m_MarkedForDestroy = true;
}

void CLaserDeath::Tick()
{
	if(m_MarkedForDestroy)
		return;

	if(Server()->Tick() > m_EndTick + Server()->TickSpeed())
	{
		Reset();
		return;
	}

	for(int i = 0; i < MAX_PARTICLES; i++)
	{
		if(Server()->Tick() == m_aStartTick[i])
			GameServer()->CreateSound(m_Pos, SOUND_BODY_LAND, m_Mask);
	}
}

void CLaserDeath::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;
	if(!CanSnapEntity(SnappingClient))
		return;
	if(!m_Mask.test(SnappingClient))
		return;

	const int SnapVer = Server()->GetClientVersion(SnappingClient);
	const bool SixUp = Server()->IsSixup(SnappingClient);

	for(int i = 0; i < MAX_PARTICLES; i++)
	{
		if(Server()->Tick() < m_aStartTick[i])
			continue;
		vec2 LaserPos = m_aPos[i];
		GameServer()->SnapLaserObject(CSnapContext(SnapVer, SixUp, SnappingClient), m_aIds[i], LaserPos, LaserPos, Server()->Tick(), -1, LASERTYPE_GUN, -1, -1);
	}
}
