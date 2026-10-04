#include "item_registry.h"

#include <base/str.h>
#include <base/system.h>

#include <algorithm>
#include <string>
#include <utility>

#include <game/server/player.h>
#include <game/server/entities/character.h>

#include "cosmetics/cosmetic_types.h"

const CFoxItemConfig *CFoxItemRegistry::FindByName(const char *pName) const
{
	if(!pName || !pName[0])
		return nullptr;
	auto it = m_Map.find(std::string(pName));
	if(it == m_Map.end())
		return nullptr;
	return &it->second;
}

CFoxItemConfig *CFoxItemRegistry::FindMutableByName(const char *pName)
{
	if(!pName || !pName[0])
		return nullptr;
	auto it = m_Map.find(std::string(pName));
	if(it == m_Map.end())
		return nullptr;
	return &it->second;
}

void CFoxItemRegistry::Init()
{
	auto add = [&](CFoxItemConfig Cfg) {
		for(const auto &kv : m_Map)
		{
			dbg_assert(str_comp_nocase(kv.second.m_pName, Cfg.m_pName) != 0, "duplicate item name");
			dbg_assert(str_comp_nocase(kv.second.m_pShortcut, Cfg.m_pShortcut) != 0, "duplicate item shortcut");
		}
		m_Map.emplace(std::string(Cfg.m_pName), std::move(Cfg));
	};

	auto NoApply = [](CPlayer &, const CFoxItemConfig &, int) {};
	auto NoRemove = [](CPlayer &, const CFoxItemConfig &, int) {};

	auto SetHaloOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHalo(true); };
	auto SetHaloOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHalo(false); };
	auto SetLovelyOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetLovely(true); };
	auto SetLovelyOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetLovely(false); };
	auto SetRotatingBallOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRotatingBall(true); };
	auto SetRotatingBallOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRotatingBall(false); };

	auto SetTrailDot = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetTrail(TRAILTYPE_DOT); };
	auto ClearTrail = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetTrail(TRAILTYPE_NONE); };

	auto SetHatHammer = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_HAMMER); };
	auto SetHatGun = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_GUN); };
	auto SetHatShotgun = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_SHOTGUN); };
	auto SetHatGrenade = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_GRENADE); };
	auto SetHatLaser = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_LASER); };
	auto SetHatNinja = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_NINJA); };
	auto SetHatParty = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_PARTY); };
	auto SetHatHeart = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_HEART); };
	auto ClearHat = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetHatType(HATTYPE_NONE); };

	auto SetDeathExplosion = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDeathEffect(DEATHTYPE_EXPLOSION); };
	auto SetDeathHammerHit = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDeathEffect(DEATHTYPE_HAMMERHIT); };
	auto SetDeathIndicator = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDeathEffect(DEATHTYPE_DAMAGEIND); };
	auto SetDeathLaser = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDeathEffect(DEATHTYPE_LASER); };
	auto ClearDeath = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDeathEffect(DEATHTYPE_NONE); };

	auto SetRainbowFeetOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRainbowFeet(true); };
	auto SetRainbowFeetOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRainbowFeet(false); };
	auto SetRainbowBodyOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRainbowBody(true); };
	auto SetRainbowBodyOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetRainbowBody(false); };

	auto SetSparkleOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetSparkle(true); };
	auto SetSparkleOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetSparkle(false); };
	auto SetInverseAimOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetInverseAim(true); };
	auto SetInverseAimOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetInverseAim(false); };

	auto SetPhaseGunOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetPhaseGun(true); };
	auto SetPhaseGunOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetPhaseGun(false); };

	auto SetGunHeart = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetGunType(CPlayer::GUNTYPE_HEART); };
	auto ClearGunHeart = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_GunType == CPlayer::GUNTYPE_HEART)
			Pl.SetGunType(CPlayer::GUNTYPE_NONE);
	};
	auto SetGunMixed = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetGunType(CPlayer::GUNTYPE_MIXED); };
	auto ClearGunMixed = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_GunType == CPlayer::GUNTYPE_MIXED)
			Pl.SetGunType(CPlayer::GUNTYPE_NONE);
	};
	auto SetGunLaser = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetGunType(CPlayer::GUNTYPE_LASER); };
	auto ClearGunLaser = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_GunType == CPlayer::GUNTYPE_LASER)
			Pl.SetGunType(CPlayer::GUNTYPE_NONE);
	};

	auto SetPhysicalBulletOn = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetPhysicalBullet(true); };
	auto SetPhysicalBulletOff = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetPhysicalBullet(false); };

	auto SetIndClockwise = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_CLOCKWISE); };
	auto ClearIndClockwise = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_CLOCKWISE)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};
	auto SetIndCounterwise = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_COUNTERWISE); };
	auto ClearIndCounterwise = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_COUNTERWISE)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};
	auto SetIndInward = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_INWARD); };
	auto ClearIndInward = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_INWARD)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};
	auto SetIndOutward = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_OUTWARD); };
	auto ClearIndOutward = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_OUTWARD)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};
	auto SetIndLine = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_LINE); };
	auto ClearIndLine = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_LINE)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};
	auto SetIndCriss = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetDamageIndType(CPlayer::INDTYPE_CRISSCROSS); };
	auto ClearIndCriss = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_DamageIndType == CPlayer::INDTYPE_CRISSCROSS)
			Pl.SetDamageIndType(CPlayer::INDTYPE_NONE);
	};

	auto SetTrailStar = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetTrail(TRAILTYPE_STAR); };
	auto ClearTrailStar = [](CPlayer &Pl, const CFoxItemConfig &, int) {
		if(Pl.m_CosmeticTrailType == TRAILTYPE_STAR)
			Pl.SetTrail(TRAILTYPE_NONE);
	};

	auto EmoticonGunApply = [](CPlayer &Pl, const CFoxItemConfig &, int OverrideValue) {
		if(OverrideValue < 0)
		{
			const int Cur = Pl.m_EmoticonGun;
			Pl.SetEmoticonGun(Cur != 0 ? 0 : 1);
		}
		else
		{
			int v = OverrideValue;
			if(v < 0)
				v = 0;
			if(v > NUM_EMOTICONS - 1)
				v = NUM_EMOTICONS - 1;
			Pl.SetEmoticonGun(v);
		}
	};
	auto EmoticonGunRemove = [](CPlayer &Pl, const CFoxItemConfig &, int) { Pl.SetEmoticonGun(0); };

	add({EFoxItemId::RainbowFeet, EFoxItemType::Rainbow, "Rainbow Feet", "R_F", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 21250, 16, 1, EFoxItemRarity::Common, "Makes your feet rainbow", SetRainbowFeetOn, SetRainbowFeetOff, 30});
	add({EFoxItemId::RainbowBody, EFoxItemType::Rainbow, "Rainbow Body", "R_B", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 22000, 20, 1, EFoxItemRarity::Common, "Makes your body rainbow", SetRainbowBodyOn, SetRainbowBodyOff, 30});

	add({EFoxItemId::Sparkle, EFoxItemType::Effect, "Sparkle", "E_S", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 1500, 5, 1, EFoxItemRarity::Common, "Makes you sparkle", SetSparkleOn, SetSparkleOff, 30});
	add({EFoxItemId::Lovely, EFoxItemType::Effect, "Lovely", "E_L", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 12500, 15, 2, EFoxItemRarity::Rare, "Spreading love huh?", SetLovelyOn, SetLovelyOff, 30});
	add({EFoxItemId::InverseAim, EFoxItemType::Effect, "Inverse Aim", "E_I", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 500000, 35, 2, EFoxItemRarity::Legendary, "Shows your aim backwards for others!", SetInverseAimOn, SetInverseAimOff, 30});
	add({EFoxItemId::RotatingBall, EFoxItemType::Effect, "Rotating Ball", "E_R", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 12500, 15, 2, EFoxItemRarity::Rare, "Ball rotate - life good", SetRotatingBallOn, SetRotatingBallOff, 30});
	add({EFoxItemId::Halo, EFoxItemType::Effect, "Halo", "E_H", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 55000, 35, 3, EFoxItemRarity::Epic, "an intertwining halo floating above you", SetHaloOn, SetHaloOff, 30});

	add({EFoxItemId::EmoticonGun, EFoxItemType::Gun, "Emoticon Gun", "G_E", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 12500, 20, 4, EFoxItemRarity::Uncommon, "Shoot emotions at people", EmoticonGunApply, EmoticonGunRemove, 30});
	add({EFoxItemId::PhaseGun, EFoxItemType::Gun, "Phase Gun", "G_P", EFoxItemFlag::Equippable, EFoxExclusiveGroup::None, 8250, 10, 2, EFoxItemRarity::Uncommon, "Your bullets defy physics", SetPhaseGunOn, SetPhaseGunOff, 30});
	add({EFoxItemId::HeartGun, EFoxItemType::Gun, "Heart Gun", "G_H", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Gun, 30000, 25, 1, EFoxItemRarity::Epic, "Shoot bullets full of love", SetGunHeart, ClearGunHeart, 30});
	add({EFoxItemId::MixedGun, EFoxItemType::Gun, "Mixed Gun", "G_M", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Gun, 35000, 35, 1, EFoxItemRarity::Epic, "Shoots Hearts and Shields", SetGunMixed, ClearGunMixed, 30});
	add({EFoxItemId::LaserGun, EFoxItemType::Gun, "Laser Gun", "G_L", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Gun, 45000, 35, 2, EFoxItemRarity::Epic, "Lasertag in DDNet?", SetGunLaser, ClearGunLaser, 30});

	add({EFoxItemId::IndicatorClockwise, EFoxItemType::Indicator, "Clockwise Indicator", "I_C", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 4500, 5, 5, EFoxItemRarity::Common, "Gun Hit -> turns Clockwise", SetIndClockwise, ClearIndClockwise, 30});
	add({EFoxItemId::IndicatorCounterclockwise, EFoxItemType::Indicator, "Counter Clockwise Indicator", "I_CC", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 4500, 5, 5, EFoxItemRarity::Common, "Gun Hit -> turns Counter-Clockwise", SetIndCounterwise, ClearIndCounterwise, 30});
	add({EFoxItemId::IndicatorInwardTurning, EFoxItemType::Indicator, "Inward Turning Indicator", "I_IT", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 8000, 15, 4, EFoxItemRarity::Uncommon, "Gun Hit -> turns Inward", SetIndInward, ClearIndInward, 30});
	add({EFoxItemId::IndicatorOutwardTurning, EFoxItemType::Indicator, "Outward Turning Indicator", "I_OT", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 8000, 15, 4, EFoxItemRarity::Uncommon, "Gun Hit -> turns Outward", SetIndOutward, ClearIndOutward, 30});
	add({EFoxItemId::IndicatorLine, EFoxItemType::Indicator, "Line Indicator", "I_L", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 6500, 10, 3, EFoxItemRarity::Uncommon, "Gun Hit -> goes in a Line", SetIndLine, ClearIndLine, 30});
	add({EFoxItemId::IndicatorCrisscross, EFoxItemType::Indicator, "Criss Cross Indicator", "I_CrCs", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DamageIndicator, 6500, 10, 4, EFoxItemRarity::Uncommon, "Gun Hit -> goes in a Criss Cross pattern", SetIndCriss, ClearIndCriss, 30});

	add({EFoxItemId::DeathExplosive, EFoxItemType::Death, "Explosive Death", "D_E", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DeathEffect, 3250, 5, 2, EFoxItemRarity::Uncommon, "Go out with a Boom!", SetDeathExplosion, ClearDeath, 30});
	add({EFoxItemId::DeathHammerHit, EFoxItemType::Death, "Hammer Hit Death", "D_H", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DeathEffect, 3250, 5, 2, EFoxItemRarity::Uncommon, "Get Bonked on death!", SetDeathHammerHit, ClearDeath, 30});
	add({EFoxItemId::DeathIndicator, EFoxItemType::Death, "Indicator Death", "D_I", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DeathEffect, 7500, 10, 4, EFoxItemRarity::Uncommon, "Creates an octagon of damage indicators", SetDeathIndicator, ClearDeath, 30});
	add({EFoxItemId::DeathLaser, EFoxItemType::Death, "Laser Death", "D_L", EFoxItemFlag::Equippable, EFoxExclusiveGroup::DeathEffect, 7500, 10, 4, EFoxItemRarity::Uncommon, "Become wizard and summon lasers on death!", SetDeathLaser, ClearDeath, 30});

	add({EFoxItemId::TrailStar, EFoxItemType::Trail, "Star Trail", "T_S", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Trail, 13000, 12, 4, EFoxItemRarity::Uncommon, "The Stars shall follow you", SetTrailStar, ClearTrailStar, 30});
	add({EFoxItemId::TrailDot, EFoxItemType::Trail, "Dot Trail", "T_D", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Trail, 13000, 12, 4, EFoxItemRarity::Uncommon, "A trail made out of small dots", SetTrailDot, ClearTrail, 30});

	add({EFoxItemId::HammerHat, EFoxItemType::Hat, "Hammer Hat", "Hm_H", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 10000, 10, 5, EFoxItemRarity::Common, "Hammer above your head", SetHatHammer, ClearHat, 30});
	add({EFoxItemId::GunHat, EFoxItemType::Hat, "Gun Hat", "G_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 10000, 10, 5, EFoxItemRarity::Common, "Gun above your head", SetHatGun, ClearHat, 30});
	add({EFoxItemId::ShotgunHat, EFoxItemType::Hat, "Shotgun Hat", "S_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 10000, 10, 5, EFoxItemRarity::Common, "Shotgun above your head", SetHatShotgun, ClearHat, 30});
	add({EFoxItemId::GrenadeHat, EFoxItemType::Hat, "Grenade Hat", "Gr_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 10000, 10, 5, EFoxItemRarity::Common, "Grenade above your head", SetHatGrenade, ClearHat, 30});
	add({EFoxItemId::LaserHat, EFoxItemType::Hat, "Laser Hat", "L_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 10000, 10, 5, EFoxItemRarity::Common, "Laser above your head", SetHatLaser, ClearHat, 30});
	add({EFoxItemId::NinjaHat, EFoxItemType::Hat, "Ninja Hat", "N_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 14000, 15, 4, EFoxItemRarity::Uncommon, "Ninja above your head", SetHatNinja, ClearHat, 30});
	add({EFoxItemId::PartyHat, EFoxItemType::Hat, "Party Hat", "P_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 14000, 15, 4, EFoxItemRarity::Uncommon, "Party time!", SetHatParty, ClearHat, 30});
	add({EFoxItemId::HeartHat, EFoxItemType::Hat, "Heart Hat", "H_Ht", EFoxItemFlag::Equippable, EFoxExclusiveGroup::Hat, 18500, 20, 3, EFoxItemRarity::Rare, "Heart above your head", SetHatHeart, ClearHat, 30});

	auto ApplyLaserGunShop = [](CPlayer &Pl, const CFoxItemConfig &, int) { if(CCharacter *pChr = Pl.GetCharacter()) { pChr->GiveWeapon(WEAPON_LASER, false); pChr->SetWeapon(WEAPON_LASER); } };
	auto ApplyShotgunShop = [](CPlayer &Pl, const CFoxItemConfig &, int) { if(CCharacter *pChr = Pl.GetCharacter()) { pChr->GiveWeapon(WEAPON_SHOTGUN, false); pChr->SetWeapon(WEAPON_SHOTGUN); } };
	auto ApplyGrenadeShop = [](CPlayer &Pl, const CFoxItemConfig &, int) { if(CCharacter *pChr = Pl.GetCharacter()) { pChr->GiveWeapon(WEAPON_GRENADE, false); pChr->SetWeapon(WEAPON_GRENADE); } };
	
	add({EFoxItemId::WeaponLaser, EFoxItemType::Weapon, "Laser", "W_L", EFoxItemFlag::Consumable, EFoxExclusiveGroup::None, 25000, 1, 1, EFoxItemRarity::Common, "Get Laser for 1 time", ApplyLaserGunShop, NoRemove, 30});
	add({EFoxItemId::WeaponShotgun, EFoxItemType::Weapon, "Shotgun", "W_S", EFoxItemFlag::Consumable, EFoxExclusiveGroup::None, 15000, 1, 1, EFoxItemRarity::Common, "Get Shotgun for 1 time", ApplyShotgunShop, NoRemove, 30});
	add({EFoxItemId::WeaponGrenade, EFoxItemType::Weapon, "Grenade", "W_G", EFoxItemFlag::Consumable, EFoxExclusiveGroup::None, 15000, 1, 1, EFoxItemRarity::Common, "Get Grenade for 1 time", ApplyGrenadeShop, NoRemove, 30});
}
