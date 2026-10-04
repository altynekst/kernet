#ifndef GAME_SERVER_FOXNET_ITEM_REGISTRY_H
#define GAME_SERVER_FOXNET_ITEM_REGISTRY_H

#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>

constexpr int MAX_ITEM_STARS = 5;

enum class EFoxItemId
{
	RainbowFeet,
	RainbowBody,
	RainbowHook,
	EmoticonGun,
	PhaseGun,
	HeartGun,
	MixedGun,
	LaserGun,
	PhysicalBullet,
	IndicatorClockwise,
	IndicatorCounterclockwise,
	IndicatorInwardTurning,
	IndicatorOutwardTurning,
	IndicatorLine,
	IndicatorCrisscross,
	DeathExplosive,
	DeathHammerHit,
	DeathIndicator,
	DeathLaser,
	TrailStar,
	TrailDot,
	HammerHat,
	GunHat,
	ShotgunHat,
	GrenadeHat,
	LaserHat,
	NinjaHat,
	PartyHat,
	TophatHat,
	HeartHat,
	Sparkle,
	InverseAim,
	Lovely,
	RotatingBall,
	Halo,
	BOOSTER,
	VIP,
	MVP,
	LootCaseCommon,
	LootCaseUncommon,
	LootCaseRare,
	LootCaseExotic,
	WeaponLaser,
	WeaponShotgun,
	WeaponGrenade,
};

enum class EFoxItemType
{
	Role,
	Case,
	Hat,
	Gun,
	Trail,
	Effect,
	Death,
	Indicator,
	Rainbow,
	Weapon,
	COUNT
};

enum class EFoxItemRarity
{
	Common,
	Uncommon,
	Rare,
	Epic,
	Mythic,
	Legendary
};

enum class EFoxItemFlag
{
	None = 0,
	Equippable = 1 << 0,
	Consumable = 1 << 1,
	LootCase = 1 << 2
};

inline constexpr EFoxItemFlag operator|(EFoxItemFlag a, EFoxItemFlag b)
{
	using U = std::underlying_type_t<EFoxItemFlag>;
	return static_cast<EFoxItemFlag>(static_cast<U>(a) | static_cast<U>(b));
}

inline constexpr bool FoxHasFlag(EFoxItemFlag f, EFoxItemFlag test)
{
	using U = std::underlying_type_t<EFoxItemFlag>;
	return (static_cast<U>(f) & static_cast<U>(test)) != 0;
}

inline std::string FoxStarsString(int Stars)
{
	std::string s;
	for(int i = 0; i < Stars; i++)
		s += "★";
	for(int i = Stars; i < MAX_ITEM_STARS; i++)
		s += "☆";
	return s;
}

inline const char *FoxItemTypeToName(EFoxItemType Type)
{
	switch(Type)
	{
	case EFoxItemType::Role: return "Rᴏʟᴇs";
	case EFoxItemType::Case: return "Cᴀsᴇs";
	case EFoxItemType::Hat: return "Hᴀᴛs";
	case EFoxItemType::Gun: return "Gᴜɴs";
	case EFoxItemType::Trail: return "Tʀᴀɪʟs";
	case EFoxItemType::Effect: return "Eғғᴇᴄᴛs";
	case EFoxItemType::Death: return "Dᴇᴀᴛʜ Eғғᴇᴄᴛs";
	case EFoxItemType::Indicator: return "Gᴜɴ Hɪᴛ Eғғᴇᴄᴛs";
	case EFoxItemType::Rainbow: return "Rᴀɪɴʙᴏᴡ Eғғᴇᴄᴛs";
	case EFoxItemType::Weapon: return "Wᴇᴀᴘᴏɴs";
	default: return "Oᴛʜᴇʀ";
	}
}

inline const char *FoxRarityToName(EFoxItemRarity Type)
{
	switch(Type)
	{
	case EFoxItemRarity::Common: return "Common";
	case EFoxItemRarity::Uncommon: return "Uncommon";
	case EFoxItemRarity::Rare: return "Rare";
	case EFoxItemRarity::Epic: return "Epic";
	case EFoxItemRarity::Mythic: return "Mythic";
	case EFoxItemRarity::Legendary: return "Legendary";
	default: return "Unknown";
	}
}

enum class EFoxExclusiveGroup
{
	None,
	Hat,
	Trail,
	Gun,
	DamageIndicator,
	DeathEffect
};

class CPlayer;

class CFoxItemConfig
{
public:
	EFoxItemId m_Id;
	EFoxItemType m_Type;
	const char *m_pName;
	const char *m_pShortcut;
	EFoxItemFlag m_Flags;
	EFoxExclusiveGroup m_Group;
	long m_Price;
	int m_MinLevel;
	int m_Stars;
	EFoxItemRarity m_Rarity;
	const char *m_pDescription;
	std::function<void(CPlayer &, const CFoxItemConfig &, int)> m_Apply;
	std::function<void(CPlayer &, const CFoxItemConfig &, int)> m_Remove;
	int m_DefaultDays;

	CFoxItemConfig(EFoxItemId Id,
		EFoxItemType Type,
		const char *pName,
		const char *pShortcut,
		EFoxItemFlag Flags,
		EFoxExclusiveGroup Group,
		int Price,
		int MinLevel,
		int Stars,
		EFoxItemRarity Rarity,
		const char *pDescription,
		std::function<void(CPlayer &, const CFoxItemConfig &, int)> Apply,
		std::function<void(CPlayer &, const CFoxItemConfig &, int)> Remove,
		int DefaultDays = 30) :
		m_Id(Id),
		m_Type(Type),
		m_pName(pName),
		m_pShortcut(pShortcut),
		m_Flags(Flags),
		m_Group(Group),
		m_Price(Price),
		m_MinLevel(MinLevel),
		m_Stars(Stars),
		m_Rarity(Rarity),
		m_pDescription(pDescription),
		m_Apply(std::move(Apply)),
		m_Remove(std::move(Remove)),
		m_DefaultDays(DefaultDays)
	{
	}
};

class CFoxItemRegistry
{
	std::unordered_map<std::string, CFoxItemConfig> m_Map;

public:
	void Init();
	const CFoxItemConfig *FindByName(const char *pName) const;
	CFoxItemConfig *FindMutableByName(const char *pName);

	const std::unordered_map<std::string, CFoxItemConfig> &Map() const { return m_Map; }
};

#endif // GAME_SERVER_FOXNET_ITEM_REGISTRY_H
