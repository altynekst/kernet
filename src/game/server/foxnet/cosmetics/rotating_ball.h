#ifndef GAME_SERVER_FOXNET_COSMETICS_ROTATING_BALL_H
#define GAME_SERVER_FOXNET_COSMETICS_ROTATING_BALL_H

#include <base/vmath.h>

#include <game/server/foxnet/entities/foxnet_entity.h>

class CRotatingBall : public CEntityOwned
{
	int m_LaserDirAngle;
	vec2 m_LaserPos;
	vec2 m_ProjPos;
	int m_ProjId;

public:
	CRotatingBall(CGameWorld *pGameWorld, int Owner, vec2 Pos);
	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
};

#endif // GAME_SERVER_FOXNET_COSMETICS_ROTATING_BALL_H
