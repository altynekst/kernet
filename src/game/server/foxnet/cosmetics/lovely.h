#ifndef GAME_SERVER_FOXNET_COSMETICS_LOVELY_H
#define GAME_SERVER_FOXNET_COSMETICS_LOVELY_H

#include <base/vmath.h>

#include <game/server/foxnet/entities/foxnet_entity.h>

class CLovely : public CEntityOwned
{
	static constexpr int MAX_HEARTS = 16;

	struct SHeart
	{
		int m_Id;
		vec2 m_Pos;
		int m_Lifespan;
	};

	SHeart m_aData[MAX_HEARTS];
	int m_SpawnDelay;

	void SpawnNewHeart();

public:
	CLovely(CGameWorld *pGameWorld, int Owner, vec2 Pos);
	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
};

#endif // GAME_SERVER_FOXNET_COSMETICS_LOVELY_H
