#ifndef GAME_SERVER_SCOREWORKER_H
#define GAME_SERVER_SCOREWORKER_H

#include <base/str.h>

#include <engine/map.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/shared/protocol.h>
#include <engine/shared/uuid_manager.h>

#include <game/server/save.h>
#include <game/voting.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class IDbConnection;
class IGameController;

enum
{
	NUM_CHECKPOINTS = MAX_CHECKPOINTS,
	TIMESTAMP_STR_LENGTH = 20, // 2019-04-02 19:38:36
};

struct CScorePlayerResult : ISqlResult
{
	CScorePlayerResult();

	enum
	{
		MAX_MESSAGES = 10,
	};

	enum Variant
	{
		DIRECT,
		ALL,
		BROADCAST,
		MAP_VOTE,
		PLAYER_INFO,
		PLAYER_TIMECP,
		ACCOUNT_REGISTER,
		ACCOUNT_LOGIN,
		ACCOUNT_PROFILE,
		ACCOUNT_MAILBOX,
		ACCOUNT_INVENTORY,
		IP_LANGUAGE,
	} m_MessageKind;
	union
	{
		char m_aaMessages[MAX_MESSAGES][512];
		char m_aBroadcast[1024];
		struct
		{
			std::optional<float> m_Time;
			float m_aTimeCp[NUM_CHECKPOINTS];
			int m_Birthday; // 0 indicates no birthday
			char m_aRequestedPlayer[MAX_NAME_LENGTH];
		} m_Info = {};
		struct
		{
			char m_aReason[VOTE_REASON_LENGTH];
			char m_aServer[32 + 1];
			char m_aMap[MAX_MAP_LENGTH + 1];
		} m_MapVote;
		struct
		{
			int64_t m_AccountId;
			char m_aUsername[MAX_NAME_LENGTH];
			char m_aLastIgn[MAX_NAME_LENGTH];
			char m_aNickProtectedIgn[MAX_NAME_LENGTH];
			char m_aRegisterDate[32];
			int m_Level;
			int m_XP;
			int m_XPToNext;
			int64_t m_PlaytimeSeconds;
			int64_t m_Money;
			int64_t m_Deaths;
			int64_t m_Kills;
			int64_t m_Combo;
			int m_RainbowOwned;
			int m_RainbowActive;
			int m_Role;
			int m_AutoLogin;
			int m_JetpackActive;
		} m_Account;
		struct
		{
			int m_NumMails;
			struct
			{
				int64_t m_MailId;
				char m_aSubject[128];
				char m_aMessage[512];
				int m_Unread;
				int m_RewardXP;
				int64_t m_RewardMoney;
				int m_RewardClaimed;
			} m_aMails[32];
		} m_Mailbox;
		struct
		{
			int m_NumItems;
			struct
			{
				char m_aItemName[64];
				int m_GroupId;
				int m_Equipped;
				int64_t m_ExpiresAt;
			} m_aItems[512];
		} m_Inventory;
	} m_Data = {}; // PLAYER_INFO

	void SetVariant(Variant v);
};

struct CSqlAccountInventoryLoadRequest : ISqlData
{
	CSqlAccountInventoryLoadRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int64_t m_AccountId;
};

struct CSqlAccountInventoryBuyRequest : ISqlData
{
	CSqlAccountInventoryBuyRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int m_ClientId;
	int64_t m_AccountId;
	char m_aItemName[64];
	int m_GroupId;
	int64_t m_Price;
	int m_DurationDays;
};

struct CSqlAccountInventorySetEquippedRequest : ISqlData
{
	CSqlAccountInventorySetEquippedRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int m_ClientId;
	int64_t m_AccountId;
	char m_aItemName[64];
	int m_GroupId;
	int m_Equipped;
};

struct CSqlAccountRegisterRequest : ISqlData
{
	CSqlAccountRegisterRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aUsername[MAX_NAME_LENGTH];
	char m_aPassword[128];
	char m_aLastIgn[MAX_NAME_LENGTH];
	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
};

struct CSqlAccountLoginRequest : ISqlData
{
	CSqlAccountLoginRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aUsername[MAX_NAME_LENGTH];
	char m_aPassword[128];
	char m_aLastIgn[MAX_NAME_LENGTH];
	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
};

struct CSqlAccountChangePasswordRequest : ISqlData
{
	CSqlAccountChangePasswordRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	int64_t m_AccountId;
	char m_aOldPassword[128];
	char m_aNewPassword[128];
};

struct CSqlIpLanguageLoadRequest : ISqlData
{
	CSqlIpLanguageLoadRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))	{}

	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
};

struct CSqlIpLanguageSetRequest : ISqlData
{
	CSqlIpLanguageSetRequest() :
		ISqlData(nullptr)	{}

	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
	int m_Language;
};

struct CSqlAccountProfileRequest : ISqlData
{
	CSqlAccountProfileRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	int64_t m_AccountId;
};

struct CSqlShopBuyRainbowRequest : ISqlData
{
	CSqlShopBuyRainbowRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int m_ClientId;
	int64_t m_AccountId;
};

struct CSqlCosmeticsSetRainbowActiveRequest : ISqlData
{
	CSqlCosmeticsSetRainbowActiveRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int m_Active;
};

struct CSqlAccountPlaytimeSaveRequest : ISqlData
{
	CSqlAccountPlaytimeSaveRequest() :
		ISqlData(nullptr)
	{
	}

	int64_t m_AccountId;
	int64_t m_PlaytimeSeconds;
};

struct CSqlAccountAddDeathRequest : ISqlData
{
	CSqlAccountAddDeathRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
};

struct CSqlAccountKillEventRequest : ISqlData
{
	CSqlAccountKillEventRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_KillerAccountId;
	int64_t m_VictimAccountId;
	int m_RewardXP;
	int64_t m_RewardMoney;
};

struct CSqlAccountAddRewardRequest : ISqlData
{
	CSqlAccountAddRewardRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int m_AddXP;
	int64_t m_AddMoney;
};

struct CSqlAccountGlobalMailRequest : ISqlData
{
	CSqlAccountGlobalMailRequest() :
		ISqlData(nullptr)
	{
	}

	char m_aSubject[128];
	char m_aMessage[512];
	int m_RewardXP;
	int64_t m_RewardMoney;
	int64_t m_ExpiresAt;
};

struct CSqlAccountMailboxLoadRequest : ISqlData
{
	CSqlAccountMailboxLoadRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int64_t m_AccountId;
};

struct CSqlAccountMailboxActionRequest : ISqlData
{
	CSqlAccountMailboxActionRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int64_t m_MailId;
};

struct CSqlAccountMailboxSetReadRequest : ISqlData
{
	CSqlAccountMailboxSetReadRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int64_t m_MailId;
	int m_Unread;
};

struct CSqlAccountSetLanguageRequest : ISqlData
{
	CSqlAccountSetLanguageRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int m_Language;
};

struct CSqlAccountSetRoleRequest : ISqlData
{
	CSqlAccountSetRoleRequest() :
		ISqlData(nullptr)
	{
	}
	char m_aUsername[MAX_NAME_LENGTH];
	int m_Role;
};

// AutoLogin: lookup account by LastIgn + IP
struct CSqlAutoLoginRequest : ISqlData
{
	CSqlAutoLoginRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	char m_aLastIgn[MAX_NAME_LENGTH];
	char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
};

// Set AutoLogin flag for an account
struct CSqlSetAutoLoginRequest : ISqlData
{
	CSqlSetAutoLoginRequest() :
		ISqlData(nullptr)
	{
	}
	int64_t m_AccountId;
	int m_AutoLogin;
};

struct CSqlAdminMoneyExpRequest : ISqlData
{
	CSqlAdminMoneyExpRequest() :
		ISqlData(nullptr)
	{
	}
	char m_aUsername[MAX_NAME_LENGTH];
	int64_t m_Money;
	int m_XP;
};

struct CSqlSetVipRequest : ISqlData
{
	CSqlSetVipRequest() :
		ISqlData(nullptr)
	{
	}
	char m_aUsername[MAX_NAME_LENGTH];
	int m_DurationDays; // 0 = remove vip
};

struct CSqlSetJetpackRequest : ISqlData
{
	CSqlSetJetpackRequest() :
		ISqlData(nullptr)
	{
	}
	char m_aUsername[MAX_NAME_LENGTH];
	int m_DurationDays; // 0 = remove jetpack role
};

struct CSqlSaveNickRequest : ISqlData
{
	CSqlSaveNickRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	int64_t m_AccountId;
	char m_aNick[MAX_NAME_LENGTH];
};

struct CSqlNickProtectedLookupRequest : ISqlData
{
	CSqlNickProtectedLookupRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	char m_aNick[MAX_NAME_LENGTH];
};

struct CScoreVipListResult : ISqlResult
{
	enum { MAX_ENTRIES = 64 };
	int m_NumEntries = 0;
	struct
	{
		char m_aUsername[MAX_NAME_LENGTH];
		int64_t m_GrantedAt;
		int64_t m_ExpiresAt;
	} m_aEntries[MAX_ENTRIES];
};

struct CSqlVipListRequest : ISqlData
{
	CSqlVipListRequest(std::shared_ptr<CScoreVipListResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
};

struct CScoreFarmersResult : ISqlResult
{
	enum
	{
		MAX_FARMERS = 128,
		TYPE_NORMAL = 0,
		TYPE_BIG = 1,
	};
	int m_NumFarmers = 0;
	struct
	{
		int64_t m_FarmerId;
		int m_Type;
		float m_X1;
		float m_Y1;
		float m_X2;
		float m_Y2;
		int64_t m_MoneyPerSec;
		int m_XPPerSec;
	} m_aFarmers[MAX_FARMERS];
};

struct CSqlLoadFarmersRequest : ISqlData
{
	CSqlLoadFarmersRequest(std::shared_ptr<CScoreFarmersResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}
	char m_aMap[MAX_MAP_LENGTH];
};

struct CSqlAddFarmerRequest : ISqlData
{
	CSqlAddFarmerRequest() :
		ISqlData(nullptr)
	{
	}
	char m_aMap[MAX_MAP_LENGTH];
	int m_Type;
	float m_X1;
	float m_Y1;
	float m_X2;
	float m_Y2;
	int64_t m_MoneyPerSec;
	int m_XPPerSec;
};

struct CScoreLoadBestTimeResult : ISqlResult
{
	std::optional<float> m_CurrentRecord = std::nullopt;
};

struct CSqlLoadBestTimeRequest : ISqlData
{
	CSqlLoadBestTimeRequest(std::shared_ptr<CScoreLoadBestTimeResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	// current map
	char m_aMap[MAX_MAP_LENGTH];
};

struct CSqlPlayerRequest : ISqlData
{
	CSqlPlayerRequest(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	// object being requested, either map (128 bytes) or player (16 bytes)
	char m_aName[MAX_MAP_LENGTH];
	// current map
	char m_aMap[MAX_MAP_LENGTH];
	char m_aRequestingPlayer[MAX_NAME_LENGTH];
	// relevant for /top5 kind of requests
	int m_Offset;
	char m_aServer[5];
};

struct CScoreRandomMapResult : ISqlResult
{
	CScoreRandomMapResult(int ClientId) :
		m_ClientId(ClientId)
	{
		m_aMap[0] = '\0';
		m_aMessage[0] = '\0';
	}
	int m_ClientId;
	char m_aMap[MAX_MAP_LENGTH];
	char m_aMessage[512];
};

struct CSqlRandomMapRequest : ISqlData
{
	CSqlRandomMapRequest(std::shared_ptr<CScoreRandomMapResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aServerType[32];
	char m_aCurrentMap[MAX_MAP_LENGTH];
	char m_aRequestingPlayer[MAX_NAME_LENGTH];
	int m_MinStars;
	int m_MaxStars;
};

struct CSqlScoreData : ISqlData
{
	CSqlScoreData(std::shared_ptr<CScorePlayerResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aMap[MAX_MAP_LENGTH];
	char m_aGameUuid[UUID_MAXSTRSIZE];
	char m_aName[MAX_MAP_LENGTH];

	int m_ClientId;
	float m_Time;
	char m_aTimestamp[TIMESTAMP_STR_LENGTH];
	float m_aCurrentTimeCp[NUM_CHECKPOINTS];
	int m_Num;
	bool m_Search;
	char m_aRequestingPlayer[MAX_NAME_LENGTH];
};

struct CScoreSaveResult : ISqlResult
{
	CScoreSaveResult(int PlayerId, const char *pPlayerName, const char *pServer) :
		m_Status(SAVE_FAILED),
		m_RequestingPlayer(PlayerId)
	{
		m_aMessage[0] = '\0';
		m_aBroadcast[0] = '\0';
		m_aCode[0] = '\0';
		m_aGeneratedCode[0] = '\0';
		str_copy(m_aRequestingPlayer, pPlayerName);
		str_copy(m_aServer, pServer);
	}
	enum
	{
		SAVE_SUCCESS,
		SAVE_WARNING,
		SAVE_FALLBACKFILE,
		// load team in the following two cases
		SAVE_FAILED,
		LOAD_SUCCESS,
		LOAD_FAILED,
	} m_Status;
	char m_aMessage[512];
	char m_aBroadcast[512];
	CSaveTeam m_SavedTeam;
	int m_RequestingPlayer;
	char m_aRequestingPlayer[MAX_NAME_LENGTH];
	CUuid m_SaveId;
	char m_aServer[5];
	char m_aCode[128];
	char m_aGeneratedCode[128];
};

struct CSqlTeamScoreData : ISqlData
{
	CSqlTeamScoreData() :
		ISqlData(nullptr)
	{
	}

	char m_aGameUuid[UUID_MAXSTRSIZE];
	char m_aMap[MAX_MAP_LENGTH];
	float m_Time;
	char m_aTimestamp[TIMESTAMP_STR_LENGTH];
	unsigned int m_Size;
	char m_aaNames[MAX_CLIENTS][MAX_NAME_LENGTH];
	CUuid m_TeamrankUuid;
};

struct CSqlTeamSaveData : ISqlData
{
	CSqlTeamSaveData(std::shared_ptr<CScoreSaveResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aClientName[MAX_NAME_LENGTH];
	char m_aMap[MAX_MAP_LENGTH];
	char m_aCode[128];
	char m_aGeneratedCode[128];
	char m_aServer[5];
};

struct CSqlTeamLoadRequest : ISqlData
{
	CSqlTeamLoadRequest(std::shared_ptr<CScoreSaveResult> pResult) :
		ISqlData(std::move(pResult))
	{
	}

	char m_aCode[128];
	char m_aMap[MAX_MAP_LENGTH];
	char m_aRequestingPlayer[MAX_NAME_LENGTH];
	// struct holding all player names in the team or an empty string
	char m_aClientNames[MAX_CLIENTS][MAX_NAME_LENGTH];
	int m_aClientId[MAX_CLIENTS];
	int m_NumPlayer;
};

class CPlayerData
{
public:
	CPlayerData()
	{
		Reset();
	}

	void Reset()
	{
		m_BestTime.reset();
		for(float &BestTimeCp : m_aBestTimeCp)
			BestTimeCp = 0;

		m_RecordStopTick = -1;
	}

	void Set(float Time, const float aTimeCp[NUM_CHECKPOINTS])
	{
		m_BestTime = Time;
		for(int i = 0; i < NUM_CHECKPOINTS; i++)
			m_aBestTimeCp[i] = aTimeCp[i];
	}

	void SetBestTimeCp(const float aTimeCp[NUM_CHECKPOINTS])
	{
		for(int i = 0; i < NUM_CHECKPOINTS; i++)
			m_aBestTimeCp[i] = aTimeCp[i];
	}

	std::optional<float> m_BestTime;
	float m_aBestTimeCp[NUM_CHECKPOINTS];

	int m_RecordStopTick;
	float m_RecordFinishTime;
};

struct CTeamrank
{
	CUuid m_TeamId;
	char m_aaNames[MAX_CLIENTS][MAX_NAME_LENGTH];
	unsigned int m_NumNames;
	CTeamrank();

	// Assumes that a database query equivalent to
	//
	//     SELECT TeamId, Name [, ...] -- the order is important
	//     FROM record_teamrace
	//     ORDER BY TeamId, Name
	//
	// was executed and that the result line of the first team member is already selected.
	// Afterwards the team member of the next team is selected.
	//
	// Returns true on SQL failure
	//
	// if another team can be extracted
	bool NextSqlResult(IDbConnection *pSqlServer, bool *pEnd, char *pError, int ErrorSize);

	bool SamePlayers(const std::vector<std::string> *pvSortedNames);

	static bool GetSqlTop5Team(IDbConnection *pSqlServer, bool *pEnd, char *pError, int ErrorSize, char (*paMessages)[512], int *StartLine, int Count);
};

struct CScoreWorker
{
	static bool LoadBestTime(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);

	static bool RandomMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool RandomUnfinishedMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool MapVote(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);

	static bool LoadPlayerData(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool LoadPlayerTimeCp(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool MapInfo(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowRank(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowTeamRank(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowTop(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowTeamTop5(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowPlayerTeamTop5(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowTimes(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowPoints(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ShowTopPoints(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool GetSaves(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool RegisterAccount(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool LoginAccount(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool ChangeAccountPassword(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool LoadAccountProfile(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool SaveAccountPlaytime(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool AddAccountDeath(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool AccountKillEvent(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool AddAccountReward(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool NewGlobalMail(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool LoadAccountMailbox(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool MailSetRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool MailMarkAllRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool MailClaimReward(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool MailClaimAllRewards(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool MailDelete(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool MailDeleteAllRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SetAccountLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SetAccountRole(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool AutoLoginLookup(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool SetAutoLogin(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool ShopBuyRainbow(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool CosmeticsSetRainbowActive(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool LoadAccountInventory(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool AccountInventoryBuy(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool AccountInventorySetEquipped(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool LoadIpLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool SetIpLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);

	static bool LoadFarmersForMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool AddFarmer(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);

	static bool AdminAddMoneyExp(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool AdminRemoveMoneyExp(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SetVip(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SetJetpack(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SaveNick(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool CheckNickProtected(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);
	static bool GetVipList(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize);

	static bool SaveTeam(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool LoadTeam(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);

	static bool SaveScore(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
	static bool SaveTeamScore(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize);
};

#endif // GAME_SERVER_SCOREWORKER_H
