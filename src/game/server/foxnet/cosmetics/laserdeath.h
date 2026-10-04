#ifndef GAME_SERVER_FOXNET_COSMETICS_LASER_DEATH_H
#define GAME_SERVER_FOXNET_COSMETICS_LASER_DEATH_H

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <game/server/foxnet/entities/foxnet_entity.h>

class CLaserDeath : public CEntityOwned
{
	static constexpr int MAX_PARTICLES = 28;
	static constexpr int TICKDELAY = 5;

	int m_EndTick;
	CClientMask m_Mask;

	int m_aIds[MAX_PARTICLES];
	vec2 m_aPos[MAX_PARTICLES];
	int m_aStartTick[MAX_PARTICLES];

public:
	CLaserDeath(CGameWorld *pGameWorld, int Owner, vec2 Pos, CClientMask Mask);
	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
};

#endif // GAME_SERVER_FOXNET_COSMETICS_LASER_DEATH_H
