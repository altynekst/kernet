/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
#ifndef GAME_CLIENT_KOG_AI_CONTROLLER_H
#define GAME_CLIENT_KOG_AI_CONTROLLER_H

#include <memory>

#include <engine/console.h>

class CGameClient;
struct CNetObj_PlayerInput;

class CKoGAIController
{
public:
	explicit CKoGAIController(CGameClient *pGameClient);
	~CKoGAIController();

	void OnConsoleInit();
	void OnReset();

	// Returns true when AI has overwritten this tick's outgoing player input.
	bool ProcessInput(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &OrigInput, bool SaveInBlockActive);

private:
	class CImpl;

	CGameClient *m_pGameClient = nullptr;
	std::unique_ptr<CImpl> m_pImpl;

	static void ConStatus(IConsole::IResult *pResult, void *pUserData);
	static void ConSaveModel(IConsole::IResult *pResult, void *pUserData);
	static void ConLoadModel(IConsole::IResult *pResult, void *pUserData);
	static void ConExportDataset(IConsole::IResult *pResult, void *pUserData);
	static void ConTrainOffline(IConsole::IResult *pResult, void *pUserData);
	static void ConResetModel(IConsole::IResult *pResult, void *pUserData);
};

#endif
