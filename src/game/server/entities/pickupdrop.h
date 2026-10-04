/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_ENTITIES_PICKUPDROP_H
#define GAME_SERVER_ENTITIES_PICKUPDROP_H

#include <base/vmath.h>

#include <game/server/entity.h>

class CPickupDrop : public CEntity
{
	int m_StartTick;
	int m_Lifetime;
	int m_PickupDelay;
	int m_Subtype;
	int m_OwnerCid;
	vec2 m_Vel;

	bool Collect();
	void Move();

public:
	CPickupDrop(CGameWorld *pGameWorld, int Subtype, vec2 Pos, vec2 Vel, int DroppedByCid, int LifetimeTicks);

	void Reset() override;
	void Tick() override;
	void TickPaused() override;
	void Snap(int SnappingClient) override;
};

#endif
