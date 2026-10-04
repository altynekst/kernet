#include "developer_identity.h"

#include <base/mem.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/system.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/external/json-parser/json.h>
#include <engine/external/monocypher/monocypher-ed25519.h>
#include <engine/external/monocypher/monocypher.h>
#include <engine/shared/config.h>
#include <engine/shared/http.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cstdlib>

namespace
{
constexpr const char *ROOT_PUBLIC_KEY = "TpGBdJqvApGS0QYQLYyl3H+A2qo4G92p8RGQ5v6Wg3Y=";
constexpr int MAX_SIGNED_SESSION_SECONDS = 45;
constexpr int64_t MAX_ROLE_CERTIFICATE_SECONDS = 10LL * 365LL * 24LL * 60LL * 60LL;
constexpr int MAX_CLOCK_SKEW_SECONDS = 120;
constexpr int POLL_INTERVAL_SECONDS = 2;
constexpr int PUBLISH_INTERVAL_SECONDS = 5;
constexpr int RETRY_INTERVAL_SECONDS = 5;
constexpr int MAX_BASE64_DECODE_BUFFER_SIZE = 66;

bool DecodeBase64Exact(void *pOutput, int OutputSize, const char *pEncoded)
{
	if(OutputSize < 0 || OutputSize > MAX_BASE64_DECODE_BUFFER_SIZE)
		return false;

	uint8_t aDecoded[MAX_BASE64_DECODE_BUFFER_SIZE];
	const int DecodedSize = str_base64_decode(aDecoded, sizeof(aDecoded), pEncoded);
	const bool Valid = DecodedSize == OutputSize;
	if(Valid)
		mem_copy(pOutput, aDecoded, OutputSize);
	crypto_wipe(aDecoded, sizeof(aDecoded));
	return Valid;
}

const json_value *JsonField(const json_value &Object, const char *pName, json_type Type)
{
	const json_value *pValue = json_object_get(&Object, pName);
	return pValue != &json_value_none && pValue->type == Type ? pValue : nullptr;
}

const char *JsonString(const json_value &Object, const char *pName)
{
	const json_value *pValue = JsonField(Object, pName, json_string);
	return pValue ? pValue->u.string.ptr : nullptr;
}

bool JsonInteger(const json_value &Object, const char *pName, int64_t &Value)
{
	const json_value *pJsonValue = JsonField(Object, pName, json_integer);
	if(!pJsonValue)
		return false;
	Value = pJsonValue->u.integer;
	return true;
}

std::string Base64(const char *pValue)
{
	const int Length = str_length(pValue);
	std::string Encoded(((Length + 2) / 3) * 4 + 1, '\0');
	str_base64(Encoded.data(), static_cast<int>(Encoded.size()), pValue, Length);
	Encoded.resize(str_length(Encoded.c_str()));
	return Encoded;
}

int64_t MonotonicAfterSeconds(int Seconds)
{
	return time_get() + static_cast<int64_t>(Seconds) * time_freq();
}

std::string EndpointUrl(const char *pBaseUrl, const char *pPath)
{
	std::string Url(pBaseUrl);
	while(!Url.empty() && Url.back() == '/')
		Url.pop_back();
	Url += pPath;
	return Url;
}

bool TrustedRootPublicKey(uint8_t aPublicKey[32])
{
	return DecodeBase64Exact(aPublicKey, 32, ROOT_PUBLIC_KEY);
}
}

void CDeveloperIdentity::AbortRequests()
{
	for(std::shared_ptr<CHttpRequest> *ppRequest : {&m_pSessionsRequest, &m_pChallengeRequest, &m_pActivateRequest})
	{
		if(*ppRequest)
			(*ppRequest)->Abort();
		ppRequest->reset();
	}
}

void CDeveloperIdentity::ResetState()
{
	AbortRequests();
	m_aVerifiedDevelopers = {};
	m_NextPollAt = 0;
	m_NextPublishAt = 0;
}

bool CDeveloperIdentity::CurrentServer(char *pBuffer, int BufferSize) const
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;
	net_addr_str(&Client()->ServerAddress(), pBuffer, BufferSize, true);
	return pBuffer[0] != '\0';
}

std::string CDeveloperIdentity::BuildCertificateMessage(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t IssuedAt, int64_t ExpiresAt)
{
	char aMessage[768];
	str_format(aMessage, sizeof(aMessage),
		"kernelnet-role-v1\n"
		"identity_id=%s\n"
		"public_key=%s\n"
		"roles=%s\n"
		"issued_at=%lld\n"
		"expires_at=%lld",
		pIdentityId, pIdentityPublicKey, pRoles, static_cast<long long>(IssuedAt), static_cast<long long>(ExpiresAt));
	return aMessage;
}

std::string CDeveloperIdentity::BuildSignedMessage(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t CertificateIssuedAt, int64_t CertificateExpiresAt, const char *pCertificateSignature, const char *pServer, int ClientId, const char *pName, const char *pClan, int64_t IssuedAt, int64_t ExpiresAt, const char *pNonce)
{
	char aMessage[1536];
	str_format(aMessage, sizeof(aMessage),
		"kernelnet-session-v2\n"
		"identity_id=%s\n"
		"identity_public_key=%s\n"
		"roles=%s\n"
		"certificate_issued_at=%lld\n"
		"certificate_expires_at=%lld\n"
		"certificate_signature=%s\n"
		"server=%s\n"
		"client_id=%d\n"
		"name=%s\n"
		"clan=%s\n"
		"issued_at=%lld\n"
		"expires_at=%lld\n"
		"nonce=%s",
		pIdentityId, pIdentityPublicKey, pRoles, static_cast<long long>(CertificateIssuedAt), static_cast<long long>(CertificateExpiresAt), pCertificateSignature,
		Base64(pServer).c_str(), ClientId, Base64(pName).c_str(), Base64(pClan).c_str(),
		static_cast<long long>(IssuedAt), static_cast<long long>(ExpiresAt), pNonce);
	return aMessage;
}

bool CDeveloperIdentity::ParseRoles(const json_value &Roles, int &RoleMask, char *pRoles, int RolesSize)
{
	if(Roles.type != json_array || Roles.u.array.length == 0 || Roles.u.array.length > 3)
		return false;
	RoleMask = 0;
	pRoles[0] = '\0';
	constexpr const char *apRoleNames[] = {"developer", "tester", "supporter"};
	constexpr int aRoleBits[] = {ROLE_DEVELOPER, ROLE_TESTER, ROLE_SUPPORTER};
	int PreviousRoleIndex = -1;
	for(unsigned int Index = 0; Index < Roles.u.array.length; ++Index)
	{
		const json_value *pRole = Roles.u.array.values[Index];
		if(!pRole || pRole->type != json_string)
			return false;
		int RoleIndex = -1;
		for(int Candidate = 0; Candidate < 3; ++Candidate)
		{
			if(str_comp(pRole->u.string.ptr, apRoleNames[Candidate]) == 0)
				RoleIndex = Candidate;
		}
		if(RoleIndex <= PreviousRoleIndex)
			return false;
		if(pRoles[0] != '\0')
			str_append(pRoles, ",", RolesSize);
		str_append(pRoles, apRoleNames[RoleIndex], RolesSize);
		RoleMask |= aRoleBits[RoleIndex];
		PreviousRoleIndex = RoleIndex;
	}
	return true;
}

bool CDeveloperIdentity::ValidateRoleCertificate(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t CertificateIssuedAt, int64_t CertificateExpiresAt, const char *pCertificateSignature)
{
	const int64_t LocalTimestamp = time_timestamp();
	if(CertificateIssuedAt > LocalTimestamp + MAX_CLOCK_SKEW_SECONDS || CertificateExpiresAt <= LocalTimestamp ||
		CertificateExpiresAt <= CertificateIssuedAt || CertificateExpiresAt - CertificateIssuedAt > MAX_ROLE_CERTIFICATE_SECONDS)
		return false;
	uint8_t aRootPublicKey[32];
	uint8_t aIdentityPublicKey[32];
	uint8_t aCertificateSignature[64];
	if(!TrustedRootPublicKey(aRootPublicKey) ||
		!DecodeBase64Exact(aIdentityPublicKey, sizeof(aIdentityPublicKey), pIdentityPublicKey) ||
		!DecodeBase64Exact(aCertificateSignature, sizeof(aCertificateSignature), pCertificateSignature))
		return false;
	const std::string Message = BuildCertificateMessage(pIdentityId, pIdentityPublicKey, pRoles, CertificateIssuedAt, CertificateExpiresAt);
	const bool Valid = crypto_ed25519_check(aCertificateSignature, aRootPublicKey, reinterpret_cast<const uint8_t *>(Message.c_str()), Message.size()) == 0;
	crypto_wipe(aCertificateSignature, sizeof(aCertificateSignature));
	return Valid;
}

bool CDeveloperIdentity::LoadSecretKey()
{
	m_SecretKeyLoadAttempted = true;
	char *pEncodedKey = Storage()->ReadFileStr(g_Config.m_ClZzDevIdentityKeyFile, IStorage::TYPE_SAVE_OR_ABSOLUTE);
	if(!pEncodedKey)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "developer_identity", "Developer key file was not found; publishing is disabled");
		return false;
	}

	char *pTrimmed = str_skip_whitespaces(pEncodedKey);
	str_utf8_trim_right(pTrimmed);
	const bool Valid = DecodeBase64Exact(m_aSecretKey, sizeof(m_aSecretKey), pTrimmed);
	free(pEncodedKey);
	if(!Valid)
	{
		crypto_wipe(m_aSecretKey, sizeof(m_aSecretKey));
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "developer_identity", "Developer key file has an invalid format");
		return false;
	}

	m_SecretKeyLoaded = true;
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "developer_identity", "Identity signing key loaded");
	return true;
}

bool CDeveloperIdentity::GenerateKey(char *pError, int ErrorSize)
{
	uint8_t aSeed[32];
	uint8_t aSecretKey[64];
	uint8_t aPublicKey[32];
	secure_random_fill(aSeed, sizeof(aSeed));
	crypto_ed25519_key_pair(aSecretKey, aPublicKey, aSeed);

	char aEncodedKey[90];
	str_base64(aEncodedKey, sizeof(aEncodedKey), aSecretKey, sizeof(aSecretKey));
	IOHANDLE File = Storage()->OpenFile(g_Config.m_ClZzDevIdentityKeyFile, IOFLAG_WRITE, IStorage::TYPE_SAVE_OR_ABSOLUTE);
	if(!File)
	{
		str_copy(pError, "Unable to create the identity key file", ErrorSize);
		crypto_wipe(aSecretKey, sizeof(aSecretKey));
		crypto_wipe(aPublicKey, sizeof(aPublicKey));
		return false;
	}

	const int EncodedLength = str_length(aEncodedKey);
	const bool Success =
		io_write(File, aEncodedKey, EncodedLength) == static_cast<unsigned>(EncodedLength) &&
		io_write(File, "\n", 1) == 1;
	io_close(File);
	if(!Success)
	{
		str_copy(pError, "Unable to write the identity key file", ErrorSize);
		crypto_wipe(aSecretKey, sizeof(aSecretKey));
		crypto_wipe(aPublicKey, sizeof(aPublicKey));
		return false;
	}

	AbortRequests();
	mem_copy(m_aSecretKey, aSecretKey, sizeof(m_aSecretKey));
	m_SecretKeyLoaded = true;
	m_SecretKeyLoadAttempted = true;
	m_NextPollAt = 0;
	m_NextPublishAt = 0;
	pError[0] = '\0';
	crypto_wipe(aSecretKey, sizeof(aSecretKey));
	crypto_wipe(aPublicKey, sizeof(aPublicKey));
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "developer_identity", "New identity signing key created");
	return true;
}

bool CDeveloperIdentity::GetPublicKey(char *pBuffer, int BufferSize) const
{
	uint8_t aSecretKey[64];
	if(m_SecretKeyLoaded)
	{
		mem_copy(aSecretKey, m_aSecretKey, sizeof(aSecretKey));
	}
	else
	{
		char *pEncodedKey = Storage()->ReadFileStr(g_Config.m_ClZzDevIdentityKeyFile, IStorage::TYPE_SAVE_OR_ABSOLUTE);
		if(!pEncodedKey)
			return false;
		char *pTrimmed = str_skip_whitespaces(pEncodedKey);
		str_utf8_trim_right(pTrimmed);
		const bool Valid = DecodeBase64Exact(aSecretKey, sizeof(aSecretKey), pTrimmed);
		free(pEncodedKey);
		if(!Valid)
		{
			crypto_wipe(aSecretKey, sizeof(aSecretKey));
			return false;
		}
	}

	str_base64(pBuffer, BufferSize, aSecretKey + 32, 32);
	crypto_wipe(aSecretKey, sizeof(aSecretKey));
	return pBuffer[0] != '\0';
}

void CDeveloperIdentity::StartSessionsRequest()
{
	char aServer[NETADDR_MAXSTRSIZE];
	if(!CurrentServer(aServer, sizeof(aServer)))
		return;
	char aEscapedServer[NETADDR_MAXSTRSIZE * 3];
	EscapeUrl(aEscapedServer, aServer);
	const std::string Url = EndpointUrl(g_Config.m_ClZzDevIdentityUrl, "/v1/sessions?server=") + aEscapedServer;
	m_pSessionsRequest = HttpGet(Url.c_str());
	m_pSessionsRequest->MaxResponseSize(64 * 1024);
	m_pSessionsRequest->LogProgress(HTTPLOG::FAILURE);
	m_pSessionsRequest->Timeout(CTimeout{2000, 5000, 500, 5});
	Http()->Run(m_pSessionsRequest);
}

bool CDeveloperIdentity::ValidateSignedSession(const json_value &Session, const char *pCurrentServer)
{
	const char *pIdentityId = JsonString(Session, "identity_id");
	const char *pIdentityPublicKey = JsonString(Session, "identity_public_key");
	const json_value *pRolesValue = JsonField(Session, "roles", json_array);
	const char *pCertificateSignature = JsonString(Session, "certificate_signature");
	const char *pServer = JsonString(Session, "server");
	const char *pName = JsonString(Session, "name");
	const char *pClan = JsonString(Session, "clan");
	const char *pNonce = JsonString(Session, "nonce");
	const char *pMessage = JsonString(Session, "message");
	const char *pSignature = JsonString(Session, "signature");
	int64_t ClientIdValue;
	int64_t CertificateIssuedAt;
	int64_t CertificateExpiresAt;
	int64_t IssuedAt;
	int64_t ExpiresAt;
	if(!pIdentityId || !pIdentityPublicKey || !pRolesValue || !pCertificateSignature || !pServer || !pName || !pClan || !pNonce || !pMessage || !pSignature ||
		!JsonInteger(Session, "certificate_issued_at", CertificateIssuedAt) || !JsonInteger(Session, "certificate_expires_at", CertificateExpiresAt) ||
		!JsonInteger(Session, "client_id", ClientIdValue) || !JsonInteger(Session, "issued_at", IssuedAt) || !JsonInteger(Session, "expires_at", ExpiresAt))
		return false;
	int RoleMask = 0;
	char aRoles[32];
	if(!ParseRoles(*pRolesValue, RoleMask, aRoles, sizeof(aRoles)) ||
		!ValidateRoleCertificate(pIdentityId, pIdentityPublicKey, aRoles, CertificateIssuedAt, CertificateExpiresAt, pCertificateSignature))
		return false;
	if(ClientIdValue < 0 || ClientIdValue >= MAX_CLIENTS || str_comp(pServer, pCurrentServer) != 0)
		return false;
	const int64_t LocalTimestamp = time_timestamp();
	if(IssuedAt < LocalTimestamp - MAX_SIGNED_SESSION_SECONDS - MAX_CLOCK_SKEW_SECONDS || IssuedAt > LocalTimestamp + MAX_CLOCK_SKEW_SECONDS ||
		ExpiresAt <= LocalTimestamp - MAX_CLOCK_SKEW_SECONDS || ExpiresAt - IssuedAt > MAX_SIGNED_SESSION_SECONDS)
		return false;

	const int ClientId = static_cast<int>(ClientIdValue);
	const CGameClient::CClientData &ClientData = GameClient()->m_aClients[ClientId];
	if(!ClientData.m_Active || str_comp(ClientData.m_aName, pName) != 0 || str_comp(ClientData.m_aClan, pClan) != 0)
		return false;
	const std::string ExpectedMessage = BuildSignedMessage(pIdentityId, pIdentityPublicKey, aRoles, CertificateIssuedAt, CertificateExpiresAt, pCertificateSignature, pServer, ClientId, pName, pClan, IssuedAt, ExpiresAt, pNonce);
	if(ExpectedMessage != pMessage)
		return false;

	uint8_t aPublicKey[32];
	uint8_t aSignature[64];
	if(!DecodeBase64Exact(aPublicKey, sizeof(aPublicKey), pIdentityPublicKey) ||
		!DecodeBase64Exact(aSignature, sizeof(aSignature), pSignature))
		return false;
	const bool SignatureValid = crypto_ed25519_check(aSignature, aPublicKey, reinterpret_cast<const uint8_t *>(pMessage), str_length(pMessage)) == 0;
	crypto_wipe(aSignature, sizeof(aSignature));
	if(!SignatureValid)
		return false;

	SVerifiedDeveloper &Verified = m_aVerifiedDevelopers[ClientId];
	const int SignedLifetime = std::clamp(static_cast<int>(ExpiresAt - IssuedAt), 1, MAX_SIGNED_SESSION_SECONDS);
	Verified.m_ExpiresAt = MonotonicAfterSeconds(SignedLifetime);
	str_copy(Verified.m_aIdentityId, pIdentityId);
	str_copy(Verified.m_aName, pName);
	str_copy(Verified.m_aClan, pClan);
	Verified.m_Roles = RoleMask;
	return true;
}

void CDeveloperIdentity::FinishSessionsRequest()
{
	if(!m_pSessionsRequest || !m_pSessionsRequest->Done())
		return;
	std::shared_ptr<CHttpRequest> pRequest = std::move(m_pSessionsRequest);
	if(pRequest->State() != EHttpState::DONE || pRequest->StatusCode() != 200)
	{
		m_NextPollAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
		return;
	}

	json_value *pRoot = pRequest->ResultJson();
	if(!pRoot || pRoot->type != json_object)
	{
		if(pRoot)
			json_value_free(pRoot);
		m_NextPollAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
		return;
	}
	const json_value *pSessions = JsonField(*pRoot, "sessions", json_array);
	if(!pSessions)
	{
		json_value_free(pRoot);
		m_NextPollAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
		return;
	}

	char aServer[NETADDR_MAXSTRSIZE];
	if(CurrentServer(aServer, sizeof(aServer)))
	{
		m_aVerifiedDevelopers = {};
		for(unsigned int Index = 0; Index < pSessions->u.array.length; ++Index)
		{
			const json_value *pSession = pSessions->u.array.values[Index];
			if(pSession && pSession->type == json_object)
				ValidateSignedSession(*pSession, aServer);
		}
	}
	json_value_free(pRoot);
	m_NextPollAt = MonotonicAfterSeconds(POLL_INTERVAL_SECONDS);
}

void CDeveloperIdentity::StartChallengeRequest()
{
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	char aServer[NETADDR_MAXSTRSIZE];
	if(LocalId < 0 || LocalId >= MAX_CLIENTS || !CurrentServer(aServer, sizeof(aServer)))
		return;
	const CGameClient::CClientData &Local = GameClient()->m_aClients[LocalId];
	if(!Local.m_Active)
		return;

	CJsonStringWriter Json;
	Json.BeginObject();
	Json.WriteAttribute("identity_id");
	Json.WriteStrValue(g_Config.m_ClZzDevIdentityId);
	Json.WriteAttribute("server");
	Json.WriteStrValue(aServer);
	Json.WriteAttribute("client_id");
	Json.WriteIntValue(LocalId);
	Json.WriteAttribute("name");
	Json.WriteStrValue(Local.m_aName);
	Json.WriteAttribute("clan");
	Json.WriteStrValue(Local.m_aClan);
	Json.EndObject();
	const std::string Body = Json.GetOutputString();
	const std::string Url = EndpointUrl(g_Config.m_ClZzDevIdentityUrl, "/v1/challenge");
	m_pChallengeRequest = HttpPostJson(Url.c_str(), Body.c_str());
	m_pChallengeRequest->MaxResponseSize(16 * 1024);
	m_pChallengeRequest->LogProgress(HTTPLOG::FAILURE);
	m_pChallengeRequest->Timeout(CTimeout{2000, 5000, 500, 5});
	Http()->Run(m_pChallengeRequest);
}

void CDeveloperIdentity::FinishChallengeRequest()
{
	if(!m_pChallengeRequest || !m_pChallengeRequest->Done())
		return;
	std::shared_ptr<CHttpRequest> pRequest = std::move(m_pChallengeRequest);
	if(pRequest->State() != EHttpState::DONE || pRequest->StatusCode() != 200)
	{
		m_NextPublishAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
		return;
	}

	json_value *pRoot = pRequest->ResultJson();
	if(!pRoot || pRoot->type != json_object)
	{
		if(pRoot)
			json_value_free(pRoot);
		m_NextPublishAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
		return;
	}
	const char *pChallengeId = JsonString(*pRoot, "challenge_id");
	const char *pMessage = JsonString(*pRoot, "message");
	const char *pIdentityId = JsonString(*pRoot, "identity_id");
	const char *pIdentityPublicKey = JsonString(*pRoot, "identity_public_key");
	const json_value *pRolesValue = JsonField(*pRoot, "roles", json_array);
	const char *pCertificateSignature = JsonString(*pRoot, "certificate_signature");
	const char *pServer = JsonString(*pRoot, "server");
	const char *pName = JsonString(*pRoot, "name");
	const char *pClan = JsonString(*pRoot, "clan");
	const char *pNonce = JsonString(*pRoot, "nonce");
	int64_t ClientIdValue;
	int64_t CertificateIssuedAt;
	int64_t CertificateExpiresAt;
	int64_t IssuedAt;
	int64_t ExpiresAt;
	bool Valid = pChallengeId && pMessage && pIdentityId && pIdentityPublicKey && pRolesValue && pCertificateSignature && pServer && pName && pClan && pNonce &&
		JsonInteger(*pRoot, "certificate_issued_at", CertificateIssuedAt) && JsonInteger(*pRoot, "certificate_expires_at", CertificateExpiresAt) &&
		JsonInteger(*pRoot, "client_id", ClientIdValue) && JsonInteger(*pRoot, "issued_at", IssuedAt) &&
		JsonInteger(*pRoot, "expires_at", ExpiresAt);

	char aCurrentServer[NETADDR_MAXSTRSIZE];
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	Valid = Valid && LocalId >= 0 && LocalId < MAX_CLIENTS && ClientIdValue == LocalId && CurrentServer(aCurrentServer, sizeof(aCurrentServer));
	int RoleMask = 0;
	char aRoles[32] = "";
	if(Valid)
		Valid = ParseRoles(*pRolesValue, RoleMask, aRoles, sizeof(aRoles)) &&
			ValidateRoleCertificate(pIdentityId, pIdentityPublicKey, aRoles, CertificateIssuedAt, CertificateExpiresAt, pCertificateSignature);
	if(Valid)
	{
		const CGameClient::CClientData &Local = GameClient()->m_aClients[LocalId];
		const int64_t LocalTimestamp = time_timestamp();
		uint8_t aIdentityPublicKey[32];
		Valid = str_comp(pIdentityId, g_Config.m_ClZzDevIdentityId) == 0 && str_comp(pServer, aCurrentServer) == 0 &&
			str_comp(pName, Local.m_aName) == 0 && str_comp(pClan, Local.m_aClan) == 0 &&
			IssuedAt >= LocalTimestamp - MAX_SIGNED_SESSION_SECONDS - MAX_CLOCK_SKEW_SECONDS && IssuedAt <= LocalTimestamp + MAX_CLOCK_SKEW_SECONDS &&
			ExpiresAt > LocalTimestamp - MAX_CLOCK_SKEW_SECONDS && ExpiresAt - IssuedAt <= MAX_SIGNED_SESSION_SECONDS &&
			DecodeBase64Exact(aIdentityPublicKey, sizeof(aIdentityPublicKey), pIdentityPublicKey) &&
			mem_comp(aIdentityPublicKey, m_aSecretKey + 32, sizeof(aIdentityPublicKey)) == 0;
	}
	if(Valid)
		Valid = BuildSignedMessage(pIdentityId, pIdentityPublicKey, aRoles, CertificateIssuedAt, CertificateExpiresAt, pCertificateSignature, pServer, LocalId, pName, pClan, IssuedAt, ExpiresAt, pNonce) == pMessage;

	if(Valid)
	{
		uint8_t aSignature[64];
		char aEncodedSignature[128];
		crypto_ed25519_sign(aSignature, m_aSecretKey, reinterpret_cast<const uint8_t *>(pMessage), str_length(pMessage));
		str_base64(aEncodedSignature, sizeof(aEncodedSignature), aSignature, sizeof(aSignature));
		crypto_wipe(aSignature, sizeof(aSignature));

		CJsonStringWriter Json;
		Json.BeginObject();
		Json.WriteAttribute("challenge_id");
		Json.WriteStrValue(pChallengeId);
		Json.WriteAttribute("signature");
		Json.WriteStrValue(aEncodedSignature);
		Json.EndObject();
		const std::string Body = Json.GetOutputString();
		const std::string Url = EndpointUrl(g_Config.m_ClZzDevIdentityUrl, "/v1/activate");
		m_pActivateRequest = HttpPostJson(Url.c_str(), Body.c_str());
		m_pActivateRequest->MaxResponseSize(16 * 1024);
		m_pActivateRequest->LogProgress(HTTPLOG::FAILURE);
		m_pActivateRequest->Timeout(CTimeout{2000, 5000, 500, 5});
		Http()->Run(m_pActivateRequest);
		crypto_wipe(aEncodedSignature, sizeof(aEncodedSignature));
	}
	json_value_free(pRoot);
	if(!Valid)
		m_NextPublishAt = MonotonicAfterSeconds(RETRY_INTERVAL_SECONDS);
}

void CDeveloperIdentity::FinishActivateRequest()
{
	if(!m_pActivateRequest || !m_pActivateRequest->Done())
		return;
	const bool Success = m_pActivateRequest->State() == EHttpState::DONE && m_pActivateRequest->StatusCode() == 200;
	m_pActivateRequest.reset();
	m_NextPublishAt = MonotonicAfterSeconds(Success ? PUBLISH_INTERVAL_SECONDS : RETRY_INTERVAL_SECONDS);
}

void CDeveloperIdentity::OnInit()
{
	ResetState();
}

void CDeveloperIdentity::OnUpdate()
{
	FinishSessionsRequest();
	FinishChallengeRequest();
	FinishActivateRequest();
	if(Client()->State() != IClient::STATE_ONLINE || g_Config.m_ClZzDevIdentityUrl[0] == '\0')
		return;

	const int64_t Now = time_get();
	if(!m_pSessionsRequest && Now >= m_NextPollAt)
	{
		StartSessionsRequest();
		m_NextPollAt = MonotonicAfterSeconds(POLL_INTERVAL_SECONDS);
	}

	if(!g_Config.m_ClZzDevIdentityPublish)
	{
		m_SecretKeyLoadAttempted = false;
		return;
	}
	if(!m_SecretKeyLoaded && !m_SecretKeyLoadAttempted && !LoadSecretKey())
		return;
	if(m_SecretKeyLoaded && !m_pChallengeRequest && !m_pActivateRequest && Now >= m_NextPublishAt)
	{
		StartChallengeRequest();
		m_NextPublishAt = MonotonicAfterSeconds(PUBLISH_INTERVAL_SECONDS);
	}
}

void CDeveloperIdentity::OnStateChange(int NewState, int OldState)
{
	if(NewState != IClient::STATE_ONLINE)
		ResetState();
}

void CDeveloperIdentity::OnShutdown()
{
	AbortRequests();
	crypto_wipe(m_aSecretKey, sizeof(m_aSecretKey));
	m_SecretKeyLoaded = false;
}

bool CDeveloperIdentity::IsVerifiedIdentity(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	const SVerifiedDeveloper &Verified = m_aVerifiedDevelopers[ClientId];
	const CGameClient::CClientData &ClientData = GameClient()->m_aClients[ClientId];
	return Verified.m_ExpiresAt > time_get() && ClientData.m_Active &&
		str_comp(Verified.m_aName, ClientData.m_aName) == 0 && str_comp(Verified.m_aClan, ClientData.m_aClan) == 0;
}

bool CDeveloperIdentity::IsVerifiedDeveloper(int ClientId) const
{
	return IsVerifiedIdentity(ClientId) && (m_aVerifiedDevelopers[ClientId].m_Roles & ROLE_DEVELOPER) != 0;
}

bool CDeveloperIdentity::IsVerifiedTester(int ClientId) const
{
	return IsVerifiedIdentity(ClientId) && (m_aVerifiedDevelopers[ClientId].m_Roles & ROLE_TESTER) != 0;
}

bool CDeveloperIdentity::IsVerifiedSupporter(int ClientId) const
{
	return IsVerifiedIdentity(ClientId) && (m_aVerifiedDevelopers[ClientId].m_Roles & ROLE_SUPPORTER) != 0;
}

const char *CDeveloperIdentity::IdentityId(int ClientId) const
{
	return IsVerifiedIdentity(ClientId) ? m_aVerifiedDevelopers[ClientId].m_aIdentityId : "";
}
