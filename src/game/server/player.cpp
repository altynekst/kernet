/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "player.h"

#include "entities/character.h"
#include "gamecontext.h"
#include "gamecontroller.h"
#include "score.h"

#include <base/log.h>
#include <base/str.h>
#include <base/system.h>

#include <engine/antibot.h>
#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/gamecore.h>
#include <game/teamscore.h>

#include <game/server/foxnet/cosmetics/cosmetic_types.h>
#include <game/server/foxnet/cosmetics/dot_trail.h>
#include <game/server/foxnet/cosmetics/halo.h>
#include <game/server/foxnet/cosmetics/lovely.h>
#include <game/server/foxnet/cosmetics/rotating_ball.h>
#include <game/server/foxnet/cosmetics/headitem.h>

MACRO_ALLOC_POOL_ID_IMPL(CPlayer, MAX_CLIENTS)

IServer *CPlayer::Server() const { return m_pGameServer->Server(); }

const char *CPlayer::L(const char *pEn, const char *pRu) const
{
	return m_Language == ELanguage::RU ? pRu : pEn;
}

CPlayer::CPlayer(CGameContext *pGameServer, uint32_t UniqueClientId, int ClientId, int Team) :
	m_UniqueClientId(UniqueClientId),
	m_Language(ELanguage::EN)
{
	m_pGameServer = pGameServer;
	m_ClientId = ClientId;
	dbg_assert(GameServer()->m_pController->IsValidTeam(Team), "Invalid Team: %d", Team);
	m_Team = Team;
	m_NumInputs = 0;
	Reset();
	GameServer()->Antibot()->OnPlayerInit(m_ClientId);
}

CPlayer::~CPlayer()
{
	GameServer()->Antibot()->OnPlayerDestroy(m_ClientId);
	delete m_pLastTarget;
	delete m_pCharacter;
	m_pCharacter = nullptr;
}

void CPlayer::Reset()
{
	m_DieTick = Server()->Tick();
	m_PreviousDieTick = m_DieTick;
	m_JoinTick = Server()->Tick();
	delete m_pCharacter;
	m_pCharacter = nullptr;
	SetSpectatorId(SPEC_FREEVIEW);
	m_LastActionTick = Server()->Tick();
	m_TeamChangeTick = Server()->Tick();
	m_LastSetTeam = 0;
	m_LastInvited = 0;
	m_WeakHookSpawn = false;

	int *pIdMap = Server()->GetIdMap(m_ClientId);
	for(int i = 1; i < VANILLA_MAX_CLIENTS; i++)
	{
		pIdMap[i] = -1;
	}
	pIdMap[0] = m_ClientId;

	// DDRace

	m_LastCommandPos = 0;
	m_LastPlaytime = 0;
	m_ChatScore = 0;
	m_Moderating = false;
	m_EyeEmoteEnabled = true;
	if(Server()->IsSixup(m_ClientId))
		m_TimerType = TIMERTYPE_SIXUP;
	else
		m_TimerType = (g_Config.m_SvDefaultTimerType == TIMERTYPE_GAMETIMER || g_Config.m_SvDefaultTimerType == TIMERTYPE_GAMETIMER_AND_BROADCAST) ? TIMERTYPE_BROADCAST : g_Config.m_SvDefaultTimerType;

	m_DefEmote = EMOTE_NORMAL;
	m_Afk = true;
	m_LastWhisperTo = -1;
	m_LastSetSpectatorMode = 0;
	m_aTimeoutCode[0] = '\0';
	delete m_pLastTarget;
	m_pLastTarget = new CNetObj_PlayerInput({0});
	m_LastTargetInit = false;
	m_TuneZone = 0;
	m_TuneZoneOld = m_TuneZone;
	m_Halloween = false;
	m_FirstPacket = true;

	m_SendVoteIndex = -1;

	if(g_Config.m_Events)
	{
		const ETimeSeason Season = time_season();
		if(Season == ETimeSeason::NEWYEAR)
		{
			m_DefEmote = EMOTE_HAPPY;
		}
		else if(Season == ETimeSeason::HALLOWEEN)
		{
			m_DefEmote = EMOTE_ANGRY;
			m_Halloween = true;
		}
		else
		{
			m_DefEmote = EMOTE_NORMAL;
		}
	}
	m_OverrideEmoteReset = -1;

	GameServer()->Score()->PlayerData(m_ClientId)->Reset();

	m_LastKickVote = 0;
	m_LastDDRaceTeamChange = 0;
	m_ShowOthers = g_Config.m_SvShowOthersDefault;
	m_ShowAll = g_Config.m_SvShowAllDefault;
	m_EnableSpectatorCount = true;
	m_ShowDistance = vec2(1200, 800);
	m_SpecTeam = false;
	m_NinjaJetpack = false;

	m_Paused = PAUSE_NONE;
	m_DND = false;
	m_Whispers = true;

	m_LastPause = 0;

	// Variable initialized:
	m_LastSqlQuery = 0;
	m_ScoreQueryResult = nullptr;
	m_ScoreFinishResult = nullptr;

	m_IpLangPromptActive = false;
	m_IpLangSuggested = 0;
	m_IpLangVoteEndTime = 0;
	m_IpLangVoteLastSeconds = -1;
	m_IpLangHasStored = false;
	m_IpLangStored = 0;
	m_aClientAddrStr[0] = 0;

	m_RainbowOwned = false;
	m_RainbowActive = false;
	m_RainbowColor = 0;
	m_RainbowSpeed = 2;
	m_RainbowFeet = false;
	m_RainbowBody = false;
	m_HookPower = HOOKTYPE_NORMAL;

	m_Sparkle = false;
	m_InverseAim = false;

	m_DamageIndType = INDTYPE_NONE;
	m_EmoticonGun = 0;
	m_PhaseGun = false;
	m_GunType = GUNTYPE_NONE;
	m_PhysicalBullet = false;

	m_CosmeticTrailType = TRAILTYPE_NONE;
	m_CosmeticHatType = HATTYPE_NONE;
	m_CosmeticDeathType = DEATHTYPE_NONE;
	m_CosmeticHalo = false;
	m_CosmeticLovely = false;
	m_CosmeticRotatingBall = false;
	m_HideOthersCosmetics = false;

	m_FoxMenuSelectedType = 0;
	m_aFoxMenuSelectedItem[0] = 0;
	m_vFoxOwnedItems.clear();
	m_FoxItemExpiresAt.clear();
	m_AccountLoggedIn = false;
	m_AccountId = -1;
	m_PersonalMenuPage = 0;
	m_PersonalMenuDirty = false;
	m_ProfileLoading = false;
	m_ProfileLoaded = false;
	m_AccountAutoLogin = false;
	m_AutoLoginPending = false;
	m_aAccountUsername[0] = 0;
	m_aAccountLastIgn[0] = 0;
	m_aAccountNickProtectedIgn[0] = 0;
	m_aAccountRegisterDate[0] = 0;
	m_AccountLevel = 1;
	m_AccountXP = 0;
	m_AccountMoney = 0;
	m_AccountRole = 0;
	m_AccountJetpackActive = false;
	m_AccountPlaytimeSeconds = 0;
	m_LastPersonalMenuRefreshTick = 0;
	m_AccountDeaths = 0;
	m_AccountKills = 0;
	m_AccountCombo = 0;
	m_LastFarmerHintTick = 0;
	m_LastFarmerHintId = -1;
	m_LastVipTeleportHintTick = 0;
	m_NameIsProtectedByAccount = false;
	m_NameProtectionLookupPending = false;
	m_NameProtectionChecked = false;
	m_aNameProtectionNick[0] = 0;
	m_LastDeathFromCid = -1;
	m_DeathFromStreak = 0;
	mem_zero(m_aNoRewardUntilFromCid, sizeof(m_aNoRewardUntilFromCid));
	m_LastAccountPlaytimeUpdateTick = Server()->Tick();
	m_vMailbox.clear();
	m_MailboxLoaded = false;
	m_MailboxOnlyUnread = false;
	m_MailboxViewIndex = -1;
	m_MailboxFetchPending = false;
	m_MailboxAttentionCount = 0;
	m_MailboxFirstLoad = true;
	m_LastMailboxFetchTick = 0;

	m_KillMarkKillerCid = -1;
	m_KillMarkWeapon = -1;
	m_KillMarkExpireTick = 0;
	m_KillMarkPausedInFreeze = false;
	m_KillMarkVictimInFreeze = false;

	m_RestoreWeaponsOnSpawn = false;
	mem_zero(m_aRestoreWeaponGot, sizeof(m_aRestoreWeaponGot));
	mem_zero(m_aRestoreWeaponAmmo, sizeof(m_aRestoreWeaponAmmo));
	m_RestoreActiveWeapon = WEAPON_HAMMER;

	int64_t Now = Server()->Tick();
	int64_t TickSpeed = Server()->TickSpeed();
	// If the player joins within ten seconds of the server becoming
	// non-empty, allow them to vote immediately. This allows players to
	// vote after map changes or when they join an empty server.
	//
	// Otherwise, block voting in the beginning after joining.
	if(Now > GameServer()->m_NonEmptySince + 10 * TickSpeed)
		m_FirstVoteTick = Now + g_Config.m_SvJoinVoteDelay * TickSpeed;
	else
		m_FirstVoteTick = Now;

	m_NotEligibleForFinish = false;
	m_EligibleForFinishCheck = 0;
	m_VotedForPractice = false;
	m_SwapTargetsClientId = -1;
	m_BirthdayAnnounced = false;
	m_RescueMode = RESCUEMODE_AUTO;
	for(int i = 0; i < MAX_CLIENTS; i++) m_aDuelRequests[i] = 0;
	m_InDuel = false;
	m_DuelOpponent = -1;
	m_DuelScore = 0;
	m_DuelSide = 0;
	m_DuelSavedTeam = 0;
	m_DuelFrozenTicks = 0;
	m_DuelSpawnPending = false;
	m_DuelSpawnPos = vec2(0, 0);

	m_CameraInfo.Reset();
}

bool CPlayer::IsProtectedNickActive() const
{
	return m_AccountLoggedIn && m_aAccountNickProtectedIgn[0] != 0 && str_comp_nocase(Server()->ClientName(m_ClientId), m_aAccountNickProtectedIgn) == 0;
}

static void RemoveOwnedEntitiesByType(CGameContext *pGameServer, int Owner, int Type)
{
	for(CEntity *pEnt = pGameServer->m_World.FindFirst(Type); pEnt;)
	{
		CEntity *pNext = pEnt->TypeNext();
		if(pEnt->GetOwnerId() == Owner)
		{
			pGameServer->m_World.RemoveEntity(pEnt);
			pEnt->Destroy();
		}
		pEnt = pNext;
	}
}

static void ClearPlayerCosmeticEntities(CGameContext *pGameServer, int Owner)
{
	RemoveOwnedEntitiesByType(pGameServer, Owner, CGameWorld::ENTTYPE_DOT_TRAIL);
	RemoveOwnedEntitiesByType(pGameServer, Owner, CGameWorld::ENTTYPE_HEADITEM);
	RemoveOwnedEntitiesByType(pGameServer, Owner, CGameWorld::ENTTYPE_HALO);
	RemoveOwnedEntitiesByType(pGameServer, Owner, CGameWorld::ENTTYPE_LOVELY);
	RemoveOwnedEntitiesByType(pGameServer, Owner, CGameWorld::ENTTYPE_ROTATING_BALL);
}

void CPlayer::SetTrail(int Type)
{
	if(m_CosmeticTrailType == Type)
		return;
	RemoveOwnedEntitiesByType(GameServer(), GetCid(), CGameWorld::ENTTYPE_DOT_TRAIL);
	m_CosmeticTrailType = Type;
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticTrailType == TRAILTYPE_DOT)
		new CDotTrail(&GameServer()->m_World, GetCid(), Pos);
}

void CPlayer::SetHatType(int Type)
{
	if(m_CosmeticHatType == Type)
		return;
	RemoveOwnedEntitiesByType(GameServer(), GetCid(), CGameWorld::ENTTYPE_HEADITEM);
	const int PrevType = m_CosmeticHatType;
	m_CosmeticHatType = Type;
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticHatType != HATTYPE_NONE && PrevType == HATTYPE_NONE)
		new CHeadItem(&GameServer()->m_World, GetCid(), Pos, HEADITEM_COSMETIC, vec2(0, -45.0f));
}

void CPlayer::SetDeathEffect(int Type)
{
	m_CosmeticDeathType = Type;
}

void CPlayer::SetHalo(bool Active)
{
	if(m_CosmeticHalo == Active)
		return;
	RemoveOwnedEntitiesByType(GameServer(), GetCid(), CGameWorld::ENTTYPE_HALO);
	m_CosmeticHalo = Active;
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticHalo)
		new CHalo(&GameServer()->m_World, GetCid(), Pos);
}

void CPlayer::SetLovely(bool Active)
{
	if(m_CosmeticLovely == Active)
		return;
	RemoveOwnedEntitiesByType(GameServer(), GetCid(), CGameWorld::ENTTYPE_LOVELY);
	m_CosmeticLovely = Active;
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticLovely)
		new CLovely(&GameServer()->m_World, GetCid(), Pos);
}

void CPlayer::SetRotatingBall(bool Active)
{
	if(m_CosmeticRotatingBall == Active)
		return;
	RemoveOwnedEntitiesByType(GameServer(), GetCid(), CGameWorld::ENTTYPE_ROTATING_BALL);
	m_CosmeticRotatingBall = Active;
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticRotatingBall)
		new CRotatingBall(&GameServer()->m_World, GetCid(), Pos);
}

void CPlayer::SetSparkle(bool Active)
{
	m_Sparkle = Active;
}

void CPlayer::SetInverseAim(bool Active)
{
	m_InverseAim = Active;
}

void CPlayer::RespawnCosmeticEntities()
{
	ClearPlayerCosmeticEntities(GameServer(), GetCid());
	const vec2 Pos = GetCharacter() ? GetCharacter()->GetPos() : vec2(0, 0);
	if(m_CosmeticTrailType == TRAILTYPE_DOT)
		new CDotTrail(&GameServer()->m_World, GetCid(), Pos);
	if(m_CosmeticHatType != HATTYPE_NONE)
		new CHeadItem(&GameServer()->m_World, GetCid(), Pos, HEADITEM_COSMETIC, vec2(0, -45.0f));
	if(m_CosmeticHalo)
		new CHalo(&GameServer()->m_World, GetCid(), Pos);
	if(m_CosmeticLovely)
		new CLovely(&GameServer()->m_World, GetCid(), Pos);
	if(m_CosmeticRotatingBall)
		new CRotatingBall(&GameServer()->m_World, GetCid(), Pos);
}

void CPlayer::SetRainbowFeet(bool Active)
{
	m_RainbowFeet = Active;
}

void CPlayer::SetRainbowBody(bool Active)
{
	m_RainbowBody = Active;
}

void CPlayer::SetHookPower(int Type)
{
	m_HookPower = Type;
}

void CPlayer::SetDamageIndType(int Type)
{
	m_DamageIndType = Type;
}

void CPlayer::SetEmoticonGun(int Type)
{
	m_EmoticonGun = Type;
}

void CPlayer::SetPhaseGun(bool Active)
{
	m_PhaseGun = Active;
}

void CPlayer::SetGunType(int Type)
{
	m_GunType = Type;
}

void CPlayer::SetPhysicalBullet(bool Active)
{
	m_PhysicalBullet = Active;
}

static int PlayerFlags_SixToSeven(int Flags)
{
	int Seven = 0;
	if(Flags & PLAYERFLAG_CHATTING)
		Seven |= protocol7::PLAYERFLAG_CHATTING;
	if(Flags & PLAYERFLAG_SCOREBOARD)
		Seven |= protocol7::PLAYERFLAG_SCOREBOARD;

	return Seven;
}

void CPlayer::Tick()
{
	if(m_RainbowActive)
	{
		if(Server()->Tick() % 2 == 1)
			m_RainbowColor = (m_RainbowColor + 1) % 256;
	}
	if(m_RainbowSpeed <= 0)
		m_RainbowSpeed = 2;
	if(m_RainbowFeet || m_RainbowBody)
	{
		if(Server()->Tick() % maximum(1, m_RainbowSpeed) == 0)
			m_RainbowColor = (m_RainbowColor + 1) % 256;
	}

	if(m_ScoreQueryResult != nullptr && m_ScoreQueryResult->m_Completed && m_SentSnaps >= 3)
	{
		// Allow ProcessScoreResult to schedule a follow-up SQL request (e.g. login -> load profile)
		// by clearing the in-progress marker before processing.
		auto pResult = std::move(m_ScoreQueryResult);
		m_ScoreQueryResult = nullptr;
		ProcessScoreResult(*pResult);
	}
	if(m_ScoreFinishResult != nullptr && m_ScoreFinishResult->m_Completed)
	{
		ProcessScoreResult(*m_ScoreFinishResult);
		m_ScoreFinishResult = nullptr;
	}

	if(!Server()->ClientIngame(m_ClientId))
		return;

	const char *pClientName = Server()->ClientName(m_ClientId);
	if(str_comp(m_aNameProtectionNick, pClientName) != 0)
	{
		str_copy(m_aNameProtectionNick, pClientName, sizeof(m_aNameProtectionNick));
		m_NameIsProtectedByAccount = false;
		m_NameProtectionLookupPending = false;
		m_NameProtectionChecked = false;
	}
	if(!m_NameProtectionChecked && !m_NameProtectionLookupPending)
	{
		if(GameServer()->Score()->CheckNickProtected(m_ClientId, pClientName))
			m_NameProtectionLookupPending = true;
	}

	if(m_KillMarkKillerCid >= 0 && !m_KillMarkVictimInFreeze && m_KillMarkExpireTick > 0 && Server()->Tick() > m_KillMarkExpireTick)
	{
		m_KillMarkKillerCid = -1;
		m_KillMarkWeapon = -1;
		m_KillMarkExpireTick = 0;
		m_KillMarkPausedInFreeze = false;
	}

	if(m_AccountLoggedIn && m_ProfileLoaded)
	{
		if(m_LastAccountPlaytimeUpdateTick == 0)
			m_LastAccountPlaytimeUpdateTick = Server()->Tick();
		const int64_t DiffTicks = Server()->Tick() - m_LastAccountPlaytimeUpdateTick;
		const int64_t TickSpeed = Server()->TickSpeed();
		if(TickSpeed > 0 && DiffTicks >= TickSpeed)
		{
			const int64_t AddSeconds = DiffTicks / TickSpeed;
			m_LastAccountPlaytimeUpdateTick += AddSeconds * TickSpeed;
			m_AccountPlaytimeSeconds += AddSeconds;
			if(m_PersonalMenuPage == 0)
				m_PersonalMenuDirty = true;
		}

		// Persist playtime every 60 seconds.
		const int64_t DiffSaveTicks = Server()->Tick() - m_LastAccountPlaytimeSaveTick;
		if(TickSpeed > 0 && DiffSaveTicks >= 60 * TickSpeed)
		{
			m_LastAccountPlaytimeSaveTick += (DiffSaveTicks / (60 * TickSpeed)) * (60 * TickSpeed);
			GameServer()->Score()->SaveAccountPlaytime(m_AccountId, m_AccountPlaytimeSeconds);
		}
	}

	// Periodically refresh mailbox in background (FoxNet-like) so main menu badge stays correct.
	// Also used to notify the player about newly received mails.
	if(m_AccountLoggedIn && m_ProfileLoaded)
	{
		const int64_t TickSpeed = Server()->TickSpeed();
		if(m_LastMailboxFetchTick == 0)
			m_LastMailboxFetchTick = Server()->Tick();
		// Refresh every 30 seconds.
		if(!m_MailboxFetchPending && TickSpeed > 0 && Server()->Tick() - m_LastMailboxFetchTick >= 30 * TickSpeed)
		{
			m_MailboxFetchPending = true;
			m_LastMailboxFetchTick = Server()->Tick();
			GameServer()->Score()->LoadAccountMailbox(m_ClientId, m_AccountId);
		}
	}

	if(m_ChatScore > 0)
		m_ChatScore--;

	if(m_Moderating && m_Afk)
	{
		m_Moderating = false;
		GameServer()->SendChatTarget(m_ClientId, "Active moderator mode disabled because you are afk.");

		if(!GameServer()->PlayerModerating())
			GameServer()->SendChat(-1, TEAM_ALL, "Server kick/spec votes are no longer actively moderated.");
	}

	// do latency stuff
	{
		IServer::CClientInfo Info;
		if(Server()->GetClientInfo(m_ClientId, &Info))
		{
			m_Latency.m_Accum += Info.m_Latency;
			m_Latency.m_AccumMax = maximum(m_Latency.m_AccumMax, Info.m_Latency);
			m_Latency.m_AccumMin = minimum(m_Latency.m_AccumMin, Info.m_Latency);
		}
		// each second
		if(Server()->Tick() % Server()->TickSpeed() == 0)
		{
			m_Latency.m_Avg = m_Latency.m_Accum / Server()->TickSpeed();
			m_Latency.m_Max = m_Latency.m_AccumMax;
			m_Latency.m_Min = m_Latency.m_AccumMin;
			m_Latency.m_Accum = 0;
			m_Latency.m_AccumMin = 1000;
			m_Latency.m_AccumMax = 0;
		}
	}

	if(Server()->GetNetErrorString(m_ClientId)[0])
	{
		SetInitialAfk(true);

		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' would have timed out, but can use timeout protection now", Server()->ClientName(m_ClientId));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
		Server()->ResetNetErrorString(m_ClientId);
	}

	if(!GameServer()->m_pController->IsGamePaused())
	{
		int EarliestRespawnTick = m_PreviousDieTick + Server()->TickSpeed() * 3;
		int RespawnTick = maximum(m_DieTick, EarliestRespawnTick) + 2;
		if(!m_pCharacter && RespawnTick <= Server()->Tick())
			m_Spawning = true;

		if(m_pCharacter)
		{
			if(m_pCharacter->IsAlive())
			{
				ProcessPause();
				if(!m_Paused)
					m_ViewPos = m_pCharacter->m_Pos;
			}
			else if(!m_pCharacter->IsPaused())
			{
				delete m_pCharacter;
				m_pCharacter = nullptr;
			}
		}
		else if(m_Spawning && !m_WeakHookSpawn)
			TryRespawn();
	}
	else
	{
		++m_DieTick;
		++m_PreviousDieTick;
		++m_JoinTick;
		++m_LastActionTick;
		++m_TeamChangeTick;
	}

	m_TuneZoneOld = m_TuneZone; // determine needed tunings with viewpos
	int CurrentIndex = GameServer()->Collision()->GetMapIndex(m_ViewPos);
	m_TuneZone = GameServer()->Collision()->IsTune(CurrentIndex);

	if(m_TuneZone != m_TuneZoneOld) // don't send tunings all the time
	{
		GameServer()->SendTuningParams(m_ClientId, m_TuneZone);
	}

	if(m_OverrideEmoteReset >= 0 && m_OverrideEmoteReset <= Server()->Tick())
	{
		m_OverrideEmoteReset = -1;
	}

	if(m_Halloween && m_pCharacter && !m_pCharacter->IsPaused())
	{
		if(1200 - ((Server()->Tick() - m_pCharacter->GetLastAction()) % (1200)) < 5)
		{
			GameServer()->SendEmoticon(GetCid(), EMOTICON_GHOST, -1);
		}
	}
}

void CPlayer::PostTick()
{
	// update latency value
	if(m_PlayerFlags & PLAYERFLAG_IN_MENU)
		m_aCurLatency[m_ClientId] = GameServer()->m_apPlayers[m_ClientId]->m_Latency.m_Min;

	if(m_PlayerFlags & PLAYERFLAG_SCOREBOARD)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
				m_aCurLatency[i] = GameServer()->m_apPlayers[i]->m_Latency.m_Min;
		}
	}

	// update view pos for spectators
	if((m_Team == TEAM_SPECTATORS || m_Paused) && m_SpectatorId != SPEC_FREEVIEW && GameServer()->m_apPlayers[m_SpectatorId] && GameServer()->m_apPlayers[m_SpectatorId]->GetCharacter())
		m_ViewPos = GameServer()->m_apPlayers[m_SpectatorId]->GetCharacter()->m_Pos;
}

void CPlayer::PostPostTick()
{
	if(!Server()->ClientIngame(m_ClientId))
		return;

	if(!GameServer()->m_pController->IsGamePaused() && !m_pCharacter && m_Spawning && m_WeakHookSpawn)
		TryRespawn();
}

void CPlayer::Snap(int SnappingClient)
{
	if(!Server()->ClientIngame(m_ClientId))
		return;

	int TranslatedId = m_ClientId;
	if(!Server()->Translate(TranslatedId, SnappingClient))
		return;

	CNetObj_ClientInfo *pClientInfo = Server()->SnapNewItem<CNetObj_ClientInfo>(TranslatedId);
	if(!pClientInfo)
		return;

	char aSnapName[MAX_NAME_LENGTH + 16];
	if(m_NameIsProtectedByAccount && !IsProtectedNickActive())
		str_format(aSnapName, sizeof(aSnapName), "fake`%s", Server()->ClientName(m_ClientId));
	else
		str_copy(aSnapName, Server()->ClientName(m_ClientId), sizeof(aSnapName));

	// Network name field is fixed-size (16 bytes for classic client info).
	// Truncate safely to byte limit and keep valid UTF-8 to avoid StrToInts assert.
	char aPackedName[sizeof(pClientInfo->m_aName) * sizeof(pClientInfo->m_aName[0])];
	str_copy(aPackedName, aSnapName, sizeof(aPackedName));
	str_utf8_fix_truncation(aPackedName);
	StrToInts(pClientInfo->m_aName, std::size(pClientInfo->m_aName), aPackedName);

	char aPackedClan[sizeof(pClientInfo->m_aClan) * sizeof(pClientInfo->m_aClan[0])];
	str_copy(aPackedClan, Server()->ClientClan(m_ClientId), sizeof(aPackedClan));
	str_utf8_fix_truncation(aPackedClan);
	StrToInts(pClientInfo->m_aClan, std::size(pClientInfo->m_aClan), aPackedClan);
	pClientInfo->m_Country = Server()->ClientCountry(m_ClientId);

	char aPackedSkin[sizeof(pClientInfo->m_aSkin) * sizeof(pClientInfo->m_aSkin[0])];
	str_copy(aPackedSkin, m_TeeInfos.m_aSkinName, sizeof(aPackedSkin));
	str_utf8_fix_truncation(aPackedSkin);
	StrToInts(pClientInfo->m_aSkin, std::size(pClientInfo->m_aSkin), aPackedSkin);
	pClientInfo->m_UseCustomColor = m_TeeInfos.m_UseCustomColor;
	pClientInfo->m_ColorBody = m_TeeInfos.m_ColorBody;
	pClientInfo->m_ColorFeet = m_TeeInfos.m_ColorFeet;
	if(m_RainbowActive)
	{
		pClientInfo->m_UseCustomColor = 1;
		const int BaseColor = m_RainbowColor * 0x010000;
		const int Color = 0xff32;
		pClientInfo->m_ColorBody = BaseColor + Color;
		pClientInfo->m_ColorFeet = BaseColor + Color;
	}
	else if(m_RainbowFeet || m_RainbowBody)
	{
		pClientInfo->m_UseCustomColor = 1;
		const int BaseColor = m_RainbowColor * 0x010000;
		const int Color = 0xff32;
		if(m_RainbowBody)
			pClientInfo->m_ColorBody = BaseColor + Color;
		if(m_RainbowFeet)
			pClientInfo->m_ColorFeet = BaseColor + Color;
	}

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	int Latency = SnappingClient == SERVER_DEMO_CLIENT ? m_Latency.m_Min : GameServer()->m_apPlayers[SnappingClient]->m_aCurLatency[m_ClientId];
	int Score = GameServer()->m_pController->SnapPlayerScore(SnappingClient, this);

	if(!Server()->IsSixup(SnappingClient))
	{
		CNetObj_PlayerInfo *pPlayerInfo = Server()->SnapNewItem<CNetObj_PlayerInfo>(TranslatedId);
		if(!pPlayerInfo)
			return;

		pPlayerInfo->m_Latency = Latency;
		pPlayerInfo->m_Score = Score;
		pPlayerInfo->m_Local = (int)(m_ClientId == SnappingClient && (m_Paused != PAUSE_PAUSED || SnappingClientVersion >= VERSION_DDNET_OLD));
		pPlayerInfo->m_ClientId = TranslatedId;
		pPlayerInfo->m_Team = m_Team;
		if(SnappingClientVersion < VERSION_DDNET_INDEPENDENT_SPECTATORS_TEAM)
		{
			// In older versions the SPECTATORS TEAM was also used if the own player is in PAUSE_PAUSED or if any player is in PAUSE_SPEC.
			pPlayerInfo->m_Team = (m_Paused != PAUSE_PAUSED || m_ClientId != SnappingClient) && m_Paused < PAUSE_SPEC ? m_Team : TEAM_SPECTATORS;
		}
	}
	else
	{
		protocol7::CNetObj_PlayerInfo *pPlayerInfo = Server()->SnapNewItem<protocol7::CNetObj_PlayerInfo>(TranslatedId);
		if(!pPlayerInfo)
			return;

		pPlayerInfo->m_PlayerFlags = PlayerFlags_SixToSeven(m_PlayerFlags);
		if(SnappingClientVersion >= VERSION_DDRACE && (m_PlayerFlags & PLAYERFLAG_AIM))
			pPlayerInfo->m_PlayerFlags |= protocol7::PLAYERFLAG_AIM;
		if(Server()->IsRconAuthed(m_ClientId) && ((SnappingClient >= 0 && Server()->IsRconAuthed(SnappingClient)) || !Server()->HasAuthHidden(m_ClientId)))
			pPlayerInfo->m_PlayerFlags |= protocol7::PLAYERFLAG_ADMIN;
		pPlayerInfo->m_Score = Score;
		pPlayerInfo->m_Latency = Latency;
	}

	if(m_ClientId == SnappingClient && (m_Team == TEAM_SPECTATORS || m_Paused))
	{
		if(!Server()->IsSixup(SnappingClient))
		{
			CNetObj_SpectatorInfo *pSpectatorInfo = Server()->SnapNewItem<CNetObj_SpectatorInfo>(m_ClientId);
			if(!pSpectatorInfo)
				return;

			pSpectatorInfo->m_SpectatorId = m_SpectatorId;
			pSpectatorInfo->m_X = m_ViewPos.x;
			pSpectatorInfo->m_Y = m_ViewPos.y;
		}
		else
		{
			protocol7::CNetObj_SpectatorInfo *pSpectatorInfo = Server()->SnapNewItem<protocol7::CNetObj_SpectatorInfo>(m_ClientId);
			if(!pSpectatorInfo)
				return;

			pSpectatorInfo->m_SpecMode = m_SpectatorId == SPEC_FREEVIEW ? protocol7::SPEC_FREEVIEW : protocol7::SPEC_PLAYER;
			pSpectatorInfo->m_SpectatorId = m_SpectatorId;
			pSpectatorInfo->m_X = m_ViewPos.x;
			pSpectatorInfo->m_Y = m_ViewPos.y;
		}
	}

	if(m_ClientId == SnappingClient)
	{
		// send extended spectator info even when playing, this allows demo to record camera settings for local player
		const int SpectatingClient = ((m_Team != TEAM_SPECTATORS && !m_Paused) || m_SpectatorId < 0 || m_SpectatorId >= MAX_CLIENTS) ? TranslatedId : m_SpectatorId;
		const CPlayer *pSpecPlayer = GameServer()->m_apPlayers[SpectatingClient];

		if(pSpecPlayer)
		{
			CNetObj_DDNetSpectatorInfo *pDDNetSpectatorInfo = Server()->SnapNewItem<CNetObj_DDNetSpectatorInfo>(TranslatedId);
			if(!pDDNetSpectatorInfo)
				return;

			pDDNetSpectatorInfo->m_HasCameraInfo = pSpecPlayer->m_CameraInfo.m_HasCameraInfo;
			pDDNetSpectatorInfo->m_Zoom = pSpecPlayer->m_CameraInfo.m_Zoom * 1000.0f;
			pDDNetSpectatorInfo->m_Deadzone = pSpecPlayer->m_CameraInfo.m_Deadzone;
			pDDNetSpectatorInfo->m_FollowFactor = pSpecPlayer->m_CameraInfo.m_FollowFactor;

			if(pSpecPlayer->m_EnableSpectatorCount && SpectatingClient == TranslatedId && SnappingClient != SERVER_DEMO_CLIENT && m_Team != TEAM_SPECTATORS && !m_Paused)
			{
				CNetObj_SpectatorCount *pSpectatorCount = Server()->SnapNewItem<CNetObj_SpectatorCount>(0);
				if(!pSpectatorCount)
				{
					return;
				}
				int SpectatorCount = 0;
				for(auto &pPlayer : GameServer()->m_apPlayers)
				{
					if(!pPlayer || !pPlayer->m_EnableSpectatorCount || pPlayer->m_ClientId == TranslatedId || pPlayer->m_Afk ||
						(Server()->IsRconAuthed(pPlayer->m_ClientId) && Server()->HasAuthHidden(pPlayer->m_ClientId)) ||
						!(pPlayer->m_Paused || pPlayer->m_Team == TEAM_SPECTATORS))
					{
						continue;
					}

					if(pPlayer->m_SpectatorId == TranslatedId)
					{
						SpectatorCount++;
					}
					else if(GameServer()->m_apPlayers[TranslatedId]->GetCharacter())
					{
						vec2 CheckPos = GameServer()->m_apPlayers[TranslatedId]->GetCharacter()->GetPos();
						float dx = pPlayer->m_ViewPos.x - CheckPos.x;
						float dy = pPlayer->m_ViewPos.y - CheckPos.y;
						if(absolute(dx) < (pPlayer->m_ShowDistance.x / 2.5f) && absolute(dy) < (pPlayer->m_ShowDistance.y / 2.3f))
							SpectatorCount++;
					}
				}
				pDDNetSpectatorInfo->m_SpectatorCount = SpectatorCount;
				pSpectatorCount->m_NumSpectators = SpectatorCount;
			}
		}
	}

	CNetObj_DDNetPlayer *pDDNetPlayer = Server()->SnapNewItem<CNetObj_DDNetPlayer>(TranslatedId);
	if(!pDDNetPlayer)
		return;

	pDDNetPlayer->m_AuthLevel = AUTHED_NO;

	pDDNetPlayer->m_Flags = 0;
	if(m_Afk)
		pDDNetPlayer->m_Flags |= EXPLAYERFLAG_AFK;
	if(m_Paused == PAUSE_SPEC)
		pDDNetPlayer->m_Flags |= EXPLAYERFLAG_SPEC;
	if(m_Paused == PAUSE_PAUSED)
		pDDNetPlayer->m_Flags |= EXPLAYERFLAG_PAUSED;

	IGameController::CFinishTime PlayerTime = GameServer()->m_pController->SnapPlayerTime(SnappingClient, this);
	pDDNetPlayer->m_FinishTimeSeconds = PlayerTime.m_Seconds;
	pDDNetPlayer->m_FinishTimeMillis = PlayerTime.m_Milliseconds;

	if(Server()->IsSixup(SnappingClient) && m_pCharacter && m_pCharacter->m_DDRaceState == ERaceState::STARTED &&
		GameServer()->m_apPlayers[SnappingClient]->m_TimerType == TIMERTYPE_SIXUP)
	{
		protocol7::CNetObj_PlayerInfoRace *pRaceInfo = Server()->SnapNewItem<protocol7::CNetObj_PlayerInfoRace>(TranslatedId);
		if(!pRaceInfo)
			return;
		pRaceInfo->m_RaceStartTick = m_pCharacter->m_StartTime;
	}

	bool ShowSpec = m_pCharacter && m_pCharacter->IsPaused() && m_pCharacter->CanSnapCharacter(SnappingClient);

	if(SnappingClient != SERVER_DEMO_CLIENT)
	{
		CPlayer *pSnapPlayer = GameServer()->m_apPlayers[SnappingClient];
		ShowSpec = ShowSpec && (GameServer()->GetDDRaceTeam(m_ClientId) == GameServer()->GetDDRaceTeam(SnappingClient) || pSnapPlayer->m_ShowOthers == SHOW_OTHERS_ON || (pSnapPlayer->GetTeam() == TEAM_SPECTATORS || pSnapPlayer->IsPaused()));
	}

	if(ShowSpec)
	{
		CNetObj_SpecChar *pSpecChar = Server()->SnapNewItem<CNetObj_SpecChar>(TranslatedId);
		if(!pSpecChar)
			return;

		pSpecChar->m_X = m_pCharacter->Core()->m_Pos.x;
		pSpecChar->m_Y = m_pCharacter->Core()->m_Pos.y;
	}
}

void CPlayer::FakeSnap()
{
	m_SentSnaps++;
	if(GetClientVersion() >= VERSION_DDNET_OLD)
		return;

	if(Server()->IsSixup(m_ClientId))
		return;

	int FakeId = VANILLA_MAX_CLIENTS - 1;

	CNetObj_ClientInfo *pClientInfo = Server()->SnapNewItem<CNetObj_ClientInfo>(FakeId);

	if(!pClientInfo)
		return;

	StrToInts(pClientInfo->m_aName, std::size(pClientInfo->m_aName), " ");
	StrToInts(pClientInfo->m_aClan, std::size(pClientInfo->m_aClan), "");
	StrToInts(pClientInfo->m_aSkin, std::size(pClientInfo->m_aSkin), "default");

	if(m_Paused != PAUSE_PAUSED)
		return;

	CNetObj_PlayerInfo *pPlayerInfo = Server()->SnapNewItem<CNetObj_PlayerInfo>(FakeId);
	if(!pPlayerInfo)
		return;

	pPlayerInfo->m_Latency = m_Latency.m_Min;
	pPlayerInfo->m_Local = 1;
	pPlayerInfo->m_ClientId = FakeId;
	pPlayerInfo->m_Score = FinishTime::NOT_FINISHED_TIMESCORE;
	pPlayerInfo->m_Team = TEAM_SPECTATORS;

	CNetObj_SpectatorInfo *pSpectatorInfo = Server()->SnapNewItem<CNetObj_SpectatorInfo>(FakeId);
	if(!pSpectatorInfo)
		return;

	pSpectatorInfo->m_SpectatorId = m_SpectatorId;
	pSpectatorInfo->m_X = m_ViewPos.x;
	pSpectatorInfo->m_Y = m_ViewPos.y;
}

void CPlayer::OnDisconnect()
{
	KillCharacter();

	m_Moderating = false;
}

void CPlayer::OnPredictedInput(const CNetObj_PlayerInput *pNewInput)
{
	// skip the input if chat is active
	if((m_PlayerFlags & PLAYERFLAG_CHATTING) && (pNewInput->m_PlayerFlags & PLAYERFLAG_CHATTING))
		return;

	AfkTimer();

	m_NumInputs++;

	if(m_pCharacter && !m_Paused && !(pNewInput->m_PlayerFlags & PLAYERFLAG_SPEC_CAM))
		m_pCharacter->OnPredictedInput(pNewInput);

	// Magic number when we can hope that client has successfully identified itself
	if(m_NumInputs == 20 && g_Config.m_SvClientSuggestion[0] != '\0' && GetClientVersion() <= VERSION_DDNET_OLD)
		GameServer()->SendBroadcast(g_Config.m_SvClientSuggestion, m_ClientId);
}

void CPlayer::OnDirectInput(const CNetObj_PlayerInput *pNewInput)
{
	Server()->SetClientFlags(m_ClientId, pNewInput->m_PlayerFlags);

	AfkTimer();

	if(((pNewInput->m_PlayerFlags & PLAYERFLAG_SPEC_CAM) || GetClientVersion() < VERSION_DDNET_PLAYERFLAG_SPEC_CAM) && ((!m_pCharacter && m_Team == TEAM_SPECTATORS) || m_Paused) && m_SpectatorId == SPEC_FREEVIEW)
		m_ViewPos = vec2(pNewInput->m_TargetX, pNewInput->m_TargetY);

	// check for activity
	// if a player is killed, their scoreboard opens automatically, so ignore that flag
	CNetObj_PlayerInput NewWithoutScoreboard = *pNewInput;
	CNetObj_PlayerInput LastWithoutScoreboard = *m_pLastTarget;
	NewWithoutScoreboard.m_PlayerFlags &= ~PLAYERFLAG_SCOREBOARD;
	LastWithoutScoreboard.m_PlayerFlags &= ~PLAYERFLAG_SCOREBOARD;
	if(mem_comp(&NewWithoutScoreboard, &LastWithoutScoreboard, sizeof(CNetObj_PlayerInput)))
	{
		mem_copy(m_pLastTarget, pNewInput, sizeof(CNetObj_PlayerInput));
		// Ignore the first direct input and keep the player afk as it is sent automatically
		if(m_LastTargetInit)
			UpdatePlaytime();
		m_LastActionTick = Server()->Tick();
		m_LastTargetInit = true;
	}
}

void CPlayer::OnPredictedEarlyInput(const CNetObj_PlayerInput *pNewInput)
{
	m_PlayerFlags = pNewInput->m_PlayerFlags;

	if(!m_pCharacter && m_Team != TEAM_SPECTATORS && (pNewInput->m_Fire & 1))
		m_Spawning = true;

	// skip the input if chat is active
	if(m_PlayerFlags & PLAYERFLAG_CHATTING)
		return;

	if(m_pCharacter && !m_Paused && !(m_PlayerFlags & PLAYERFLAG_SPEC_CAM))
		m_pCharacter->OnDirectInput(pNewInput);
}

int CPlayer::GetClientVersion() const
{
	return m_pGameServer->GetClientVersion(m_ClientId);
}

CCharacter *CPlayer::GetCharacter()
{
	if(m_pCharacter && m_pCharacter->IsAlive())
		return m_pCharacter;
	return nullptr;
}

const CCharacter *CPlayer::GetCharacter() const
{
	if(m_pCharacter && m_pCharacter->IsAlive())
		return m_pCharacter;
	return nullptr;
}

void CPlayer::KillCharacter(int Weapon, bool SendKillMsg)
{
	if(m_pCharacter)
	{
		m_pCharacter->Die(m_ClientId, Weapon, SendKillMsg);

		delete m_pCharacter;
		m_pCharacter = nullptr;
	}
}

void CPlayer::Respawn(bool WeakHook)
{
	if(m_Team != TEAM_SPECTATORS)
	{
		m_WeakHookSpawn = WeakHook;
		m_Spawning = true;
	}
}

CCharacter *CPlayer::ForceSpawn(vec2 Pos)
{
	// Safety: kill existing character first to avoid pool 'already used' assert.
	// This can happen if ForceSpawn is called while an old character still exists.
	if(m_pCharacter)
		KillCharacter(WEAPON_GAME, false);
	m_Spawning = false;
	m_pCharacter = new(m_ClientId) CCharacter(&GameServer()->m_World, GameServer()->GetLastPlayerInput(m_ClientId));
	m_pCharacter->Spawn(this, Pos);
	m_Team = TEAM_GAME;
	return m_pCharacter;
}

void CPlayer::SetTeam(int Team, bool DoChatMsg)
{
	KillCharacter();

	m_Team = Team;
	m_LastSetTeam = Server()->Tick();
	m_LastActionTick = Server()->Tick();
	SetSpectatorId(SPEC_FREEVIEW);

	protocol7::CNetMsg_Sv_Team Msg;
	Msg.m_ClientId = m_ClientId;
	Msg.m_Team = m_Team;
	Msg.m_Silent = !DoChatMsg;
	Msg.m_CooldownTick = m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, -1);

	if(Team == TEAM_SPECTATORS)
	{
		// update spectator modes
		for(auto &pPlayer : GameServer()->m_apPlayers)
		{
			if(pPlayer && pPlayer->m_SpectatorId == m_ClientId)
				pPlayer->SetSpectatorId(SPEC_FREEVIEW);
		}
	}

	Server()->ExpireServerInfo();
}

bool CPlayer::SetTimerType(int TimerType)
{
	if(TimerType == TIMERTYPE_DEFAULT)
	{
		if(Server()->IsSixup(m_ClientId))
			m_TimerType = TIMERTYPE_SIXUP;
		else
			SetTimerType(g_Config.m_SvDefaultTimerType);

		return true;
	}

	if(Server()->IsSixup(m_ClientId))
	{
		if(TimerType == TIMERTYPE_SIXUP || TimerType == TIMERTYPE_NONE)
		{
			m_TimerType = TimerType;
			return true;
		}
		else
			return false;
	}

	if(TimerType == TIMERTYPE_GAMETIMER)
	{
		if(GetClientVersion() >= VERSION_DDNET_GAMETICK)
			m_TimerType = TimerType;
		else
			return false;
	}
	else if(TimerType == TIMERTYPE_GAMETIMER_AND_BROADCAST)
	{
		if(GetClientVersion() >= VERSION_DDNET_GAMETICK)
			m_TimerType = TimerType;
		else
		{
			m_TimerType = TIMERTYPE_BROADCAST;
			return false;
		}
	}
	else
		m_TimerType = TimerType;

	return true;
}

void CPlayer::TryRespawn()
{
	vec2 SpawnPos;

	if(!GameServer()->m_pController->CanSpawn(m_Team, &SpawnPos, m_ClientId))
		return;

	m_WeakHookSpawn = false;
	m_Spawning = false;
	m_pCharacter = new(m_ClientId) CCharacter(&GameServer()->m_World, GameServer()->GetLastPlayerInput(m_ClientId));
	m_ViewPos = SpawnPos;
	m_pCharacter->Spawn(this, SpawnPos);
	GameServer()->CreatePlayerSpawn(SpawnPos, GameServer()->m_pController->GetMaskForPlayerWorldEvent(m_ClientId));

	if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		m_pCharacter->SetSolo(true);
}

void CPlayer::UpdatePlaytime()
{
	m_LastPlaytime = time_get();
}

void CPlayer::AfkTimer()
{
	SetAfk(g_Config.m_SvMaxAfkTime != 0 && m_LastPlaytime < time_get() - time_freq() * g_Config.m_SvMaxAfkTime);
}

void CPlayer::SetAfk(bool Afk)
{
	if(m_Afk != Afk)
	{
		Server()->ExpireServerInfo();
		m_Afk = Afk;
	}
}

void CPlayer::SetInitialAfk(bool Afk)
{
	if(g_Config.m_SvMaxAfkTime == 0)
	{
		SetAfk(false);
		return;
	}

	SetAfk(Afk);

	// Ensure that the AFK state is not reset again automatically
	if(Afk)
		m_LastPlaytime = time_get() - time_freq() * g_Config.m_SvMaxAfkTime - 1;
	else
		m_LastPlaytime = time_get();
}

int CPlayer::GetDefaultEmote() const
{
	if(m_OverrideEmoteReset >= 0)
		return m_OverrideEmote;

	return m_DefEmote;
}

void CPlayer::OverrideDefaultEmote(int Emote, int Tick)
{
	m_OverrideEmote = Emote;
	m_OverrideEmoteReset = Tick;
	m_LastEyeEmote = Server()->Tick();
}

bool CPlayer::CanOverrideDefaultEmote() const
{
	return m_LastEyeEmote == 0 || m_LastEyeEmote + (int64_t)g_Config.m_SvEyeEmoteChangeDelay * Server()->TickSpeed() < Server()->Tick();
}

bool CPlayer::CanSpec() const
{
	return m_pCharacter->IsGrounded() && m_pCharacter->m_Pos == m_pCharacter->m_PrevPos;
}

void CPlayer::ProcessPause()
{
	if(m_ForcePauseTime && m_ForcePauseTime < Server()->Tick())
	{
		m_ForcePauseTime = 0;
		GameServer()->SendChatTarget(m_ClientId, "The force pause timer is now over, you can exit with /spec");
	}

	if(m_Paused == PAUSE_SPEC && !m_pCharacter->IsPaused() && CanSpec())
	{
		m_pCharacter->Pause(true);
		GameServer()->CreateDeath(m_pCharacter->m_Pos, m_ClientId, GameServer()->m_pController->GetMaskForPlayerWorldEvent(m_ClientId));
		GameServer()->CreateSound(m_pCharacter->m_Pos, SOUND_PLAYER_DIE, GameServer()->m_pController->GetMaskForPlayerWorldEvent(m_ClientId));
	}
}

int CPlayer::Pause(int State, bool Force)
{
	if(State < PAUSE_NONE || State > PAUSE_SPEC) // Invalid pause state passed
		return 0;

	if(!m_pCharacter)
		return 0;

	char aBuf[128];
	if(State != m_Paused)
	{
		// Get to wanted state
		switch(State)
		{
		case PAUSE_PAUSED:
		case PAUSE_NONE:
			if(m_pCharacter->IsPaused()) // First condition might be unnecessary
			{
				if(!Force && m_LastPause && m_LastPause + (int64_t)g_Config.m_SvSpecFrequency * Server()->TickSpeed() > Server()->Tick())
				{
					GameServer()->SendChatTarget(m_ClientId, "Can't /spec that quickly.");
					return m_Paused; // Do not update state. Do not collect $200
				}
				m_pCharacter->Pause(false);
				m_ViewPos = m_pCharacter->m_Pos;
				GameServer()->CreatePlayerSpawn(m_pCharacter->m_Pos, GameServer()->m_pController->GetMaskForPlayerWorldEvent(m_ClientId));
			}
			[[fallthrough]];
		case PAUSE_SPEC:
			if(g_Config.m_SvPauseMessages)
			{
				str_format(aBuf, sizeof(aBuf), (State > PAUSE_NONE) ? "'%s' speced" : "'%s' resumed", Server()->ClientName(m_ClientId));
				GameServer()->SendChat(-1, TEAM_ALL, aBuf);
			}
			break;
		}

		// Update state
		m_Paused = State;
		m_LastPause = Server()->Tick();

		// Sixup needs a teamchange
		protocol7::CNetMsg_Sv_Team Msg;
		Msg.m_ClientId = m_ClientId;
		Msg.m_CooldownTick = Server()->Tick();
		Msg.m_Silent = true;
		Msg.m_Team = m_Paused ? protocol7::TEAM_SPECTATORS : m_Team;

		GameServer()->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, m_ClientId);
	}

	return m_Paused;
}

int CPlayer::ForcePause(int Time)
{
	m_ForcePauseTime = Server()->Tick() + Server()->TickSpeed() * Time;

	if(g_Config.m_SvPauseMessages)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "'%s' was force-paused for %ds", Server()->ClientName(m_ClientId), Time);
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
	}

	return Pause(PAUSE_SPEC, true);
}

int CPlayer::IsPaused() const
{
	return m_ForcePauseTime ? m_ForcePauseTime : -1 * m_Paused;
}

bool CPlayer::IsPlaying() const
{
	return m_pCharacter && m_pCharacter->IsAlive();
}

void CPlayer::SpectatePlayerName(const char *pName)
{
	if(!pName)
		return;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i != m_ClientId && Server()->ClientIngame(i) && !str_comp(pName, Server()->ClientName(i)))
		{
			SetSpectatorId(i);
			return;
		}
	}
}

void CPlayer::SetSpectatorId(int Id)
{
	m_SpectatorId = Id;
}

void CPlayer::ProcessScoreResult(CScorePlayerResult &Result)
{
	if(Result.m_Success) // SQL request was successful
	{
		switch(Result.m_MessageKind)
		{
		case CScorePlayerResult::DIRECT:
			for(auto &aMessage : Result.m_Data.m_aaMessages)
			{
				if(aMessage[0] == 0)
					break;
				if(str_startswith(aMessage, "SHOP_RAINBOW_OK "))
				{
					const char *p = aMessage + 17;
					m_AccountMoney = str_toint64_base(p);
					m_RainbowOwned = true;
					m_RainbowActive = true;
					m_PersonalMenuDirty = true;
					GameServer()->SendChatTarget(m_ClientId, L("Purchased Rainbow Tee.", "Радужный тии куплен."));
				}
				else if(str_startswith(aMessage, "NICK_PROTECTED "))
				{
					const char *p = aMessage + 15;
					const int Protected = str_toint(p);
					while(*p && *p != ' ')
						++p;
					if(*p == ' ')
						++p;
					if(*p && str_comp(p, Server()->ClientName(m_ClientId)) != 0)
						continue;
					m_NameIsProtectedByAccount = Protected != 0;
					m_NameProtectionLookupPending = false;
					m_NameProtectionChecked = true;
				}
				else if(str_startswith(aMessage, "INV_BUY_OK "))
				{
					const char *p = aMessage + 11;
					m_AccountMoney = str_toint64_base(p);
					m_PersonalMenuDirty = true;
					// Reload inventory so the item appears immediately and equipped state can be persisted.
					if(m_AccountLoggedIn)
						GameServer()->Score()->LoadAccountInventory(m_ClientId, m_AccountId);
					GameServer()->SendChatTarget(m_ClientId, L("Item purchased.", "Предмет куплен."));
				}
				else if(str_comp(aMessage, "INV_OWNED") == 0)
				{
					GameServer()->SendChatTarget(m_ClientId, L("You already own this item.", "Этот предмет уже куплен."));
				}
				else if(str_comp(aMessage, "INV_NO_MONEY") == 0)
				{
					GameServer()->SendChatTarget(m_ClientId, L("Not enough money.", "Недостаточно денег."));
				}
				else if(str_comp(aMessage, "INV_NOT_OWNED") == 0)
				{
					GameServer()->SendChatTarget(m_ClientId, L("You don't own this item.", "У вас нет этого предмета."));
				}
				else if(str_startswith(aMessage, "INV_EQUIP_OK "))
				{
					m_PersonalMenuDirty = true;
					// Keep DB and local state in sync: reload inventory for consistent equipped flags.
					if(m_AccountLoggedIn)
						GameServer()->Score()->LoadAccountInventory(m_ClientId, m_AccountId);
				}
				else if(str_comp(aMessage, "SHOP_RAINBOW_OWNED") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("You already own Rainbow Tee.", "Радужный тии уже куплен."));
				else if(str_comp(aMessage, "SHOP_RAINBOW_NO_MONEY") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("Not enough money.", "Недостаточно денег."));
				else if(str_comp(aMessage, "SHOP_RAINBOW_LOW_LEVEL") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("You need level 15.", "Нужен 15 уровень."));
				else if(str_startswith(aMessage, "SAVENICK_OK "))
				{
					str_copy(m_aAccountNickProtectedIgn, aMessage + 11, sizeof(m_aAccountNickProtectedIgn));
					str_utf8_fix_truncation(m_aAccountNickProtectedIgn);
					m_NameIsProtectedByAccount = false;
					m_NameProtectionLookupPending = false;
					m_NameProtectionChecked = false;
					m_PersonalMenuDirty = true;
					GameServer()->SendChatTarget(m_ClientId, L("Nickname protection was updated.", "Nickname protection was updated."));
				}
				else if(str_comp(aMessage, "ERR_SAVENICK_TAKEN") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("This nickname is already protected by another account.", "This nickname is already protected by another account."));
				else if(str_comp(aMessage, "ERR_SAVENICK_EMPTY") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("Nickname cannot be empty.", "Nickname cannot be empty."));
				else if(str_comp(aMessage, "CHANGEPASSWORD_OK") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("Password changed successfully.", "Пароль успешно изменён."));
				else if(str_comp(aMessage, "ERR_CHANGEPASSWORD_EMPTY") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("Old/new password cannot be empty.", "Старый и новый пароль не должны быть пустыми."));
				else
					GameServer()->SendChatTarget(m_ClientId, aMessage);
			}
			break;
		case CScorePlayerResult::ALL:
		{
			bool PrimaryMessage = true;
			for(auto &aMessage : Result.m_Data.m_aaMessages)
			{
				if(aMessage[0] == 0)
					break;

				if(GameServer()->ProcessSpamProtection(m_ClientId) && PrimaryMessage)
					break;

				GameServer()->SendChat(-1, TEAM_ALL, aMessage, -1);
				PrimaryMessage = false;
			}
			break;
		}
		case CScorePlayerResult::BROADCAST:
			if(Result.m_Data.m_aBroadcast[0] != 0)
				GameServer()->SendBroadcast(Result.m_Data.m_aBroadcast, -1);
			break;
		case CScorePlayerResult::MAP_VOTE:
			GameServer()->m_VoteType = CGameContext::VOTE_TYPE_OPTION;
			GameServer()->m_LastMapVote = time_get();

			char aCmd[256];
			str_format(aCmd, sizeof(aCmd),
				"sv_reset_file types/%s/flexreset.cfg; change_map \"%s\"",
				Result.m_Data.m_MapVote.m_aServer, Result.m_Data.m_MapVote.m_aMap);

			char aChatmsg[512];
			str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s' (%s)",
				Server()->ClientName(m_ClientId), Result.m_Data.m_MapVote.m_aMap, "/map");

			GameServer()->CallVote(m_ClientId, Result.m_Data.m_MapVote.m_aMap, aCmd, "/map", aChatmsg);
			break;
		case CScorePlayerResult::PLAYER_INFO:
		{
			if(Result.m_Data.m_Info.m_Time.has_value())
			{
				GameServer()->Score()->PlayerData(m_ClientId)->Set(Result.m_Data.m_Info.m_Time.value(), Result.m_Data.m_Info.m_aTimeCp);
				Server()->SetClientScore(m_ClientId, Result.m_Data.m_Info.m_Time.value());
				// update map best time if player's time is better
				if(!GameServer()->m_pController->m_CurrentRecord.has_value() ||
					Result.m_Data.m_Info.m_Time.value() < GameServer()->m_pController->m_CurrentRecord.value())
				{
					GameServer()->Score()->LoadBestTime();
				}
			}
			Server()->ExpireServerInfo();
			int Birthday = Result.m_Data.m_Info.m_Birthday;
			if(Birthday != 0 && !m_BirthdayAnnounced && GetCharacter())
			{
				char aBuf[512];
				str_format(aBuf, sizeof(aBuf),
					"Happy DDNet birthday to %s for finishing their first map %d year%s ago!",
					Server()->ClientName(m_ClientId), Birthday, Birthday > 1 ? "s" : "");
				GameServer()->SendChat(-1, TEAM_ALL, aBuf, m_ClientId);
				str_format(aBuf, sizeof(aBuf),
					"Happy DDNet birthday, %s!\nYou have finished your first map exactly %d year%s ago!",
					Server()->ClientName(m_ClientId), Birthday, Birthday > 1 ? "s" : "");
				GameServer()->SendBroadcast(aBuf, m_ClientId);
				m_BirthdayAnnounced = true;

				GameServer()->CreateBirthdayEffect(GetCharacter()->m_Pos, GetCharacter()->TeamMask());
			}
			GameServer()->SendRecord(m_ClientId);
			break;
		}
		case CScorePlayerResult::PLAYER_TIMECP:
			GameServer()->Score()->PlayerData(m_ClientId)->SetBestTimeCp(Result.m_Data.m_Info.m_aTimeCp);
			char aBuf[128], aTime[32];
			str_time_float(Result.m_Data.m_Info.m_Time.value(), ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "Showing the checkpoint times for '%s' with a race time of %s", Result.m_Data.m_Info.m_aRequestedPlayer, aTime);
			GameServer()->SendChatTarget(m_ClientId, aBuf);
			break;
		case CScorePlayerResult::ACCOUNT_REGISTER:
			for(auto &aMessage : Result.m_Data.m_aaMessages)
			{
				if(aMessage[0] == 0)
					break;
				if(str_comp(aMessage, "ERR_USERNAME_EXISTS") == 0)
					GameServer()->SendChatTarget(m_ClientId, L("Username already exists.", "Имя уже занято."));
				else
					GameServer()->SendChatTarget(m_ClientId, aMessage);
			}
			break;
		case CScorePlayerResult::IP_LANGUAGE:
		{
			// Protocol: IP_LANG <has> <lang>
			int Has = 0;
			int Lang = 0;
			if(str_startswith(Result.m_Data.m_aaMessages[0], "IP_LANG "))
			{
				const char *p = &Result.m_Data.m_aaMessages[0][8];
				Has = str_toint(p);
				if(const char *pSpace = str_find(p, " "))
					Lang = str_toint(pSpace + 1);
			}

			m_IpLangHasStored = Has != 0;
			m_IpLangStored = Lang;
			if(m_IpLangHasStored)
			{
				m_Language = (m_IpLangStored == 1) ? ELanguage::RU : ELanguage::EN;
				m_PersonalMenuDirty = true;
			}
			else
			{
				// Suggest RU for unknown IP via per-client vote UI (8 seconds)
				m_IpLangPromptActive = true;
				m_IpLangSuggested = 1;
				GameServer()->StartIpLangVote(m_ClientId);
			}
			break;
		}
		case CScorePlayerResult::ACCOUNT_LOGIN:
			// AUTOLOGIN_SKIP: no account found with AutoLogin=1 for this IGN - just ignore silently
			if(str_comp(Result.m_Data.m_aaMessages[0], "AUTOLOGIN_SKIP") == 0)
			{
				m_AutoLoginPending = false;
				if(m_aClientAddrStr[0])
					GameServer()->Score()->LoadIpLanguage(m_ClientId, m_aClientAddrStr);
				break;
			}
			if(str_startswith(Result.m_Data.m_aaMessages[0], "LOGIN_OK "))
			{
				// Protocol: LOGIN_OK <AccountId> [LanguageInt] [HintWasShown]
				const char *p = &Result.m_Data.m_aaMessages[0][9];
				int64_t NewAccountId = str_toint64_base(p);
				int AccountLanguage = (int)ELanguage::EN;
				int HintWasShown = 1;
				if(const char *pSpace = str_find(p, " "))
				{
					p = pSpace + 1;
					AccountLanguage = str_toint(p);
					if(const char *pSpace2 = str_find(p, " "))
					{
						p = pSpace2 + 1;
						HintWasShown = str_toint(p);
					}
				}
				const bool IsAutoLogin = m_AutoLoginPending;
				m_AutoLoginPending = false;
				m_AccountId = NewAccountId;
				m_Language = AccountLanguage == (int)ELanguage::RU ? ELanguage::RU : ELanguage::EN;
				m_AccountLoggedIn = m_AccountId > 0;
				if(m_AccountLoggedIn)
				{
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(i == m_ClientId)
							continue;
						CPlayer *pOther = GameServer()->m_apPlayers[i];
						if(!pOther)
							continue;
						if(pOther->m_AccountLoggedIn && pOther->m_AccountId == m_AccountId)
						{
							m_AccountLoggedIn = false;
							m_AccountId = -1;
							m_aAccountNickProtectedIgn[0] = 0;
							m_AccountJetpackActive = false;
							m_ProfileLoaded = false;
							m_ProfileLoading = false;
							if(!IsAutoLogin)
								GameServer()->SendChatTarget(m_ClientId, L("This account is already in game.", "Этот аккаунт уже в игре."));
							m_PersonalMenuDirty = true;
							return;
						}
					}
					m_ProfileLoaded = false;
					if(!m_ProfileLoading)
					{
						m_ProfileLoading = true;
						GameServer()->Score()->LoadAccountProfile(m_ClientId, m_AccountId);
					}
				}
				m_PersonalMenuDirty = true;
				if(!IsAutoLogin)
				{
					if(HintWasShown == 0)
					{
						GameServer()->SendChatTarget(m_ClientId, L("Tip: You can change language in Settings -> Language.", "Подсказка: язык можно сменить в Настройки -> Язык."));
					}
					GameServer()->SendChatTarget(m_ClientId, L("Logged in successfully.", "Успешный вход."));
				}
				else
				{
					GameServer()->SendChatTarget(m_ClientId, L("Logged in successfully.", "Успешный вход."));
				}
			}
			else
			{
				// Only show errors if this was a manual login attempt (not autologin)
				if(!m_AutoLoginPending)
				{
					for(auto &aMessage : Result.m_Data.m_aaMessages)
					{
						if(aMessage[0] == 0)
							break;
						if(str_comp(aMessage, "ERR_ACCOUNT_NOT_FOUND") == 0)
							GameServer()->SendChatTarget(m_ClientId, L("Account not found.", "Аккаунт не найден."));
						else if(str_comp(aMessage, "ERR_WRONG_PASSWORD") == 0)
							GameServer()->SendChatTarget(m_ClientId, L("Wrong password.", "Неверный пароль."));
						else
							GameServer()->SendChatTarget(m_ClientId, aMessage);
					}
				}
				m_AutoLoginPending = false;
			}
			break;
		case CScorePlayerResult::ACCOUNT_PROFILE:
		if(Result.m_Data.m_aaMessages[0][0])
		{
			log_info("account", "ACCOUNT_PROFILE message: '%s'", Result.m_Data.m_aaMessages[0]);
			// If SQL worker reports no profile row, don't mark as loaded.
			if(str_comp(Result.m_Data.m_aaMessages[0], "Profile not found") == 0)
			{
				m_ProfileLoaded = false;
				m_ProfileLoading = false;
				m_PersonalMenuDirty = true;
				break;
			}
		}
		m_ProfileLoaded = true;
		m_ProfileLoading = false;
		str_copy(m_aAccountUsername, Result.m_Data.m_Account.m_aUsername, sizeof(m_aAccountUsername));
		str_copy(m_aAccountLastIgn, Result.m_Data.m_Account.m_aLastIgn, sizeof(m_aAccountLastIgn));
		str_copy(m_aAccountNickProtectedIgn, Result.m_Data.m_Account.m_aNickProtectedIgn, sizeof(m_aAccountNickProtectedIgn));
		str_utf8_fix_truncation(m_aAccountNickProtectedIgn);
		str_copy(m_aAccountRegisterDate, Result.m_Data.m_Account.m_aRegisterDate, sizeof(m_aAccountRegisterDate));
		m_AccountLevel = maximum(1, Result.m_Data.m_Account.m_Level);
		m_AccountXP = Result.m_Data.m_Account.m_XP;
		m_AccountMoney = Result.m_Data.m_Account.m_Money;
		m_AccountRole = Result.m_Data.m_Account.m_Role;
		m_AccountJetpackActive = Result.m_Data.m_Account.m_JetpackActive != 0;
		m_AccountPlaytimeSeconds = Result.m_Data.m_Account.m_PlaytimeSeconds;
		m_AccountDeaths = Result.m_Data.m_Account.m_Deaths;
		m_AccountKills = Result.m_Data.m_Account.m_Kills;
		m_AccountCombo = Result.m_Data.m_Account.m_Combo;
		m_RainbowOwned = Result.m_Data.m_Account.m_RainbowOwned != 0;
		m_RainbowActive = Result.m_Data.m_Account.m_RainbowActive != 0;
		m_AccountAutoLogin = Result.m_Data.m_Account.m_AutoLogin != 0;
		log_info("account", "ACCOUNT_PROFILE applied: user='%s' lvl=%d xp=%d money=%lld play=%lld deaths=%lld",
			m_aAccountUsername, m_AccountLevel, m_AccountXP, (long long)m_AccountMoney, (long long)m_AccountPlaytimeSeconds, (long long)m_AccountDeaths);
		// Load FoxNet inventory after profile so the menu and cosmetics reflect persistence.
		if(m_AccountLoggedIn)
			GameServer()->Score()->LoadAccountInventory(m_ClientId, m_AccountId);
		m_PersonalMenuDirty = true;
		break;
		case CScorePlayerResult::ACCOUNT_MAILBOX:
			m_MailboxFetchPending = false;
			m_vMailbox.clear();
			for(int i = 0; i < Result.m_Data.m_Mailbox.m_NumMails && i < 32; i++)
			{
				CMail Mail;
				Mail.m_MailId = Result.m_Data.m_Mailbox.m_aMails[i].m_MailId;
				str_copy(Mail.m_aSubject, Result.m_Data.m_Mailbox.m_aMails[i].m_aSubject, sizeof(Mail.m_aSubject));
				str_copy(Mail.m_aMessage, Result.m_Data.m_Mailbox.m_aMails[i].m_aMessage, sizeof(Mail.m_aMessage));
				Mail.m_Unread = Result.m_Data.m_Mailbox.m_aMails[i].m_Unread != 0;
				Mail.m_RewardXP = Result.m_Data.m_Mailbox.m_aMails[i].m_RewardXP;
				Mail.m_RewardMoney = Result.m_Data.m_Mailbox.m_aMails[i].m_RewardMoney;
				Mail.m_RewardClaimed = Result.m_Data.m_Mailbox.m_aMails[i].m_RewardClaimed != 0;
				m_vMailbox.push_back(Mail);
			}
			m_MailboxLoaded = true;
			{
				int NewAttention = 0;
				for(const auto &Mail : m_vMailbox)
				{
					const bool HasReward = (Mail.m_RewardXP != 0 || Mail.m_RewardMoney != 0);
					const bool RewardUnclaimed = HasReward && !Mail.m_RewardClaimed;
					if(Mail.m_Unread || RewardUnclaimed)
						NewAttention++;
				}
				if(!m_MailboxFirstLoad && NewAttention > m_MailboxAttentionCount)
					GameServer()->SendChatTarget(m_ClientId, L("You have received new mail.", "Вы получили новую почту."));
				m_MailboxAttentionCount = NewAttention;
				m_MailboxFirstLoad = false;
			}
			if(m_PersonalMenuPage == 4 || m_PersonalMenuPage == 5)
				m_PersonalMenuDirty = true;
			break;
		case CScorePlayerResult::ACCOUNT_INVENTORY:
			m_vFoxOwnedItems.clear();
			m_FoxItemExpiresAt.clear();
			// Clear all Fox cosmetics first, then apply equipped from DB.
			SetHalo(false);
			SetLovely(false);
			SetRotatingBall(false);
			SetSparkle(false);
			SetInverseAim(false);
			SetRainbowFeet(false);
			SetRainbowBody(false);
			SetHookPower(HOOKTYPE_NORMAL);
			SetDamageIndType(INDTYPE_NONE);
			SetEmoticonGun(0);
			SetPhaseGun(false);
			SetGunType(GUNTYPE_NONE);
			SetPhysicalBullet(false);
			SetTrail(TRAILTYPE_NONE);
			SetHatType(HATTYPE_NONE);
			SetDeathEffect(DEATHTYPE_NONE);

			for(int i = 0; i < Result.m_Data.m_Inventory.m_NumItems && i < (int)std::size(Result.m_Data.m_Inventory.m_aItems); i++)
			{
				const auto &It = Result.m_Data.m_Inventory.m_aItems[i];
				if(It.m_aItemName[0] == 0)
					continue;
				m_vFoxOwnedItems.emplace_back(It.m_aItemName);
				m_FoxItemExpiresAt[It.m_aItemName] = It.m_ExpiresAt;
				if(It.m_Equipped != 0)
				{
					const CFoxItemConfig *pCfg = GameServer()->FoxItemRegistry().FindByName(It.m_aItemName);
					if(pCfg)
						pCfg->m_Apply(*this, *pCfg, -1);
				}
			}
			m_PersonalMenuDirty = true;
			break;
		}
	}
}

vec2 CPlayer::CCameraInfo::ConvertTargetToWorld(vec2 Position, vec2 Target) const
{
	vec2 TargetCameraOffset(0, 0);
	float l = length(Target);

	if(l > 0.0001f) // make sure that this isn't 0
	{
		float OffsetAmount = maximum(l - m_Deadzone, 0.0f) * (m_FollowFactor / 100.0f);
		TargetCameraOffset = normalize_pre_length(Target, l) * OffsetAmount;
	}

	return Position + (Target - TargetCameraOffset) * m_Zoom + TargetCameraOffset;
}

void CPlayer::CCameraInfo::Write(const CNetMsg_Cl_CameraInfo *Msg)
{
	m_HasCameraInfo = true;
	m_Zoom = Msg->m_Zoom / 1000.0f;
	m_Deadzone = Msg->m_Deadzone;
	m_FollowFactor = Msg->m_FollowFactor;
}

void CPlayer::CCameraInfo::Reset()
{
	m_HasCameraInfo = false;
	m_Zoom = 1.0f;
	m_Deadzone = 0.0f;
	m_FollowFactor = 0.0f;
}

