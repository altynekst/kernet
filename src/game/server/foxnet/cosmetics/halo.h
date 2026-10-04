#ifndef GAME_SERVER_FOXNET_COSMETICS_HALO_H
#define GAME_SERVER_FOXNET_COSMETICS_HALO_H

#include <base/vmath.h>

#include <game/server/foxnet/entities/foxnet_entity.h>

class CHalo : public CEntityOwned
{
	static constexpr int NUM_IDS = 8;

	struct SSnap
	{
		int m_Id;
		vec2 m_Pos;
	};

	SSnap m_aSnap[NUM_IDS];
	int m_StartTick;

	void SetData();

public:
	CHalo(CGameWorld *pGameWorld, int Owner, vec2 Pos);
	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
};

#endif // GAME_SERVER_FOXNET_COSMETICS_HALO_H
