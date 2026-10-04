#include "scoreworker.h"

#include <base/log.h>
#include <base/str.h>
#include <base/system.h>

#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/sql_string_helpers.h>
#include <engine/shared/config.h>

#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/system.h>

#include <cmath>

// "6b407e81-8b77-3e04-a207-8da17f37d000"
// "save-no-save-id@ddnet.tw"
static const CUuid UUID_NO_SAVE_ID =
	{{0x6b, 0x40, 0x7e, 0x81, 0x8b, 0x77, 0x3e, 0x04,
		0xa2, 0x07, 0x8d, 0xa1, 0x7f, 0x37, 0xd0, 0x00}};

CScorePlayerResult::CScorePlayerResult()
{
	SetVariant(Variant::DIRECT);
}

static void HashPasswordSha256(const char *pPassword, const unsigned char *pSalt, int SaltLen, unsigned char *pOut, int OutLen)
{
	SHA256_CTX Ctx;
	sha256_init(&Ctx);
	sha256_update(&Ctx, pPassword, str_length(pPassword));
	sha256_update(&Ctx, pSalt, SaltLen);
	SHA256_DIGEST Digest = sha256_finish(&Ctx);
	mem_copy(pOut, Digest.data, minimum<int>(OutLen, (int)sizeof(Digest.data)));
}

static int AccountLevelXPToNext(int Level)
{
	if(Level < 1)
		Level = 1;
	const double Base = 800.0;
	double Need = Base;
	for(int Lvl = 2; Lvl <= Level; Lvl++)
	{
		const double Mult = maximum(1.01, 1.10 - 0.0009 * (Lvl - 1));
		Need *= Mult;
	}
	return maximum(1, (int)std::ceil(Need));
}

static int AccountLevelFromXP(int XP)
{
	int Level = 1;
	int Remaining = maximum(0, XP);
	for(;;)
	{
		const int Need = AccountLevelXPToNext(Level);
		if(Remaining < Need)
			break;
		Remaining -= Need;
		Level++;
		if(Level > 100000)
			break;
	}
	return Level;
}

static int AccountLegacyLevelFromXP(int XP)
{
	int Level = 1;
	int Remaining = maximum(0, XP);
	for(;;)
	{
		const int Need = maximum(1, (int)std::ceil(150.0 * std::pow(1.15, (double)(Level - 1))));
		if(Remaining < Need)
			break;
		Remaining -= Need;
		Level++;
		if(Level > 100000)
			break;
	}
	return Level;
}

static int64_t AccountXPForLevel(int Level)
{
	int64_t Total = 0;
	for(int L = 1; L < maximum(1, Level); L++)
		Total += AccountLevelXPToNext(L);
	return Total;
}

bool CScoreWorker::RegisterAccount(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountRegisterRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_REGISTER);

	log_info("account", "RegisterAccount: user='%s'", pData->m_aUsername);

	// Check existing username
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "SELECT Id FROM %s_accounts WHERE Username = ? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount PrepareStatement failed: %s", pError);
		return false;
	}
	pSqlServer->BindString(1, pData->m_aUsername);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount Step failed: %s", pError);
		return false;
	}
	if(!End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_USERNAME_EXISTS", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	unsigned char aSalt[16];
	secure_random_fill(aSalt, sizeof(aSalt));
	unsigned char aHash[32];
	HashPasswordSha256(pData->m_aPassword, aSalt, sizeof(aSalt), aHash, sizeof(aHash));

	// Insert account
	str_format(aBuf, sizeof(aBuf), "INSERT INTO %s_accounts (Username, PwHash, PwSalt, LastIgn, LastIP) VALUES (?, ?, ?, ?, ?)", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount PrepareStatement insert failed: %s", pError);
		return false;
	}
	pSqlServer->BindString(1, pData->m_aUsername);
	pSqlServer->BindBlob(2, aHash, sizeof(aHash));
	pSqlServer->BindBlob(3, aSalt, sizeof(aSalt));
	pSqlServer->BindString(4, pData->m_aLastIgn);
	pSqlServer->BindString(5, pData->m_aClientAddrStr);
	int NumUpdated;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount ExecuteUpdate insert failed: %s", pError);
		return false;
	}

	// Get inserted id
	str_format(aBuf, sizeof(aBuf), "SELECT Id FROM %s_accounts WHERE Username = ? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aUsername);
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
		return false;
	int64_t AccountId = pSqlServer->GetInt64(1);

	// Insert stats defaults
	str_format(aBuf, sizeof(aBuf), "INSERT INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount PrepareStatement stats insert failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Insert settings defaults (language = EN, hint not shown, not manual)
	str_format(aBuf, sizeof(aBuf), "INSERT INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) VALUES (?, 0, 0, 0, 0, 1)", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "RegisterAccount PrepareStatement settings insert failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_copy(pResult->m_Data.m_aaMessages[0], "Registered successfully. Now use /login", sizeof(pResult->m_Data.m_aaMessages[0]));
	return true;
}

bool CScoreWorker::AddAccountReward(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountAddRewardRequest *>(pGameData);
	(void)w;
	char aBuf[256];
	int NumUpdated = 0;

	// Ensure stats row exists
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Increment XP and Money
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET XP=XP+?, Money=Money+? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_AddXP);
	pSqlServer->BindInt64(2, pData->m_AddMoney);
	pSqlServer->BindInt64(3, pData->m_AccountId);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::LoadAccountInventory(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountInventoryLoadRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	char aSql[512];
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_INVENTORY);
	pResult->m_Data.m_Inventory.m_NumItems = 0;
	const int64_t Now = time_timestamp();

	// Best-effort schema upgrade for timed cosmetics.
	{
		char aAlter1[256], aAlter2[256];
		str_format(aAlter1, sizeof(aAlter1), "ALTER TABLE %s_account_inventory ADD COLUMN FirstBoughtAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		str_format(aAlter2, sizeof(aAlter2), "ALTER TABLE %s_account_inventory ADD COLUMN ExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		int Dummy = 0;
		char aErr[256];
		if(pSqlServer->PrepareStatement(aAlter1, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
		if(pSqlServer->PrepareStatement(aAlter2, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
	}

	// Remove expired entries and backfill FirstBoughtAt if missing.
	str_format(aSql, sizeof(aSql), "DELETE FROM %s_account_inventory WHERE AccountId=? AND ExpiresAt>0 AND ExpiresAt<=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	int Dummy = 0;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, Now);
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	str_format(aSql, sizeof(aSql), "UPDATE %s_account_inventory SET FirstBoughtAt=? WHERE AccountId=? AND FirstBoughtAt<=0", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, Now);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	// Backfill old "permanent" cosmetics to timed 30-day entries.
	// Weapon shop entries stay one-time.
	const int64_t DefaultExpire = Now + 30LL * 86400LL;
	str_format(aSql, sizeof(aSql),
		"UPDATE %s_account_inventory SET ExpiresAt=? "
		"WHERE AccountId=? AND ExpiresAt<=0 AND LOWER(ItemName) NOT IN ('laser','shotgun','grenade')",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, DefaultExpire);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	str_format(aSql, sizeof(aSql),
		"SELECT ItemName, GroupId, Equipped, ExpiresAt FROM %s_account_inventory "
		"WHERE AccountId=? AND (ExpiresAt<=0 OR ExpiresAt>?) ORDER BY ItemName ASC",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, Now);

	bool End;
	while(true)
	{
		if(!pSqlServer->Step(&End, pError, ErrorSize))
			return false;
		if(End)
			break;
		if(pResult->m_Data.m_Inventory.m_NumItems >= (int)std::size(pResult->m_Data.m_Inventory.m_aItems))
			break;
		auto &It = pResult->m_Data.m_Inventory.m_aItems[pResult->m_Data.m_Inventory.m_NumItems++];
		pSqlServer->GetString(1, It.m_aItemName, sizeof(It.m_aItemName));
		It.m_GroupId = pSqlServer->GetInt(2);
		It.m_Equipped = pSqlServer->GetInt(3);
		It.m_ExpiresAt = pSqlServer->GetInt64(4);
	}
	return true;
}

bool CScoreWorker::AccountInventoryBuy(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountInventoryBuyRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;

	pResult->SetVariant(CScorePlayerResult::DIRECT);
	char aSql[512];
	int NumUpdated = 0;
	const int64_t Now = time_timestamp();
	const int64_t ExpiresAt = pData->m_DurationDays > 0 ? (Now + (int64_t)pData->m_DurationDays * 86400LL) : 0;

	// ensure stats row exists
	str_format(aSql, sizeof(aSql), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Best-effort schema upgrade for timed cosmetics.
	{
		char aAlter1[256], aAlter2[256];
		str_format(aAlter1, sizeof(aAlter1), "ALTER TABLE %s_account_inventory ADD COLUMN FirstBoughtAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		str_format(aAlter2, sizeof(aAlter2), "ALTER TABLE %s_account_inventory ADD COLUMN ExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		int Dummy = 0;
		char aErr[256];
		if(pSqlServer->PrepareStatement(aAlter1, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
		if(pSqlServer->PrepareStatement(aAlter2, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
	}

	// try to insert inventory row (if already owned -> report)
	str_format(aSql, sizeof(aSql), "%s INTO %s_account_inventory (AccountId, ItemName, GroupId, Equipped, FirstBoughtAt, ExpiresAt) VALUES (?, ?, ?, 0, ?, ?)",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindString(2, pData->m_aItemName);
	pSqlServer->BindInt(3, pData->m_GroupId);
	pSqlServer->BindInt64(4, Now);
	pSqlServer->BindInt64(5, ExpiresAt);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;
	if(NumUpdated == 0)
	{
		// If item exists but expired, renew it.
		str_format(aSql, sizeof(aSql),
			"UPDATE %s_account_inventory SET Equipped=0, "
			"FirstBoughtAt=CASE WHEN FirstBoughtAt<=0 THEN ? ELSE FirstBoughtAt END, ExpiresAt=? "
			"WHERE AccountId=? AND ItemName=? AND ExpiresAt>0 AND ExpiresAt<=?",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
			return false;
		pSqlServer->BindInt64(1, Now);
		pSqlServer->BindInt64(2, ExpiresAt);
		pSqlServer->BindInt64(3, pData->m_AccountId);
		pSqlServer->BindString(4, pData->m_aItemName);
		pSqlServer->BindInt64(5, Now);
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
			return false;
		if(NumUpdated == 0)
		{
			str_copy(pResult->m_Data.m_aaMessages[0], "INV_OWNED", sizeof(pResult->m_Data.m_aaMessages[0]));
			return true;
		}
	}

	// deduct money only if enough
	str_format(aSql, sizeof(aSql), "UPDATE %s_account_stats SET Money=Money-? WHERE AccountId=? AND Money>=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_Price);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	pSqlServer->BindInt64(3, pData->m_Price);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;
	if(NumUpdated == 0)
	{
		// rollback ownership insert
		str_format(aSql, sizeof(aSql), "DELETE FROM %s_account_inventory WHERE AccountId=? AND ItemName=?", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
			return false;
		pSqlServer->BindInt64(1, pData->m_AccountId);
		pSqlServer->BindString(2, pData->m_aItemName);
		pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
		str_copy(pResult->m_Data.m_aaMessages[0], "INV_NO_MONEY", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	// query new money
	int64_t Money = 0;
	bool End;
	str_format(aSql, sizeof(aSql), "SELECT Money FROM %s_account_stats WHERE AccountId=? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(!End)
		Money = pSqlServer->GetInt64(1);

	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "INV_BUY_OK %lld", (long long)Money);
	return true;
}

bool CScoreWorker::AccountInventorySetEquipped(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountInventorySetEquippedRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;

	pResult->SetVariant(CScorePlayerResult::DIRECT);
	char aSql[512];
	int NumUpdated = 0;
	{
		char aAlter[256];
		str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_inventory ADD COLUMN ExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		int Dummy = 0;
		char aErr[256];
		if(pSqlServer->PrepareStatement(aAlter, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
	}

	// verify ownership exists
	bool End;
	str_format(aSql, sizeof(aSql), "SELECT 1 FROM %s_account_inventory WHERE AccountId=? AND ItemName=? AND (ExpiresAt<=0 OR ExpiresAt>?) LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	const int64_t Now = time_timestamp();
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindString(2, pData->m_aItemName);
	pSqlServer->BindInt64(3, Now);
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "INV_NOT_OWNED", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	// If equipping, unequip other items in same group
	if(pData->m_Equipped != 0 && pData->m_GroupId != 0)
	{
		str_format(aSql, sizeof(aSql), "UPDATE %s_account_inventory SET Equipped=0 WHERE AccountId=? AND GroupId=?", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
			return false;
		pSqlServer->BindInt64(1, pData->m_AccountId);
		pSqlServer->BindInt(2, pData->m_GroupId);
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
			return false;
	}

	str_format(aSql, sizeof(aSql), "UPDATE %s_account_inventory SET Equipped=? WHERE AccountId=? AND ItemName=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Equipped);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	pSqlServer->BindString(3, pData->m_aItemName);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "INV_EQUIP_OK %d", pData->m_Equipped);
	return true;
}

bool CScoreWorker::LoadFarmersForMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlLoadFarmersRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScoreFarmersResult *>(pGameData->m_pResult.get());

	pResult->m_NumFarmers = 0;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT FarmerId, Type, X1, Y1, X2, Y2, MoneyPerSec, XPPerSec "
		"FROM %s_farmers WHERE Map=? ORDER BY FarmerId ASC",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aMap);

	bool End;
	while(true)
	{
		if(!pSqlServer->Step(&End, pError, ErrorSize))
			return false;
		if(End)
			break;
		if(pResult->m_NumFarmers >= CScoreFarmersResult::MAX_FARMERS)
			break;
		auto &F = pResult->m_aFarmers[pResult->m_NumFarmers++];
		F.m_FarmerId = pSqlServer->GetInt64(1);
		F.m_Type = pSqlServer->GetInt(2);
		F.m_X1 = pSqlServer->GetFloat(3);
		F.m_Y1 = pSqlServer->GetFloat(4);
		F.m_X2 = pSqlServer->GetFloat(5);
		F.m_Y2 = pSqlServer->GetFloat(6);
		F.m_MoneyPerSec = pSqlServer->GetInt64(7);
		F.m_XPPerSec = pSqlServer->GetInt(8);
	}
	return true;
}

bool CScoreWorker::AddFarmer(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAddFarmerRequest *>(pGameData);
	(void)w;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"INSERT INTO %s_farmers (Map, Type, X1, Y1, X2, Y2, MoneyPerSec, XPPerSec, CreatedAt) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	int Param = 1;
	pSqlServer->BindString(Param++, pData->m_aMap);
	pSqlServer->BindInt(Param++, pData->m_Type);
	pSqlServer->BindFloat(Param++, pData->m_X1);
	pSqlServer->BindFloat(Param++, pData->m_Y1);
	pSqlServer->BindFloat(Param++, pData->m_X2);
	pSqlServer->BindFloat(Param++, pData->m_Y2);
	pSqlServer->BindInt64(Param++, pData->m_MoneyPerSec);
	pSqlServer->BindInt(Param++, pData->m_XPPerSec);
	pSqlServer->BindInt64(Param++, time_timestamp());
	int NumInserted = 0;
	return pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize);
}

static bool PurgeExpiredMailboxMails(IDbConnection *pSqlServer, int64_t Now, char *pError, int ErrorSize)
{
	char aPurge[256];
	str_format(aPurge, sizeof(aPurge), "DELETE FROM %s_account_mailbox WHERE ExpiresAt < ?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aPurge, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, Now);
	int NumDeleted = 0;
	return pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize);
}

bool CScoreWorker::LoadAccountMailbox(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxLoadRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_MAILBOX);

	const int64_t Now = time_timestamp();
	if(!PurgeExpiredMailboxMails(pSqlServer, Now, pError, ErrorSize))
		return false;

	char aSql[512];
	str_format(aSql, sizeof(aSql),
		"SELECT MailId, Subject, Message, Unread, RewardXP, RewardMoney, RewardClaimed "
		"FROM %s_account_mailbox WHERE AccountId = ? AND ExpiresAt >= ? "
		"ORDER BY MailId DESC LIMIT 32",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, Now);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	int Count = 0;
	while(!End && Count < 32)
	{
		auto &M = pResult->m_Data.m_Mailbox.m_aMails[Count];
		M.m_MailId = pSqlServer->GetInt64(1);
		pSqlServer->GetString(2, M.m_aSubject, sizeof(M.m_aSubject));
		pSqlServer->GetString(3, M.m_aMessage, sizeof(M.m_aMessage));
		M.m_Unread = pSqlServer->GetInt(4);
		M.m_RewardXP = pSqlServer->GetInt(5);
		M.m_RewardMoney = pSqlServer->GetInt64(6);
		M.m_RewardClaimed = pSqlServer->GetInt(7);
		Count++;
		if(!pSqlServer->Step(&End, pError, ErrorSize))
			return false;
	}
	pResult->m_Data.m_Mailbox.m_NumMails = Count;
	return true;
}

bool CScoreWorker::MailMarkAllRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxActionRequest *>(pGameData);
	(void)w;
	char aSql[256];
	str_format(aSql, sizeof(aSql), "UPDATE %s_account_mailbox SET Unread=0 WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::MailSetRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxSetReadRequest *>(pGameData);
	(void)w;
	char aSql[256];
	str_format(aSql, sizeof(aSql), "UPDATE %s_account_mailbox SET Unread=? WHERE AccountId=? AND MailId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Unread);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	pSqlServer->BindInt64(3, pData->m_MailId);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::MailClaimReward(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxActionRequest *>(pGameData);
	(void)w;
	const int64_t Now = time_timestamp();
	if(!PurgeExpiredMailboxMails(pSqlServer, Now, pError, ErrorSize))
		return false;

	// Read reward from mail (only if not claimed)
	char aSel[512];
	str_format(aSel, sizeof(aSel),
		"SELECT m.RewardXP, m.RewardMoney, COALESCE(st.Role,0) FROM %s_account_mailbox m "
		"LEFT JOIN %s_account_settings st ON st.AccountId=m.AccountId "
		"WHERE m.AccountId=? AND m.MailId=? AND m.RewardClaimed=0 AND (m.RewardXP<>0 OR m.RewardMoney<>0) LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSel, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, pData->m_MailId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
		return true;
	int RewardXP = pSqlServer->GetInt(1);
	int64_t RewardMoney = pSqlServer->GetInt64(2);
	const int Role = pSqlServer->GetInt(3);
	if(Role == 1)
	{
		RewardXP *= 2;
		RewardMoney *= 2;
	}

	// Apply reward
	char aUpdStats[256];
	str_format(aUpdStats, sizeof(aUpdStats),
		"UPDATE %s_account_stats SET XP=XP+?, Money=Money+? WHERE AccountId=?",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aUpdStats, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, RewardXP);
	pSqlServer->BindInt64(2, RewardMoney);
	pSqlServer->BindInt64(3, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Mark claimed
	char aUpdMail[256];
	str_format(aUpdMail, sizeof(aUpdMail),
		"UPDATE %s_account_mailbox SET RewardClaimed=1, Unread=0 WHERE AccountId=? AND MailId=?",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aUpdMail, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, pData->m_MailId);
	NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::MailClaimAllRewards(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxActionRequest *>(pGameData);
	(void)w;
	const int64_t Now = time_timestamp();
	if(!PurgeExpiredMailboxMails(pSqlServer, Now, pError, ErrorSize))
		return false;

	// Sum unclaimed rewards
	char aSel[512];
	str_format(aSel, sizeof(aSel),
		"SELECT COALESCE(SUM(RewardXP),0), COALESCE(SUM(RewardMoney),0), COALESCE(st.Role,0) FROM %s_account_mailbox m "
		"LEFT JOIN %s_account_settings st ON st.AccountId=m.AccountId "
		"WHERE m.AccountId=? AND m.RewardClaimed=0 AND (m.RewardXP<>0 OR m.RewardMoney<>0)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSel, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
		return true;
	int SumXP = pSqlServer->GetInt(1);
	int64_t SumMoney = pSqlServer->GetInt64(2);
	const int Role = pSqlServer->GetInt(3);
	if(Role == 1)
	{
		SumXP *= 2;
		SumMoney *= 2;
	}
	if(SumXP == 0 && SumMoney == 0)
		return true;

	char aUpdStats[256];
	str_format(aUpdStats, sizeof(aUpdStats),
		"UPDATE %s_account_stats SET XP=XP+?, Money=Money+? WHERE AccountId=?",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aUpdStats, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, SumXP);
	pSqlServer->BindInt64(2, SumMoney);
	pSqlServer->BindInt64(3, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	char aUpdMail[256];
	str_format(aUpdMail, sizeof(aUpdMail),
		"UPDATE %s_account_mailbox SET RewardClaimed=1, Unread=0 "
		"WHERE AccountId=? AND RewardClaimed=0 AND (RewardXP<>0 OR RewardMoney<>0)",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aUpdMail, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::MailDelete(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxActionRequest *>(pGameData);
	(void)w;
	char aSql[512];
	// Allow delete if no reward, or reward is claimed
	str_format(aSql, sizeof(aSql),
		"DELETE FROM %s_account_mailbox "
		"WHERE AccountId=? AND MailId=? AND ((RewardXP=0 AND RewardMoney=0) OR RewardClaimed=1)",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, pData->m_MailId);
	int NumDeleted = 0;
	return pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize);
}

bool CScoreWorker::MailDeleteAllRead(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountMailboxActionRequest *>(pGameData);
	(void)w;
	char aSql[512];
	// Delete read mails that have no reward, or reward is already claimed
	str_format(aSql, sizeof(aSql),
		"DELETE FROM %s_account_mailbox "
		"WHERE AccountId=? AND ((RewardXP=0 AND RewardMoney=0) OR RewardClaimed=1)",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	int NumDeleted = 0;
	return pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize);
}

bool CScoreWorker::NewGlobalMail(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountGlobalMailRequest *>(pGameData);
	(void)w;
	const int64_t Now = time_timestamp();

	// Purge expired mails
	char aPurge[256];
	str_format(aPurge, sizeof(aPurge), "DELETE FROM %s_account_mailbox WHERE ExpiresAt < ?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aPurge, pError, ErrorSize))
	{
		log_error("mail", "NewGlobalMail purge PrepareStatement failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, Now);
	int NumDeleted = 0;
	if(!pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize))
	{
		log_error("mail", "NewGlobalMail purge ExecuteUpdate failed: %s", pError);
		return false;
	}

	// Insert for all accounts
	char aSql[512];
	str_format(aSql, sizeof(aSql),
		"INSERT INTO %s_account_mailbox (AccountId, Subject, Message, CreatedAt, ExpiresAt, Unread, RewardXP, RewardMoney, RewardClaimed) "
		"SELECT Id, ?, ?, ?, ?, 1, ?, ?, 0 FROM %s_accounts",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aSql, pError, ErrorSize))
	{
		log_error("mail", "NewGlobalMail PrepareStatement failed: %s", pError);
		return false;
	}
	int Param = 1;
	pSqlServer->BindString(Param++, pData->m_aSubject);
	pSqlServer->BindString(Param++, pData->m_aMessage);
	pSqlServer->BindInt64(Param++, Now);
	pSqlServer->BindInt64(Param++, pData->m_ExpiresAt);
	pSqlServer->BindInt(Param++, pData->m_RewardXP);
	pSqlServer->BindInt64(Param++, pData->m_RewardMoney);
	int NumInserted = 0;
	if(!pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize))
	{
		log_error("mail", "NewGlobalMail ExecuteUpdate failed: %s", pError);
		return false;
	}
	log_info("mail", "NewGlobalMail ok: inserted=%d expiresAt=%lld", NumInserted, (long long)pData->m_ExpiresAt);
	return true;
}

bool CScoreWorker::SaveAccountPlaytime(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountPlaytimeSaveRequest *>(pGameData);
	(void)w;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET PlaytimeSeconds=? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "SaveAccountPlaytime PrepareStatement failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, pData->m_PlaytimeSeconds);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
	{
		log_error("account", "SaveAccountPlaytime ExecuteUpdate failed: %s", pError);
		return false;
	}
	return true;
}

bool CScoreWorker::AddAccountDeath(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountAddDeathRequest *>(pGameData);
	(void)w;
	char aBuf[256];
	int NumUpdated = 0;

	// Ensure stats row exists
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Increment deaths
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET Deaths=Deaths+1, Combo=0 WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::AccountKillEvent(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountKillEventRequest *>(pGameData);
	(void)w;
	char aBuf[256];
	int NumUpdated = 0;

	// Ensure stats rows exist
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_KillerAccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	if(pData->m_VictimAccountId > 0)
	{
		str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
			return false;
		pSqlServer->BindInt64(1, pData->m_VictimAccountId);
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
			return false;
	}

	// Update killer stats
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET Kills=Kills+1, Combo=Combo+1, XP=XP+?, Money=Money+? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_RewardXP);
	pSqlServer->BindInt64(2, pData->m_RewardMoney);
	pSqlServer->BindInt64(3, pData->m_KillerAccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Reset victim combo
	if(pData->m_VictimAccountId > 0)
	{
		str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET Combo=0 WHERE AccountId=?", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
			return false;
		pSqlServer->BindInt64(1, pData->m_VictimAccountId);
		return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
	}
	return true;
}

bool CScoreWorker::LoginAccount(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountLoginRequest *>(pGameData);
	if(!pData)
		return false;
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_LOGIN);

	log_info("account", "LoginAccount: user='%s'", pData->m_aUsername);

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "SELECT Id, PwHash, PwSalt FROM %s_accounts WHERE Username = ? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "LoginAccount PrepareStatement failed: %s", pError);
		return false;
	}
	pSqlServer->BindString(1, pData->m_aUsername);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_ACCOUNT_NOT_FOUND", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}
	int64_t AccountId = pSqlServer->GetInt64(1);
	unsigned char aStoredHash[32];
	unsigned char aStoredSalt[16];
	if(pSqlServer->GetBlob(2, aStoredHash, sizeof(aStoredHash)) != (int)sizeof(aStoredHash) ||
		pSqlServer->GetBlob(3, aStoredSalt, sizeof(aStoredSalt)) != (int)sizeof(aStoredSalt))
	{
		str_copy(pError, "invalid password hash/salt length", ErrorSize);
		return false;
	}

	unsigned char aHash[32];
	HashPasswordSha256(pData->m_aPassword, aStoredSalt, sizeof(aStoredSalt), aHash, sizeof(aHash));
	if(mem_comp(aHash, aStoredHash, sizeof(aHash)) != 0)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_WRONG_PASSWORD", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	// update last login + last ign + last ip
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_accounts SET LastLoginDate=CURRENT_TIMESTAMP, LastIgn=?, LastIP=? WHERE Id=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "LoginAccount PrepareStatement update failed: %s", pError);
		return false;
	}
	pSqlServer->BindString(1, pData->m_aLastIgn);
	pSqlServer->BindString(2, pData->m_aClientAddrStr);
	pSqlServer->BindInt64(3, AccountId);
	int NumUpdated;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Ensure settings row exists
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) VALUES (?, 0, 0, 0, 0, 1)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "LoginAccount PrepareStatement settings insert-ignore failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Read settings
	int Language = 0;
	int LangHintShown = 0;
	int LangManual = 0;
	str_format(aBuf, sizeof(aBuf), "SELECT Language, LangHintShown, LangManual, Role FROM %s_account_settings WHERE AccountId=? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		log_error("account", "LoginAccount PrepareStatement settings select failed: %s", pError);
		return false;
	}
	pSqlServer->BindInt64(1, AccountId);
	bool End2;
	if(!pSqlServer->Step(&End2, pError, ErrorSize))
		return false;
	if(!End2)
	{
		Language = pSqlServer->GetInt(1);
		LangHintShown = pSqlServer->GetInt(2);
		LangManual = pSqlServer->GetInt(3);
	}

	// If player never changed language in account settings, try to apply per-IP language
	if(LangManual == 0 && pData->m_aClientAddrStr[0])
	{
		int IpLang = -1;
		char aIpQry[256];
		str_format(aIpQry, sizeof(aIpQry), "SELECT Language FROM %s_ip_language WHERE IpAddr=? LIMIT 1", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aIpQry, pError, ErrorSize))
			return false;
		pSqlServer->BindString(1, pData->m_aClientAddrStr);
		bool EndIp;
		if(!pSqlServer->Step(&EndIp, pError, ErrorSize))
			return false;
		if(!EndIp)
			IpLang = pSqlServer->GetInt(1);
		if(IpLang == 0 || IpLang == 1)
		{
			Language = IpLang;
			// Also store it in account settings but keep LangManual=0.
			char aUpd[256];
			str_format(aUpd, sizeof(aUpd), "UPDATE %s_account_settings SET Language=? WHERE AccountId=?", pSqlServer->GetPrefix());
			if(!pSqlServer->PrepareStatement(aUpd, pError, ErrorSize))
				return false;
			pSqlServer->BindInt(1, Language);
			pSqlServer->BindInt64(2, AccountId);
			int NumUpd2 = 0;
			if(!pSqlServer->ExecuteUpdate(&NumUpd2, pError, ErrorSize))
				return false;
		}
	}

	// Mark hint as shown after first login
	int HintWasShown = LangHintShown;
	if(LangHintShown == 0)
	{
		str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_settings SET LangHintShown=1 WHERE AccountId=?", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			log_error("account", "LoginAccount PrepareStatement settings update hint failed: %s", pError);
			return false;
		}
		pSqlServer->BindInt64(1, AccountId);
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
			return false;
	}

	// put account id into result first message as hidden protocol (parsed server-side later)
	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "LOGIN_OK %lld %d %d", (long long)AccountId, Language, HintWasShown);
	return true;
}

bool CScoreWorker::ChangeAccountPassword(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountChangePasswordRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	pResult->SetVariant(CScorePlayerResult::DIRECT);

	if(!pData->m_aOldPassword[0] || !pData->m_aNewPassword[0])
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_CHANGEPASSWORD_EMPTY", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "SELECT PwHash, PwSalt FROM %s_accounts WHERE Id=? LIMIT 1", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_ACCOUNT_NOT_FOUND", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	unsigned char aStoredHash[32];
	unsigned char aStoredSalt[16];
	if(pSqlServer->GetBlob(1, aStoredHash, sizeof(aStoredHash)) != (int)sizeof(aStoredHash) ||
		pSqlServer->GetBlob(2, aStoredSalt, sizeof(aStoredSalt)) != (int)sizeof(aStoredSalt))
	{
		str_copy(pError, "invalid password hash/salt length", ErrorSize);
		return false;
	}

	unsigned char aOldHash[32];
	HashPasswordSha256(pData->m_aOldPassword, aStoredSalt, sizeof(aStoredSalt), aOldHash, sizeof(aOldHash));
	if(mem_comp(aOldHash, aStoredHash, sizeof(aOldHash)) != 0)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_WRONG_PASSWORD", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	unsigned char aNewSalt[16];
	unsigned char aNewHash[32];
	secure_random_fill(aNewSalt, sizeof(aNewSalt));
	HashPasswordSha256(pData->m_aNewPassword, aNewSalt, sizeof(aNewSalt), aNewHash, sizeof(aNewHash));

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_accounts SET PwHash=?, PwSalt=? WHERE Id=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindBlob(1, aNewHash, sizeof(aNewHash));
	pSqlServer->BindBlob(2, aNewSalt, sizeof(aNewSalt));
	pSqlServer->BindInt64(3, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_copy(pResult->m_Data.m_aaMessages[0], "CHANGEPASSWORD_OK", sizeof(pResult->m_Data.m_aaMessages[0]));
	return true;
}

bool CScoreWorker::LoadIpLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlIpLanguageLoadRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::IP_LANGUAGE);
	// Protocol: IP_LANG <has> <lang>
	int Has = 0;
	int Lang = 0;
	if(pData->m_aClientAddrStr[0])
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "SELECT Language FROM %s_ip_language WHERE IpAddr=? LIMIT 1", pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
			return false;
		pSqlServer->BindString(1, pData->m_aClientAddrStr);
		bool End;
		if(!pSqlServer->Step(&End, pError, ErrorSize))
			return false;
		if(!End)
		{
			Has = 1;
			Lang = pSqlServer->GetInt(1);
		}
	}
	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "IP_LANG %d %d", Has, Lang);
	return true;
}

bool CScoreWorker::SetIpLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlIpLanguageSetRequest *>(pGameData);
	(void)w;
	if(!pData->m_aClientAddrStr[0])
		return true;
	if(pData->m_Language != 0 && pData->m_Language != 1)
		return true;
	char aBuf[256];
	int NumUpdated = 0;
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_ip_language (IpAddr, Language, UpdatedAt) VALUES (?, ?, ?)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aClientAddrStr);
	pSqlServer->BindInt(2, pData->m_Language);
	pSqlServer->BindInt64(3, time_timestamp());
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_ip_language SET Language=?, UpdatedAt=? WHERE IpAddr=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Language);
	pSqlServer->BindInt64(2, time_timestamp());
	pSqlServer->BindString(3, pData->m_aClientAddrStr);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::SetAccountLanguage(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountSetLanguageRequest *>(pGameData);
	(void)w;
	char aBuf[256];

	// Ensure row exists
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) VALUES (?, 0, 1, 0, 0, 1)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Update language and mark as manual
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_settings SET Language=?, LangManual=1 WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Language);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::SetAccountRole(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountSetRoleRequest *>(pGameData);
	(void)w;
	if(!pData || !pData->m_aUsername[0])
		return true;
	if(pData->m_Role < 0)
		return true;
	if(pData->m_Role > 1000)
		return true;

	char aBuf[512];
	int NumUpdated = 0;

	// Ensure settings row exists for this username
	str_format(aBuf, sizeof(aBuf),
		"%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) "
		"SELECT a.Id, 0, 0, 0, 0, 1 FROM %s_accounts a WHERE a.Username=? LIMIT 1",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aUsername);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Update role
	str_format(aBuf, sizeof(aBuf),
		"UPDATE %s_account_settings SET Role=? "
		"WHERE AccountId=(SELECT Id FROM %s_accounts WHERE Username=? LIMIT 1)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Role);
	pSqlServer->BindString(2, pData->m_aUsername);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::AutoLoginLookup(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAutoLoginRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_LOGIN);

	if(!pData->m_aLastIgn[0] || !pData->m_aClientAddrStr[0])
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "AUTOLOGIN_SKIP", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT a.Id, COALESCE(st.Language, 0), COALESCE(st.LangHintShown, 0), a.LastIP, a.LastIgn, COALESCE(st.AutoLogin, 0) "
		"FROM %s_accounts a "
		"LEFT JOIN %s_account_settings st ON st.AccountId = a.Id "
		"WHERE a.LastIgn = ? "
		"ORDER BY a.LastLoginDate DESC "
		"LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aLastIgn);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "AUTOLOGIN_SKIP", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}
	
	int64_t AccountId = pSqlServer->GetInt64(1);
	int Language = pSqlServer->GetInt(2);
	int HintWasShown = pSqlServer->GetInt(3);
	char aStoredIP[64];
	pSqlServer->GetString(4, aStoredIP, sizeof(aStoredIP));
	char aStoredIgn[MAX_NAME_LENGTH];
	pSqlServer->GetString(5, aStoredIgn, sizeof(aStoredIgn));
	int AutoLogin = pSqlServer->GetInt(6);

	// FoxNet checks:
	if(str_comp(aStoredIP, pData->m_aClientAddrStr) != 0 || AutoLogin == 0 || str_comp(aStoredIgn, pData->m_aLastIgn) != 0)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "AUTOLOGIN_SKIP", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	// Encode as LOGIN_OK so the existing handler can reuse it
	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
		"LOGIN_OK %lld %d %d", (long long)AccountId, Language, HintWasShown);

	// Update LastLoginDate like FoxNet does on login
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_accounts SET LastLoginDate=CURRENT_TIMESTAMP WHERE Id=?", pSqlServer->GetPrefix());
	if(pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		pSqlServer->BindInt64(1, AccountId);
		int NumUpdated;
		pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
	}

	return true;
}

bool CScoreWorker::SetAutoLogin(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlSetAutoLoginRequest *>(pGameData);
	(void)w;
	if(!pData)
		return false;

	char aBuf[256];
	int NumUpdated = 0;
	// Ensure row exists
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) VALUES (?, 0, 0, 0, 0, ?)",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt(2, pData->m_AutoLogin);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_settings SET AutoLogin=? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_AutoLogin);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::SaveNick(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlSaveNickRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	pResult->SetVariant(CScorePlayerResult::DIRECT);

	char aNick[MAX_NAME_LENGTH];
	str_copy(aNick, pData->m_aNick, sizeof(aNick));
	str_utf8_fix_truncation(aNick);
	char *pStart = aNick;
	while(*pStart == ' ' || *pStart == '\t')
		pStart++;
	char *pEnd = pStart + str_length(pStart);
	while(pEnd > pStart && (pEnd[-1] == ' ' || pEnd[-1] == '\t'))
		--pEnd;
	*pEnd = 0;
	if(!pStart[0])
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_SAVENICK_EMPTY", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	char aBuf[512];
	char aAlterNick[256];
	str_format(aAlterNick, sizeof(aAlterNick), "ALTER TABLE %s_account_settings ADD COLUMN NickProtectedIgn VARCHAR(64) NOT NULL DEFAULT ''", pSqlServer->GetPrefix());
	{
		int Dummy = 0;
		char aDummyErr[256];
		if(pSqlServer->PrepareStatement(aAlterNick, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
	}

	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) VALUES (?, 0, 0, 0, 0, 1)",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	int NumUpdated = 0;
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf),
		"SELECT AccountId FROM %s_account_settings WHERE LOWER(COALESCE(NickProtectedIgn,''))=LOWER(?) AND AccountId<>? LIMIT 1",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pStart);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(!End)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "ERR_SAVENICK_TAKEN", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_settings SET NickProtectedIgn=? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pStart);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "SAVENICK_OK %s", pStart);
	return true;
}

bool CScoreWorker::CheckNickProtected(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlNickProtectedLookupRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	if(!pData || !pResult)
		return false;
	pResult->SetVariant(CScorePlayerResult::DIRECT);
	if(!pData->m_aNick[0])
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "NICK_PROTECTED 0", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	char aAlterNick[256];
	str_format(aAlterNick, sizeof(aAlterNick), "ALTER TABLE %s_account_settings ADD COLUMN NickProtectedIgn VARCHAR(64) NOT NULL DEFAULT ''", pSqlServer->GetPrefix());
	{
		int Dummy = 0;
		char aDummyErr[256];
		if(pSqlServer->PrepareStatement(aAlterNick, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT 1 FROM %s_account_settings WHERE LOWER(COALESCE(NickProtectedIgn,''))=LOWER(?) LIMIT 1",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aNick);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	str_format(
		pResult->m_Data.m_aaMessages[0],
		sizeof(pResult->m_Data.m_aaMessages[0]),
		"NICK_PROTECTED %d %s",
		End ? 0 : 1,
		pData->m_aNick);
	return true;
}

bool CScoreWorker::LoadAccountProfile(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAccountProfileRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::ACCOUNT_PROFILE);

	log_info("account", "LoadAccountProfile: AccountId=%lld", (long long)pData->m_AccountId);

	char aBuf[1024];
	char aAlterNick[256];
	char aAlterJetpack[256];
	char aAlterXpMigrated[256];
	char aAlterRainbowExpires[256];
	str_format(aAlterNick, sizeof(aAlterNick), "ALTER TABLE %s_account_settings ADD COLUMN NickProtectedIgn VARCHAR(64) NOT NULL DEFAULT ''", pSqlServer->GetPrefix());
	str_format(aAlterJetpack, sizeof(aAlterJetpack), "ALTER TABLE %s_account_settings ADD COLUMN JetpackExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
	str_format(aAlterXpMigrated, sizeof(aAlterXpMigrated), "ALTER TABLE %s_account_settings ADD COLUMN XpMigrated INT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
	str_format(aAlterRainbowExpires, sizeof(aAlterRainbowExpires), "ALTER TABLE %s_account_cosmetics ADD COLUMN RainbowExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
	{
		int Dummy = 0;
		char aDummyErr[256];
		if(pSqlServer->PrepareStatement(aAlterNick, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
		if(pSqlServer->PrepareStatement(aAlterJetpack, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
		if(pSqlServer->PrepareStatement(aAlterXpMigrated, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
		if(pSqlServer->PrepareStatement(aAlterRainbowExpires, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
	}
	const int64_t NowTs = time_timestamp();
	const int64_t RainbowDefaultExpire = NowTs + 30LL * 86400LL;
	{
		int Dummy = 0;
		str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowExpiresAt=? WHERE AccountId=? AND RainbowOwned<>0 AND RainbowExpiresAt<=0", pSqlServer->GetPrefix());
		if(pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			pSqlServer->BindInt64(1, RainbowDefaultExpire);
			pSqlServer->BindInt64(2, pData->m_AccountId);
			pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize);
		}
		str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowOwned=0, RainbowActive=0 WHERE AccountId=? AND RainbowExpiresAt>0 AND RainbowExpiresAt<=?", pSqlServer->GetPrefix());
		if(pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			pSqlServer->BindInt64(1, pData->m_AccountId);
			pSqlServer->BindInt64(2, NowTs);
			pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize);
		}
	}

	str_format(aBuf, sizeof(aBuf),
		"SELECT a.Username, a.LastIgn, a.RegisterDate, "
		"COALESCE(s.XP, 0), COALESCE(s.Money, 0), COALESCE(s.PlaytimeSeconds, 0), COALESCE(s.Deaths, 0), COALESCE(s.Kills, 0), COALESCE(s.Combo, 0), "
		"CASE WHEN COALESCE(c.RainbowOwned, 0)<>0 AND COALESCE(c.RainbowExpiresAt, 0)>? THEN 1 ELSE 0 END, "
		"CASE WHEN COALESCE(c.RainbowActive, 0)<>0 AND COALESCE(c.RainbowExpiresAt, 0)>? THEN 1 ELSE 0 END, COALESCE(st.Role, 0), COALESCE(st.AutoLogin, 0), "
		"COALESCE(st.NickProtectedIgn, ''), COALESCE(st.JetpackExpiresAt, 0), COALESCE(st.XpMigrated, 0) "
		"FROM %s_accounts a "
		"LEFT JOIN %s_account_stats s ON s.AccountId = a.Id "
		"LEFT JOIN %s_account_cosmetics c ON c.AccountId = a.Id "
		"LEFT JOIN %s_account_settings st ON st.AccountId = a.Id "
		"WHERE a.Id = ? LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, NowTs);
	pSqlServer->BindInt64(2, NowTs);
	pSqlServer->BindInt64(3, pData->m_AccountId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
	{
		log_info("account", "LoadAccountProfile: no row (End=true) AccountId=%lld", (long long)pData->m_AccountId);
		str_copy(pResult->m_Data.m_aaMessages[0], "Profile not found", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}
	pSqlServer->GetString(1, pResult->m_Data.m_Account.m_aUsername, sizeof(pResult->m_Data.m_Account.m_aUsername));
	pSqlServer->GetString(2, pResult->m_Data.m_Account.m_aLastIgn, sizeof(pResult->m_Data.m_Account.m_aLastIgn));
	pSqlServer->GetString(3, pResult->m_Data.m_Account.m_aRegisterDate, sizeof(pResult->m_Data.m_Account.m_aRegisterDate));
	pResult->m_Data.m_Account.m_XP = pSqlServer->GetInt(4);
	pResult->m_Data.m_Account.m_Money = pSqlServer->GetInt64(5);
	pResult->m_Data.m_Account.m_PlaytimeSeconds = pSqlServer->GetInt64(6);
	pResult->m_Data.m_Account.m_Deaths = pSqlServer->GetInt64(7);
	pResult->m_Data.m_Account.m_Kills = pSqlServer->GetInt64(8);
	pResult->m_Data.m_Account.m_Combo = pSqlServer->GetInt64(9);
	pResult->m_Data.m_Account.m_RainbowOwned = pSqlServer->GetInt(10);
	pResult->m_Data.m_Account.m_RainbowActive = pSqlServer->GetInt(11);
	pResult->m_Data.m_Account.m_Role = pSqlServer->GetInt(12);
	pResult->m_Data.m_Account.m_AutoLogin = pSqlServer->GetInt(13);
	pSqlServer->GetString(14, pResult->m_Data.m_Account.m_aNickProtectedIgn, sizeof(pResult->m_Data.m_Account.m_aNickProtectedIgn));
	const int64_t JetpackExpiresAt = pSqlServer->GetInt64(15);
	const int XpMigrated = pSqlServer->GetInt(16);
	pResult->m_Data.m_Account.m_JetpackActive = JetpackExpiresAt > time_timestamp() ? 1 : 0;

	int64_t TotalXP = pResult->m_Data.m_Account.m_XP;
	if(XpMigrated == 0)
	{
		const int OldLevel = AccountLegacyLevelFromXP((int)maximum<int64_t>(0, TotalXP));
		TotalXP = AccountXPForLevel(OldLevel);

		int Dummy = 0;
		char aUpdateStats[256];
		str_format(aUpdateStats, sizeof(aUpdateStats), "UPDATE %s_account_stats SET XP=? WHERE AccountId=?", pSqlServer->GetPrefix());
		if(pSqlServer->PrepareStatement(aUpdateStats, pError, ErrorSize))
		{
			pSqlServer->BindInt64(1, TotalXP);
			pSqlServer->BindInt64(2, pData->m_AccountId);
			pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize);
		}

		char aUpdateSettings[256];
		str_format(aUpdateSettings, sizeof(aUpdateSettings), "UPDATE %s_account_settings SET XpMigrated=1 WHERE AccountId=?", pSqlServer->GetPrefix());
		if(pSqlServer->PrepareStatement(aUpdateSettings, pError, ErrorSize))
		{
			pSqlServer->BindInt64(1, pData->m_AccountId);
			pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize);
		}
	}

	const int Level = AccountLevelFromXP((int)maximum<int64_t>(0, TotalXP));
	const int XPInto = (int)(maximum<int64_t>(0, TotalXP) - AccountXPForLevel(Level));
	pResult->m_Data.m_Account.m_Level = Level;
	pResult->m_Data.m_Account.m_XP = XPInto;
	pResult->m_Data.m_Account.m_XPToNext = AccountLevelXPToNext(Level);

	log_info("account", "LoadAccountProfile: ok user='%s' lvl=%d xp=%d money=%lld play=%lld deaths=%lld kills=%lld combo=%lld",
		pResult->m_Data.m_Account.m_aUsername,
		pResult->m_Data.m_Account.m_Level,
		pResult->m_Data.m_Account.m_XP,
		(long long)pResult->m_Data.m_Account.m_Money,
		(long long)pResult->m_Data.m_Account.m_PlaytimeSeconds,
		(long long)pResult->m_Data.m_Account.m_Deaths,
		(long long)pResult->m_Data.m_Account.m_Kills,
		(long long)pResult->m_Data.m_Account.m_Combo);
	return true;
}

bool CScoreWorker::ShopBuyRainbow(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlShopBuyRainbowRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::DIRECT);

	// Ensure rows exist
	char aBuf[256];
	int NumUpdated = 0;
	const int64_t Now = time_timestamp();
	const int64_t ExpiresAt = Now + 30LL * 86400LL;
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) VALUES (?, 0, 0, 0, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	{
		char aAlter[256];
		str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_cosmetics ADD COLUMN RainbowExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		int Dummy = 0;
		char aErr[256];
		if(pSqlServer->PrepareStatement(aAlter, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
	}
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_cosmetics (AccountId, RainbowOwned, RainbowActive, RainbowExpiresAt) VALUES (?, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;
	// Backfill old permanent rainbow and disable expired one.
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowExpiresAt=? WHERE AccountId=? AND RainbowOwned<>0 AND RainbowExpiresAt<=0", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, ExpiresAt);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowOwned=0, RainbowActive=0 WHERE AccountId=? AND RainbowExpiresAt>0 AND RainbowExpiresAt<=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, Now);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	// Read money/xp and cosmetics
	int XP = 0;
	int64_t Money = 0;
	int Owned = 0;
	str_format(aBuf, sizeof(aBuf),
		"SELECT COALESCE(s.XP,0), COALESCE(s.Money,0), "
		"CASE WHEN COALESCE(c.RainbowOwned,0)<>0 AND COALESCE(c.RainbowExpiresAt,0)>? THEN 1 ELSE 0 END "
		"FROM %s_account_stats s LEFT JOIN %s_account_cosmetics c ON c.AccountId=s.AccountId "
		"WHERE s.AccountId=? LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, Now);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	if(End)
		return true;
	XP = pSqlServer->GetInt(1);
	Money = pSqlServer->GetInt64(2);
	Owned = pSqlServer->GetInt(3);

	const int Level = AccountLevelFromXP(XP);
	if(Level < 15)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "SHOP_RAINBOW_LOW_LEVEL", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}
	if(Owned != 0)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "SHOP_RAINBOW_OWNED", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}
	if(Money < 5000)
	{
		str_copy(pResult->m_Data.m_aaMessages[0], "SHOP_RAINBOW_NO_MONEY", sizeof(pResult->m_Data.m_aaMessages[0]));
		return true;
	}

	Money -= 5000;
	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_stats SET Money=? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, Money);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowOwned=1, RainbowActive=1, RainbowExpiresAt=? WHERE AccountId=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, ExpiresAt);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]), "SHOP_RAINBOW_OK %lld", (long long)Money);
	return true;
}

bool CScoreWorker::CosmeticsSetRainbowActive(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlCosmeticsSetRainbowActiveRequest *>(pGameData);
	(void)w;
	char aBuf[256];
	int NumUpdated = 0;
	const int64_t Now = time_timestamp();
	{
		char aAlter[256];
		str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_cosmetics ADD COLUMN RainbowExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		int Dummy = 0;
		char aErr[256];
		if(pSqlServer->PrepareStatement(aAlter, aErr, sizeof(aErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aErr, sizeof(aErr));
	}
	str_format(aBuf, sizeof(aBuf), "%s INTO %s_account_cosmetics (AccountId, RainbowOwned, RainbowActive, RainbowExpiresAt) VALUES (?, 0, 0, 0)", pSqlServer->InsertIgnore(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowOwned=0, RainbowActive=0 WHERE AccountId=? AND RainbowExpiresAt>0 AND RainbowExpiresAt<=?", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, pData->m_AccountId);
	pSqlServer->BindInt64(2, Now);
	if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		return false;

	str_format(aBuf, sizeof(aBuf), "UPDATE %s_account_cosmetics SET RainbowActive=? WHERE AccountId=? AND (RainbowExpiresAt<=0 OR RainbowExpiresAt>?)", pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_Active);
	pSqlServer->BindInt64(2, pData->m_AccountId);
	pSqlServer->BindInt64(3, Now);
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

void CScorePlayerResult::SetVariant(Variant v)
{
	m_MessageKind = v;
	switch(v)
	{
	case DIRECT:
	case ALL:
	case ACCOUNT_REGISTER:
	case ACCOUNT_LOGIN:
	case ACCOUNT_PROFILE:
	case ACCOUNT_MAILBOX:
	case ACCOUNT_INVENTORY:
		for(auto &aMessage : m_Data.m_aaMessages)
			aMessage[0] = 0;
		m_Data.m_Mailbox.m_NumMails = 0;
		m_Data.m_Inventory.m_NumItems = 0;
		break;
	case BROADCAST:
		m_Data.m_aBroadcast[0] = 0;
		break;
	case MAP_VOTE:
		m_Data.m_MapVote.m_aMap[0] = '\0';
		m_Data.m_MapVote.m_aReason[0] = '\0';
		m_Data.m_MapVote.m_aServer[0] = '\0';
		break;
	case PLAYER_INFO:
		m_Data.m_Info.m_Birthday = 0;
		m_Data.m_Info.m_Time.reset();
		for(float &TimeCp : m_Data.m_Info.m_aTimeCp)
			TimeCp = 0;
		break;
	case PLAYER_TIMECP:
		m_Data.m_Info.m_aRequestedPlayer[0] = '\0';
		m_Data.m_Info.m_Time.reset();
		for(float &TimeCp : m_Data.m_Info.m_aTimeCp)
			TimeCp = 0;
		break;
	}
}

CTeamrank::CTeamrank() :
	m_NumNames(0)
{
	for(auto &aName : m_aaNames)
		aName[0] = '\0';
	mem_zero(&m_TeamId.m_aData, sizeof(m_TeamId));
}

bool CTeamrank::NextSqlResult(IDbConnection *pSqlServer, bool *pEnd, char *pError, int ErrorSize)
{
	pSqlServer->GetBlob(1, m_TeamId.m_aData, sizeof(m_TeamId.m_aData));
	pSqlServer->GetString(2, m_aaNames[0], sizeof(m_aaNames[0]));
	m_NumNames = 1;
	bool End = false;
	while(pSqlServer->Step(&End, pError, ErrorSize) && !End)
	{
		CUuid TeamId;
		pSqlServer->GetBlob(1, TeamId.m_aData, sizeof(TeamId.m_aData));
		if(m_TeamId != TeamId)
		{
			*pEnd = false;
			return true;
		}
		pSqlServer->GetString(2, m_aaNames[m_NumNames], sizeof(m_aaNames[m_NumNames]));
		m_NumNames++;
	}
	if(!End)
	{
		return false;
	}
	*pEnd = true;
	return true;
}

bool CTeamrank::SamePlayers(const std::vector<std::string> *pvSortedNames)
{
	if(pvSortedNames->size() != m_NumNames)
		return false;
	for(unsigned int i = 0; i < m_NumNames; i++)
	{
		if(str_comp(pvSortedNames->at(i).c_str(), m_aaNames[i]) != 0)
			return false;
	}
	return true;
}

bool CTeamrank::GetSqlTop5Team(IDbConnection *pSqlServer, bool *pEnd, char *pError, int ErrorSize, char (*paMessages)[512], int *Line, int Count)
{
	char aTime[32];
	int StartLine = *Line;
	for(*Line = StartLine; *Line < StartLine + Count; (*Line)++)
	{
		bool Last = false;
		float Time = pSqlServer->GetFloat(2);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		int Rank = pSqlServer->GetInt(3);
		int TeamSize = pSqlServer->GetInt(4);

		char aNames[2300] = {0};
		for(int i = 0; i < TeamSize; i++)
		{
			char aName[MAX_NAME_LENGTH];
			pSqlServer->GetString(1, aName, sizeof(aName));
			str_append(aNames, aName);
			if(i < TeamSize - 2)
				str_append(aNames, ", ");
			else if(i == TeamSize - 2)
				str_append(aNames, " & ");
			if(!pSqlServer->Step(&Last, pError, ErrorSize))
			{
				return false;
			}
			if(Last)
			{
				break;
			}
		}
		str_format(paMessages[*Line], sizeof(paMessages[*Line]), "%d. %s Team Time: %s",
			Rank, aNames, aTime);
		if(Last)
		{
			(*Line)++;
			break;
		}
	}
	return true;
}

bool CScoreWorker::LoadBestTime(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlLoadBestTimeRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScoreLoadBestTimeResult *>(pGameData->m_pResult.get());

	char aBuf[512];
	// get the best time
	str_format(aBuf, sizeof(aBuf),
		"SELECT Time FROM %s_race WHERE Map=? ORDER BY `Time` ASC LIMIT 1",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		pResult->m_CurrentRecord = pSqlServer->GetFloat(1);
	}

	return true;
}

// update stuff
bool CScoreWorker::LoadPlayerData(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	pResult->SetVariant(CScorePlayerResult::PLAYER_INFO);

	char aBuf[1024];
	// get best race time
	str_format(aBuf, sizeof(aBuf),
		"SELECT"
		"  (SELECT Time FROM %s_race WHERE Map = ? AND Name = ? ORDER BY Time ASC LIMIT 1) AS minTime, "
		"  cp1, cp2, cp3, cp4, cp5, cp6, cp7, cp8, cp9, cp10, cp11, cp12, cp13, cp14, "
		"  cp15, cp16, cp17, cp18, cp19, cp20, cp21, cp22, cp23, cp24, cp25, "
		"  (cp1 + cp2 + cp3 + cp4 + cp5 + cp6 + cp7 + cp8 + cp9 + cp10 + cp11 + cp12 + cp13 + cp14 + "
		"  cp15 + cp16 + cp17 + cp18 + cp19 + cp20 + cp21 + cp22 + cp23 + cp24 + cp25 > 0) AS hasCP, Time "
		"FROM %s_race "
		"WHERE Map = ? AND Name = ? "
		"ORDER BY hasCP DESC, Time ASC "
		"LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}

	const char *pPlayer = pData->m_aName[0] != '\0' ? pData->m_aName : pData->m_aRequestingPlayer;
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pData->m_aRequestingPlayer);
	pSqlServer->BindString(3, pData->m_aMap);
	pSqlServer->BindString(4, pPlayer);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		if(!pSqlServer->IsNull(1))
		{
			// get the best time
			float Time = pSqlServer->GetFloat(1);
			pResult->m_Data.m_Info.m_Time = Time;
		}

		for(int i = 0; i < NUM_CHECKPOINTS; i++)
		{
			pResult->m_Data.m_Info.m_aTimeCp[i] = pSqlServer->GetFloat(i + 2);
		}
	}

	// birthday check
	str_format(aBuf, sizeof(aBuf),
		"SELECT CURRENT_TIMESTAMP AS Current, MIN(Timestamp) AS Stamp "
		"FROM %s_race "
		"WHERE Name = ?",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aRequestingPlayer);

	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End && !pSqlServer->IsNull(2))
	{
		char aCurrent[TIMESTAMP_STR_LENGTH];
		pSqlServer->GetString(1, aCurrent, sizeof(aCurrent));
		char aStamp[TIMESTAMP_STR_LENGTH];
		pSqlServer->GetString(2, aStamp, sizeof(aStamp));
		int CurrentYear, CurrentMonth, CurrentDay;
		int StampYear, StampMonth, StampDay;
		if(sscanf(aCurrent, "%d-%d-%d", &CurrentYear, &CurrentMonth, &CurrentDay) == 3 && sscanf(aStamp, "%d-%d-%d", &StampYear, &StampMonth, &StampDay) == 3 && CurrentMonth == StampMonth && CurrentDay == StampDay)
			pResult->m_Data.m_Info.m_Birthday = CurrentYear - StampYear;
	}
	return true;
}

bool CScoreWorker::LoadPlayerTimeCp(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	char aBuf[1024];
	str_format(aBuf, sizeof(aBuf),
		"SELECT"
		"  Time, cp1, cp2, cp3, cp4, cp5, cp6, cp7, cp8, cp9, cp10, cp11, cp12, cp13, "
		"  cp14, cp15, cp16, cp17, cp18, cp19, cp20, cp21, cp22, cp23, cp24, cp25 "
		"FROM %s_race "
		"WHERE Map = ? AND Name = ? AND "
		"  (cp1 + cp2 + cp3 + cp4 + cp5 + cp6 + cp7 + cp8 + cp9 + cp10 + cp11 + cp12 + cp13 + cp14 + "
		"  cp15 + cp16 + cp17 + cp18 + cp19 + cp20 + cp21 + cp22 + cp23 + cp24 + cp25) > 0 "
		"ORDER BY Time ASC "
		"LIMIT 1",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}

	const char *pPlayer = pData->m_aName[0] != '\0' ? pData->m_aName : pData->m_aRequestingPlayer;
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pPlayer);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		pResult->SetVariant(CScorePlayerResult::PLAYER_TIMECP);
		pResult->m_Data.m_Info.m_Time = pSqlServer->GetFloat(1);
		for(int i = 0; i < NUM_CHECKPOINTS; i++)
		{
			pResult->m_Data.m_Info.m_aTimeCp[i] = pSqlServer->GetFloat(i + 2);
		}
		str_copy(pResult->m_Data.m_Info.m_aRequestedPlayer, pPlayer, sizeof(pResult->m_Data.m_Info.m_aRequestedPlayer));
	}
	else
	{
		pResult->SetVariant(CScorePlayerResult::DIRECT);
		str_format(paMessages[0], sizeof(paMessages[0]), "'%s' has no checkpoint times available", pPlayer);
	}
	return true;
}

bool CScoreWorker::MapVote(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	char aFuzzyMap[128];
	str_copy(aFuzzyMap, pData->m_aName, sizeof(aFuzzyMap));
	sqlstr::FuzzyString(aFuzzyMap, sizeof(aFuzzyMap));

	char aMapPrefix[128];
	str_copy(aMapPrefix, pData->m_aName, sizeof(aMapPrefix));
	str_append(aMapPrefix, "%");

	char aBuf[768];
	str_format(aBuf, sizeof(aBuf),
		"SELECT Map, Server "
		"FROM %s_maps "
		"WHERE Map LIKE %s "
		"ORDER BY "
		"  CASE WHEN LOWER(Map) = LOWER(?) THEN 0 ELSE 1 END, "
		"  CASE WHEN Map LIKE ? THEN 0 ELSE 1 END, "
		"  LENGTH(Map), Map "
		"LIMIT 1",
		pSqlServer->GetPrefix(), pSqlServer->CollateNocase());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, aFuzzyMap);
	pSqlServer->BindString(2, pData->m_aName);
	pSqlServer->BindString(3, aMapPrefix);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		pResult->SetVariant(CScorePlayerResult::MAP_VOTE);
		auto *pMapVote = &pResult->m_Data.m_MapVote;
		pSqlServer->GetString(1, pMapVote->m_aMap, sizeof(pMapVote->m_aMap));
		pSqlServer->GetString(2, pMapVote->m_aServer, sizeof(pMapVote->m_aServer));
		str_copy(pMapVote->m_aReason, "/map", sizeof(pMapVote->m_aReason));

		for(char *p = pMapVote->m_aServer; *p; p++) // lower case server
			*p = tolower(*p);
	}
	else
	{
		pResult->SetVariant(CScorePlayerResult::DIRECT);
		str_format(paMessages[0], sizeof(paMessages[0]),
			"No map like \"%s\" found. "
			"Try adding a '%%' at the start if you don't know the first character. "
			"Example: /map %%castle for \"Out of Castle\"",
			pData->m_aName);
	}
	return true;
}

bool CScoreWorker::MapInfo(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());

	char aFuzzyMap[128];
	str_copy(aFuzzyMap, pData->m_aName, sizeof(aFuzzyMap));
	sqlstr::FuzzyString(aFuzzyMap, sizeof(aFuzzyMap));

	char aMapPrefix[128];
	str_copy(aMapPrefix, pData->m_aName, sizeof(aMapPrefix));
	str_append(aMapPrefix, "%");

	char aCurrentTimestamp[512];
	pSqlServer->ToUnixTimestamp("CURRENT_TIMESTAMP", aCurrentTimestamp, sizeof(aCurrentTimestamp));
	char aTimestamp[512];
	pSqlServer->ToUnixTimestamp("l.Timestamp", aTimestamp, sizeof(aTimestamp));

	char aMedianMapTime[2048];
	char aBuf[4096];
	str_format(aBuf, sizeof(aBuf),
		"SELECT l.Map, l.Server, Mapper, Points, Stars, "
		"  (SELECT COUNT(Name) FROM %s_race WHERE Map = l.Map) AS Finishes, "
		"  (SELECT COUNT(DISTINCT Name) FROM %s_race WHERE Map = l.Map) AS Finishers, "
		"  (%s) AS Median, "
		"  %s AS Stamp, "
		"  %s-%s AS Ago, "
		"  (SELECT MIN(Time) FROM %s_race WHERE Map = l.Map AND Name = ?) AS OwnTime "
		"FROM ("
		"  SELECT * FROM %s_maps "
		"  WHERE Map LIKE %s "
		"  ORDER BY "
		"    CASE WHEN LOWER(Map) = LOWER(?) THEN 0 ELSE 1 END, "
		"    CASE WHEN Map LIKE ? THEN 0 ELSE 1 END, "
		"    LENGTH(Map), "
		"    Map "
		"  LIMIT 1"
		") as l",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(),
		pSqlServer->MedianMapTime(aMedianMapTime, sizeof(aMedianMapTime)),
		aTimestamp, aCurrentTimestamp, aTimestamp,
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(),
		pSqlServer->CollateNocase());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aRequestingPlayer);
	pSqlServer->BindString(2, aFuzzyMap);
	pSqlServer->BindString(3, pData->m_aName);
	pSqlServer->BindString(4, aMapPrefix);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		char aMap[MAX_MAP_LENGTH];
		pSqlServer->GetString(1, aMap, sizeof(aMap));
		char aServer[32];
		pSqlServer->GetString(2, aServer, sizeof(aServer));
		char aMapper[128];
		pSqlServer->GetString(3, aMapper, sizeof(aMapper));
		int Points = pSqlServer->GetInt(4);
		int Stars = pSqlServer->GetInt(5);
		int Finishes = pSqlServer->GetInt(6);
		int Finishers = pSqlServer->GetInt(7);
		float Median = !pSqlServer->IsNull(8) ? pSqlServer->GetInt(8) : -1.0f;
		int Stamp = pSqlServer->GetInt(9);
		int Ago = pSqlServer->GetInt(10);
		float OwnTime = !pSqlServer->IsNull(11) ? pSqlServer->GetFloat(11) : -1.0f;

		char aAgoString[40] = "\0";
		char aReleasedString[60] = "\0";
		if(Stamp != 0)
		{
			sqlstr::AgoTimeToString(Ago, aAgoString, sizeof(aAgoString));
			str_format(aReleasedString, sizeof(aReleasedString), ", released %s ago", aAgoString);
		}

		char aMedianString[60] = "\0";
		if(Median > 0)
		{
			str_time((int64_t)Median * 100, ETimeFormat::HOURS, aBuf, sizeof(aBuf));
			str_format(aMedianString, sizeof(aMedianString), " in %s median", aBuf);
		}

		char aStars[20];
		switch(Stars)
		{
		case 0: str_copy(aStars, "✰✰✰✰✰", sizeof(aStars)); break;
		case 1: str_copy(aStars, "★✰✰✰✰", sizeof(aStars)); break;
		case 2: str_copy(aStars, "★★✰✰✰", sizeof(aStars)); break;
		case 3: str_copy(aStars, "★★★✰✰", sizeof(aStars)); break;
		case 4: str_copy(aStars, "★★★★✰", sizeof(aStars)); break;
		case 5: str_copy(aStars, "★★★★★", sizeof(aStars)); break;
		default: aStars[0] = '\0';
		}

		char aOwnFinishesString[40] = "\0";
		if(OwnTime > 0)
		{
			str_time_float(OwnTime, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));
			str_format(aOwnFinishesString, sizeof(aOwnFinishesString),
				", your time: %s", aBuf);
		}

		str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
			"\"%s\" by %s on %s, %s, %d %s%s, %d %s by %d %s%s%s",
			aMap, aMapper, aServer, aStars,
			Points, Points == 1 ? "point" : "points",
			aReleasedString,
			Finishes, Finishes == 1 ? "finish" : "finishes",
			Finishers, Finishers == 1 ? "tee" : "tees",
			aMedianString, aOwnFinishesString);
	}
	else
	{
		str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
			"No map like \"%s\" found.", pData->m_aName);
	}
	return true;
}

bool CScoreWorker::SaveScore(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlScoreData *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	char aBuf[1024];

	if(w == Write::NORMAL_SUCCEEDED)
	{
		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_race_backup WHERE GameId=? AND Name=? AND Timestamp=%s",
			pSqlServer->GetPrefix(), pSqlServer->InsertTimestampAsUtc());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGameUuid);
		pSqlServer->BindString(2, pData->m_aName);
		pSqlServer->BindString(3, pData->m_aTimestamp);
		pSqlServer->Print();
		int NumDeleted;
		if(!pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize))
		{
			return false;
		}
		if(NumDeleted == 0)
		{
			log_warn("sql", "Rank got moved out of backup database, will show up as duplicate rank in MySQL");
		}
		return true;
	}
	if(w == Write::NORMAL_FAILED)
	{
		int NumUpdated;
		// move to non-tmp table succeeded. delete from backup again
		str_format(aBuf, sizeof(aBuf),
			"INSERT INTO %s_race SELECT * FROM %s_race_backup WHERE GameId=? AND Name=? AND Timestamp=%s",
			pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->InsertTimestampAsUtc());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGameUuid);
		pSqlServer->BindString(2, pData->m_aName);
		pSqlServer->BindString(3, pData->m_aTimestamp);
		pSqlServer->Print();
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		{
			return false;
		}

		// move to non-tmp table succeeded. delete from backup again
		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_race_backup WHERE GameId=? AND Name=? AND Timestamp=%s",
			pSqlServer->GetPrefix(), pSqlServer->InsertTimestampAsUtc());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGameUuid);
		pSqlServer->BindString(2, pData->m_aName);
		pSqlServer->BindString(3, pData->m_aTimestamp);
		pSqlServer->Print();
		if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
		{
			return false;
		}
		if(NumUpdated == 0)
		{
			log_warn("sql", "Rank got moved out of backup database, will show up as duplicate rank in MySQL");
		}
		return true;
	}

	if(w == Write::NORMAL)
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT COUNT(*) AS NumFinished FROM %s_race WHERE Map=? AND Name=? ORDER BY time ASC LIMIT 1",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aMap);
		pSqlServer->BindString(2, pData->m_aName);

		bool End;
		if(!pSqlServer->Step(&End, pError, ErrorSize))
		{
			return false;
		}
		int NumFinished = pSqlServer->GetInt(1);
		if(NumFinished == 0)
		{
			str_format(aBuf, sizeof(aBuf), "SELECT Points FROM %s_maps WHERE Map=?", pSqlServer->GetPrefix());
			if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
			{
				return false;
			}
			pSqlServer->BindString(1, pData->m_aMap);

			bool End2;
			if(!pSqlServer->Step(&End2, pError, ErrorSize))
			{
				return false;
			}
			if(!End2)
			{
				int Points = pSqlServer->GetInt(1);
				if(!pSqlServer->AddPoints(pData->m_aName, Points, pError, ErrorSize))
				{
					return false;
				}
				str_format(paMessages[0], sizeof(paMessages[0]),
					"You earned %d point%s for finishing this map!",
					Points, Points == 1 ? "" : "s");
			}
		}
	}

	// save score. Can't fail, because no UNIQUE/PRIMARY KEY constrain is defined.
	str_format(aBuf, sizeof(aBuf),
		"%s INTO %s_race%s("
		"	Map, Name, Timestamp, Time, Server, "
		"	cp1, cp2, cp3, cp4, cp5, cp6, cp7, cp8, cp9, cp10, cp11, cp12, cp13, "
		"	cp14, cp15, cp16, cp17, cp18, cp19, cp20, cp21, cp22, cp23, cp24, cp25, "
		"	GameId, DDNet7) "
		"VALUES (?, ?, %s, %.2f, ?, "
		"	%.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, "
		"	%.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, "
		"	%.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, "
		"	?, %s)",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(),
		w == Write::NORMAL ? "" : "_backup",
		pSqlServer->InsertTimestampAsUtc(), pData->m_Time,
		pData->m_aCurrentTimeCp[0], pData->m_aCurrentTimeCp[1], pData->m_aCurrentTimeCp[2],
		pData->m_aCurrentTimeCp[3], pData->m_aCurrentTimeCp[4], pData->m_aCurrentTimeCp[5],
		pData->m_aCurrentTimeCp[6], pData->m_aCurrentTimeCp[7], pData->m_aCurrentTimeCp[8],
		pData->m_aCurrentTimeCp[9], pData->m_aCurrentTimeCp[10], pData->m_aCurrentTimeCp[11],
		pData->m_aCurrentTimeCp[12], pData->m_aCurrentTimeCp[13], pData->m_aCurrentTimeCp[14],
		pData->m_aCurrentTimeCp[15], pData->m_aCurrentTimeCp[16], pData->m_aCurrentTimeCp[17],
		pData->m_aCurrentTimeCp[18], pData->m_aCurrentTimeCp[19], pData->m_aCurrentTimeCp[20],
		pData->m_aCurrentTimeCp[21], pData->m_aCurrentTimeCp[22], pData->m_aCurrentTimeCp[23],
		pData->m_aCurrentTimeCp[24], pSqlServer->False());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pData->m_aName);
	pSqlServer->BindString(3, pData->m_aTimestamp);
	pSqlServer->BindString(4, g_Config.m_SvSqlServerName);
	pSqlServer->BindString(5, pData->m_aGameUuid);
	pSqlServer->Print();
	int NumInserted;
	return pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize);
}

bool CScoreWorker::SaveTeamScore(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlTeamScoreData *>(pGameData);

	char aBuf[512];

	if(w == Write::NORMAL_SUCCEEDED)
	{
		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_teamrace_backup WHERE Id=?",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}

		// copy uuid, because mysql BindBlob doesn't support const buffers
		CUuid TeamrankId = pData->m_TeamrankUuid;
		pSqlServer->BindBlob(1, TeamrankId.m_aData, sizeof(TeamrankId.m_aData));
		pSqlServer->Print();
		int NumDeleted;
		if(!pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize))
		{
			return false;
		}
		if(NumDeleted == 0)
		{
			log_warn("sql", "Teamrank got moved out of backup database, will show up as duplicate teamrank in MySQL");
		}
		return true;
	}
	if(w == Write::NORMAL_FAILED)
	{
		int NumInserted;
		CUuid TeamrankId = pData->m_TeamrankUuid;

		str_format(aBuf, sizeof(aBuf),
			"INSERT INTO %s_teamrace SELECT * FROM %s_teamrace_backup WHERE Id=?",
			pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindBlob(1, TeamrankId.m_aData, sizeof(TeamrankId.m_aData));
		pSqlServer->Print();
		if(!pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize))
		{
			return false;
		}

		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_teamrace_backup WHERE Id=?",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindBlob(1, TeamrankId.m_aData, sizeof(TeamrankId.m_aData));
		pSqlServer->Print();
		return pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize);
	}

	if(w == Write::NORMAL)
	{
		// get the names sorted in a tab separated string
		std::vector<std::string> vNames;
		vNames.reserve(pData->m_Size);
		for(unsigned int i = 0; i < pData->m_Size; i++)
			vNames.emplace_back(pData->m_aaNames[i]);

		std::sort(vNames.begin(), vNames.end());
		str_format(aBuf, sizeof(aBuf),
			"SELECT l.Id, Name, Time "
			"FROM (" // preselect teams with first name in team
			"  SELECT ID "
			"  FROM %s_teamrace "
			"  WHERE Map = ? AND Name = ? AND DDNet7 = %s"
			") as l INNER JOIN %s_teamrace AS r ON l.Id = r.Id "
			"ORDER BY l.Id, Name COLLATE %s",
			pSqlServer->GetPrefix(), pSqlServer->False(), pSqlServer->GetPrefix(), pSqlServer->BinaryCollate());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aMap);
		pSqlServer->BindString(2, pData->m_aaNames[0]);

		bool FoundTeam = false;
		float Time;
		CTeamrank Teamrank;
		bool End;
		if(!pSqlServer->Step(&End, pError, ErrorSize))
		{
			return false;
		}
		if(!End)
		{
			bool SearchTeamEnd = false;
			while(!SearchTeamEnd)
			{
				Time = pSqlServer->GetFloat(3);
				if(!Teamrank.NextSqlResult(pSqlServer, &SearchTeamEnd, pError, ErrorSize))
				{
					return false;
				}
				if(Teamrank.SamePlayers(&vNames))
				{
					FoundTeam = true;
					break;
				}
			}
		}
		if(FoundTeam)
		{
			dbg_msg("sql", "found team rank from same team (old time: %f, new time: %f)", Time, pData->m_Time);
			if(pData->m_Time < Time)
			{
				str_format(aBuf, sizeof(aBuf),
					"UPDATE %s_teamrace SET Time=%.2f, Timestamp=%s, DDNet7=%s, GameId=? WHERE Id = ?",
					pSqlServer->GetPrefix(), pData->m_Time, pSqlServer->InsertTimestampAsUtc(), pSqlServer->False());
				if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
				{
					return false;
				}
				pSqlServer->BindString(1, pData->m_aTimestamp);
				pSqlServer->BindString(2, pData->m_aGameUuid);
				pSqlServer->BindBlob(3, Teamrank.m_TeamId.m_aData, sizeof(Teamrank.m_TeamId.m_aData));
				pSqlServer->Print();
				int NumUpdated;
				if(!pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize))
				{
					return false;
				}
				// return error if we didn't update any rows
				return NumUpdated != 0;
			}
			return true;
		}
	}

	for(unsigned int i = 0; i < pData->m_Size; i++)
	{
		// if no entry found... create a new one
		str_format(aBuf, sizeof(aBuf),
			"%s INTO %s_teamrace%s(Map, Name, Timestamp, Time, Id, GameId, DDNet7) "
			"VALUES (?, ?, %s, %.2f, ?, ?, %s)",
			pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(),
			w == Write::NORMAL ? "" : "_backup",
			pSqlServer->InsertTimestampAsUtc(), pData->m_Time, pSqlServer->False());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aMap);
		pSqlServer->BindString(2, pData->m_aaNames[i]);
		pSqlServer->BindString(3, pData->m_aTimestamp);
		// copy uuid, because mysql BindBlob doesn't support const buffers
		CUuid TeamrankId = pData->m_TeamrankUuid;
		pSqlServer->BindBlob(4, TeamrankId.m_aData, sizeof(TeamrankId.m_aData));
		pSqlServer->BindString(5, pData->m_aGameUuid);
		pSqlServer->Print();
		int NumInserted;
		if(!pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize))
		{
			return false;
		}
	}
	return true;
}

bool CScoreWorker::ShowRank(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());

	char aServerLike[16];
	str_format(aServerLike, sizeof(aServerLike), "%%%s%%", pData->m_aServer);

	// check sort method
	char aBuf[600];
	str_format(aBuf, sizeof(aBuf),
		"SELECT Ranking, Time, PercentRank "
		"FROM ("
		"  SELECT RANK() OVER w AS Ranking, PERCENT_RANK() OVER w as PercentRank, MIN(Time) AS Time, Name "
		"  FROM %s_race "
		"  WHERE Map = ? "
		"  AND Server LIKE ? "
		"  GROUP BY Name "
		"  WINDOW w AS (ORDER BY MIN(Time))"
		") as a "
		"WHERE Name = ?",
		pSqlServer->GetPrefix());

	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, aServerLike);
	pSqlServer->BindString(3, pData->m_aName);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}

	char aRegionalRank[16];
	if(End)
	{
		str_copy(aRegionalRank, "unranked", sizeof(aRegionalRank));
	}
	else
	{
		str_format(aRegionalRank, sizeof(aRegionalRank), "rank %d", pSqlServer->GetInt(1));
	}

	const char *pAny = "%";

	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pAny);
	pSqlServer->BindString(3, pData->m_aName);

	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}

	if(!End)
	{
		int Rank = pSqlServer->GetInt(1);
		float Time = pSqlServer->GetFloat(2);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));

		if(g_Config.m_SvHideScore)
		{
			str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
				"Your time: %s", aBuf);
		}
		else
		{
			pResult->m_MessageKind = CScorePlayerResult::ALL;
			// CEIL and FLOOR are not supported in SQLite
			int BetterThanPercent = std::floor(100.0f - 100.0f * pSqlServer->GetFloat(3));

			if(str_comp_nocase(pData->m_aRequestingPlayer, pData->m_aName) == 0)
			{
				str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
					"%s - %s - better than %d%%",
					pData->m_aName, aBuf, BetterThanPercent);
			}
			else
			{
				str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
					"%s - %s - better than %d%% - requested by %s",
					pData->m_aName, aBuf, BetterThanPercent, pData->m_aRequestingPlayer);
			}

			if(g_Config.m_SvRegionalRankings)
			{
				str_format(pResult->m_Data.m_aaMessages[1], sizeof(pResult->m_Data.m_aaMessages[1]),
					"Global rank %d - %s %s",
					Rank, pData->m_aServer, aRegionalRank);
			}
			else
			{
				str_format(pResult->m_Data.m_aaMessages[1], sizeof(pResult->m_Data.m_aaMessages[1]),
					"Global rank %d", Rank);
			}
		}
	}
	else
	{
		str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
			"%s is not ranked", pData->m_aName);
	}
	return true;
}

bool CScoreWorker::ShowTeamRank(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());

	// check sort method
	char aBuf[2400];

	str_format(aBuf, sizeof(aBuf),
		"SELECT l.Id, Name, Time, Ranking, PercentRank "
		"FROM (" // teamrank score board
		"  SELECT RANK() OVER w AS Ranking, PERCENT_RANK() OVER w AS PercentRank, Id "
		"  FROM %s_teamrace "
		"  WHERE Map = ? "
		"  GROUP BY ID "
		"  WINDOW w AS (ORDER BY Min(Time))"
		") AS TeamRank INNER JOIN (" // select rank with Name in team
		"  SELECT ID "
		"  FROM %s_teamrace "
		"  WHERE Map = ? AND Name = ? "
		"  ORDER BY Time "
		"  LIMIT 1"
		") AS l ON TeamRank.Id = l.Id "
		"INNER JOIN %s_teamrace AS r ON l.Id = r.Id ",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pData->m_aMap);
	pSqlServer->BindString(3, pData->m_aName);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		float Time = pSqlServer->GetFloat(3);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));
		int Rank = pSqlServer->GetInt(4);
		// CEIL and FLOOR are not supported in SQLite
		int BetterThanPercent = std::floor(100.0f - 100.0f * pSqlServer->GetFloat(5));
		CTeamrank Teamrank;
		if(!Teamrank.NextSqlResult(pSqlServer, &End, pError, ErrorSize))
		{
			return false;
		}

		char aFormattedNames[512] = "";
		for(unsigned int Name = 0; Name < Teamrank.m_NumNames; Name++)
		{
			str_append(aFormattedNames, Teamrank.m_aaNames[Name]);

			if(Name < Teamrank.m_NumNames - 2)
				str_append(aFormattedNames, ", ");
			else if(Name < Teamrank.m_NumNames - 1)
				str_append(aFormattedNames, " & ");
		}

		if(g_Config.m_SvHideScore)
		{
			str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
				"Your team time: %s, better than %d%%", aBuf, BetterThanPercent);
		}
		else
		{
			pResult->m_MessageKind = CScorePlayerResult::ALL;
			str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
				"%d. %s Team time: %s, better than %d%%, requested by %s",
				Rank, aFormattedNames, aBuf, BetterThanPercent, pData->m_aRequestingPlayer);
		}
	}
	else
	{
		str_format(pResult->m_Data.m_aaMessages[0], sizeof(pResult->m_Data.m_aaMessages[0]),
			"%s has no team ranks", pData->m_aName);
	}
	return true;
}

bool CScoreWorker::ShowTop(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());

	int LimitStart = maximum(absolute(pData->m_Offset) - 1, 0);
	const char *pOrder = pData->m_Offset >= 0 ? "ASC" : "DESC";
	const char *pAny = "%";

	// check sort method
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT Name, Time, Ranking "
		"FROM ("
		"  SELECT RANK() OVER w AS Ranking, MIN(Time) AS Time, Name "
		"  FROM %s_race "
		"  WHERE Map = ? "
		"  AND Server LIKE ? "
		"  GROUP BY Name "
		"  WINDOW w AS (ORDER BY MIN(Time))"
		") as a "
		"ORDER BY Ranking %s "
		"LIMIT %d, ?",
		pSqlServer->GetPrefix(),
		pOrder,
		LimitStart);

	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pAny);
	pSqlServer->BindInt(3, 5);

	// show top
	int Line = 0;
	str_copy(pResult->m_Data.m_aaMessages[Line], "------------ Global Top ------------", sizeof(pResult->m_Data.m_aaMessages[Line]));
	Line++;

	char aTime[32];
	bool End = false;

	while(pSqlServer->Step(&End, pError, ErrorSize) && !End)
	{
		char aName[MAX_NAME_LENGTH];
		pSqlServer->GetString(1, aName, sizeof(aName));
		float Time = pSqlServer->GetFloat(2);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		int Rank = pSqlServer->GetInt(3);
		str_format(pResult->m_Data.m_aaMessages[Line], sizeof(pResult->m_Data.m_aaMessages[Line]),
			"%d. %s Time: %s", Rank, aName, aTime);

		Line++;
	}

	if(!g_Config.m_SvRegionalRankings)
	{
		str_copy(pResult->m_Data.m_aaMessages[Line], "-----------------------------------------", sizeof(pResult->m_Data.m_aaMessages[Line]));
		return End;
	}

	char aServerLike[16];
	str_format(aServerLike, sizeof(aServerLike), "%%%s%%", pData->m_aServer);

	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, aServerLike);
	pSqlServer->BindInt(3, 3);

	str_format(pResult->m_Data.m_aaMessages[Line], sizeof(pResult->m_Data.m_aaMessages[Line]),
		"------------ %s Top ------------", pData->m_aServer);
	Line++;

	// show top
	while(pSqlServer->Step(&End, pError, ErrorSize) && !End)
	{
		char aName[MAX_NAME_LENGTH];
		pSqlServer->GetString(1, aName, sizeof(aName));
		float Time = pSqlServer->GetFloat(2);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		int Rank = pSqlServer->GetInt(3);
		str_format(pResult->m_Data.m_aaMessages[Line], sizeof(pResult->m_Data.m_aaMessages[Line]),
			"%d. %s Time: %s", Rank, aName, aTime);
		Line++;
	}

	return End;
}

bool CScoreWorker::ShowTeamTop5(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	int LimitStart = maximum(absolute(pData->m_Offset) - 1, 0);
	const char *pOrder = pData->m_Offset >= 0 ? "ASC" : "DESC";
	const char *pAny = "%";

	// check sort method
	char aBuf[1024];

	str_format(aBuf, sizeof(aBuf),
		"SELECT Name, Time, Ranking, TeamSize "
		"FROM ("
		"  SELECT TeamSize, Ranking, Id, Server "
		"  FROM (" // teamrank score board
		"    SELECT RANK() OVER w AS Ranking, COUNT(*) AS Teamsize, Id, Server "
		"    FROM ("
		"      SELECT tr.Map, tr.Time, tr.Id, ("
		"        SELECT rr.Server FROM %s_race AS rr "
		"        WHERE rr.Map = tr.Map AND rr.Name = tr.Name AND rr.Time = tr.Time "
		"        LIMIT 1"
		"      ) AS Server "
		"      FROM %s_teamrace AS tr "
		"      WHERE tr.Map = ? "
		"    ) AS ll "
		"    GROUP BY ID "
		"    WINDOW w AS (ORDER BY Min(Time))"
		"  ) as l1 "
		"  WHERE Server LIKE ? "
		"  ORDER BY Ranking %s "
		"  LIMIT %d, ?"
		") as l2 "
		"INNER JOIN %s_teamrace as r ON l2.Id = r.Id "
		"ORDER BY Ranking %s, r.Id, Name ASC",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pOrder, LimitStart, pSqlServer->GetPrefix(), pOrder);
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pAny);
	pSqlServer->BindInt(3, 5);

	int Line = 0;
	str_copy(paMessages[Line++], "------- Team Top 5 -------", sizeof(paMessages[Line]));

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		if(!CTeamrank::GetSqlTop5Team(pSqlServer, &End, pError, ErrorSize, paMessages, &Line, 5))
		{
			return false;
		}
	}

	if(!g_Config.m_SvRegionalRankings)
	{
		str_copy(paMessages[Line], "-------------------------------", sizeof(paMessages[Line]));
		return true;
	}

	char aServerLike[16];
	str_format(aServerLike, sizeof(aServerLike), "%%%s%%", pData->m_aServer);

	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, aServerLike);
	pSqlServer->BindInt(3, 3);

	str_format(pResult->m_Data.m_aaMessages[Line], sizeof(pResult->m_Data.m_aaMessages[Line]),
		"----- %s Team Top -----", pData->m_aServer);
	Line++;

	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		if(!CTeamrank::GetSqlTop5Team(pSqlServer, &End, pError, ErrorSize, paMessages, &Line, 3))
		{
			return false;
		}
	}
	return true;
}

bool CScoreWorker::ShowPlayerTeamTop5(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	int LimitStart = maximum(absolute(pData->m_Offset) - 1, 0);
	const char *pOrder = pData->m_Offset >= 0 ? "ASC" : "DESC";

	// check sort method
	char aBuf[2400];

	str_format(aBuf, sizeof(aBuf),
		"SELECT l.Id, Name, Time, Ranking "
		"FROM (" // teamrank score board
		"  SELECT RANK() OVER w AS Ranking, Id "
		"  FROM %s_teamrace "
		"  WHERE Map = ? "
		"  GROUP BY ID "
		"  WINDOW w AS (ORDER BY Min(Time))"
		") AS TeamRank INNER JOIN (" // select rank with Name in team
		"  SELECT ID "
		"  FROM %s_teamrace "
		"  WHERE Map = ? AND Name = ? "
		"  ORDER BY Time %s "
		"  LIMIT %d, 5 "
		") AS l ON TeamRank.Id = l.Id "
		"INNER JOIN %s_teamrace AS r ON l.Id = r.Id "
		"ORDER BY Time %s, l.Id, Name ASC",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pOrder, LimitStart, pSqlServer->GetPrefix(), pOrder);
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, pData->m_aMap);
	pSqlServer->BindString(3, pData->m_aName);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		// show teamtop5
		int Line = 0;
		str_copy(paMessages[Line++], "------- Team Top 5 -------", sizeof(paMessages[Line]));

		for(Line = 1; Line < 6; Line++) // print
		{
			float Time = pSqlServer->GetFloat(3);
			str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));
			int Rank = pSqlServer->GetInt(4);
			CTeamrank Teamrank;
			bool Last;
			if(!Teamrank.NextSqlResult(pSqlServer, &Last, pError, ErrorSize))
			{
				return false;
			}

			char aFormattedNames[512] = "";
			for(unsigned int Name = 0; Name < Teamrank.m_NumNames; Name++)
			{
				str_append(aFormattedNames, Teamrank.m_aaNames[Name]);

				if(Name < Teamrank.m_NumNames - 2)
					str_append(aFormattedNames, ", ");
				else if(Name < Teamrank.m_NumNames - 1)
					str_append(aFormattedNames, " & ");
			}

			str_format(paMessages[Line], sizeof(paMessages[Line]), "%d. %s Team Time: %s",
				Rank, aFormattedNames, aBuf);
			if(Last)
			{
				Line++;
				break;
			}
		}
		str_copy(paMessages[Line], "---------------------------------", sizeof(paMessages[Line]));
	}
	else
	{
		if(pData->m_Offset == 0)
			str_format(paMessages[0], sizeof(paMessages[0]), "%s has no team ranks", pData->m_aName);
		else
			str_format(paMessages[0], sizeof(paMessages[0]), "%s has no team ranks in the specified range", pData->m_aName);
	}
	return true;
}

bool CScoreWorker::ShowTimes(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	int LimitStart = maximum(absolute(pData->m_Offset) - 1, 0);
	const char *pOrder = pData->m_Offset >= 0 ? "DESC" : "ASC";

	char aCurrentTimestamp[512];
	pSqlServer->ToUnixTimestamp("CURRENT_TIMESTAMP", aCurrentTimestamp, sizeof(aCurrentTimestamp));
	char aTimestamp[512];
	pSqlServer->ToUnixTimestamp("Timestamp", aTimestamp, sizeof(aTimestamp));
	char aBuf[512];
	if(pData->m_aName[0] != '\0') // last 5 times of a player
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Time, (%s-%s) as Ago, %s as Stamp, Server "
			"FROM %s_race "
			"WHERE Map = ? AND Name = ? "
			"ORDER BY Timestamp %s "
			"LIMIT ?, 5",
			aCurrentTimestamp, aTimestamp, aTimestamp,
			pSqlServer->GetPrefix(), pOrder);
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aMap);
		pSqlServer->BindString(2, pData->m_aName);
		pSqlServer->BindInt(3, LimitStart);
	}
	else // last 5 times of server
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Time, (%s-%s) as Ago, %s as Stamp, Server, Name "
			"FROM %s_race "
			"WHERE Map = ? "
			"ORDER BY Timestamp %s "
			"LIMIT ?, 5",
			aCurrentTimestamp, aTimestamp, aTimestamp,
			pSqlServer->GetPrefix(), pOrder);
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aMap);
		pSqlServer->BindInt(2, LimitStart);
	}

	// show top5
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(End)
	{
		str_copy(paMessages[0], "There are no times in the specified range", sizeof(paMessages[0]));
		return true;
	}

	str_copy(paMessages[0], "------------- Last Times -------------", sizeof(paMessages[0]));
	int Line = 1;

	do
	{
		float Time = pSqlServer->GetFloat(1);
		str_time_float(Time, ETimeFormat::HOURS_CENTISECS, aBuf, sizeof(aBuf));
		int Ago = pSqlServer->GetInt(2);
		int Stamp = pSqlServer->GetInt(3);
		char aServer[5];
		pSqlServer->GetString(4, aServer, sizeof(aServer));
		char aServerFormatted[8] = "\0";
		if(str_comp(aServer, "UNK") != 0)
			str_format(aServerFormatted, sizeof(aServerFormatted), "[%s] ", aServer);

		char aAgoString[40] = "\0";
		sqlstr::AgoTimeToString(Ago, aAgoString, sizeof(aAgoString));

		if(pData->m_aName[0] != '\0') // last 5 times of a player
		{
			if(Stamp == 0) // stamp is 00:00:00 cause it's an old entry from old times where there where no stamps yet
				str_format(paMessages[Line], sizeof(paMessages[Line]),
					"%s%s, don't know how long ago", aServerFormatted, aBuf);
			else
				str_format(paMessages[Line], sizeof(paMessages[Line]),
					"%s%s ago, %s", aServerFormatted, aAgoString, aBuf);
		}
		else // last 5 times of the server
		{
			char aName[MAX_NAME_LENGTH];
			pSqlServer->GetString(5, aName, sizeof(aName));
			if(Stamp == 0) // stamp is 00:00:00 cause it's an old entry from old times where there where no stamps yet
			{
				str_format(paMessages[Line], sizeof(paMessages[Line]),
					"%s%s, %s, don't know when", aServerFormatted, aName, aBuf);
			}
			else
			{
				str_format(paMessages[Line], sizeof(paMessages[Line]),
					"%s%s, %s ago, %s", aServerFormatted, aName, aAgoString, aBuf);
			}
		}
		Line++;
	} while(pSqlServer->Step(&End, pError, ErrorSize) && !End);
	if(!End)
	{
		return false;
	}
	str_copy(paMessages[Line], "-------------------------------------------", sizeof(paMessages[Line]));

	return true;
}

bool CScoreWorker::ShowPoints(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT ("
		"  SELECT COUNT(Name) + 1 FROM %s_points WHERE Points > ("
		"    SELECT Points FROM %s_points WHERE Name = ?"
		")) as Ranking, Points, Name "
		"FROM %s_points WHERE Name = ?",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aName);
	pSqlServer->BindString(2, pData->m_aName);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		int Rank = pSqlServer->GetInt(1);
		int Count = pSqlServer->GetInt(2);
		char aName[MAX_NAME_LENGTH];
		pSqlServer->GetString(3, aName, sizeof(aName));
		pResult->m_MessageKind = CScorePlayerResult::ALL;
		str_format(paMessages[0], sizeof(paMessages[0]),
			"%d. %s Points: %d, requested by %s",
			Rank, aName, Count, pData->m_aRequestingPlayer);
	}
	else
	{
		str_format(paMessages[0], sizeof(paMessages[0]),
			"%s has not collected any points so far", pData->m_aName);
	}
	return true;
}

bool CScoreWorker::ShowTopPoints(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	int LimitStart = maximum(pData->m_Offset - 1, 0);

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT RANK() OVER (ORDER BY a.Points DESC) as Ranking, Points, Name "
		"FROM ("
		"  SELECT Points, Name "
		"  FROM %s_points "
		"  ORDER BY Points DESC LIMIT ?"
		") as a "
		"ORDER BY Ranking ASC, Name ASC LIMIT ?, 5",
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindInt(1, LimitStart + 5);
	pSqlServer->BindInt(2, LimitStart);

	// show top points
	str_copy(paMessages[0], "-------- Top Points --------", sizeof(paMessages[0]));

	bool End = false;
	int Line = 1;
	while(pSqlServer->Step(&End, pError, ErrorSize) && !End)
	{
		int Rank = pSqlServer->GetInt(1);
		int Points = pSqlServer->GetInt(2);
		char aName[MAX_NAME_LENGTH];
		pSqlServer->GetString(3, aName, sizeof(aName));
		str_format(paMessages[Line], sizeof(paMessages[Line]),
			"%d. %s Points: %d", Rank, aName, Points);
		Line++;
	}
	if(!End)
	{
		return false;
	}
	str_copy(paMessages[Line], "-------------------------------", sizeof(paMessages[Line]));

	return true;
}

bool CScoreWorker::RandomMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlRandomMapRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScoreRandomMapResult *>(pGameData->m_pResult.get());

	char aBuf[512];
	if(in_range(pData->m_MinStars, 0, 5) && in_range(pData->m_MaxStars, 0, 5))
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Map FROM %s_maps "
			"WHERE Server = ? AND Map != ? AND Stars BETWEEN ? AND ? "
			"ORDER BY %s LIMIT 1",
			pSqlServer->GetPrefix(), pSqlServer->Random());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindInt(3, pData->m_MinStars);
		pSqlServer->BindInt(4, pData->m_MaxStars);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Map FROM %s_maps "
			"WHERE Server = ? AND Map != ? "
			"ORDER BY %s LIMIT 1",
			pSqlServer->GetPrefix(), pSqlServer->Random());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
	}
	pSqlServer->BindString(1, pData->m_aServerType);
	pSqlServer->BindString(2, pData->m_aCurrentMap);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		pSqlServer->GetString(1, pResult->m_aMap, sizeof(pResult->m_aMap));
	}
	else
	{
		str_copy(pResult->m_aMessage, "No maps found on this server!", sizeof(pResult->m_aMessage));
	}
	return true;
}

bool CScoreWorker::RandomUnfinishedMap(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlRandomMapRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScoreRandomMapResult *>(pGameData->m_pResult.get());

	char aBuf[512];
	if(in_range(pData->m_MinStars, 0, 5) && in_range(pData->m_MaxStars, 0, 5))
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Map "
			"FROM %s_maps "
			"WHERE Server = ? AND Map != ? AND Stars BETWEEN ? AND ? AND Map NOT IN ("
			"  SELECT Map "
			"  FROM %s_race "
			"  WHERE Name = ?"
			") ORDER BY %s "
			"LIMIT 1",
			pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->Random());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aServerType);
		pSqlServer->BindString(2, pData->m_aCurrentMap);
		pSqlServer->BindInt(3, pData->m_MinStars);
		pSqlServer->BindInt(4, pData->m_MaxStars);
		pSqlServer->BindString(5, pData->m_aRequestingPlayer);
	}
	else
	{
		str_format(aBuf, sizeof(aBuf),
			"SELECT Map "
			"FROM %s_maps AS maps "
			"WHERE Server = ? AND Map != ? AND Map NOT IN ("
			"  SELECT Map "
			"  FROM %s_race as race "
			"  WHERE Name = ?"
			") ORDER BY %s "
			"LIMIT 1",
			pSqlServer->GetPrefix(), pSqlServer->GetPrefix(), pSqlServer->Random());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aServerType);
		pSqlServer->BindString(2, pData->m_aCurrentMap);
		pSqlServer->BindString(3, pData->m_aRequestingPlayer);
	}

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		pSqlServer->GetString(1, pResult->m_aMap, sizeof(pResult->m_aMap));
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "%s has no more unfinished maps on this server!", pData->m_aRequestingPlayer);
		str_copy(pResult->m_aMessage, aBuf, sizeof(pResult->m_aMessage));
	}
	return true;
}

bool CScoreWorker::SaveTeam(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlTeamSaveData *>(pGameData);
	auto *pResult = dynamic_cast<CScoreSaveResult *>(pGameData->m_pResult.get());

	if(w == Write::NORMAL_SUCCEEDED)
	{
		// write succeeded on mysql server. delete from sqlite again
		char aBuf[128] = {0};
		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_saves_backup WHERE Code = ?",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGeneratedCode);
		bool End;
		return pSqlServer->Step(&End, pError, ErrorSize);
	}
	if(w == Write::NORMAL_FAILED)
	{
		char aBuf[256] = {0};
		bool End;
		// move to non-tmp table succeeded. delete from backup again
		str_format(aBuf, sizeof(aBuf),
			"INSERT INTO %s_saves SELECT * FROM %s_saves_backup WHERE Code = ?",
			pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGeneratedCode);
		if(!pSqlServer->Step(&End, pError, ErrorSize))
		{
			return false;
		}

		// move to non-tmp table succeeded. delete from backup again
		str_format(aBuf, sizeof(aBuf),
			"DELETE FROM %s_saves_backup WHERE Code = ?",
			pSqlServer->GetPrefix());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pData->m_aGeneratedCode);
		return pSqlServer->Step(&End, pError, ErrorSize);
	}

	char aSaveId[UUID_MAXSTRSIZE];
	FormatUuid(pResult->m_SaveId, aSaveId, UUID_MAXSTRSIZE);

	char *pSaveState = pResult->m_SavedTeam.GetString();
	char aBuf[65536];

	dbg_msg("score/dbg", "code=%s failure=%d", pData->m_aCode, (int)w);
	bool UseGeneratedCode = pData->m_aCode[0] == '\0' || w != Write::NORMAL;

	str_copy(pResult->m_aGeneratedCode, pData->m_aGeneratedCode);
	str_copy(pResult->m_aCode, UseGeneratedCode ? "" : pData->m_aCode);

	bool Retry = false;
	// two tries, first use the user provided code, then the autogenerated
	do
	{
		Retry = false;
		char aCode[128] = {0};
		if(UseGeneratedCode)
			str_copy(aCode, pData->m_aGeneratedCode, sizeof(aCode));
		else
			str_copy(aCode, pData->m_aCode, sizeof(aCode));

		str_format(aBuf, sizeof(aBuf),
			"%s INTO %s_saves%s(Savegame, Map, Code, Timestamp, Server, SaveId, DDNet7) "
			"VALUES (?, ?, ?, CURRENT_TIMESTAMP, ?, ?, %s)",
			pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(),
			w == Write::NORMAL ? "" : "_backup", pSqlServer->False());
		if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		{
			return false;
		}
		pSqlServer->BindString(1, pSaveState);
		pSqlServer->BindString(2, pData->m_aMap);
		pSqlServer->BindString(3, aCode);
		pSqlServer->BindString(4, pData->m_aServer);
		pSqlServer->BindString(5, aSaveId);
		pSqlServer->Print();
		int NumInserted;
		if(!pSqlServer->ExecuteUpdate(&NumInserted, pError, ErrorSize))
		{
			return false;
		}
		if(NumInserted == 1)
		{
			pResult->m_Status = CScoreSaveResult::SAVE_SUCCESS;
			if(UseGeneratedCode)
			{
				pResult->m_aCode[0] = '\0';
			}
			if(w != Write::NORMAL)
			{
				if(str_comp(pData->m_aServer, g_Config.m_SvSqlServerName) == 0)
				{
					pResult->m_aServer[0] = '\0';
				}
				pResult->m_Status = CScoreSaveResult::SAVE_FALLBACKFILE;
			}
		}
		else if(!UseGeneratedCode)
		{
			UseGeneratedCode = true;
			Retry = true;
		}
	} while(Retry);

	if(
		pResult->m_Status != CScoreSaveResult::SAVE_SUCCESS &&
		pResult->m_Status != CScoreSaveResult::SAVE_WARNING &&
		pResult->m_Status != CScoreSaveResult::SAVE_FALLBACKFILE)
	{
		dbg_msg("sql", "ERROR: This save-code already exists");
		pResult->m_Status = CScoreSaveResult::SAVE_FAILED;
		str_copy(pResult->m_aMessage, "This save-code already exists", sizeof(pResult->m_aMessage));
	}
	return true;
}

bool CScoreWorker::LoadTeam(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	if(w == Write::NORMAL_SUCCEEDED || w == Write::BACKUP_FIRST)
		return true;
	const auto *pData = dynamic_cast<const CSqlTeamLoadRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScoreSaveResult *>(pGameData->m_pResult.get());
	pResult->m_Status = CScoreSaveResult::LOAD_FAILED;

	char aCurrentTimestamp[512];
	pSqlServer->ToUnixTimestamp("CURRENT_TIMESTAMP", aCurrentTimestamp, sizeof(aCurrentTimestamp));
	char aTimestamp[512];
	pSqlServer->ToUnixTimestamp("Timestamp", aTimestamp, sizeof(aTimestamp));

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT Savegame, %s-%s AS Ago, SaveId "
		"FROM %s_saves "
		"where Code = ? AND Map = ? AND DDNet7 = %s",
		aCurrentTimestamp, aTimestamp,
		pSqlServer->GetPrefix(), pSqlServer->False());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aCode);
	pSqlServer->BindString(2, pData->m_aMap);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(End)
	{
		str_copy(pResult->m_aMessage, "No such savegame for this map", sizeof(pResult->m_aMessage));
		return true;
	}

	pResult->m_SaveId = UUID_NO_SAVE_ID;
	if(!pSqlServer->IsNull(3))
	{
		char aSaveId[UUID_MAXSTRSIZE];
		pSqlServer->GetString(3, aSaveId, sizeof(aSaveId));
		if(ParseUuid(&pResult->m_SaveId, aSaveId) || pResult->m_SaveId == UUID_NO_SAVE_ID)
		{
			str_copy(pResult->m_aMessage, "Unable to load savegame: SaveId corrupted", sizeof(pResult->m_aMessage));
			return true;
		}
	}

	char aSaveString[65536];
	pSqlServer->GetString(1, aSaveString, sizeof(aSaveString));
	int Num = pResult->m_SavedTeam.FromString(aSaveString);

	if(Num != 0)
	{
		str_copy(pResult->m_aMessage, "Unable to load savegame: data corrupted", sizeof(pResult->m_aMessage));
		return true;
	}

	bool Found = false;
	for(int i = 0; i < pResult->m_SavedTeam.GetMembersCount(); i++)
	{
		if(str_comp(pResult->m_SavedTeam.m_pSavedTees[i].GetName(), pData->m_aRequestingPlayer) == 0)
		{
			Found = true;
			break;
		}
	}
	if(!Found)
	{
		str_copy(pResult->m_aMessage, "This save exists, but you are not part of it. "
					      "Make sure you use the same name as you had when saving. "
					      "If you saved with an already used code, you get a new random save code, "
					      "check ddnet-saves.txt in config_directory.",
			sizeof(pResult->m_aMessage));
		return true;
	}

	int Since = pSqlServer->GetInt(2);
	if(Since < g_Config.m_SvSaveSwapGamesDelay)
	{
		str_format(pResult->m_aMessage, sizeof(pResult->m_aMessage),
			"You have to wait %d seconds until you can load this savegame",
			g_Config.m_SvSaveSwapGamesDelay - Since);
		return true;
	}

	bool CanLoad = pResult->m_SavedTeam.MatchPlayers(
		pData->m_aClientNames, pData->m_aClientId, pData->m_NumPlayer,
		pResult->m_aMessage, sizeof(pResult->m_aMessage));

	if(!CanLoad)
		return true;

	str_format(aBuf, sizeof(aBuf),
		"DELETE FROM %s_saves "
		"WHERE Code = ? AND Map = ? AND SaveId %s",
		pSqlServer->GetPrefix(),
		pResult->m_SaveId != UUID_NO_SAVE_ID ? "= ?" : "IS NULL");
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aCode);
	pSqlServer->BindString(2, pData->m_aMap);
	char aUuid[UUID_MAXSTRSIZE];
	if(pResult->m_SaveId != UUID_NO_SAVE_ID)
	{
		FormatUuid(pResult->m_SaveId, aUuid, sizeof(aUuid));
		pSqlServer->BindString(3, aUuid);
	}
	pSqlServer->Print();
	int NumDeleted;
	if(!pSqlServer->ExecuteUpdate(&NumDeleted, pError, ErrorSize))
	{
		return false;
	}

	if(NumDeleted != 1)
	{
		str_copy(pResult->m_aMessage, "Unable to load savegame: loaded on a different server", sizeof(pResult->m_aMessage));
		return true;
	}

	pResult->m_Status = CScoreSaveResult::LOAD_SUCCESS;
	str_copy(pResult->m_aMessage, "Loading successfully done", sizeof(pResult->m_aMessage));
	return true;
}

bool CScoreWorker::GetSaves(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlPlayerRequest *>(pGameData);
	auto *pResult = dynamic_cast<CScorePlayerResult *>(pGameData->m_pResult.get());
	auto *paMessages = pResult->m_Data.m_aaMessages;

	char aSaveLike[128] = "";
	str_append(aSaveLike, "%\n");
	sqlstr::EscapeLike(aSaveLike + str_length(aSaveLike),
		pData->m_aRequestingPlayer,
		sizeof(aSaveLike) - str_length(aSaveLike));
	str_append(aSaveLike, "\t%");

	char aCurrentTimestamp[512];
	pSqlServer->ToUnixTimestamp("CURRENT_TIMESTAMP", aCurrentTimestamp, sizeof(aCurrentTimestamp));
	char aMaxTimestamp[512];
	pSqlServer->ToUnixTimestamp("MAX(Timestamp)", aMaxTimestamp, sizeof(aMaxTimestamp));

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT COUNT(*) AS NumSaves, %s-%s AS Ago "
		"FROM %s_saves "
		"WHERE Map = ? AND Savegame LIKE ?",
		aCurrentTimestamp, aMaxTimestamp,
		pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	pSqlServer->BindString(1, pData->m_aMap);
	pSqlServer->BindString(2, aSaveLike);

	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
	{
		return false;
	}
	if(!End)
	{
		int NumSaves = pSqlServer->GetInt(1);
		char aLastSavedString[60] = "\0";
		if(!pSqlServer->IsNull(2))
		{
			int Ago = pSqlServer->GetInt(2);
			char aAgoString[40] = "\0";
			sqlstr::AgoTimeToString(Ago, aAgoString, sizeof(aAgoString));
			str_format(aLastSavedString, sizeof(aLastSavedString), ", last saved %s ago", aAgoString);
		}

		str_format(paMessages[0], sizeof(paMessages[0]),
			"%s has %d save%s on %s%s",
			pData->m_aRequestingPlayer,
			NumSaves, NumSaves == 1 ? "" : "s",
			pData->m_aMap, aLastSavedString);
	}
	return true;
}

bool CScoreWorker::AdminAddMoneyExp(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAdminMoneyExpRequest *>(pGameData);
	(void)w;
	// Ensure stats row exists
	char aEnsure[256];
	str_format(aEnsure, sizeof(aEnsure),
		"%s INTO %s_account_stats (AccountId, XP, Money, PlaytimeSeconds, Deaths, Kills, Combo) "
		"SELECT Id, 0, 0, 0, 0, 0, 0 FROM %s_accounts WHERE Username=?",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aEnsure, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aUsername);
	int Dummy = 0;
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"UPDATE %s_account_stats SET XP=XP+?, Money=Money+? "
		"WHERE AccountId=(SELECT Id FROM %s_accounts WHERE Username=? LIMIT 1)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_XP);
	pSqlServer->BindInt64(2, pData->m_Money);
	pSqlServer->BindString(3, pData->m_aUsername);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::AdminRemoveMoneyExp(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlAdminMoneyExpRequest *>(pGameData);
	(void)w;
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"UPDATE %s_account_stats SET "
		"XP=CASE WHEN XP>? THEN XP-? ELSE 0 END, "
		"Money=CASE WHEN Money>? THEN Money-? ELSE 0 END "
		"WHERE AccountId=(SELECT Id FROM %s_accounts WHERE Username=? LIMIT 1)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, pData->m_XP);
	pSqlServer->BindInt(2, pData->m_XP);
	pSqlServer->BindInt64(3, pData->m_Money);
	pSqlServer->BindInt64(4, pData->m_Money);
	pSqlServer->BindString(5, pData->m_aUsername);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::SetVip(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlSetVipRequest *>(pGameData);
	(void)w;
	const int64_t Now = time_timestamp();
	const int NewRole = (pData->m_DurationDays > 0) ? 1 : 0;
	const int64_t ExpiresAt = (pData->m_DurationDays > 0) ? (Now + (int64_t)pData->m_DurationDays * 86400LL) : 0;
	const int64_t GrantedAt = (pData->m_DurationDays > 0) ? Now : 0;

	// Ensure settings row exists
	char aEnsure[512];
	str_format(aEnsure, sizeof(aEnsure),
		"%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) "
		"SELECT Id, 0, 0, 0, 0, 1 FROM %s_accounts WHERE Username=?",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aEnsure, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aUsername);
	int Dummy = 0;
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	// Try to add VipGrantedAt and VipExpiresAt columns if they don't exist (idempotent)
	// We use a best-effort approach: ignore errors from ALTER TABLE
	{
		char aAlter1[256], aAlter2[256];
		str_format(aAlter1, sizeof(aAlter1), "ALTER TABLE %s_account_settings ADD COLUMN VipGrantedAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		str_format(aAlter2, sizeof(aAlter2), "ALTER TABLE %s_account_settings ADD COLUMN VipExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		char aDummy[256];
		// Ignore errors (column may already exist)
		pSqlServer->PrepareStatement(aAlter1, aDummy, sizeof(aDummy));
		pSqlServer->ExecuteUpdate(&Dummy, aDummy, sizeof(aDummy));
		pSqlServer->PrepareStatement(aAlter2, aDummy, sizeof(aDummy));
		pSqlServer->ExecuteUpdate(&Dummy, aDummy, sizeof(aDummy));
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"UPDATE %s_account_settings SET Role=?, VipGrantedAt=?, VipExpiresAt=? "
		"WHERE AccountId=(SELECT Id FROM %s_accounts WHERE Username=? LIMIT 1)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt(1, NewRole);
	pSqlServer->BindInt64(2, GrantedAt);
	pSqlServer->BindInt64(3, ExpiresAt);
	pSqlServer->BindString(4, pData->m_aUsername);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::SetJetpack(IDbConnection *pSqlServer, const ISqlData *pGameData, Write w, char *pError, int ErrorSize)
{
	const auto *pData = dynamic_cast<const CSqlSetJetpackRequest *>(pGameData);
	(void)w;
	if(!pData || !pData->m_aUsername[0] || pData->m_DurationDays < 0)
		return true;

	const int64_t Now = time_timestamp();
	const int64_t ExpiresAt = (pData->m_DurationDays > 0) ? (Now + (int64_t)pData->m_DurationDays * 86400LL) : 0;

	char aEnsure[512];
	str_format(aEnsure, sizeof(aEnsure),
		"%s INTO %s_account_settings (AccountId, Language, LangHintShown, LangManual, Role, AutoLogin) "
		"SELECT Id, 0, 0, 0, 0, 1 FROM %s_accounts WHERE Username=?",
		pSqlServer->InsertIgnore(), pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aEnsure, pError, ErrorSize))
		return false;
	pSqlServer->BindString(1, pData->m_aUsername);
	int Dummy = 0;
	if(!pSqlServer->ExecuteUpdate(&Dummy, pError, ErrorSize))
		return false;

	char aAlter[256];
	str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_settings ADD COLUMN JetpackExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
	{
		char aDummyErr[256];
		if(pSqlServer->PrepareStatement(aAlter, aDummyErr, sizeof(aDummyErr)))
			pSqlServer->ExecuteUpdate(&Dummy, aDummyErr, sizeof(aDummyErr));
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"UPDATE %s_account_settings SET JetpackExpiresAt=? "
		"WHERE AccountId=(SELECT Id FROM %s_accounts WHERE Username=? LIMIT 1)",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	pSqlServer->BindInt64(1, ExpiresAt);
	pSqlServer->BindString(2, pData->m_aUsername);
	int NumUpdated = 0;
	return pSqlServer->ExecuteUpdate(&NumUpdated, pError, ErrorSize);
}

bool CScoreWorker::GetVipList(IDbConnection *pSqlServer, const ISqlData *pGameData, char *pError, int ErrorSize)
{
	auto *pResult = dynamic_cast<CScoreVipListResult *>(pGameData->m_pResult.get());
	if(!pResult)
		return false;

	// Try to add columns if missing (best-effort)
	{
		char aAlter1[256], aAlter2[256];
		str_format(aAlter1, sizeof(aAlter1), "ALTER TABLE %s_account_settings ADD COLUMN VipGrantedAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		str_format(aAlter2, sizeof(aAlter2), "ALTER TABLE %s_account_settings ADD COLUMN VipExpiresAt BIGINT NOT NULL DEFAULT 0", pSqlServer->GetPrefix());
		char aDummy[256];
		int Dummy = 0;
		pSqlServer->PrepareStatement(aAlter1, aDummy, sizeof(aDummy));
		pSqlServer->ExecuteUpdate(&Dummy, aDummy, sizeof(aDummy));
		pSqlServer->PrepareStatement(aAlter2, aDummy, sizeof(aDummy));
		pSqlServer->ExecuteUpdate(&Dummy, aDummy, sizeof(aDummy));
	}

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SELECT a.Username, COALESCE(s.VipGrantedAt,0), COALESCE(s.VipExpiresAt,0) "
		"FROM %s_account_settings s "
		"JOIN %s_accounts a ON a.Id=s.AccountId "
		"WHERE s.Role=1 ORDER BY a.Username LIMIT 64",
		pSqlServer->GetPrefix(), pSqlServer->GetPrefix());
	if(!pSqlServer->PrepareStatement(aBuf, pError, ErrorSize))
		return false;
	bool End;
	if(!pSqlServer->Step(&End, pError, ErrorSize))
		return false;
	int Count = 0;
	while(!End && Count < CScoreVipListResult::MAX_ENTRIES)
	{
		pSqlServer->GetString(1, pResult->m_aEntries[Count].m_aUsername, sizeof(pResult->m_aEntries[Count].m_aUsername));
		pResult->m_aEntries[Count].m_GrantedAt = pSqlServer->GetInt64(2);
		pResult->m_aEntries[Count].m_ExpiresAt = pSqlServer->GetInt64(3);
		Count++;
		if(!pSqlServer->Step(&End, pError, ErrorSize))
			return false;
	}
	pResult->m_NumEntries = Count;
	return true;
}
