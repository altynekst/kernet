#ifndef GAME_CLIENT_COMPONENTS_DEVELOPER_IDENTITY_H
#define GAME_CLIENT_COMPONENTS_DEVELOPER_IDENTITY_H

#include <game/client/component.h>

#include <engine/shared/protocol.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class CHttpRequest;
typedef struct _json_value json_value;

class CDeveloperIdentity : public CComponent
{
	static constexpr int ROLE_DEVELOPER = 1 << 0;
	static constexpr int ROLE_TESTER = 1 << 1;
	static constexpr int ROLE_SUPPORTER = 1 << 2;

	struct SVerifiedDeveloper
	{
		int64_t m_ExpiresAt = 0;
		char m_aIdentityId[32] = "";
		char m_aName[MAX_NAME_LENGTH] = "";
		char m_aClan[MAX_CLAN_LENGTH] = "";
		int m_Roles = 0;
	};

	std::array<SVerifiedDeveloper, MAX_CLIENTS> m_aVerifiedDevelopers;
	std::shared_ptr<CHttpRequest> m_pSessionsRequest;
	std::shared_ptr<CHttpRequest> m_pChallengeRequest;
	std::shared_ptr<CHttpRequest> m_pActivateRequest;
	int64_t m_NextPollAt = 0;
	int64_t m_NextPublishAt = 0;
	uint8_t m_aSecretKey[64] = {};
	bool m_SecretKeyLoaded = false;
	bool m_SecretKeyLoadAttempted = false;

	void ResetState();
	void AbortRequests();
	bool LoadSecretKey();
	bool CurrentServer(char *pBuffer, int BufferSize) const;
	void StartSessionsRequest();
	void FinishSessionsRequest();
	void StartChallengeRequest();
	void FinishChallengeRequest();
	void FinishActivateRequest();
	bool ValidateSignedSession(const json_value &Session, const char *pCurrentServer);
	static bool ParseRoles(const json_value &Roles, int &RoleMask, char *pRoles, int RolesSize);
	static bool ValidateRoleCertificate(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t CertificateIssuedAt, int64_t CertificateExpiresAt, const char *pCertificateSignature);
	static std::string BuildCertificateMessage(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t IssuedAt, int64_t ExpiresAt);
	static std::string BuildSignedMessage(const char *pIdentityId, const char *pIdentityPublicKey, const char *pRoles, int64_t CertificateIssuedAt, int64_t CertificateExpiresAt, const char *pCertificateSignature, const char *pServer, int ClientId, const char *pName, const char *pClan, int64_t IssuedAt, int64_t ExpiresAt, const char *pNonce);
	bool IsVerifiedIdentity(int ClientId) const;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnUpdate() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnShutdown() override;
	bool GenerateKey(char *pError, int ErrorSize);
	bool GetPublicKey(char *pBuffer, int BufferSize) const;
	bool IsVerifiedDeveloper(int ClientId) const;
	bool IsVerifiedTester(int ClientId) const;
	bool IsVerifiedSupporter(int ClientId) const;
	const char *IdentityId(int ClientId) const;
};

#endif
