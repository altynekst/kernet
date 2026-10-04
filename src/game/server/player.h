/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_PLAYER_H
#define GAME_SERVER_PLAYER_H

#include <string>

#include "teeinfo.h"

#include <base/str.h>
#include <base/vmath.h>

#include <engine/server/databases/connection_pool.h>

#include "save.h"

#include <game/alloc.h>
#include <game/gamecore.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class CCharacter;
class CGameContext;
class IServer;
struct CNetObj_PlayerInput;
struct CNetMsg_Cl_CameraInfo;
struct CScorePlayerResult;

// player object
class CPlayer
{
	MACRO_ALLOC_POOL_ID()

public:
	enum
	{
		HOOKTYPE_NORMAL = 0,
		HOOKTYPE_RAINBOW = 1,
	};

	enum
	{
		INDTYPE_NONE = 0,
		INDTYPE_CLOCKWISE,
		INDTYPE_COUNTERWISE,
		INDTYPE_INWARD,
		INDTYPE_OUTWARD,
		INDTYPE_LINE,
		INDTYPE_CRISSCROSS,
	};

	enum
	{
		GUNTYPE_NONE = 0,
		GUNTYPE_HEART,
		GUNTYPE_MIXED,
		GUNTYPE_LASER,
	};

	void SetTrail(int Type);
	void SetHatType(int Type);
	void SetDeathEffect(int Type);
	void SetHalo(bool Active);
	void SetLovely(bool Active);
	void SetRotatingBall(bool Active);
	void SetSparkle(bool Active);
	void SetInverseAim(bool Active);
	void RespawnCosmeticEntities();
	void SetRainbowFeet(bool Active);
	void SetRainbowBody(bool Active);
	void SetHookPower(int Type);
	void SetDamageIndType(int Type);
	void SetEmoticonGun(int Type);
	void SetPhaseGun(bool Active);
	void SetGunType(int Type);
	void SetPhysicalBullet(bool Active);
	int AccountMoneyMultiplier() const { return m_AccountRole == 1 ? 2 : 1; }
	int AccountXPMultiplier() const { return m_AccountRole == 1 ? 2 : 1; }
	bool HasPermanentJetpack() const { return m_AccountLoggedIn && m_AccountJetpackActive; }
	bool IsProtectedNickActive() const;

	enum class ELanguage
	{
		EN = 0,
		RU = 1,
	};

	CPlayer(CGameContext *pGameServer, uint32_t UniqueClientId, int ClientId, int Team);
	~CPlayer();

	void Reset();

	void TryRespawn();
	void Respawn(bool WeakHook = false); // with WeakHook == true the character will be spawned after all calls of Tick from other Players
	CCharacter *ForceSpawn(vec2 Pos); // required for loading savegames
	void SetTeam(int Team, bool DoChatMsg = true);
	int GetTeam() const { return m_Team; }
	int GetCid() const { return m_ClientId; }
	uint32_t GetUniqueCid() const { return m_UniqueClientId; }
	int GetClientVersion() const;
	bool SetTimerType(int TimerType);

	void Tick();
	void PostTick();

	// will be called after all Tick and PostTick calls from other players
	void PostPostTick();
	void Snap(int SnappingClient);
	void FakeSnap();

	void OnDirectInput(const CNetObj_PlayerInput *pNewInput);
	void OnPredictedInput(const CNetObj_PlayerInput *pNewInput);
	void OnPredictedEarlyInput(const CNetObj_PlayerInput *pNewInput);
	void OnDisconnect();

	void KillCharacter(int Weapon = WEAPON_GAME, bool SendKillMsg = true);
	CCharacter *GetCharacter();
	const CCharacter *GetCharacter() const;

	void SpectatePlayerName(const char *pName);

	//---------------------------------------------------------
	// this is used for snapping so we know how we can clip the view for the player
	vec2 m_ViewPos;
	int m_TuneZone;
	int m_TuneZoneOld;

	// states if the client is chatting, accessing a menu etc.
	int m_PlayerFlags;

	// used for snapping to just update latency if the scoreboard is active
	int m_aCurLatency[MAX_CLIENTS];

	int m_SentSnaps = 0;

	int SpectatorId() const { return m_SpectatorId; }
	void SetSpectatorId(int Id);

	bool m_IsReady;

	//
	int m_Vote;
	int m_VotePos;
	//
	int m_LastVoteCall;
	int m_LastVoteTry;
	int m_LastChat;
	int m_LastSetTeam;
	int m_LastSetSpectatorMode;
	int m_LastChangeInfo;
	int m_LastEmote;
	int m_LastEmoteGlobal;
	int m_LastKill;
	int m_aLastCommands[4];
	int m_LastCommandPos;
	int m_LastWhisperTo;
	int m_LastInvited;

	int m_SendVoteIndex;

	CTeeInfo m_TeeInfos;

	int m_DieTick;
	int m_PreviousDieTick;
	int m_JoinTick;
	int m_LastActionTick;
	int m_TeamChangeTick;

	// network latency calculations
	struct
	{
		int m_Accum;
		int m_AccumMin;
		int m_AccumMax;
		int m_Avg;
		int m_Min;
		int m_Max;
	} m_Latency;

private:
	const uint32_t m_UniqueClientId;
	CCharacter *m_pCharacter;
	int m_NumInputs;
	CGameContext *m_pGameServer;

	CGameContext *GameServer() const { return m_pGameServer; }
	IServer *Server() const;

	//
	bool m_Spawning;
	bool m_WeakHookSpawn;
	int m_ClientId;
	int m_Team;

	// used for spectator mode
	int m_SpectatorId;

	int m_Paused;
	int64_t m_ForcePauseTime;
	int64_t m_LastPause;
	bool m_Afk;

	int m_DefEmote;
	int m_OverrideEmote;
	int m_OverrideEmoteReset;
	bool m_Halloween;

public:
	enum
	{
		PAUSE_NONE = 0,
		PAUSE_PAUSED,
		PAUSE_SPEC
	};

	enum
	{
		TIMERTYPE_DEFAULT = -1,
		TIMERTYPE_GAMETIMER,
		TIMERTYPE_BROADCAST,
		TIMERTYPE_GAMETIMER_AND_BROADCAST,
		TIMERTYPE_SIXUP,
		TIMERTYPE_NONE,
	};

	bool m_DND;
	bool m_Whispers;
	int64_t m_FirstVoteTick;
	char m_aTimeoutCode[64];

	void ProcessPause();
	int Pause(int State, bool Force);
	int ForcePause(int Time);
	int IsPaused() const;
	bool CanSpec() const;

	bool IsPlaying() const;
	int64_t m_LastKickVote;
	int64_t m_LastDDRaceTeamChange;
	int m_ShowOthers;
	bool m_ShowAll;
	bool m_EnableSpectatorCount;
	vec2 m_ShowDistance;
	bool m_SpecTeam;
	bool m_NinjaJetpack;

	// camera info is used sparingly for converting aim target to absolute world coordinates
	class CCameraInfo
	{
		friend class CPlayer;
		bool m_HasCameraInfo;
		float m_Zoom;
		int m_Deadzone;
		int m_FollowFactor;

	public:
		vec2 ConvertTargetToWorld(vec2 Position, vec2 Target) const;
		void Write(const CNetMsg_Cl_CameraInfo *pMsg);
		void Reset();
	} m_CameraInfo;

	int m_ChatScore;

	bool m_Moderating;

	void UpdatePlaytime();
	void AfkTimer();
	void SetAfk(bool Afk);
	void SetInitialAfk(bool Afk);
	bool IsAfk() const { return m_Afk; }

	int64_t m_LastPlaytime;
	int64_t m_LastEyeEmote;
	int64_t m_LastBroadcast;
	bool m_LastBroadcastImportance;

	CNetObj_PlayerInput *m_pLastTarget;
	bool m_LastTargetInit;

	bool m_EyeEmoteEnabled;
	int m_TimerType;

	int GetDefaultEmote() const;
	void OverrideDefaultEmote(int Emote, int Tick);
	bool CanOverrideDefaultEmote() const;

	bool m_FirstPacket;
	int64_t m_LastSqlQuery;
	void ProcessScoreResult(CScorePlayerResult &Result);
	std::shared_ptr<CScorePlayerResult> m_ScoreQueryResult;
	std::shared_ptr<CScorePlayerResult> m_ScoreFinishResult;
	bool m_NotEligibleForFinish;
	int64_t m_EligibleForFinishCheck;
	bool m_VotedForPractice;
	int m_SwapTargetsClientId; //Client ID of the swap target for the given player
	bool m_BirthdayAnnounced;

	ELanguage m_Language;
	const char *L(const char *pEn, const char *pRu) const;

	bool m_IpLangPromptActive;
	int m_IpLangSuggested;
	int64_t m_IpLangVoteEndTime;
	int m_IpLangVoteLastSeconds;
	bool m_IpLangHasStored;
	int m_IpLangStored;
	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];

	bool m_RainbowOwned;
	bool m_RainbowActive;
	int m_RainbowColor;
	int m_RainbowSpeed;
	bool m_RainbowFeet;
	bool m_RainbowBody;
	int m_HookPower;

	bool m_Sparkle;
	bool m_InverseAim;

	int m_DamageIndType;
	int m_EmoticonGun;
	bool m_PhaseGun;
	int m_GunType;
	bool m_PhysicalBullet;

	int m_CosmeticTrailType;
	int m_CosmeticHatType;
	int m_CosmeticDeathType;
	bool m_CosmeticHalo;
	bool m_CosmeticLovely;
	bool m_CosmeticRotatingBall;
	bool m_HideOthersCosmetics;

	int m_FoxMenuSelectedType;
	char m_aFoxMenuSelectedItem[64];
	std::vector<std::string> m_vFoxOwnedItems;
	std::unordered_map<std::string, int64_t> m_FoxItemExpiresAt;

	bool m_AccountLoggedIn;
	int64_t m_AccountId;
	int m_PersonalMenuPage;
	bool m_PersonalMenuDirty;
	bool m_ProfileLoading;
	bool m_ProfileLoaded;
	bool m_AccountAutoLogin;
	bool m_AutoLoginPending;
	char m_aAccountUsername[MAX_NAME_LENGTH];
	char m_aAccountLastIgn[MAX_NAME_LENGTH];
	char m_aAccountNickProtectedIgn[MAX_NAME_LENGTH];
	char m_aAccountRegisterDate[32];
	int m_AccountLevel;
	int m_AccountXP;
	int64_t m_AccountMoney;
	int m_AccountRole;
	bool m_AccountJetpackActive;
	int64_t m_AccountPlaytimeSeconds;
	int64_t m_LastPersonalMenuRefreshTick;
	int64_t m_AccountDeaths;
	int64_t m_AccountKills;
	int64_t m_AccountCombo;
	int64_t m_LastFarmerHintTick;
	int64_t m_LastFarmerHintId;
	int64_t m_LastVipTeleportHintTick;
	bool m_NameIsProtectedByAccount;
	bool m_NameProtectionLookupPending;
	bool m_NameProtectionChecked;
	char m_aNameProtectionNick[MAX_NAME_LENGTH];

	// Anti-farm (kill rewards)
	int m_LastDeathFromCid;
	int m_DeathFromStreak;
	int64_t m_aNoRewardUntilFromCid[MAX_CLIENTS];
	int64_t m_LastAccountPlaytimeUpdateTick;
	int64_t m_LastAccountPlaytimeSaveTick;

	int m_KillMarkKillerCid;
	int m_KillMarkWeapon;
	int64_t m_KillMarkExpireTick;
	bool m_KillMarkPausedInFreeze;
	bool m_KillMarkVictimInFreeze;

	bool m_RestoreWeaponsOnSpawn;
	bool m_aRestoreWeaponGot[NUM_WEAPONS];
	int m_aRestoreWeaponAmmo[NUM_WEAPONS];
	int m_RestoreActiveWeapon;

	struct CMail
	{
		int64_t m_MailId;
		char m_aSubject[128];
		char m_aMessage[512];
		bool m_Unread;
		int m_RewardXP;
		int64_t m_RewardMoney;
		bool m_RewardClaimed;
	};
	std::vector<CMail> m_vMailbox;
	bool m_MailboxLoaded;
	bool m_MailboxOnlyUnread;
	int m_MailboxViewIndex;
	bool m_MailboxFetchPending;
	int m_MailboxAttentionCount;
	bool m_MailboxFirstLoad; // true after login reset, skip notification on first load
	int64_t m_LastMailboxFetchTick;

	int m_RescueMode;

	int m_aDuelRequests[MAX_CLIENTS];
	bool m_InDuel;
	int m_DuelOpponent;
	int m_DuelScore;
	int m_DuelSide;
	int m_DuelTeam;
	CSaveTee m_DuelSavedTee;
	int m_DuelSavedTeam;
	int m_DuelFrozenTicks;
	bool m_DuelSpawnPending;
	vec2 m_DuelSpawnPos;

	CSaveTee m_LastTeleTee;
	std::optional<CSaveTee> m_LastDeath;
};

#endif
