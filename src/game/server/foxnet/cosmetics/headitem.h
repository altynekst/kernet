#ifndef GAME_SERVER_FOXNET_COSMETICS_HEADITEM_H
#define GAME_SERVER_FOXNET_COSMETICS_HEADITEM_H

#include <base/vmath.h>

#include <game/server/foxnet/entities/foxnet_entity.h>

enum
{
	HEADITEM_COSMETIC = 1,
};

class CHeadItem : public CEntityOwned
{
	int m_Type;
	vec2 m_Offset;
	int m_SnapId;

public:
	CHeadItem(CGameWorld *pGameWorld, int Owner, vec2 Pos, int Type, vec2 Offset);
	void Reset() override;
	void Tick() override;
	void Snap(int SnappingClient) override;
};

#endif // GAME_SERVER_FOXNET_COSMETICS_HEADITEM_H
