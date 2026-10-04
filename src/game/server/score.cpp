#include "score.h"

#include "player.h"
#include "save.h"
#include "scoreworker.h"

#include <base/log.h>
#include <base/system.h>

#include <engine/server.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/shared/config.h>
#include <engine/shared/console.h>
#include <engine/shared/linereader.h>
#include <engine/storage.h>

#include <generated/wordlist.h>

#include <game/server/gamecontext.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/team_state.h>

#include <memory>

class IDbConnection;

std::shared_ptr<CScorePlayerResult> CScore::NewSqlPlayerResult(int ClientId)
{
	CPlayer *pCurPlayer = GameServer()->m_apPlayers[ClientId];
	if(pCurPlayer->m_ScoreQueryResult != nullptr) // TODO: send player a message: "too many requests"
	{
		log_info("account", "NewSqlPlayerResult: already in progress for cid=%d", ClientId);
		return nullptr;
	}
	pCurPlayer->m_ScoreQueryResult = std::make_shared<CScorePlayerResult>();
	return pCurPlayer->m_ScoreQueryResult;
}

void CScore::ExecPlayerThread(
	bool (*pFuncPtr)(IDbConnection *, const ISqlData *, char *pError, int ErrorSize),
	const char *pThreadName,
	int ClientId,
	const char *pName,
	int Offset)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlPlayerRequest>(pResult);
	str_copy(Tmp->m_aName, pName, sizeof(Tmp->m_aName));
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	str_copy(Tmp->m_aServer, g_Config.m_SvSqlServerName, sizeof(Tmp->m_aServer));
	str_copy(Tmp->m_aRequestingPlayer, Server()->ClientName(ClientId), sizeof(Tmp->m_aRequestingPlayer));
	Tmp->m_Offset = Offset;

	m_pPool->Execute(pFuncPtr, std::move(Tmp), pThreadName);
}

bool CScore::RateLimitPlayer(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer == nullptr)
		return true;
	if(pPlayer->m_LastSqlQuery + (int64_t)g_Config.m_SvSqlQueriesDelay * Server()->TickSpeed() >= Server()->Tick())
		return true;
	pPlayer->m_LastSqlQuery = Server()->Tick();
	return false;
}

void CScore::GeneratePassphrase(char *pBuf, int BufSize)
{
	for(int i = 0; i < 3; i++)
	{
		if(i != 0)
			str_append(pBuf, " ", BufSize);
		// TODO: decide if the slight bias towards lower numbers is ok
		int Rand = m_Prng.RandomBits() % m_vWordlist.size();
		str_append(pBuf, m_vWordlist[Rand].c_str(), BufSize);
	}
}

CScore::CScore(CGameContext *pGameServer, CDbConnectionPool *pPool) :
	m_pPool(pPool),
	m_pGameServer(pGameServer),
	m_pServer(pGameServer->Server())
{
	LoadBestTime();

	uint64_t aSeed[2];
	secure_random_fill(aSeed, sizeof(aSeed));
	m_Prng.Seed(aSeed);

	CLineReader LineReader;
	if(LineReader.OpenFile(GameServer()->Storage()->OpenFile("wordlist.txt", IOFLAG_READ, IStorage::TYPE_ALL)))
	{
		while(const char *pLine = LineReader.Get())
		{
			char aWord[32] = {0};
			sscanf(pLine, "%*s %31s", aWord);
			aWord[31] = 0;
			m_vWordlist.emplace_back(aWord);
		}
	}
	else
	{
		dbg_msg("sql", "failed to open wordlist, using fallback");
		m_vWordlist.assign(std::begin(g_aFallbackWordlist), std::end(g_aFallbackWordlist));
	}

	if(m_vWordlist.size() < 1000)
	{
		dbg_msg("sql", "too few words in wordlist");
		Server()->SetErrorShutdown("sql too few words in wordlist");
		return;
	}
}

void CScore::LoadBestTime()
{
	if(m_pGameServer->m_pController->m_pLoadBestTimeResult)
		return; // already in progress

	auto LoadBestTimeResult = std::make_shared<CScoreLoadBestTimeResult>();
	m_pGameServer->m_pController->m_pLoadBestTimeResult = LoadBestTimeResult;

	auto Tmp = std::make_unique<CSqlLoadBestTimeRequest>(LoadBestTimeResult);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	m_pPool->Execute(CScoreWorker::LoadBestTime, std::move(Tmp), "load best time");
}

void CScore::LoadMapInfo()
{
	if(m_pGameServer->m_pLoadMapInfoResult)
		return; // already in progress

	auto pResult = std::make_shared<CScorePlayerResult>();
	m_pGameServer->m_pLoadMapInfoResult = pResult;

	auto Tmp = std::make_unique<CSqlPlayerRequest>(pResult);
	str_copy(Tmp->m_aName, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aName));
	Tmp->m_aRequestingPlayer[0] = '\0'; // no player, so no "your time" in result
	m_pPool->Execute(CScoreWorker::MapInfo, std::move(Tmp), "load map info");
}

void CScore::LoadPlayerData(int ClientId, const char *pName)
{
	ExecPlayerThread(CScoreWorker::LoadPlayerData, "load player data", ClientId, pName, 0);
}

void CScore::RegisterAccount(int ClientId, const char *pUsername, const char *pPassword)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	log_info("account", "Schedule RegisterAccount cid=%d user='%s'", ClientId, pUsername);
	auto Tmp = std::make_unique<CSqlAccountRegisterRequest>(pResult);
	str_copy(Tmp->m_aUsername, pUsername, sizeof(Tmp->m_aUsername));
	str_copy(Tmp->m_aPassword, pPassword, sizeof(Tmp->m_aPassword));
	str_copy(Tmp->m_aLastIgn, Server()->ClientName(ClientId), sizeof(Tmp->m_aLastIgn));
	const char *pAddr = Server()->ClientAddrString(ClientId, false);
	str_copy(Tmp->m_aClientAddrStr, pAddr ? pAddr : "", sizeof(Tmp->m_aClientAddrStr));
	m_pPool->Execute(CScoreWorker::RegisterAccount, std::move(Tmp), "account register");
}

void CScore::LoginAccount(int ClientId, const char *pUsername, const char *pPassword, const char *pClientAddrStr)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	log_info("account", "Schedule LoginAccount cid=%d user='%s'", ClientId, pUsername);
	auto Tmp = std::make_unique<CSqlAccountLoginRequest>(pResult);
	str_copy(Tmp->m_aUsername, pUsername, sizeof(Tmp->m_aUsername));
	str_copy(Tmp->m_aPassword, pPassword, sizeof(Tmp->m_aPassword));
	str_copy(Tmp->m_aLastIgn, Server()->ClientName(ClientId), sizeof(Tmp->m_aLastIgn));
	str_copy(Tmp->m_aClientAddrStr, pClientAddrStr ? pClientAddrStr : "", sizeof(Tmp->m_aClientAddrStr));
	m_pPool->Execute(CScoreWorker::LoginAccount, std::move(Tmp), "account login");
}

void CScore::ChangeAccountPassword(int ClientId, int64_t AccountId, const char *pOldPassword, const char *pNewPassword)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlAccountChangePasswordRequest>(pResult);
	Tmp->m_AccountId = AccountId;
	str_copy(Tmp->m_aOldPassword, pOldPassword ? pOldPassword : "", sizeof(Tmp->m_aOldPassword));
	str_copy(Tmp->m_aNewPassword, pNewPassword ? pNewPassword : "", sizeof(Tmp->m_aNewPassword));
	m_pPool->Execute(CScoreWorker::ChangeAccountPassword, std::move(Tmp), "account change password");
}

void CScore::LoadIpLanguage(int ClientId, const char *pClientAddrStr)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlIpLanguageLoadRequest>(pResult);
	str_copy(Tmp->m_aClientAddrStr, pClientAddrStr ? pClientAddrStr : "", sizeof(Tmp->m_aClientAddrStr));
	m_pPool->Execute(CScoreWorker::LoadIpLanguage, std::move(Tmp), "ip language load");
}

void CScore::SetIpLanguage(const char *pClientAddrStr, int Language)
{
	auto Tmp = std::make_unique<CSqlIpLanguageSetRequest>();
	str_copy(Tmp->m_aClientAddrStr, pClientAddrStr ? pClientAddrStr : "", sizeof(Tmp->m_aClientAddrStr));
	Tmp->m_Language = Language;
	m_pPool->ExecuteWrite(CScoreWorker::SetIpLanguage, std::move(Tmp), "ip language set");
}

void CScore::AutoLoginLookup(int ClientId, const char *pLastIgn, const char *pClientAddrStr)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	log_info("account", "Schedule AutoLoginLookup cid=%d ign='%s'", ClientId, pLastIgn);
	auto Tmp = std::make_unique<CSqlAutoLoginRequest>(pResult);
	str_copy(Tmp->m_aLastIgn, pLastIgn ? pLastIgn : "", sizeof(Tmp->m_aLastIgn));
	str_copy(Tmp->m_aClientAddrStr, pClientAddrStr ? pClientAddrStr : "", sizeof(Tmp->m_aClientAddrStr));
	m_pPool->Execute(CScoreWorker::AutoLoginLookup, std::move(Tmp), "account autologin lookup");
}

void CScore::SetAutoLogin(int64_t AccountId, int Value)
{
	auto Tmp = std::make_unique<CSqlSetAutoLoginRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_AutoLogin = Value;
	m_pPool->ExecuteWrite(CScoreWorker::SetAutoLogin, std::move(Tmp), "account set autologin");
}

void CScore::SaveNick(int ClientId, int64_t AccountId, const char *pNick)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlSaveNickRequest>(pResult);
	Tmp->m_AccountId = AccountId;
	str_copy(Tmp->m_aNick, pNick ? pNick : "", sizeof(Tmp->m_aNick));
	m_pPool->Execute(CScoreWorker::SaveNick, std::move(Tmp), "account save nick");
}

bool CScore::CheckNickProtected(int ClientId, const char *pNick)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return false;
	auto Tmp = std::make_unique<CSqlNickProtectedLookupRequest>(pResult);
	str_copy(Tmp->m_aNick, pNick ? pNick : "", sizeof(Tmp->m_aNick));
	m_pPool->Execute(CScoreWorker::CheckNickProtected, std::move(Tmp), "account check protected nick");
	return true;
}

void CScore::ShopBuyRainbow(int ClientId, int64_t AccountId)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlShopBuyRainbowRequest>(pResult);
	Tmp->m_ClientId = ClientId;
	Tmp->m_AccountId = AccountId;
	m_pPool->Execute(CScoreWorker::ShopBuyRainbow, std::move(Tmp), "shop buy rainbow");
}

void CScore::CosmeticsSetRainbowActive(int64_t AccountId, int Active)
{
	auto Tmp = std::make_unique<CSqlCosmeticsSetRainbowActiveRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_Active = Active;
	m_pPool->ExecuteWrite(CScoreWorker::CosmeticsSetRainbowActive, std::move(Tmp), "cosmetics set rainbow active");
}

void CScore::LoadAccountInventory(int ClientId, int64_t AccountId)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlAccountInventoryLoadRequest>(pResult);
	Tmp->m_AccountId = AccountId;
	m_pPool->Execute(CScoreWorker::LoadAccountInventory, std::move(Tmp), "account inventory load");
}

void CScore::AccountInventoryBuy(int ClientId, int64_t AccountId, const char *pItemName, int GroupId, int64_t Price, int DurationDays)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlAccountInventoryBuyRequest>(pResult);
	Tmp->m_ClientId = ClientId;
	Tmp->m_AccountId = AccountId;
	str_copy(Tmp->m_aItemName, pItemName ? pItemName : "", sizeof(Tmp->m_aItemName));
	Tmp->m_GroupId = GroupId;
	Tmp->m_Price = Price;
	Tmp->m_DurationDays = DurationDays;
	m_pPool->Execute(CScoreWorker::AccountInventoryBuy, std::move(Tmp), "account inventory buy");
}

void CScore::AccountInventorySetEquipped(int ClientId, int64_t AccountId, const char *pItemName, int GroupId, bool Equipped)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlAccountInventorySetEquippedRequest>(pResult);
	Tmp->m_ClientId = ClientId;
	Tmp->m_AccountId = AccountId;
	str_copy(Tmp->m_aItemName, pItemName ? pItemName : "", sizeof(Tmp->m_aItemName));
	Tmp->m_GroupId = GroupId;
	Tmp->m_Equipped = Equipped ? 1 : 0;
	m_pPool->Execute(CScoreWorker::AccountInventorySetEquipped, std::move(Tmp), "account inventory set equipped");
}

void CScore::LoadAccountProfile(int ClientId, int64_t AccountId)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	log_info("account", "Schedule LoadAccountProfile cid=%d AccountId=%lld", ClientId, (long long)AccountId);
	auto Tmp = std::make_unique<CSqlAccountProfileRequest>(pResult);
	Tmp->m_AccountId = AccountId;
	m_pPool->Execute(CScoreWorker::LoadAccountProfile, std::move(Tmp), "account profile");
}

void CScore::SaveAccountPlaytime(int64_t AccountId, int64_t PlaytimeSeconds)
{
	auto Tmp = std::make_unique<CSqlAccountPlaytimeSaveRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_PlaytimeSeconds = PlaytimeSeconds;
	m_pPool->ExecuteWrite(CScoreWorker::SaveAccountPlaytime, std::move(Tmp), "account playtime save");
}

void CScore::AddAccountDeath(int64_t AccountId)
{
	auto Tmp = std::make_unique<CSqlAccountAddDeathRequest>();
	Tmp->m_AccountId = AccountId;
	m_pPool->ExecuteWrite(CScoreWorker::AddAccountDeath, std::move(Tmp), "account add death");
}

void CScore::AccountKillEvent(int64_t KillerAccountId, int64_t VictimAccountId, int RewardXP, int64_t RewardMoney)
{
	auto Tmp = std::make_unique<CSqlAccountKillEventRequest>();
	Tmp->m_KillerAccountId = KillerAccountId;
	Tmp->m_VictimAccountId = VictimAccountId;
	Tmp->m_RewardXP = RewardXP;
	Tmp->m_RewardMoney = RewardMoney;
	m_pPool->ExecuteWrite(CScoreWorker::AccountKillEvent, std::move(Tmp), "account kill event");
}

void CScore::AddAccountReward(int64_t AccountId, int AddXP, int64_t AddMoney)
{
	auto Tmp = std::make_unique<CSqlAccountAddRewardRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_AddXP = AddXP;
	Tmp->m_AddMoney = AddMoney;
	m_pPool->ExecuteWrite(CScoreWorker::AddAccountReward, std::move(Tmp), "account add reward");
}

void CScore::SetAccountLanguage(int64_t AccountId, int Language)
{
	auto Tmp = std::make_unique<CSqlAccountSetLanguageRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_Language = Language;
	m_pPool->ExecuteWrite(CScoreWorker::SetAccountLanguage, std::move(Tmp), "account set language");
}

void CScore::SetAccountRole(const char *pUsername, int Role)
{
	auto Tmp = std::make_unique<CSqlAccountSetRoleRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_Role = Role;
	m_pPool->ExecuteWrite(CScoreWorker::SetAccountRole, std::move(Tmp), "account set role");
}

void CScore::NewGlobalMail(const char *pSubject, const char *pMessage, int RewardXP, int64_t RewardMoney)
{
	auto Tmp = std::make_unique<CSqlAccountGlobalMailRequest>();
	str_copy(Tmp->m_aSubject, pSubject, sizeof(Tmp->m_aSubject));
	str_copy(Tmp->m_aMessage, pMessage, sizeof(Tmp->m_aMessage));
	Tmp->m_RewardXP = RewardXP;
	Tmp->m_RewardMoney = RewardMoney;
	// 30 days
	Tmp->m_ExpiresAt = time_timestamp() + 30LL * 24LL * 60LL * 60LL;
	m_pPool->ExecuteWrite(CScoreWorker::NewGlobalMail, std::move(Tmp), "account global mail");
}

void CScore::LoadAccountMailbox(int ClientId, int64_t AccountId)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlAccountMailboxLoadRequest>(pResult);
	Tmp->m_AccountId = AccountId;
	m_pPool->Execute(CScoreWorker::LoadAccountMailbox, std::move(Tmp), "account mailbox");
}

void CScore::MailSetRead(int64_t AccountId, int64_t MailId, bool Unread)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxSetReadRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = MailId;
	Tmp->m_Unread = Unread ? 1 : 0;
	m_pPool->ExecuteWrite(CScoreWorker::MailSetRead, std::move(Tmp), "account mail set read");
}

void CScore::MailMarkAllRead(int64_t AccountId)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxActionRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = 0;
	m_pPool->ExecuteWrite(CScoreWorker::MailMarkAllRead, std::move(Tmp), "account mail mark all read");
}

void CScore::MailClaimReward(int64_t AccountId, int64_t MailId)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxActionRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = MailId;
	m_pPool->ExecuteWrite(CScoreWorker::MailClaimReward, std::move(Tmp), "account mail claim reward");
}

void CScore::MailClaimAllRewards(int64_t AccountId)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxActionRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = 0;
	m_pPool->ExecuteWrite(CScoreWorker::MailClaimAllRewards, std::move(Tmp), "account mail claim all rewards");
}

void CScore::MailDelete(int64_t AccountId, int64_t MailId)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxActionRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = MailId;
	m_pPool->ExecuteWrite(CScoreWorker::MailDelete, std::move(Tmp), "account mail delete");
}

void CScore::MailDeleteAllRead(int64_t AccountId)
{
	auto Tmp = std::make_unique<CSqlAccountMailboxActionRequest>();
	Tmp->m_AccountId = AccountId;
	Tmp->m_MailId = 0;
	m_pPool->ExecuteWrite(CScoreWorker::MailDeleteAllRead, std::move(Tmp), "account mail delete all read");
}

void CScore::LoadFarmersForMap(std::shared_ptr<CScoreFarmersResult> pResult, const char *pMap)
{
	auto Tmp = std::make_unique<CSqlLoadFarmersRequest>(pResult);
	str_copy(Tmp->m_aMap, pMap, sizeof(Tmp->m_aMap));
	m_pPool->Execute(CScoreWorker::LoadFarmersForMap, std::move(Tmp), "load farmers");
}

void CScore::AddFarmer(const char *pMap, int Type, float X1, float Y1, float X2, float Y2, int64_t MoneyPerSec, int XPPerSec)
{
	auto Tmp = std::make_unique<CSqlAddFarmerRequest>();
	str_copy(Tmp->m_aMap, pMap, sizeof(Tmp->m_aMap));
	Tmp->m_Type = Type;
	Tmp->m_X1 = X1;
	Tmp->m_Y1 = Y1;
	Tmp->m_X2 = X2;
	Tmp->m_Y2 = Y2;
	Tmp->m_MoneyPerSec = MoneyPerSec;
	Tmp->m_XPPerSec = XPPerSec;
	m_pPool->ExecuteWrite(CScoreWorker::AddFarmer, std::move(Tmp), "add farmer");
}

void CScore::LoadPlayerTimeCp(int ClientId, const char *pName)
{
	ExecPlayerThread(CScoreWorker::LoadPlayerTimeCp, "load player timecp", ClientId, pName, 0);
}

void CScore::MapVote(int ClientId, const char *pMapName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::MapVote, "map vote", ClientId, pMapName, 0);
}

void CScore::MapInfo(int ClientId, const char *pMapName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::MapInfo, "map info", ClientId, pMapName, 0);
}

void CScore::SaveScore(int ClientId, int TimeTicks, const char *pTimestamp, const float aTimeCp[NUM_CHECKPOINTS], bool NotEligible)
{
	CConsole *pCon = (CConsole *)GameServer()->Console();
	if(pCon->Cheated() || NotEligible)
		return;

	GameServer()->TeehistorianRecordPlayerFinish(ClientId, TimeTicks);

	CPlayer *pCurPlayer = GameServer()->m_apPlayers[ClientId];
	if(pCurPlayer->m_ScoreFinishResult != nullptr)
		dbg_msg("sql", "WARNING: previous save score result didn't complete, overwriting it now");
	pCurPlayer->m_ScoreFinishResult = std::make_shared<CScorePlayerResult>();
	auto Tmp = std::make_unique<CSqlScoreData>(pCurPlayer->m_ScoreFinishResult);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	FormatUuid(GameServer()->GameUuid(), Tmp->m_aGameUuid, sizeof(Tmp->m_aGameUuid));
	Tmp->m_ClientId = ClientId;
	str_copy(Tmp->m_aName, Server()->ClientName(ClientId), sizeof(Tmp->m_aName));
	Tmp->m_Time = (float)(TimeTicks) / (float)Server()->TickSpeed();
	str_copy(Tmp->m_aTimestamp, pTimestamp, sizeof(Tmp->m_aTimestamp));
	for(int i = 0; i < NUM_CHECKPOINTS; i++)
		Tmp->m_aCurrentTimeCp[i] = aTimeCp[i];

	m_pPool->ExecuteWrite(CScoreWorker::SaveScore, std::move(Tmp), "save score");
}

void CScore::SaveTeamScore(int Team, int *pClientIds, unsigned int Size, int TimeTicks, const char *pTimestamp)
{
	CConsole *pCon = (CConsole *)GameServer()->Console();
	if(pCon->Cheated())
		return;
	for(unsigned int i = 0; i < Size; i++)
	{
		if(GameServer()->m_apPlayers[pClientIds[i]]->m_NotEligibleForFinish)
			return;
	}

	GameServer()->TeehistorianRecordTeamFinish(Team, TimeTicks);

	auto Tmp = std::make_unique<CSqlTeamScoreData>();
	for(unsigned int i = 0; i < Size; i++)
		str_copy(Tmp->m_aaNames[i], Server()->ClientName(pClientIds[i]), sizeof(Tmp->m_aaNames[i]));
	Tmp->m_Size = Size;
	Tmp->m_Time = (float)TimeTicks / (float)Server()->TickSpeed();
	str_copy(Tmp->m_aTimestamp, pTimestamp, sizeof(Tmp->m_aTimestamp));
	FormatUuid(GameServer()->GameUuid(), Tmp->m_aGameUuid, sizeof(Tmp->m_aGameUuid));
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	Tmp->m_TeamrankUuid = RandomUuid();

	m_pPool->ExecuteWrite(CScoreWorker::SaveTeamScore, std::move(Tmp), "save team score");
}

void CScore::ShowRank(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowRank, "show rank", ClientId, pName, 0);
}

void CScore::ShowTeamRank(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTeamRank, "show team rank", ClientId, pName, 0);
}

void CScore::ShowTop(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTop, "show top5", ClientId, "", Offset);
}

void CScore::ShowTeamTop5(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTeamTop5, "show team top5", ClientId, "", Offset);
}

void CScore::ShowPlayerTeamTop5(int ClientId, const char *pName, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowPlayerTeamTop5, "show team top5 player", ClientId, pName, Offset);
}

void CScore::ShowTimes(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTimes, "show times", ClientId, "", Offset);
}

void CScore::ShowTimes(int ClientId, const char *pName, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTimes, "show times", ClientId, pName, Offset);
}

void CScore::ShowPoints(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowPoints, "show points", ClientId, pName, 0);
}

void CScore::ShowTopPoints(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTopPoints, "show top points", ClientId, "", Offset);
}

void CScore::RandomMap(int ClientId, int MinStars, int MaxStars)
{
	auto pResult = std::make_shared<CScoreRandomMapResult>(ClientId);
	GameServer()->m_SqlRandomMapResult = pResult;

	auto Tmp = std::make_unique<CSqlRandomMapRequest>(pResult);
	Tmp->m_MinStars = MinStars;
	Tmp->m_MaxStars = MaxStars;
	str_copy(Tmp->m_aCurrentMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aCurrentMap));
	str_copy(Tmp->m_aServerType, g_Config.m_SvServerType, sizeof(Tmp->m_aServerType));
	str_copy(Tmp->m_aRequestingPlayer, ClientId == -1 ? "nameless tee" : GameServer()->Server()->ClientName(ClientId), sizeof(Tmp->m_aRequestingPlayer));

	m_pPool->Execute(CScoreWorker::RandomMap, std::move(Tmp), "random map");
}

void CScore::RandomUnfinishedMap(int ClientId, int MinStars, int MaxStars)
{
	auto pResult = std::make_shared<CScoreRandomMapResult>(ClientId);
	GameServer()->m_SqlRandomMapResult = pResult;

	auto Tmp = std::make_unique<CSqlRandomMapRequest>(pResult);
	Tmp->m_MinStars = MinStars;
	Tmp->m_MaxStars = MaxStars;
	str_copy(Tmp->m_aCurrentMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aCurrentMap));
	str_copy(Tmp->m_aServerType, g_Config.m_SvServerType, sizeof(Tmp->m_aServerType));
	str_copy(Tmp->m_aRequestingPlayer, ClientId == -1 ? "nameless tee" : GameServer()->Server()->ClientName(ClientId), sizeof(Tmp->m_aRequestingPlayer));

	m_pPool->Execute(CScoreWorker::RandomUnfinishedMap, std::move(Tmp), "random unfinished map");
}

void CScore::SaveTeam(int ClientId, const char *pCode, const char *pServer)
{
	if(RateLimitPlayer(ClientId))
		return;
	auto *pController = GameServer()->m_pController;
	int Team = pController->Teams().m_Core.Team(ClientId);
	if(pController->Teams().GetSaving(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team save already in progress");
		return;
	}
	if(pController->Teams().IsPractice(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team save disabled for teams in practice mode");
		return;
	}

	auto SaveResult = std::make_shared<CScoreSaveResult>(ClientId, Server()->ClientName(ClientId), pServer);
	SaveResult->m_SaveId = RandomUuid();
	ESaveResult Result = SaveResult->m_SavedTeam.Save(GameServer(), Team);
	if(CSaveTeam::HandleSaveError(Result, ClientId, GameServer()))
		return;
	pController->Teams().SetSaving(Team, SaveResult);

	auto Tmp = std::make_unique<CSqlTeamSaveData>(SaveResult);
	str_copy(Tmp->m_aCode, pCode, sizeof(Tmp->m_aCode));
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	str_copy(Tmp->m_aServer, pServer, sizeof(Tmp->m_aServer));
	str_copy(Tmp->m_aClientName, this->Server()->ClientName(ClientId), sizeof(Tmp->m_aClientName));
	Tmp->m_aGeneratedCode[0] = '\0';
	GeneratePassphrase(Tmp->m_aGeneratedCode, sizeof(Tmp->m_aGeneratedCode));

	pController->Teams().KillCharacterOrTeam(ClientId, Team);

	GameServer()->SendSaveCode(
		Team,
		SaveResult->m_SavedTeam.GetMembersCount(),
		SAVESTATE_PENDING,
		"",
		SaveResult->m_aRequestingPlayer,
		Tmp->m_aServer,
		Tmp->m_aGeneratedCode,
		Tmp->m_aCode);

	m_pPool->ExecuteWrite(CScoreWorker::SaveTeam, std::move(Tmp), "save team");
}

void CScore::LoadTeam(const char *pCode, int ClientId)
{
	if(RateLimitPlayer(ClientId))
		return;
	auto *pController = GameServer()->m_pController;
	int Team = pController->Teams().m_Core.Team(ClientId);
	if(pController->Teams().GetSaving(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team load already in progress");
		return;
	}
	if(!pController->Teams().IsValidTeamNumber(Team) || (g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && Team == TEAM_FLOCK))
	{
		GameServer()->SendChatTarget(ClientId, "You have to be in a team (from 1-63)");
		return;
	}
	if(pController->Teams().GetTeamState(Team) != ETeamState::OPEN)
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while racing");
		return;
	}
	if(pController->Teams().TeamFlock(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while in team 0 mode");
		return;
	}
	if(pController->Teams().IsPractice(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while practice is enabled");
		return;
	}
	auto SaveResult = std::make_shared<CScoreSaveResult>(ClientId, Server()->ClientName(ClientId), g_Config.m_SvSqlServerName);
	SaveResult->m_Status = CScoreSaveResult::LOAD_FAILED;
	pController->Teams().SetSaving(Team, SaveResult);
	auto Tmp = std::make_unique<CSqlTeamLoadRequest>(SaveResult);
	str_copy(Tmp->m_aCode, pCode, sizeof(Tmp->m_aCode));
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName(), sizeof(Tmp->m_aMap));
	str_copy(Tmp->m_aRequestingPlayer, Server()->ClientName(ClientId), sizeof(Tmp->m_aRequestingPlayer));
	Tmp->m_NumPlayer = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(pController->Teams().m_Core.Team(i) == Team)
		{
			// put all names at the beginning of the array
			str_copy(Tmp->m_aClientNames[Tmp->m_NumPlayer], Server()->ClientName(i), sizeof(Tmp->m_aClientNames[Tmp->m_NumPlayer]));
			Tmp->m_aClientId[Tmp->m_NumPlayer] = i;
			Tmp->m_NumPlayer++;
		}
	}
	m_pPool->ExecuteWrite(CScoreWorker::LoadTeam, std::move(Tmp), "load team");
}

void CScore::GetSaves(int ClientId)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::GetSaves, "get saves", ClientId, "", 0);
}

void CScore::AdminAddMoney(const char *pUsername, int64_t Amount)
{
	auto Tmp = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_Money = Amount;
	Tmp->m_XP = 0;
	m_pPool->ExecuteWrite(CScoreWorker::AdminAddMoneyExp, std::move(Tmp), "admin add money");
}

void CScore::AdminRemoveMoney(const char *pUsername, int64_t Amount)
{
	auto Tmp = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_Money = Amount;
	Tmp->m_XP = 0;
	m_pPool->ExecuteWrite(CScoreWorker::AdminRemoveMoneyExp, std::move(Tmp), "admin remove money");
}

void CScore::AdminAddExp(const char *pUsername, int Amount)
{
	auto Tmp = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_Money = 0;
	Tmp->m_XP = Amount;
	m_pPool->ExecuteWrite(CScoreWorker::AdminAddMoneyExp, std::move(Tmp), "admin add exp");
}

void CScore::AdminRemoveExp(const char *pUsername, int Amount)
{
	auto Tmp = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_Money = 0;
	Tmp->m_XP = Amount;
	m_pPool->ExecuteWrite(CScoreWorker::AdminRemoveMoneyExp, std::move(Tmp), "admin remove exp");
}

void CScore::SetVip(const char *pUsername, int DurationDays)
{
	auto Tmp = std::make_unique<CSqlSetVipRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_DurationDays = DurationDays;
	m_pPool->ExecuteWrite(CScoreWorker::SetVip, std::move(Tmp), "set vip");
}

void CScore::SetJetpack(const char *pUsername, int DurationDays)
{
	auto Tmp = std::make_unique<CSqlSetJetpackRequest>();
	str_copy(Tmp->m_aUsername, pUsername ? pUsername : "", sizeof(Tmp->m_aUsername));
	Tmp->m_DurationDays = DurationDays;
	m_pPool->ExecuteWrite(CScoreWorker::SetJetpack, std::move(Tmp), "set jetpack");
}

void CScore::GetVipList(std::shared_ptr<CScoreVipListResult> pResult)
{
	auto Tmp = std::make_unique<CSqlVipListRequest>(pResult);
	m_pPool->Execute(CScoreWorker::GetVipList, std::move(Tmp), "get vip list");
}

void CScore::TransferMoney(int FromClientId, int ToClientId, int64_t Amount)
{
	CPlayer *pFrom = GameServer()->m_apPlayers[FromClientId];
	CPlayer *pTo = GameServer()->m_apPlayers[ToClientId];
	if(!pFrom || !pTo)
		return;
	if(!pFrom->m_AccountLoggedIn || !pTo->m_AccountLoggedIn)
		return;
	if(pFrom->m_AccountMoney < Amount)
		return;

	// Deduct from sender in-memory immediately
	pFrom->m_AccountMoney -= Amount;
	pTo->m_AccountMoney += Amount;

	// Persist both changes
	auto TmpDeduct = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(TmpDeduct->m_aUsername, pFrom->m_aAccountUsername, sizeof(TmpDeduct->m_aUsername));
	TmpDeduct->m_Money = Amount;
	TmpDeduct->m_XP = 0;
	m_pPool->ExecuteWrite(CScoreWorker::AdminRemoveMoneyExp, std::move(TmpDeduct), "transfer money deduct");

	auto TmpAdd = std::make_unique<CSqlAdminMoneyExpRequest>();
	str_copy(TmpAdd->m_aUsername, pTo->m_aAccountUsername, sizeof(TmpAdd->m_aUsername));
	TmpAdd->m_Money = Amount;
	TmpAdd->m_XP = 0;
	m_pPool->ExecuteWrite(CScoreWorker::AdminAddMoneyExp, std::move(TmpAdd), "transfer money add");
}
