#ifndef GAME_SERVER_SCORE_H
#define GAME_SERVER_SCORE_H

#include <string>
#include <vector>

#include "scoreworker.h"

#include <game/prng.h>

class CDbConnectionPool;
class CGameContext;
class IDbConnection;
class IServer;
struct ISqlData;

class CScore
{
	CPlayerData m_aPlayerData[MAX_CLIENTS];
	CDbConnectionPool *m_pPool;

	CGameContext *GameServer() const { return m_pGameServer; }
	IServer *Server() const { return m_pServer; }
	CGameContext *m_pGameServer;
	IServer *m_pServer;

	std::vector<std::string> m_vWordlist;
	CPrng m_Prng;
	void GeneratePassphrase(char *pBuf, int BufSize);

	// returns new SqlResult bound to the player, if no current Thread is active for this player
	std::shared_ptr<CScorePlayerResult> NewSqlPlayerResult(int ClientId);
	// Creates for player database requests
	void ExecPlayerThread(
		bool (*pFuncPtr)(IDbConnection *, const ISqlData *, char *pError, int ErrorSize),
		const char *pThreadName,
		int ClientId,
		const char *pName,
		int Offset);

	// returns true if the player should be rate limited
	bool RateLimitPlayer(int ClientId);

public:
	CScore(CGameContext *pGameServer, CDbConnectionPool *pPool);

	CPlayerData *PlayerData(int Id) { return &m_aPlayerData[Id]; }

	void LoadBestTime();
	void LoadMapInfo();
	void MapInfo(int ClientId, const char *pMapName);
	void MapVote(int ClientId, const char *pMapName);
	void LoadPlayerData(int ClientId, const char *pName = "");
	void LoadPlayerTimeCp(int ClientId, const char *pName = "");
	void SaveScore(int ClientId, int TimeTicks, const char *pTimestamp, const float aTimeCp[NUM_CHECKPOINTS], bool NotEligible);

	void SaveTeamScore(int Team, int *pClientIds, unsigned int Size, int TimeTicks, const char *pTimestamp);

	void ShowTop(int ClientId, int Offset = 1);
	void ShowRank(int ClientId, const char *pName);

	void ShowTeamTop5(int ClientId, int Offset = 1);
	void ShowPlayerTeamTop5(int ClientId, const char *pName, int Offset = 1);
	void ShowTeamRank(int ClientId, const char *pName);

	void ShowTopPoints(int ClientId, int Offset = 1);
	void ShowPoints(int ClientId, const char *pName);

	void ShowTimes(int ClientId, const char *pName, int Offset = 1);
	void ShowTimes(int ClientId, int Offset = 1);

	void RandomMap(int ClientId, int MinStars, int MaxStars);
	void RandomUnfinishedMap(int ClientId, int MinStars, int MaxStars);

	void SaveTeam(int ClientId, const char *pCode, const char *pServer);
	void LoadTeam(const char *pCode, int ClientId);
	void GetSaves(int ClientId);
	void RegisterAccount(int ClientId, const char *pUsername, const char *pPassword);
	void LoginAccount(int ClientId, const char *pUsername, const char *pPassword, const char *pClientAddrStr);
	void ChangeAccountPassword(int ClientId, int64_t AccountId, const char *pOldPassword, const char *pNewPassword);
	void LoadAccountProfile(int ClientId, int64_t AccountId);
	void SaveAccountPlaytime(int64_t AccountId, int64_t PlaytimeSeconds);
	void AddAccountDeath(int64_t AccountId);
	void AccountKillEvent(int64_t KillerAccountId, int64_t VictimAccountId, int RewardXP, int64_t RewardMoney);
	void AddAccountReward(int64_t AccountId, int AddXP, int64_t AddMoney);
	void SetAccountLanguage(int64_t AccountId, int Language);
	void SetAccountRole(const char *pUsername, int Role);
	void ShopBuyRainbow(int ClientId, int64_t AccountId);
	void CosmeticsSetRainbowActive(int64_t AccountId, int Active);
	void LoadAccountInventory(int ClientId, int64_t AccountId);
	void AccountInventoryBuy(int ClientId, int64_t AccountId, const char *pItemName, int GroupId, int64_t Price, int DurationDays);
	void AccountInventorySetEquipped(int ClientId, int64_t AccountId, const char *pItemName, int GroupId, bool Equipped);
	void LoadIpLanguage(int ClientId, const char *pClientAddrStr);
	void SetIpLanguage(const char *pClientAddrStr, int Language);
	void AutoLoginLookup(int ClientId, const char *pLastIgn, const char *pClientAddrStr);
	void SetAutoLogin(int64_t AccountId, int Value);
	void SaveNick(int ClientId, int64_t AccountId, const char *pNick);
	bool CheckNickProtected(int ClientId, const char *pNick);
	void NewGlobalMail(const char *pSubject, const char *pMessage, int RewardXP, int64_t RewardMoney);
	void LoadAccountMailbox(int ClientId, int64_t AccountId);
	void MailSetRead(int64_t AccountId, int64_t MailId, bool Unread);
	void MailMarkAllRead(int64_t AccountId);
	void MailClaimReward(int64_t AccountId, int64_t MailId);
	void MailClaimAllRewards(int64_t AccountId);
	void MailDelete(int64_t AccountId, int64_t MailId);
	void MailDeleteAllRead(int64_t AccountId);

	void LoadFarmersForMap(std::shared_ptr<CScoreFarmersResult> pResult, const char *pMap);
	void AddFarmer(const char *pMap, int Type, float X1, float Y1, float X2, float Y2, int64_t MoneyPerSec, int XPPerSec);

	void AdminAddMoney(const char *pUsername, int64_t Amount);
	void AdminRemoveMoney(const char *pUsername, int64_t Amount);
	void AdminAddExp(const char *pUsername, int Amount);
	void AdminRemoveExp(const char *pUsername, int Amount);
	void SetVip(const char *pUsername, int DurationDays);
	void SetJetpack(const char *pUsername, int DurationDays);
	void GetVipList(std::shared_ptr<CScoreVipListResult> pResult);
	void TransferMoney(int FromClientId, int ToClientId, int64_t Amount);
};

#endif // GAME_SERVER_SCORE_H
