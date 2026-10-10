/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more
 * information. */
/* If you are missing that file, acquire a complete release at teeworlds.com. */

#include "gameclient.h"

#include "components/background.h"
#include "components/binds.h"
#include "components/broadcast.h"
#include "components/camera.h"
#include "components/chat.h"
#include "components/console.h"
#include "components/controls.h"
#include "components/countryflags.h"
#include "components/damageind.h"
#include "components/debughud.h"
#include "components/effects.h"
#include "components/emoticon.h"
#include "components/freezebars.h"
#include "components/ghost.h"
#include "components/hud.h"
#include "components/infomessages.h"
#include "components/items.h"
#include "components/mapimages.h"
#include "components/maplayers.h"
#include "components/mapsounds.h"
#include "components/menu_background.h"
#include "components/menus.h"
#include "components/motd.h"
#include "components/nameplates.h"
#include "components/particles.h"
#include "components/players.h"
#include "components/race_demo.h"
#include "components/scoreboard.h"
#include "components/skins.h"
#include "components/skins7.h"
#include "components/sounds.h"
#include "components/spectator.h"
#include "components/statboard.h"
#include "components/tclient/zz_theme.h"
#include "components/tclient/zz_theme_runtime.h"
#include "components/voting.h"
#include "kog_ai_controller.h"
#include "lineinput.h"
#include "prediction/entities/character.h"
#include "prediction/entities/laser.h"
#include "prediction/entities/projectile.h"
#include "race.h"
#include "render.h"

#include <base/hash.h>
#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/net.h>
#include <base/system.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client/checksum.h>
#include <engine/client/enums.h>
#include <engine/demo.h>
#include <engine/discord.h>
#include <engine/editor.h>
#include <engine/engine.h>
#include <engine/favorites.h>
#include <engine/friends.h>
#include <engine/graphics.h>
#include <engine/map.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/csv.h>
#include <engine/shared/jsonwriter.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>
#include <engine/updater.h>

#include <generated/client_data.h>
#include <generated/client_data7.h>
#include <generated/protocol.h>
#include <generated/protocol7.h>
#include <generated/protocolglue.h>

#include <game/client/laser_data.h>
#include <game/client/projectile_data.h>
#include <game/localization.h>
#include <game/mapitems.h>
#include <game/version.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <random>

using namespace std::chrono_literals;

const char *CGameClient::Version() const { return GAME_VERSION; }

static void FentBotMakeUniqueDatasetFilename(CGameClient *pClient,
	const char *pFolder,
	const char *pMap, const char *pTag,
	char *pOut, int OutSize);

namespace
{
	bool DmdIsFreezeMapIndex(CCollision *pCollision, int MapIndex,
		const std::vector<SSwitchers> *pSwitchers, int Team, bool IncludeDeepFreeze)
	{
		if(MapIndex < 0)
			return false;
		auto IsFreezeTile = [IncludeDeepFreeze](int Tile) {
			return Tile == TILE_FREEZE ||
				(IncludeDeepFreeze && (Tile == TILE_DFREEZE || Tile == TILE_LFREEZE));
		};
		if(IsFreezeTile(pCollision->GetTileIndex(MapIndex)) ||
			IsFreezeTile(pCollision->GetFrontTileIndex(MapIndex)))
			return true;
		if(!IsFreezeTile(pCollision->GetSwitchType(MapIndex)))
			return false;
		const int SwitchNumber = pCollision->GetSwitchNumber(MapIndex);
		if(SwitchNumber == 0)
			return true;
		return pSwitchers && SwitchNumber >= 0 &&
		       SwitchNumber < (int)pSwitchers->size() && Team >= 0 &&
		       Team < NUM_DDRACE_TEAMS && (*pSwitchers)[SwitchNumber].m_aStatus[Team];
	}

	bool DmdIsFreezeAtPoint(CCollision *pCollision, const vec2 &Point,
		const std::vector<SSwitchers> *pSwitchers, int Team)
	{
		return DmdIsFreezeMapIndex(pCollision, pCollision->GetPureMapIndex(Point),
			pSwitchers, Team, false);
	}

	float DmdFreezeClearance(CCollision *pCollision, const vec2 &Point,
		const std::vector<SSwitchers> *pSwitchers, int Team)
	{
		constexpr float TileSize = 32.0f;
		float BestDistance = std::numeric_limits<float>::infinity();
		const int CenterX = (int)floorf(Point.x / TileSize);
		const int CenterY = (int)floorf(Point.y / TileSize);
		for(int TileY = CenterY - 2; TileY <= CenterY + 2; TileY++)
		{
			if(TileY < 0 || TileY >= pCollision->GetHeight())
				continue;
			for(int TileX = CenterX - 2; TileX <= CenterX + 2; TileX++)
			{
				if(TileX < 0 || TileX >= pCollision->GetWidth())
					continue;
				const int MapIndex = TileY * pCollision->GetWidth() + TileX;
				if(!DmdIsFreezeMapIndex(pCollision, MapIndex, pSwitchers, Team, true))
					continue;
				const vec2 Nearest(
					std::clamp(Point.x, TileX * TileSize, (TileX + 1) * TileSize),
					std::clamp(Point.y, TileY * TileSize, (TileY + 1) * TileSize));
				vec2 CollisionPos;
				vec2 BeforeCollision;
				if(distance(Point, Nearest) > 0.01f &&
					pCollision->IntersectLine(Point, Nearest, &CollisionPos, &BeforeCollision))
					continue;
				BestDistance = minimum(BestDistance, distance(Point, Nearest));
			}
		}
		return BestDistance;
	}

	bool DmdIsFreezeNearTee(CCollision *pCollision, const vec2 &Point,
		const std::vector<SSwitchers> *pSwitchers, int Team)
	{
		// This is only an early-warning sensor. The exact center-point check remains
		// authoritative when deciding whether a returning laser can unfreeze the tee.
		const float Radius = CCharacterCore::PhysicalSize() * 0.65f;
		const float Diagonal = Radius * 0.70710678f;
		const vec2 aOffsets[] = {
			vec2(0.0f, 0.0f),
			vec2(Radius, 0.0f),
			vec2(-Radius, 0.0f),
			vec2(0.0f, Radius),
			vec2(0.0f, -Radius),
			vec2(Diagonal, Diagonal),
			vec2(Diagonal, -Diagonal),
			vec2(-Diagonal, Diagonal),
			vec2(-Diagonal, -Diagonal),
		};
		for(const vec2 &Offset : aOffsets)
		{
			if(DmdIsFreezeAtPoint(pCollision, Point + Offset, pSwitchers, Team))
				return true;
		}
		return false;
	}

	bool DmdSegmentTouchesFreeze(CCollision *pCollision, const vec2 &From,
		const vec2 &To,
		const std::vector<SSwitchers> *pSwitchers,
		int Team)
	{
		const int Steps = std::clamp((int)ceilf(distance(From, To) / 16.0f), 1, 8);
		for(int StepIndex = 1; StepIndex <= Steps; StepIndex++)
		{
			if(DmdIsFreezeAtPoint(pCollision,
				   mix(From, To, (float)StepIndex / (float)Steps),
				   pSwitchers, Team))
				return true;
		}
		return false;
	}

	void DmdPredictCharacterPath(
		const CCharacterCore *pCore, int PredictedTicks, vec2 *pPosArray,
		CCollision *pCollision, bool StopInputOnFreeze = false,
		bool *pTouchesFreeze = nullptr,
		const std::vector<SSwitchers> *pSwitchers = nullptr, int Team = 0)
	{
		CCharacterCore PredictedCore = *pCore;
		PredictedCore.SetCoreWorld(nullptr, pCollision, nullptr);
		vec2 PreviousPos = PredictedCore.m_Pos;
		bool FreezeMovement = StopInputOnFreeze && PredictedCore.m_IsInFreeze;

		for(int i = 0; i < PredictedTicks; i++)
		{
			pPosArray[i] = PredictedCore.m_Pos;
			if(pTouchesFreeze)
				pTouchesFreeze[i] =
					DmdIsFreezeAtPoint(pCollision, PredictedCore.m_Pos, pSwitchers, Team);
			if(i + 1 >= PredictedTicks)
				break;

			if(FreezeMovement)
			{
				PredictedCore.m_Direction = 0;
				PredictedCore.m_Input.m_Direction = 0;
				PredictedCore.m_Input.m_Jump = 0;
				PredictedCore.m_Input.m_Hook = 0;
				PredictedCore.SetHookedPlayer(-1);
				PredictedCore.m_HookState = HOOK_IDLE;
				PredictedCore.m_HookPos = PredictedCore.m_Pos;
			}
			PredictedCore.Tick(false, false);
			PredictedCore.Move();
			PredictedCore.Quantize();
			const bool TouchesFreeze =
				(StopInputOnFreeze || pTouchesFreeze) &&
				DmdSegmentTouchesFreeze(pCollision, PreviousPos, PredictedCore.m_Pos,
					pSwitchers, Team);
			if(StopInputOnFreeze && TouchesFreeze)
				FreezeMovement = true;
			PreviousPos = PredictedCore.m_Pos;
		}
	}

	// Lightweight physics sandbox used for broad candidate searches. It copies
	// only character cores within hook range and never allocates projectiles,
	// lasers, pickups or render/prediction entities. Promising candidates are
	// still verified with a complete CGameWorld before input is overridden.
	class CDmdFastCoreWorld
	{
		CWorldCore m_World;
		CTeamsCore m_Teams;
		CCharacterCore m_aCores[MAX_CLIENTS];
		bool m_aActive[MAX_CLIENTS] = {};
		int m_LocalId;

	public:
		CDmdFastCoreWorld(CGameWorld &Source, CCollision *pCollision, int LocalId) :
			m_Teams(Source.m_Teams),
			m_LocalId(LocalId)
		{
			CCharacter *pLocal = Source.GetCharacterById(LocalId);
			if(!pLocal)
				return;
			const float CopyRadius =
				(float)pLocal->Core()->m_Tuning.m_HookLength + 96.0f;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				CCharacter *pCharacter = Source.GetCharacterById(ClientId);
				if(!pCharacter ||
					(ClientId != LocalId &&
						distance(pLocal->Core()->m_Pos,
							pCharacter->Core()->m_Pos) > CopyRadius &&
						pLocal->Core()->HookedPlayer() != ClientId))
					continue;
				m_aCores[ClientId] = *pCharacter->Core();
				m_aActive[ClientId] = true;
				m_World.m_apCharacters[ClientId] = &m_aCores[ClientId];
			}
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(m_aActive[ClientId])
					m_aCores[ClientId].SetCoreWorld(&m_World, pCollision, &m_Teams);
			}
		}

		CCharacterCore *LocalCore()
		{
			return m_LocalId >= 0 && m_LocalId < MAX_CLIENTS &&
				m_aActive[m_LocalId] ?
				&m_aCores[m_LocalId] : nullptr;
		}

		void Step(const CNetObj_PlayerInput &Input)
		{
			CCharacterCore *pLocal = LocalCore();
			if(!pLocal)
				return;
			pLocal->m_Input = Input;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(m_aActive[ClientId])
					m_aCores[ClientId].Tick(true, false);
			}
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(m_aActive[ClientId])
					m_aCores[ClientId].TickDeferred();
			}
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(!m_aActive[ClientId])
					continue;
				m_aCores[ClientId].Move();
				m_aCores[ClientId].Quantize();
			}
		}
	};

} // namespace

void CGameClient::ConZzFentBotSavePlrStart(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pTag = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(pClient->m_FentBotSavePlrActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"SavePlr recording already active");
		return;
	}
	if(!pClient->m_Snap.m_pLocalCharacter)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Not in game");
		return;
	}
	if(!pClient->Storage()->CreateFolder("fentbot", IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/saveplr",
			IStorage::TYPE_SAVE))
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to create fentbot/saveplr folder");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	FentBotMakeUniqueDatasetFilename(pClient, "fentbot/saveplr",
		pClient->Map()->BaseName(), pTag, aFilename,
		sizeof(aFilename));
	IOHANDLE Handle = pClient->Storage()->OpenFile(aFilename, IOFLAG_WRITE,
		IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open saveplr record file");
		return;
	}

	io_write(Handle, "FENTBC01", 8);
	int Version = 1;
	io_write(Handle, &Version, (int)sizeof(Version));
	int ObsDim = FENTBOT_OBS_DIM;
	io_write(Handle, &ObsDim, (int)sizeof(ObsDim));
	int Actions = FENTBOT_ACTIONS;
	io_write(Handle, &Actions, (int)sizeof(Actions));

	pClient->m_FentBotSavePlrHandle = Handle;
	pClient->m_FentBotSavePlrActive = true;
	pClient->m_FentBotSavePlrWritten = 0;
	{
		char aMsg[512];
		str_format(aMsg, sizeof(aMsg), "SavePlr recording started: %s", aFilename);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
	}
}

void CGameClient::ConZzFentBotSavePlrStop(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	(void)pResult;
	if(!pClient->m_FentBotSavePlrActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"SavePlr recording is not active");
		return;
	}
	if(pClient->m_FentBotSavePlrHandle)
		io_close(pClient->m_FentBotSavePlrHandle);
	pClient->m_FentBotSavePlrHandle = 0;
	pClient->m_FentBotSavePlrActive = false;
	char aMsg[256];
	str_format(aMsg, sizeof(aMsg), "SavePlr recording stopped (samples=%llu)",
		(unsigned long long)pClient->m_FentBotSavePlrWritten);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

void CGameClient::ConZzFentBotSave10Sec(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pTag = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(!pClient->m_Snap.m_pLocalCharacter)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Not in game");
		return;
	}
	if(pClient->m_vFentBotRecent.empty() || pClient->m_FentBotRecentCount <= 0)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"No recent samples yet");
		return;
	}
	if(!pClient->Storage()->CreateFolder("fentbot", IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/savedin10sec",
			IStorage::TYPE_SAVE))
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to create fentbot/savedin10sec folder");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	FentBotMakeUniqueDatasetFilename(pClient, "fentbot/savedin10sec",
		pClient->Map()->BaseName(), pTag, aFilename,
		sizeof(aFilename));
	IOHANDLE Handle = pClient->Storage()->OpenFile(aFilename, IOFLAG_WRITE,
		IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open savedin10sec file");
		return;
	}

	io_write(Handle, "FENTBC01", 8);
	int Version = 1;
	io_write(Handle, &Version, (int)sizeof(Version));
	int ObsDim = FENTBOT_OBS_DIM;
	io_write(Handle, &ObsDim, (int)sizeof(ObsDim));
	int Actions = FENTBOT_ACTIONS;
	io_write(Handle, &Actions, (int)sizeof(Actions));

	const int Tps = std::max(1, pClient->Client()->GameTickSpeed());
	const int Want =
		std::clamp(Tps * 10, 1, (int)pClient->m_vFentBotRecent.size());
	const int N = std::min(pClient->m_FentBotRecentCount, Want);
	const int Cap = (int)pClient->m_vFentBotRecent.size();
	const int Start = (pClient->m_FentBotRecentHead - N + Cap) % Cap;
	for(int i = 0; i < N; i++)
	{
		const int idx = (Start + i) % Cap;
		const int A = pClient->m_vFentBotRecent[idx].m_Action;
		io_write(Handle, &A, (int)sizeof(A));
		io_write(
			Handle, pClient->m_vFentBotRecent[idx].m_Obs.data(),
			(int)(pClient->m_vFentBotRecent[idx].m_Obs.size() * sizeof(float)));
	}
	io_close(Handle);
	char aMsg[512];
	str_format(aMsg, sizeof(aMsg), "Saved last 10 sec (samples=%d): %s", N,
		aFilename);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

bool CGameClient::FentBotPyInferAction(int DummyIndex, const vec2 &LocalPos,
	const vec2 &Vel, const vec2 &Goal,
	int &OutAction)
{
	OutAction = -1;
	if(g_Config.m_ClZzFentBotPyInfer == 0)
		return false;

	// Build observation (same layout as in-client PPO/BC infer)
	auto EncodeTile = [&](int T) -> float {
		if(T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE)
			return 2.0f / 3.0f;
		if(T == TILE_DEATH)
			return 1.0f;
		if(T == TILE_SOLID || T == TILE_NOHOOK)
			return 1.0f / 3.0f;
		return 0.0f;
	};
	auto AppendNearestPlayers = [&](std::array<float, FENTBOT_OBS_DIM> &Obs,
					    int &o) {
		struct SCand
		{
			float m_D2;
			int m_Id;
		};
		SCand aCands[MAX_CLIENTS];
		int Num = 0;
		const float Radius = (float)(FENTBOT_PLAYER_RADIUS_TILES * 32);
		const float R2 = Radius * Radius;
		const int LocalId = m_Snap.m_LocalClientId;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == LocalId)
				continue;
			if(!m_Snap.m_aCharacters[i].m_Active)
				continue;
			const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
			const vec2 d = P - LocalPos;
			const float d2 = dot(d, d);
			if(d2 > R2)
				continue;
			aCands[Num++] = {d2, i};
		}
		std::sort(aCands, aCands + Num,
			[](const SCand &A, const SCand &B) { return A.m_D2 < B.m_D2; });
		for(int k = 0; k < FENTBOT_PLAYERS_K; k++)
		{
			if(k < Num)
			{
				const int Id = aCands[k].m_Id;
				const vec2 P = m_aClients[Id].m_RegularPredicted.m_Pos;
				const vec2 V = m_aClients[Id].m_RegularPredicted.m_Vel;
				const vec2 d = P - LocalPos;
				Obs[o++] = d.x / 1000.0f;
				Obs[o++] = d.y / 1000.0f;
				Obs[o++] = V.x / 1000.0f;
				Obs[o++] = V.y / 1000.0f;
			}
			else
			{
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
			}
		}
	};

	std::array<float, FENTBOT_OBS_DIM> Obs;
	const vec2 dd = Goal - LocalPos;
	Obs[0] = dd.x / 1000.0f;
	Obs[1] = dd.y / 1000.0f;
	Obs[2] = Vel.x / 1000.0f;
	Obs[3] = Vel.y / 1000.0f;
	const int W = Collision()->GetWidth();
	const int H = Collision()->GetHeight();
	const int Center = Collision()->GetPureMapIndex(LocalPos);
	int cx = 0;
	int cy = 0;
	if(Center >= 0)
	{
		cx = Center % W;
		cy = Center / W;
	}
	int o = 4;
	for(int dy = -FENTBOT_GRID_R; dy <= FENTBOT_GRID_R; dy++)
	{
		for(int dx = -FENTBOT_GRID_R; dx <= FENTBOT_GRID_R; dx++)
		{
			const int tx = std::clamp(cx + dx, 0, std::max(0, W - 1));
			const int ty = std::clamp(cy + dy, 0, std::max(0, H - 1));
			const int Idx = ty * W + tx;
			Obs[o++] = EncodeTile(Collision()->GetTileIndex(Idx));
			Obs[o++] = EncodeTile(Collision()->GetFrontTileIndex(Idx));
		}
	}
	AppendNearestPlayers(Obs, o);

	NETADDR Addr;
	if(net_addr_from_str(&Addr, g_Config.m_ClZzFentBotPyAddr) != 0)
		return false;

	const int64_t Now = time_get();
	if(!m_FentBotPySock || !m_FentBotPyConnected)
	{
		// Throttle reconnect attempts.
		if(m_FentBotPyLastConnectAttempt != 0 &&
			Now - m_FentBotPyLastConnectAttempt < time_freq() / 2)
			return false;
		m_FentBotPyLastConnectAttempt = Now;

		if(m_FentBotPySock)
		{
			net_tcp_close(m_FentBotPySock);
			m_FentBotPySock = nullptr;
		}

		NETADDR Bind = NETADDR_ZEROED;
		Bind.type = Addr.type;
		Bind.port = 0;
		m_FentBotPySock = net_tcp_create(Bind);
		if(!m_FentBotPySock)
			return false;

		// Non-blocking connect.
		(void)net_tcp_connect_non_blocking(m_FentBotPySock, Addr);
		m_FentBotPyConnected = true;
	}

	// Send request.
	static const char s_aMagic[8] = {'F', 'E', 'N', 'T', 'P', 'Y', '0', '1'};
	int32_t ObsDim = (int32_t)Obs.size();
	if(net_tcp_send(m_FentBotPySock, s_aMagic, (int)sizeof(s_aMagic)) < 0 ||
		net_tcp_send(m_FentBotPySock, &ObsDim, (int)sizeof(ObsDim)) < 0 ||
		net_tcp_send(m_FentBotPySock, Obs.data(),
			(int)(Obs.size() * sizeof(float))) < 0)
	{
		net_tcp_close(m_FentBotPySock);
		m_FentBotPySock = nullptr;
		m_FentBotPyConnected = false;
		return false;
	}

	// Receive response with timeout.
	const int TimeoutMs = std::clamp(g_Config.m_ClZzFentBotPyTimeoutMs, 0, 50);
	if(TimeoutMs > 0)
		(void)net_socket_read_wait(m_FentBotPySock,
			std::chrono::milliseconds(TimeoutMs));

	int32_t Action = -1;
	const int Recv = net_tcp_recv(m_FentBotPySock, &Action, (int)sizeof(Action));
	if(Recv != (int)sizeof(Action))
	{
		if(Recv < 0)
		{
			net_tcp_close(m_FentBotPySock);
			m_FentBotPySock = nullptr;
			m_FentBotPyConnected = false;
		}
		return false;
	}

	if(Action < 0 || Action >= FENTBOT_ACTIONS)
		return false;
	OutAction = (int)Action;
	return true;
}

bool CGameClient::CompanionPyInferAction(int DummyIndex, const vec2 &DummyPos,
	const vec2 &DummyVel, const vec2 &Goal,
	int &OutAction)
{
	OutAction = -1;
	if(g_Config.m_ClZzCompanionPyInfer == 0)
		return false;

	static int64_t s_DbgSpamWindowStart = 0;
	static int s_DbgSpamWindowCount = 0;
	auto DbgSpam = [&](const char *pMsg) {
		const int64_t Now = time_get();
		if(s_DbgSpamWindowStart == 0 ||
			Now - s_DbgSpamWindowStart >= time_freq())
		{
			s_DbgSpamWindowStart = Now;
			s_DbgSpamWindowCount = 0;
		}
		if(s_DbgSpamWindowCount >= 4)
			return;
		s_DbgSpamWindowCount++;
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/companion", pMsg);
	};
	auto DbgImportant = [&](const char *pMsg) {
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/companion", pMsg);
	};
	auto TypeStr = [&](int Type) {
		if((Type & NETTYPE_IPV4) != 0)
			return "ipv4";
		if((Type & NETTYPE_IPV6) != 0)
			return "ipv6";
		return "invalid";
	};

	// Build observation (same layout as FentBot)
	auto EncodeTile = [&](int T) -> float {
		if(T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE)
			return 2.0f / 3.0f;
		if(T == TILE_DEATH)
			return 1.0f;
		if(T == TILE_SOLID || T == TILE_NOHOOK)
			return 1.0f / 3.0f;
		return 0.0f;
	};
	auto AppendNearestPlayers = [&](std::array<float, FENTBOT_OBS_DIM> &Obs,
					    int &o) {
		struct SCand
		{
			float m_D2;
			int m_Id;
		};
		SCand aCands[MAX_CLIENTS];
		int Num = 0;
		const float Radius = (float)(FENTBOT_PLAYER_RADIUS_TILES * 32);
		const float R2 = Radius * Radius;
		const int LocalId = m_Snap.m_LocalClientId;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == LocalId)
				continue;
			if(!m_Snap.m_aCharacters[i].m_Active)
				continue;
			const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
			const vec2 d = P - DummyPos;
			const float d2 = dot(d, d);
			if(d2 > R2)
				continue;
			aCands[Num++] = {d2, i};
		}
		std::sort(aCands, aCands + Num,
			[](const SCand &A, const SCand &B) { return A.m_D2 < B.m_D2; });
		for(int k = 0; k < FENTBOT_PLAYERS_K; k++)
		{
			if(k < Num)
			{
				const int Id = aCands[k].m_Id;
				const vec2 P = m_aClients[Id].m_RegularPredicted.m_Pos;
				const vec2 V = m_aClients[Id].m_RegularPredicted.m_Vel;
				const vec2 d = P - DummyPos;
				Obs[o++] = d.x / 1000.0f;
				Obs[o++] = d.y / 1000.0f;
				Obs[o++] = V.x / 1000.0f;
				Obs[o++] = V.y / 1000.0f;
			}
			else
			{
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
				Obs[o++] = 0.0f;
			}
		}
	};

	std::array<float, FENTBOT_OBS_DIM> Obs;
	const vec2 dd = Goal - DummyPos;
	Obs[0] = dd.x / 1000.0f;
	Obs[1] = dd.y / 1000.0f;
	Obs[2] = DummyVel.x / 1000.0f;
	Obs[3] = DummyVel.y / 1000.0f;
	const int W = Collision()->GetWidth();
	const int H = Collision()->GetHeight();
	const int Center = Collision()->GetPureMapIndex(DummyPos);
	int cx = 0;
	int cy = 0;
	if(Center >= 0)
	{
		cx = Center % W;
		cy = Center / W;
	}
	int o = 4;
	for(int dy = -FENTBOT_GRID_R; dy <= FENTBOT_GRID_R; dy++)
	{
		for(int dx = -FENTBOT_GRID_R; dx <= FENTBOT_GRID_R; dx++)
		{
			const int tx = std::clamp(cx + dx, 0, std::max(0, W - 1));
			const int ty = std::clamp(cy + dy, 0, std::max(0, H - 1));
			const int Idx = ty * W + tx;
			Obs[o++] = EncodeTile(Collision()->GetTileIndex(Idx));
			Obs[o++] = EncodeTile(Collision()->GetFrontTileIndex(Idx));
		}
	}
	AppendNearestPlayers(Obs, o);

	NETADDR Addr;
	char aAddr[128];
	str_copy(aAddr, g_Config.m_ClZzCompanionPyAddr, sizeof(aAddr));
	char *pAddrTrim = str_skip_whitespaces(aAddr);
	char *pEnd = pAddrTrim + str_length(pAddrTrim);
	while(pEnd > pAddrTrim && str_isspace(pEnd[-1]))
		*--pEnd = '\0';
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "py_addr='%s'", pAddrTrim);
		DbgSpam(aMsg);
	}
	if(net_addr_from_str(&Addr, pAddrTrim) != 0)
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "invalid cl_zz_companion_py_addr: '%s'",
			pAddrTrim);
		DbgImportant(aMsg);
		return false;
	}
	{
		char aMsg[256];
		char aNetAddr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aNetAddr, sizeof(aNetAddr), true);
		str_format(aMsg, sizeof(aMsg), "parsed_addr=%s type=%s", aNetAddr,
			TypeStr(Addr.type));
		DbgSpam(aMsg);
	}

	const int64_t Now = time_get();
	if(!m_CompanionPySock || !m_CompanionPyConnected)
	{
		if(m_CompanionPyLastConnectAttempt != 0 &&
			Now - m_CompanionPyLastConnectAttempt < time_freq() / 2)
			return false;
		m_CompanionPyLastConnectAttempt = Now;

		if(m_CompanionPySock)
		{
			net_tcp_close(m_CompanionPySock);
			m_CompanionPySock = nullptr;
		}

		NETADDR Bind = NETADDR_ZEROED;
		Bind.type = Addr.type;
		Bind.port = 0;
		m_CompanionPySock = net_tcp_create(Bind);
		if(!m_CompanionPySock)
		{
			DbgImportant("net_tcp_create failed");
			return false;
		}

		if(net_tcp_connect(m_CompanionPySock, &Addr) != 0)
		{
			char aMsg[256];
			str_format(aMsg, sizeof(aMsg), "tcp connect failed (closing socket): %s",
				net_error_message().c_str());
			DbgImportant(aMsg);
			net_tcp_close(m_CompanionPySock);
			m_CompanionPySock = nullptr;
			m_CompanionPyConnected = false;
			return false;
		}
		else
		{
			char aMsg[256];
			str_copy(aMsg, "tcp connect ok", sizeof(aMsg));
			DbgImportant(aMsg);
		}
		m_CompanionPyConnected = true;
	}

	static const char s_aMagic[8] = {'F', 'E', 'N', 'T', 'P', 'Y', '0', '1'};
	int32_t ObsDim = (int32_t)Obs.size();
	const int MagicSent =
		net_tcp_send(m_CompanionPySock, s_aMagic, (int)sizeof(s_aMagic));
	const int DimSent = MagicSent >= 0 ? net_tcp_send(m_CompanionPySock, &ObsDim,
						     (int)sizeof(ObsDim)) :
					     -1;
	const int ObsSent = (DimSent >= 0) ? net_tcp_send(m_CompanionPySock, Obs.data(),
						     (int)(Obs.size() * sizeof(float))) :
					     -1;
	if(MagicSent < 0 || DimSent < 0 || ObsSent < 0)
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg),
			"tcp send failed (closing socket): magic=%d dim=%d obs=%d "
			"obs_bytes=%d err=%s",
			MagicSent, DimSent, ObsSent, (int)(Obs.size() * sizeof(float)),
			net_error_message().c_str());
		DbgImportant(aMsg);
		net_tcp_close(m_CompanionPySock);
		m_CompanionPySock = nullptr;
		m_CompanionPyConnected = false;
		return false;
	}

	const int TimeoutMs = std::clamp(g_Config.m_ClZzCompanionPyTimeoutMs, 0, 50);
	if(TimeoutMs > 0)
		(void)net_socket_read_wait(m_CompanionPySock,
			std::chrono::milliseconds(TimeoutMs));

	int32_t Action = -1;
	const int Recv =
		net_tcp_recv(m_CompanionPySock, &Action, (int)sizeof(Action));
	if(Recv != (int)sizeof(Action))
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "tcp recv failed/timeout: recv=%d err=%s",
			Recv, net_error_message().c_str());
		DbgImportant(aMsg);
		if(Recv < 0)
		{
			net_tcp_close(m_CompanionPySock);
			m_CompanionPySock = nullptr;
			m_CompanionPyConnected = false;
		}
		return false;
	}
	if(Action < 0 || Action >= FENTBOT_ACTIONS)
		return false;
	OutAction = (int)Action;
	return true;
}

void CGameClient::FentBotUpdatePlan(int LocalId, int DummyIndex)
{
	if(!m_FentBotHasWaypoint)
		return;
	if(!m_Snap.m_pLocalCharacter)
		return;

	const int NowTick = Client()->GameTick(DummyIndex);
	if(!m_FentBotPlanDirty && m_FentBotPlanLastTick == NowTick)
		return;

	const vec2 LocalPos = m_aClients[LocalId].m_RegularPredicted.m_Pos;
	const vec2 GoalPos = m_FentBotWaypoint;
	const int W = Collision()->GetWidth();
	const int H = Collision()->GetHeight();
	if(W <= 0 || H <= 0)
		return;

	const int Total = W * H;

	const int StartIndex = Collision()->GetPureMapIndex(LocalPos);
	const int GoalIndex = Collision()->GetPureMapIndex(GoalPos);
	if(StartIndex < 0 || GoalIndex < 0)
	{
		m_FentBotGlobalPathLen = 0;
		m_FentBotHasSubgoal = false;
		m_FentBotPlanDirty = false;
		m_FentBotPlanLastTick = NowTick;
		return;
	}

	auto IsBlockedIndex = [&](int Index) -> bool {
		const int Tile = Collision()->GetTileIndex(Index);
		const int FTile = Collision()->GetFrontTileIndex(Index);
		auto IsBad = [&](int T) -> bool {
			return T == TILE_SOLID || T == TILE_NOHOOK || T == TILE_DEATH ||
			       T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE;
		};
		return IsBad(Tile) || IsBad(FTile);
	};
	auto IsGeomBlockedIndex = [&](int Index) -> bool {
		const int Tile = Collision()->GetTileIndex(Index);
		const int FTile = Collision()->GetFrontTileIndex(Index);
		auto IsGeomBad = [&](int T) -> bool {
			return T == TILE_SOLID || T == TILE_NOHOOK;
		};
		return IsGeomBad(Tile) || IsGeomBad(FTile);
	};

	if(!m_FentBotDistValid || m_FentBotDistW != W || m_FentBotDistH != H ||
		m_FentBotDistGoalIndex != GoalIndex)
	{
		m_FentBotDistW = W;
		m_FentBotDistH = H;
		m_FentBotDistGoalIndex = GoalIndex;
		m_vFentBotDist.assign(Total, -1);
		struct SDNode
		{
			int m_D;
			int m_I;
		};
		auto Cmp = [](const SDNode &A, const SDNode &B) { return A.m_D > B.m_D; };
		std::priority_queue<SDNode, std::vector<SDNode>, decltype(Cmp)> pq(Cmp);
		m_vFentBotDist[GoalIndex] = 0;
		pq.push({0, GoalIndex});
		const float HookLen = (float)m_aTuning[DummyIndex].m_HookLength;
		int StartFoundDist = -1;
		const int StopMargin = 2048;
		while(!pq.empty())
		{
			const SDNode CurN = pq.top();
			pq.pop();
			const int Cur = CurN.m_I;
			const int CurD = CurN.m_D;
			if(StartFoundDist >= 0 && CurD > StartFoundDist + StopMargin)
				break;
			if(Cur < 0 || Cur >= Total)
				continue;
			if(m_vFentBotDist[Cur] != CurD)
				continue;
			if(Cur == StartIndex)
				StartFoundDist = CurD;
			const int X = Cur % W;
			const int Y = Cur / W;
			const vec2 CurPos = Collision()->GetPos(Cur);
			const int aDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
			const int aDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
			for(int k = 0; k < 8; k++)
			{
				const int Nx = X + aDx[k];
				const int Ny = Y + aDy[k];
				if(Nx < 0 || Ny < 0 || Nx >= W || Ny >= H)
					continue;
				if(aDx[k] != 0 && aDy[k] != 0)
				{
					const int N1 = Y * W + Nx;
					const int N2 = Ny * W + X;
					if(IsGeomBlockedIndex(N1) || IsGeomBlockedIndex(N2))
						continue;
				}
				const int N = Ny * W + Nx;
				if(N < 0 || N >= Total)
					continue;
				if(IsBlockedIndex(N))
					continue;
				const int Nd = CurD + 1;
				if(m_vFentBotDist[N] == -1 || Nd < m_vFentBotDist[N])
				{
					m_vFentBotDist[N] = Nd;
					pq.push({Nd, N});
				}
			}
			static const int HOOK_DIRS = 16;
			for(int i = 0; i < HOOK_DIRS; i++)
			{
				const float Ang = (2.0f * pi) * ((float)i / (float)HOOK_DIRS);
				const vec2 Dir = normalize(vec2(cosf(Ang), sinf(Ang)));
				vec2 HitPos;
				int TeleNr = 0;
				const int Hit = Collision()->IntersectLineTeleHook(
					CurPos, CurPos + Dir * HookLen, &HitPos, nullptr, &TeleNr);
				if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK)
					continue;
				const vec2 DestPos = HitPos - Dir * 32.0f;
				const int DestIdx = Collision()->GetPureMapIndex(DestPos);
				if(DestIdx < 0 || DestIdx >= Total)
					continue;
				if(IsBlockedIndex(DestIdx))
					continue;
				const int Cost = 6;
				const int Nd = CurD + Cost;
				if(m_vFentBotDist[DestIdx] == -1 || Nd < m_vFentBotDist[DestIdx])
				{
					m_vFentBotDist[DestIdx] = Nd;
					pq.push({Nd, DestIdx});
				}
			}
		}
		m_FentBotDistValid = true;
	}

	if(IsBlockedIndex(StartIndex) || IsBlockedIndex(GoalIndex))
	{
		m_FentBotGlobalPathLen = 0;
		m_FentBotHasSubgoal = false;
		m_FentBotPlanDirty = false;
		m_FentBotPlanLastTick = NowTick;
		return;
	}

	std::vector<int> vPrev;
	vPrev.assign(Total, -1);
	std::queue<int> q;
	vPrev[StartIndex] = StartIndex;
	q.push(StartIndex);

	bool Found = false;
	while(!q.empty())
	{
		const int Cur = q.front();
		q.pop();
		if(Cur == GoalIndex)
		{
			Found = true;
			break;
		}
		const int X = Cur % W;
		const int Y = Cur / W;
		const int aDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
		const int aDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
		for(int k = 0; k < 8; k++)
		{
			const int Nx = X + aDx[k];
			const int Ny = Y + aDy[k];
			if(Nx < 0 || Ny < 0 || Nx >= W || Ny >= H)
				continue;
			// Prevent diagonal corner-cutting through walls.
			if(aDx[k] != 0 && aDy[k] != 0)
			{
				const int N1 = Y * W + Nx;
				const int N2 = Ny * W + X;
				if(IsGeomBlockedIndex(N1) || IsGeomBlockedIndex(N2))
					continue;
			}
			const int N = Ny * W + Nx;
			if(vPrev[N] != -1)
				continue;
			if(IsBlockedIndex(N))
				continue;
			vPrev[N] = Cur;
			q.push(N);
		}
	}

	if(!Found)
	{
		m_FentBotGlobalPathLen = 0;
		m_FentBotHasSubgoal = false;
		m_FentBotPlanDirty = false;
		m_FentBotPlanLastTick = NowTick;
		return;
	}

	std::vector<int> vPath;
	vPath.reserve(256);
	int It = GoalIndex;
	while(It >= 0 && It != vPrev[It])
	{
		vPath.push_back(It);
		It = vPrev[It];
		if((int)vPath.size() > Total)
			break;
	}
	vPath.push_back(StartIndex);
	std::reverse(vPath.begin(), vPath.end());

	m_FentBotGlobalPathLen = 0;
	const int MaxOut = (int)m_aFentBotGlobalPath.size();
	for(int i = 0; i < (int)vPath.size() && i < MaxOut; i++)
	{
		m_aFentBotGlobalPath[i] = Collision()->GetPos(vPath[i]);
		m_FentBotGlobalPathLen = i + 1;
	}

	if(m_FentBotGlobalPathLen >= 2)
	{
		int Closest = 0;
		float ClosestD = 1e18f;
		for(int i = 0; i < m_FentBotGlobalPathLen; i++)
		{
			const float d = distance(LocalPos, m_aFentBotGlobalPath[i]);
			if(d < ClosestD)
			{
				ClosestD = d;
				Closest = i;
			}
		}

		// Micro-waypoint: pick a target a short distance ahead along the path.
		// This helps in narrow corridors and small upward adjustments.
		constexpr float SUBGOAL_DIST = 96.0f;
		float Accum = 0.0f;
		int Ahead = std::clamp(Closest + 1, 1, m_FentBotGlobalPathLen - 1);
		for(int i = Closest + 1; i < m_FentBotGlobalPathLen; i++)
		{
			const vec2 A = m_aFentBotGlobalPath[i - 1];
			const vec2 B = m_aFentBotGlobalPath[i];
			Accum += distance(A, B);
			Ahead = i;
			if(Accum >= SUBGOAL_DIST)
				break;
		}
		m_FentBotSubgoal = m_aFentBotGlobalPath[Ahead];
		m_FentBotHasSubgoal = true;
	}
	else
	{
		m_FentBotHasSubgoal = false;
	}

	m_FentBotPlanDirty = false;
	m_FentBotPlanLastTick = NowTick;
}
const char *CGameClient::NetVersion() const { return GAME_NETVERSION; }
const char *CGameClient::NetVersion7() const { return GAME_NETVERSION7; }
int CGameClient::DDNetVersion() const { return DDNET_VERSION_NUMBER; }
const char *CGameClient::DDNetVersionStr() const { return m_aDDNetVersionStr; }
int CGameClient::ClientVersion7() const { return CLIENT_VERSION7; }
const char *CGameClient::GetItemName(int Type) const
{
	return m_NetObjHandler.GetObjName(Type);
}

void CGameClient::OnConsoleInit()
{
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pClient = Kernel()->RequestInterface<IClient>();
	m_pTextRender = Kernel()->RequestInterface<ITextRender>();
	m_pSound = Kernel()->RequestInterface<ISound>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
	m_pInput = Kernel()->RequestInterface<IInput>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pDemoPlayer = Kernel()->RequestInterface<IDemoPlayer>();
	m_pServerBrowser = Kernel()->RequestInterface<IServerBrowser>();
	m_pEditor = Kernel()->RequestInterface<IEditor>();
	m_pFavorites = Kernel()->RequestInterface<IFavorites>();
	m_pFriends = Kernel()->RequestInterface<IFriends>();
	m_pFoes = Client()->Foes();
	m_pDiscord = Kernel()->RequestInterface<IDiscord>();
#if defined(CONF_AUTOUPDATE)
	m_pUpdater = Kernel()->RequestInterface<IUpdater>();
#endif
	m_pHttp = Kernel()->RequestInterface<IHttp>();
	m_pMap = CreateMap();

	// make a list of all the systems, make sure to add them in the correct render
	// order
	m_vpAll.insert(
		m_vpAll.end(),
		{&m_Skins,
			&m_Skins7,
			&m_CountryFlags,
			&m_MapImages,
			&m_Effects, // doesn't render anything, just updates effects
			&m_SkinProfiles, // TClient
			&m_Binds,
			&m_Binds.m_SpecialBinds,
			&m_Controls,
			&m_Camera,
			&m_Sounds,
			&m_Voting,
			&m_Particles, // doesn't render anything, just updates all the particles
			&m_RaceDemo,
			&m_Rainbow, // TClient
			&m_MapSounds,
			&m_Censor,
			&m_Background, // render instead of m_MapLayersBackground when
				       // g_Config.m_ClOverlayEntities == 100
			&m_MapLayersBackground, // first to render
			&m_BgDraw, // TClient
			&m_Particles.m_RenderTrail,
			&m_Particles.m_RenderTrailExtra,
			&m_Items,
			&m_Trails, // TClient
			&m_Translate, // TClient
			&m_Ghost,
			&m_TClient, // TClient (Must be before chat and players)
			&m_DeveloperIdentity,
			&m_ZZHookAimbot,
			&m_Players,
			&m_MovingTilesBackground, // TClient
			&m_MapLayersForeground,
			&m_MovingTilesForeground, // TClient
			&m_Outlines, // TClient
			&m_Mumble, // TClient
			&m_Pet, // TClient
			&m_Particles.m_RenderExplosions,
			&m_Particles.m_RenderExtra,
			&m_Particles.m_RenderGeneral,
			&m_ZZWorldPostProcess,
			&m_NamePlates,
			&m_FreezeBars,
			&m_DamageInd,
			&m_PlayerIndicator, // TClient
			&m_Mod, // TClient
			&m_CustomCommunities, // TClient
			&m_MusicSMTC,
			&m_Hud,
			&m_Spectator,
			&m_Emoticon,
			&m_BindChat, // TClient
			&m_BindWheel, // TClient
			&m_WarList, // TClient
			&m_StatusBar, // TClient
			&m_InfoMessages,
			&m_Chat,
			&m_Broadcast,
			&m_ImportantAlert,
			&m_DebugHud,
			&m_TouchControls,
			&m_Scoreboard,
			&m_Statboard,
			&m_Motd,
			&m_Menus,
			&m_ZZClickGui,
			&m_Tooltips,
			&m_Scripting, // TClient
			&m_KeyBinder,
			&m_GameConsole,
			&m_MenuBackground,
			&m_ZZVisualEffects});

	// build the input stack
	m_vpInput.insert(
		m_vpInput.end(),
		{&m_KeyBinder, // this will take over all input when we want to bind a key
			&m_Binds.m_SpecialBinds, &m_ZZVisualEffects, &m_ZZClickGui,
			&m_GameConsole,
			&m_Chat, // chat has higher prio, due to that you can quit it by pressing
				 // esc
			&m_Scoreboard,
			&m_Motd, // for pressing esc to remove it
			&m_Spectator,
			&m_BindWheel, // TClient
			&m_Emoticon, &m_ImportantAlert, &m_Menus, &m_Controls, &m_TouchControls,
			&m_Binds});

	// initialize client data
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CClientData &Client = m_aClients[ClientId];
		Client.m_pGameClient = this;
		Client.m_ClientId = ClientId;
	}

	// add basic console commands
	Console()->Register("team", "i[team-id]", CFGFLAG_CLIENT, ConTeam, this,
		"Switch team");
	Console()->Register("kill", "", CFGFLAG_CLIENT, ConKill, this,
		"Kill yourself to restart");
	Console()->Register("ready_change", "", CFGFLAG_CLIENT, ConReadyChange7, this,
		"Change ready state (0.7 only)");
	Console()->Register("zz_fentbot_plan", "", CFGFLAG_CLIENT, ConZzFentBotPlan,
		this, "KernelNet: recalculate FentBot global plan");
	Console()->Register(
		"zz_fentbot_export_dataset", "?i[episodes] ?i[steps] ?r[filename]",
		CFGFLAG_CLIENT, ConZzFentBotExportDataset, this,
		"KernelNet: export FentBot dataset (jsonl) using local simulation");
	Console()->Register(
		"zz_fentbot_record_start", "?r[filename]", CFGFLAG_CLIENT,
		ConZzFentBotRecordStart, this,
		"KernelNet: start recording live gameplay to FentBot BC dataset");
	Console()->Register("zz_fentbot_record_stop", "", CFGFLAG_CLIENT,
		ConZzFentBotRecordStop, this,
		"KernelNet: stop recording live gameplay");
	Console()->Register(
		"zz_fentbot_clip_start", "?r[tag]", CFGFLAG_CLIENT, ConZzFentBotClipStart,
		this, "KernelNet: start recording a clip/trick to FentBot BC dataset");
	Console()->Register("zz_fentbot_clip_stop", "", CFGFLAG_CLIENT,
		ConZzFentBotClipStop, this,
		"KernelNet: stop recording clip/trick");
	Console()->Register("zz_fentbot_saveplr_start", "?r[tag]", CFGFLAG_CLIENT,
		ConZzFentBotSavePlrStart, this,
		"KernelNet: start recording save-player/rescue gameplay "
		"to FentBot BC dataset");
	Console()->Register("zz_fentbot_saveplr_stop", "", CFGFLAG_CLIENT,
		ConZzFentBotSavePlrStop, this,
		"KernelNet: stop recording save-player/rescue");
	Console()->Register("zz_fentbot_save_10sec", "?r[tag]", CFGFLAG_CLIENT,
		ConZzFentBotSave10Sec, this,
		"KernelNet: save last 10 seconds as a BC dataset clip");
	Console()->Register(
		"zz_fentbot_bc_infer", "i[on]", CFGFLAG_CLIENT, ConZzFentBotBcInfer, this,
		"KernelNet: enable/disable BC inference (embedded model)");
	Console()->Register("zz_fentbot_bc_train", "r[file] ?i[epochs]",
		CFGFLAG_CLIENT, ConZzFentBotBcTrain, this,
		"KernelNet: train BC model from recorded dataset");
	Console()->Register("zz_fentbot_load_model", "r[file]", CFGFLAG_CLIENT,
		ConZzFentBotLoadModel, this,
		"KernelNet: load PPO/BC model from save dir");
	Console()->Register("zz_fentbot_save_model", "r[file]", CFGFLAG_CLIENT,
		ConZzFentBotSaveModel, this,
		"KernelNet: save PPO/BC model to save dir");
	Console()->Register("zz_fentbot_train_start", "", CFGFLAG_CLIENT,
		ConZzFentBotTrainStart, this,
		"KernelNet: start in-client training (needs waypoint)");
	Console()->Register("zz_fentbot_train_stop", "", CFGFLAG_CLIENT,
		ConZzFentBotTrainStop, this,
		"KernelNet: stop in-client training");
	Console()->Register("zz_ai_mode", "?i[mode]", CFGFLAG_CLIENT, ConZzAiMode,
		this,
		"KernelNet AI quick mode: 0=off 1=fentbot 2=python 3=bc "
		"4=companion 5=kog");
	Console()->Register("zz_ai", "?i[mode]", CFGFLAG_CLIENT, ConZzAiMode, this,
		"KernelNet AI quick mode alias (supports mode 5=kog)");
	if(!m_pKoGAIController)
		m_pKoGAIController = std::make_unique<CKoGAIController>(this);
	m_pKoGAIController->OnConsoleInit();

	// register game commands to allow the client prediction to load settings from
	// the map
	Console()->Register("tune", "s[tuning] ?f[value]", CFGFLAG_GAME, ConTuneParam,
		this, "Tune variable to value");
	Console()->Register("tune_zone", "i[zone] s[tuning] f[value]", CFGFLAG_GAME,
		ConTuneZone, this, "Tune in zone a variable to value");
	Console()->Register("mapbug", "s[mapbug]", CFGFLAG_GAME, ConMapbug, this,
		"Enable map compatibility mode using the specified bug "
		"(example: grenade-doubleexplosion@ddnet.tw)");

	for(auto &pComponent : m_vpAll)
		pComponent->OnInterfacesInit(this);

	m_LocalServer.OnInterfacesInit(this);

	// let all the other components register their console commands
	for(auto &pComponent : m_vpAll)
		pComponent->OnConsoleInit();

	Console()->Chain("cl_languagefile", ConchainLanguageUpdate, this);

	Console()->Chain("player_name", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_clan", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_country", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_use_custom_color", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_color_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_color_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_skin", ConchainSpecialInfoupdate, this);

	Console()->Chain("player7_skin", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_marking", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_decoration", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_hands", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_eyes", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_marking", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_decoration", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_hands", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_eyes", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_body", ConchainSpecialInfoupdate,
		this);
	Console()->Chain("player7_use_custom_color_marking",
		ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_decoration",
		ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_hands", ConchainSpecialInfoupdate,
		this);
	Console()->Chain("player7_use_custom_color_feet", ConchainSpecialInfoupdate,
		this);
	Console()->Chain("player7_use_custom_color_eyes", ConchainSpecialInfoupdate,
		this);

	Console()->Chain("dummy_name", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_clan", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_country", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_use_custom_color", ConchainSpecialDummyInfoupdate,
		this);
	Console()->Chain("dummy_color_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_color_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_skin", ConchainSpecialDummyInfoupdate, this);

	Console()->Chain("dummy7_skin", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_marking", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_decoration", ConchainSpecialDummyInfoupdate,
		this);
	Console()->Chain("dummy7_skin_hands", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_eyes", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_marking", ConchainSpecialDummyInfoupdate,
		this);
	Console()->Chain("dummy7_color_decoration", ConchainSpecialDummyInfoupdate,
		this);
	Console()->Chain("dummy7_color_hands", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_eyes", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_body",
		ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_marking",
		ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_decoration",
		ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_hands",
		ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_feet",
		ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_eyes",
		ConchainSpecialDummyInfoupdate, this);

	Console()->Chain("cl_skin_download_url", ConchainRefreshSkins, this);
	Console()->Chain("cl_skin_community_download_url", ConchainRefreshSkins,
		this);
	Console()->Chain("cl_skin_prefix", ConchainRefreshSkins, this);
	Console()->Chain("cl_download_skins", ConchainRefreshSkins, this);
	Console()->Chain("cl_download_community_skins", ConchainRefreshSkins, this);
	Console()->Chain("cl_vanilla_skins_only", ConchainRefreshSkins, this);
	Console()->Chain("events", ConchainRefreshEventSkins, this);

	Console()->Chain("cl_dummy", ConchainSpecialDummy, this);

	Console()->Chain("cl_menu_map", ConchainMenuMap, this);
}

static void GenerateTimeoutCode(char *pTimeoutCode)
{
	if(pTimeoutCode[0] == '\0' ||
		str_comp(pTimeoutCode, "hGuEYnfxicsXGwFq") == 0)
	{
		for(unsigned int i = 0; i < 16; i++)
		{
			if(rand() % 2)
				pTimeoutCode[i] = (char)((rand() % ('z' - 'a' + 1)) + 'a');
			else
				pTimeoutCode[i] = (char)((rand() % ('Z' - 'A' + 1)) + 'A');
		}
	}
}

void CGameClient::InitializeLanguage()
{
	// set the language
	g_Localization.LoadIndexfile(Storage(), Console());
	if(g_Config.m_ClShowWelcome)
		str_copy(g_Config.m_ClLanguagefile, "languages/russian.txt", sizeof(g_Config.m_ClLanguagefile));
	g_Localization.Load(g_Config.m_ClLanguagefile, Storage(), Console());

	// TClient
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "tclient/%s", g_Config.m_ClLanguagefile);
	g_Localization.Load(aBuf, Storage(), Console(), false);
}

void CGameClient::ForceUpdateConsoleRemoteCompletionSuggestions()
{
	m_GameConsole.ForceUpdateRemoteCompletionSuggestions();
}

void CGameClient::OnInit()
{
	const int64_t OnInitStart = time_get();

	// AI and FentBot were removed from the public client. Clear persisted values
	// before any component can schedule training, inference or automated input.
	g_Config.m_ClZzFentBotEnabled = 0;
	g_Config.m_ClZzFentBotAiInfer = 0;
	g_Config.m_ClZzFentBotPyInfer = 0;
	g_Config.m_ClZzFentBotTrain = 0;
	g_Config.m_ClZzFentBotAutoRecord = 0;
	g_Config.m_ClZzCompanionEnabled = 0;
	g_Config.m_ClZzCompanionPyInfer = 0;
	g_Config.m_ClZzKogAiEnabled = 0;
	// Removed helpers must also stay disabled for users with old saved configs.
	g_Config.m_ClKnTileAlign = 0;
	g_Config.m_ClKnMimic = 0;
	g_Config.m_ClKnAntiPushJump = 0;
	g_Config.m_ClZzAvoidJump = 0;
	Client()->SetLoadingCallback([this](IClient::ELoadingCallbackDetail Detail) {
		const char *pTitle;
		if(Detail == IClient::LOADING_CALLBACK_DETAIL_DEMO ||
			DemoPlayer()->IsPlaying())
		{
			pTitle = Localize("Preparing demo playback");
		}
		else
		{
			pTitle = Localize("Connected");
		}

		const char *pMessage;
		switch(Detail)
		{
		case IClient::LOADING_CALLBACK_DETAIL_MAP:
			pMessage = Localize("Loading map file from storage");
			break;
		case IClient::LOADING_CALLBACK_DETAIL_DEMO:
			pMessage = Localize("Loading demo file from storage");
			break;
		default:
			dbg_assert_failed("Invalid callback loading detail");
		}
		m_Menus.RenderLoading(pTitle, pMessage, 0);
	});

	m_pGraphics = Kernel()->RequestInterface<IGraphics>();

	// propagate pointers
	m_UI.Init(Kernel());
	m_RenderTools.Init(Graphics(), TextRender(), this); // TClient
	m_RenderMap.Init(Graphics(), TextRender());

	if(GIT_SHORTREV_HASH)
	{
		str_format(m_aDDNetVersionStr, sizeof(m_aDDNetVersionStr), "%s %s (%s)",
			GAME_NAME, GAME_RELEASE_VERSION, GIT_SHORTREV_HASH);
	}
	else
	{
		str_format(m_aDDNetVersionStr, sizeof(m_aDDNetVersionStr), "%s %s",
			GAME_NAME, GAME_RELEASE_VERSION);
	}

	// TODO: this should be different
	// setup item sizes
	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Client()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));
	// HACK: only set static size for items, which were available in the first 0.7
	// release so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Client()->SnapSetStaticsize7(i, m_NetObjHandler7.GetObjSize(i));

	if(!TextRender()->LoadFonts())
	{
		Client()->AddWarning(
			SWarning(Localize("Some fonts could not be loaded. Check the local "
					  "console for details.")));
	}
	TextRender()->SetFontLanguageVariant(g_Config.m_ClLanguagefile);

	// update and swap after font loading, they are quite huge
	Client()->UpdateAndSwap();

	const char *pLoadingDDNetCaption = Localize("Loading DDNet Client");
	const char *pLoadingMessageComponents = Localize("Initializing components");
	const char *pLoadingMessageComponentsSpecial =
		Localize("Why are you slowmo replaying to read this?");
	char aLoadingMessage[256];

	// init all components
	int SkippedComps = 1;
	int CompCounter = 1;
	const int NumComponents = ComponentCount();
	for(int i = NumComponents - 1; i >= 0; --i)
	{
		m_vpAll[i]->OnInit();
		// try to render a frame after each component, also flushes GPU uploads
		if(m_Menus.IsInit())
		{
			str_format(aLoadingMessage, std::size(aLoadingMessage), "%s [%d/%d]",
				CompCounter == NumComponents ? pLoadingMessageComponentsSpecial : pLoadingMessageComponents,
				CompCounter, NumComponents);
			m_Menus.RenderLoading(pLoadingDDNetCaption, aLoadingMessage,
				SkippedComps);
			SkippedComps = 1;
		}
		else
		{
			++SkippedComps;
		}
		++CompCounter;
	}

	m_GameSkinLoaded = false;
	m_ParticlesSkinLoaded = false;
	m_EmoticonsSkinLoaded = false;
	m_HudSkinLoaded = false;

	// setup load amount, load textures
	const char *pLoadingMessageAssets = Localize("Initializing assets");
	for(int i = 0; i < g_pData->m_NumImages; i++)
	{
		if(i == IMAGE_GAME)
			LoadGameSkin(g_Config.m_ClAssetGame);
		else if(i == IMAGE_EMOTICONS)
			LoadEmoticonsSkin(g_Config.m_ClAssetEmoticons);
		else if(i == IMAGE_PARTICLES)
			LoadParticlesSkin(g_Config.m_ClAssetParticles);
		else if(i == IMAGE_HUD)
			LoadHudSkin(g_Config.m_ClAssetHud);
		else if(i == IMAGE_EXTRAS)
			LoadExtrasSkin(g_Config.m_ClAssetExtras);
		else if(i == IMAGE_CURSOR)
		{
			CImageInfo CursorImage;
			if(Graphics()->LoadPng(CursorImage, g_pData->m_aImages[i].m_pFilename,
				   IStorage::TYPE_ALL) &&
				CursorImage.m_Format == CImageInfo::FORMAT_RGBA)
			{
				const SZZThemePalette Palette = GetZZThemePalette(
					g_Config.m_ClZzTheme,
					color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
				const ColorRGBA CursorAccent = ZZThemeLerp(
					Palette.m_Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
				ZzRecolorCursorPixels(CursorImage, 0, 0, CursorImage.m_Width,
					CursorImage.m_Height, CursorAccent);
				g_pData->m_aImages[i].m_Id = Graphics()->LoadTextureRawMove(
					CursorImage, 0, "zz-theme-ui-cursor");
			}
			else
			{
				CursorImage.Free();
				g_pData->m_aImages[i].m_Id = Graphics()->LoadTexture(
					g_pData->m_aImages[i].m_pFilename, IStorage::TYPE_ALL);
			}
		}
		else if(g_pData->m_aImages[i].m_pFilename[0] ==
			'\0') // handle special null image without filename
			g_pData->m_aImages[i].m_Id = IGraphics::CTextureHandle();
		else
			g_pData->m_aImages[i].m_Id = Graphics()->LoadTexture(
				g_pData->m_aImages[i].m_pFilename, IStorage::TYPE_ALL);
		m_Menus.RenderLoading(pLoadingDDNetCaption, pLoadingMessageAssets, 1);
	}

	m_GameWorld.Init(Collision(), m_aTuningList, &m_MapBugs);
	OnReset();

	// Set free binds to DDRace binds if it's active
	m_Binds.SetDDRaceBinds(true);

	GenerateTimeoutCode(g_Config.m_ClTimeoutCode);
	GenerateTimeoutCode(g_Config.m_ClDummyTimeoutCode);

	// Aggressively try to grab window again since some Windows users report
	// window not being focused after starting client.
	Graphics()->SetWindowGrab(true);

	CChecksumData *pChecksum = Client()->ChecksumData();
	pChecksum->m_SizeofGameClient = sizeof(*this);
	pChecksum->m_NumComponents = m_vpAll.size();
	for(size_t i = 0; i < m_vpAll.size(); i++)
	{
		if(i >= std::size(pChecksum->m_aComponentsChecksum))
		{
			break;
		}
		int Size = m_vpAll[i]->Sizeof();
		pChecksum->m_aComponentsChecksum[i] = Size;
	}

	m_Menus.FinishLoading();
	log_trace("gameclient", "initialization finished after %.2fms",
		(time_get() - OnInitStart) * 1000.0f / (float)time_freq());
}

void CGameClient::OnUpdate()
{
	HandleLanguageChanged();

	CUIElementBase::Init(
		Ui()); // update static pointer because game and editor use separate UI

	// handle mouse movement
	float x = 0.0f, y = 0.0f;
	IInput::ECursorType CursorType = Input()->CursorRelative(&x, &y);
	if(CursorType != IInput::CURSOR_NONE)
	{
		for(auto &pComponent : m_vpInput)
		{
			if(pComponent->OnCursorMove(x, y, CursorType))
				break;
		}
	}

	// handle touch events
	const std::vector<IInput::CTouchFingerState> &vTouchFingerStates =
		Input()->TouchFingerStates();
	bool TouchHandled = false;
	for(auto &pComponent : m_vpInput)
	{
		if(TouchHandled)
		{
			// Also update inactive components so they can handle touch fingers being
			// released.
			pComponent->OnTouchState({});
		}
		else if(pComponent->OnTouchState(vTouchFingerStates))
		{
			Input()->ClearTouchDeltas();
			TouchHandled = true;
		}
	}

	// handle key presses
	Input()->ConsumeEvents([&](const IInput::CEvent &Event) {
		for(auto &pComponent : m_vpInput)
		{
			// Events with flag `FLAG_RELEASE` must always be forwarded to all
			// components so keys being released can be handled in all components also
			// after some components have been disabled.
			if(pComponent->OnInput(Event) &&
				(Event.m_Flags & ~IInput::FLAG_RELEASE) != 0)
				break;
		}
	});

	if(g_Config.m_ClSubTickAiming && m_Binds.m_MouseOnAction)
	{
		m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy] =
			m_Controls.m_aMousePos[g_Config.m_ClDummy];
		m_Binds.m_MouseOnAction = false;
	}

	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnUpdate();
	}

	UpdateFlyRide();
	FentBotTrainUpdate();
}

void CGameClient::FentBotTrainUpdate()
{
	const bool TrainEnabled = g_Config.m_ClZzFentBotTrain != 0;
	if(!TrainEnabled)
	{
		m_FentBotTrainWasEnabled = false;
		m_FentBotTrainWorldInited = false;
		m_FentBotTrainResetWorldInited = false;
		return;
	}

	if(!m_FentBotTrainWasEnabled)
	{
		m_FentBotTrainWasEnabled = true;
		m_FentBotTrainSteps = 0;
		m_FentBotTrainEpisodesDone = 0;
		m_FentBotTrainBestReturn = -1e18f;
		m_FentBotBestPathLen = 0;
		m_FentBotTrainWorldInited = false;
		m_FentBotTrainResetWorldInited = false;
		m_FentBotTrainEpStep = 0;
		m_FentBotTrainEpReturn = 0.0f;
		m_FentBotTrainEpBestDist = 0.0f;
		m_FentBotTrainEpStagnantSteps = 0;
		m_vFentBotTrainEpPos.clear();
		m_vFentBotTrainDist.clear();
		m_FentBotTrainDistW = 0;
		m_FentBotTrainDistH = 0;
		m_FentBotTrainDistGoalIndex = -1;
		m_FentBotTrainActionsTotal = 0;
		m_FentBotTrainActionsHook = 0;
		m_FentBotTrainActionsJump = 0;
		m_FentBotTrainEpisodesGoal = 0;
		m_FentBotTrainEpisodesFreeze = 0;
		m_FentBotTrainEpisodesStuck = 0;
		m_FentBotTrainEpisodesDeath = 0;
		m_FentBotTrainEpisodesMaxSteps = 0;
		m_FentBotTrainEpisodeStepsTotal = 0;
		m_vFentBotRollout.clear();
		m_vFentBotRollout.resize(
			std::clamp(g_Config.m_ClZzFentBotTrainReplaySize, 256, 32768));
		m_FentBotRolloutPos = 0;
		m_FentBotRolloutSize = (int)m_vFentBotRollout.size();
		if(!m_FentBotAiModelLoaded)
		{
			std::mt19937 Rng((uint32_t)time_get());
			std::uniform_real_distribution<float> U(-0.02f, 0.02f);
			for(float &v : m_aFentBotPpoW1)
				v = U(Rng);
			for(float &v : m_aFentBotPpoB1)
				v = 0.0f;
			for(float &v : m_aFentBotPpoWpi)
				v = U(Rng);
			for(float &v : m_aFentBotPpoBpi)
				v = 0.0f;
			for(float &v : m_aFentBotPpoWv)
				v = U(Rng);
			m_FentBotPpoBv = 0.0f;
			m_FentBotAiModelLoaded = true;
		}
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Training started");
	}
	else
	{
		const int WantedSize =
			std::clamp(g_Config.m_ClZzFentBotTrainReplaySize, 256, 32768);
		if((int)m_vFentBotRollout.size() != WantedSize)
		{
			m_vFentBotRollout.clear();
			m_vFentBotRollout.resize(WantedSize);
			m_FentBotRolloutPos = 0;
			m_FentBotRolloutSize = (int)m_vFentBotRollout.size();
			m_FentBotTrainSteps = 0;
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
				"Rollout resized (cleared)");
		}
	}

	if(!m_Snap.m_pLocalCharacter)
		return;
	if(!m_FentBotHasWaypoint)
		return;

	static std::mt19937 s_Rng((uint32_t)time_get());
	std::uniform_real_distribution<float> s_U01(0.0f, 1.0f);

	struct SAimCand
	{
		float x;
		float y;
	};
	static const SAimCand s_aAimCands[] = {
		{0.0f, -1.0f},
		{-0.3713906763f, -0.9284766909f},
		{0.3713906763f, -0.9284766909f},
		{-1.0f, 0.0f},
		{1.0f, 0.0f},
	};
	constexpr int AimCount = (int)std::size(s_aAimCands);
	constexpr int ActionCount = 60;

	auto IsFreezeTile = [&](int Tile) {
		return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
	};
	auto IsFreezeAt = [&](const vec2 &Pos) {
		const int PureIndex = Collision()->GetPureMapIndex(Pos);
		if(PureIndex < 0)
			return false;
		const int Tile = Collision()->GetTileIndex(PureIndex);
		const int FTile = Collision()->GetFrontTileIndex(PureIndex);
		return IsFreezeTile(Tile) || IsFreezeTile(FTile);
	};

	auto EncodeTile = [&](int T) -> float {
		if(T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE)
			return 2.0f / 3.0f;
		if(T == TILE_DEATH)
			return 1.0f;
		if(T == TILE_SOLID || T == TILE_NOHOOK)
			return 1.0f / 3.0f;
		return 0.0f;
	};

	auto BuildObs = [&](const vec2 &Pos, const vec2 &Vel, const vec2 &Goal,
				std::array<float, FENTBOT_OBS_DIM> &Out) {
		auto AppendNearestPlayers = [&](std::array<float, FENTBOT_OBS_DIM> &Obs,
						    int &o) {
			struct SCand
			{
				float m_D2;
				int m_Id;
			};
			SCand aCands[MAX_CLIENTS];
			int Num = 0;
			const float Radius = (float)(FENTBOT_PLAYER_RADIUS_TILES * 32);
			const float R2 = Radius * Radius;
			const int LocalId = m_Snap.m_LocalClientId;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(i == LocalId)
					continue;
				if(!m_Snap.m_aCharacters[i].m_Active)
					continue;
				const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
				const vec2 d = P - Pos;
				const float d2 = dot(d, d);
				if(d2 > R2)
					continue;
				aCands[Num++] = {d2, i};
			}
			std::sort(aCands, aCands + Num,
				[](const SCand &A, const SCand &B) { return A.m_D2 < B.m_D2; });
			for(int k = 0; k < FENTBOT_PLAYERS_K; k++)
			{
				if(k < Num)
				{
					const int Id = aCands[k].m_Id;
					const vec2 P = m_aClients[Id].m_RegularPredicted.m_Pos;
					const vec2 V = m_aClients[Id].m_RegularPredicted.m_Vel;
					const vec2 d = P - Pos;
					Obs[o++] = d.x / 1000.0f;
					Obs[o++] = d.y / 1000.0f;
					Obs[o++] = V.x / 1000.0f;
					Obs[o++] = V.y / 1000.0f;
				}
				else
				{
					Obs[o++] = 0.0f;
					Obs[o++] = 0.0f;
					Obs[o++] = 0.0f;
					Obs[o++] = 0.0f;
				}
			}
		};
		const vec2 d = Goal - Pos;
		Out[0] = d.x / 1000.0f;
		Out[1] = d.y / 1000.0f;
		Out[2] = Vel.x / 1000.0f;
		Out[3] = Vel.y / 1000.0f;
		const int W = Collision()->GetWidth();
		const int H = Collision()->GetHeight();
		const int Center = Collision()->GetPureMapIndex(Pos);
		int cx = 0;
		int cy = 0;
		if(Center >= 0)
		{
			cx = Center % W;
			cy = Center / W;
		}
		int o = 4;
		for(int dy = -FENTBOT_GRID_R; dy <= FENTBOT_GRID_R; dy++)
		{
			for(int dx = -FENTBOT_GRID_R; dx <= FENTBOT_GRID_R; dx++)
			{
				const int tx = std::clamp(cx + dx, 0, std::max(0, W - 1));
				const int ty = std::clamp(cy + dy, 0, std::max(0, H - 1));
				const int Idx = ty * W + tx;
				Out[o++] = EncodeTile(Collision()->GetTileIndex(Idx));
				Out[o++] = EncodeTile(Collision()->GetFrontTileIndex(Idx));
			}
		}
		AppendNearestPlayers(Out, o);
	};

	auto ForwardPpo = [&](const std::array<float, FENTBOT_OBS_DIM> &Obs,
				  float aLogits[ActionCount], float &V) {
		float aZ[FENTBOT_HIDDEN];
		float aH[FENTBOT_HIDDEN];
		for(int i = 0; i < FENTBOT_HIDDEN; i++)
		{
			float v = m_aFentBotPpoB1[i];
			const int Base = i * FENTBOT_OBS_DIM;
			for(int j = 0; j < FENTBOT_OBS_DIM; j++)
				v += m_aFentBotPpoW1[Base + j] * Obs[j];
			aZ[i] = v;
			aH[i] = v > 0.0f ? v : 0.0f;
		}
		for(int a = 0; a < ActionCount; a++)
		{
			float q = m_aFentBotPpoBpi[a];
			const int Base = a * FENTBOT_HIDDEN;
			for(int i = 0; i < FENTBOT_HIDDEN; i++)
				q += m_aFentBotPpoWpi[Base + i] * aH[i];
			aLogits[a] = q;
		}
		float vv = m_FentBotPpoBv;
		for(int i = 0; i < FENTBOT_HIDDEN; i++)
			vv += m_aFentBotPpoWv[i] * aH[i];
		V = vv;
	};

	auto LogSoftmax = [&](const float aLogits[ActionCount],
				  float aLogP[ActionCount]) {
		float M = aLogits[0];
		for(int a = 1; a < ActionCount; a++)
			M = std::max(M, aLogits[a]);
		float S = 0.0f;
		for(int a = 0; a < ActionCount; a++)
			S += expf(aLogits[a] - M);
		const float LogZ = M + logf(std::max(1e-12f, S));
		for(int a = 0; a < ActionCount; a++)
			aLogP[a] = aLogits[a] - LogZ;
	};

	auto SampleAction = [&](const float aLogP[ActionCount]) -> int {
		float r = s_U01(s_Rng);
		float c = 0.0f;
		for(int a = 0; a < ActionCount; a++)
		{
			c += expf(aLogP[a]);
			if(r <= c)
				return a;
		}
		return ActionCount - 1;
	};

	auto DecodeAction = [&](int A, int &Dir, bool &Jump, bool &Hook,
				    int &AimIndex) {
		AimIndex = A % AimCount;
		int t = A / AimCount;
		Hook = (t & 1) != 0;
		t >>= 1;
		Jump = (t & 1) != 0;
		t >>= 1;
		Dir = (t % 3) - 1;
	};

	const int TicksBudget =
		std::clamp(g_Config.m_ClZzFentBotTrainTicksPerFrame, 1, 200000);
	const int MaxSteps =
		std::clamp(g_Config.m_ClZzFentBotTrainMaxSteps, 1, 20000);
	const int Batch = std::clamp(g_Config.m_ClZzFentBotTrainBatchSize, 16, 4096);
	const float Gamma =
		(float)std::clamp(g_Config.m_ClZzFentBotTrainGammaPermille, 0, 1000) /
		1000.0f;
	const float Lr =
		(float)std::clamp(g_Config.m_ClZzFentBotTrainLrMicro, 1, 1000000) * 1e-6f;
	const float EpsInfer =
		(float)std::clamp(g_Config.m_ClZzFentBotAiEpsilon, 0, 100) / 100.0f;
	// Exploration schedule: start at max(infer_epsilon, 0.40) and decay towards
	// 0.05 over ~300k steps.
	const float EpsStart = std::max(0.40f, EpsInfer);
	const float EpsMin = 0.05f;
	const float EpsDecaySteps = 300000.0f;
	const float EpsTrain = std::max(
		EpsMin, EpsStart - (EpsStart - EpsMin) *
					   std::min(1.0f, (float)m_FentBotTrainSteps /
								  EpsDecaySteps));
	const float PpoClip = 0.2f;
	const float PpoLambda = 0.95f;
	const float VfCoef = 0.5f;
	const float EntCoef = 0.01f;
	const int PpoEpochs = 4;

	const int LocalId = m_Snap.m_LocalClientId;
	const int DummyIndex = g_Config.m_ClDummy;
	const int NowTick = Client()->GameTick(DummyIndex);
	const float HookLen = (float)m_aTuning[DummyIndex].m_HookLength;
	const int64_t FrameStart = time_get();
	const int64_t FrameBudget =
		time_freq() / 250; // ~4ms per frame max for training
	const float StepPenalty = -0.01f;
	const float FreezePenalty = -100.0f;
	const float DeathPenalty = -100.0f;
	const float StuckPenalty = -25.0f;
	const float GoalBonus = 100.0f;
	const float TileProgressScale = 0.25f;
	const float EuclidProgressScale = 0.0025f;
	const float ProgressClipAbs = 5.0f;

	static uint64_t s_DistSamples = 0;
	static uint64_t s_DistValidSamples = 0;
	static uint64_t s_DistSeededSamples = 0;
	static uint64_t s_DistGoalBlockedSamples = 0;

	// Build/refresh BFS distance field to waypoint for stable progress reward.
	const int W = Collision()->GetWidth();
	const int H = Collision()->GetHeight();
	const int GoalIndex = Collision()->GetPureMapIndex(m_FentBotWaypoint);
	auto IsBlockedIndex = [&](int Index) -> bool {
		const int Tile = Collision()->GetTileIndex(Index);
		const int FTile = Collision()->GetFrontTileIndex(Index);
		auto IsBad = [&](int T) -> bool {
			return T == TILE_SOLID || T == TILE_NOHOOK || T == TILE_DEATH ||
			       T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE;
		};
		return IsBad(Tile) || IsBad(FTile);
	};

	if(W > 0 && H > 0 && GoalIndex >= 0)
	{
		const bool NeedRebuild = m_FentBotTrainDistW != W ||
					 m_FentBotTrainDistH != H ||
					 m_FentBotTrainDistGoalIndex != GoalIndex ||
					 (int)m_vFentBotTrainDist.size() != W * H;
		if(NeedRebuild)
		{
			m_FentBotTrainDistW = W;
			m_FentBotTrainDistH = H;
			m_FentBotTrainDistGoalIndex = GoalIndex;
			m_vFentBotTrainDist.assign(W * H, -1);
			bool Seeded = false;
			int SeedIndex = GoalIndex;
			if(IsBlockedIndex(SeedIndex))
			{
				// Waypoint can be inside/over blocked tiles (freeze/solid). Find
				// nearest walkable tile as a seed.
				s_DistGoalBlockedSamples++;
				const int gx = SeedIndex % W;
				const int gy = SeedIndex / W;
				const int MaxR = 8;
				for(int r = 1; r <= MaxR && !Seeded; r++)
				{
					for(int dy = -r; dy <= r && !Seeded; dy++)
					{
						for(int dx = -r; dx <= r && !Seeded; dx++)
						{
							// Search only the border of the square for efficiency.
							if(std::abs(dx) != r && std::abs(dy) != r)
								continue;
							const int nx = gx + dx;
							const int ny = gy + dy;
							if(nx < 0 || ny < 0 || nx >= W || ny >= H)
								continue;
							const int N = ny * W + nx;
							if(IsBlockedIndex(N))
								continue;
							SeedIndex = N;
							Seeded = true;
						}
					}
				}
			}
			else
			{
				Seeded = true;
			}

			if(Seeded)
			{
				std::queue<int> Q;
				m_vFentBotTrainDist[SeedIndex] = 0;
				Q.push(SeedIndex);
				while(!Q.empty())
				{
					const int Cur = Q.front();
					Q.pop();
					const int d = m_vFentBotTrainDist[Cur];
					const int x = Cur % W;
					const int y = Cur / W;
					const int aNx[4] = {x - 1, x + 1, x, x};
					const int aNy[4] = {y, y, y - 1, y + 1};
					for(int i = 0; i < 4; i++)
					{
						const int nx = aNx[i];
						const int ny = aNy[i];
						if(nx < 0 || ny < 0 || nx >= W || ny >= H)
							continue;
						const int N = ny * W + nx;
						if(m_vFentBotTrainDist[N] != -1)
							continue;
						if(IsBlockedIndex(N))
							continue;
						m_vFentBotTrainDist[N] = d + 1;
						Q.push(N);
					}
				}
				s_DistSeededSamples++;
			}
		}
	}

	// Capture a stable reset snapshot once (avoid resetting to a frozen/dead live
	// state).
	if(!m_FentBotTrainResetWorldInited)
	{
		m_FentBotTrainResetWorld.CopyWorldClean(&m_RegularPredictedWorld);
		m_FentBotTrainResetWorld.m_GameTick = NowTick;
		m_FentBotTrainResetWorldInited =
			m_FentBotTrainResetWorld.GetCharacterById(LocalId) != nullptr;
	}

	int Simulated = 0;
	int EpisodesFinishedThisFrame = 0;
	const int StagnantLimit = 64;

	auto StartNewEpisode = [&]() -> bool {
		if(m_FentBotTrainResetWorldInited)
			m_FentBotTrainWorld.CopyWorldClean(&m_FentBotTrainResetWorld);
		else
			m_FentBotTrainWorld.CopyWorldClean(&m_RegularPredictedWorld);
		m_FentBotTrainWorld.m_GameTick = NowTick;
		CCharacter *pChar0 = m_FentBotTrainWorld.GetCharacterById(LocalId);
		if(!pChar0)
			return false;
		const vec2 Goal0 = m_FentBotWaypoint;
		m_FentBotTrainEpStep = 0;
		m_FentBotTrainEpReturn = 0.0f;
		// Use BFS tile distance when available, else fallback to euclidean.
		int StartIdx = Collision()->GetPureMapIndex(pChar0->Core()->m_Pos);
		if(StartIdx >= 0 && StartIdx < (int)m_vFentBotTrainDist.size() &&
			m_vFentBotTrainDist[StartIdx] >= 0)
			m_FentBotTrainEpBestDist = (float)m_vFentBotTrainDist[StartIdx];
		else
			m_FentBotTrainEpBestDist = distance(pChar0->Core()->m_Pos, Goal0);
		m_FentBotTrainEpStagnantSteps = 0;
		m_vFentBotTrainEpPos.clear();
		m_vFentBotTrainEpPos.reserve(256);
		m_FentBotTrainWorldInited = true;
		return true;
	};

	if(!m_FentBotTrainWorldInited)
	{
		if(!StartNewEpisode())
			return;
	}

	while(Simulated < TicksBudget)
	{
		if(time_get() - FrameStart > FrameBudget)
			break;

		CCharacter *pChar = m_FentBotTrainWorld.GetCharacterById(LocalId);
		if(!pChar)
		{
			m_FentBotTrainWorldInited = false;
			break;
		}

		const vec2 Goal = m_FentBotWaypoint;

		const vec2 Pos0 = pChar->Core()->m_Pos;
		const vec2 Vel0 = pChar->Core()->m_Vel;
		m_vFentBotTrainEpPos.push_back(Pos0);
		const int Pos0Idx = Collision()->GetPureMapIndex(Pos0);
		const int Dist0 =
			(Pos0Idx >= 0 && Pos0Idx < (int)m_vFentBotTrainDist.size()) ? m_vFentBotTrainDist[Pos0Idx] : -1;

		// Early termination if we're already done.
		if(IsFreezeAt(Pos0) || distance(Pos0, Goal) < 32.0f ||
			m_FentBotTrainEpStep >= MaxSteps)
		{
			if(IsFreezeAt(Pos0))
				m_FentBotTrainEpisodesFreeze++;
			else if(distance(Pos0, Goal) < 32.0f)
				m_FentBotTrainEpisodesGoal++;
			else
				m_FentBotTrainEpisodesMaxSteps++;
			m_FentBotTrainEpisodeStepsTotal += (uint64_t)m_FentBotTrainEpStep;
			// finalize episode
			m_vFentBotTrainEpPos.push_back(Pos0);
			m_FentBotTrainEpisodesDone++;
			EpisodesFinishedThisFrame++;
			if(m_FentBotTrainEpReturn > m_FentBotTrainBestReturn)
			{
				m_FentBotTrainBestReturn = m_FentBotTrainEpReturn;
				m_FentBotBestPathLen = std::clamp((int)m_vFentBotTrainEpPos.size(), 0,
					(int)std::size(m_aFentBotBestPath));
				for(int i = 0; i < m_FentBotBestPathLen; i++)
					m_aFentBotBestPath[i] = m_vFentBotTrainEpPos[i];
			}
			m_FentBotTrainWorldInited = false;
			if(!StartNewEpisode())
				break;
			continue;
		}

		const vec2 d = Goal - Pos0;
		std::array<float, FENTBOT_OBS_DIM> Obs;
		BuildObs(Pos0, Vel0, Goal, Obs);
		float aLogits[ActionCount];
		float V0 = 0.0f;
		ForwardPpo(Obs, aLogits, V0);
		float aLogPAll[ActionCount];
		LogSoftmax(aLogits, aLogPAll);
		int Action = SampleAction(aLogPAll);

		int Dir = 0;
		bool Jump = false;
		bool Hook = false;
		int AimIndex = 0;
		DecodeAction(Action, Dir, Jump, Hook, AimIndex);
		m_FentBotTrainActionsTotal++;
		if(Hook)
			m_FentBotTrainActionsHook++;
		if(Jump)
			m_FentBotTrainActionsJump++;

		CNetObj_PlayerInput Act{};
		Act.m_Direction = Dir;
		Act.m_TargetX = (int)round_to_int(s_aAimCands[AimIndex].x * 1000.0f);
		Act.m_TargetY = (int)round_to_int(s_aAimCands[AimIndex].y * 1000.0f);
		if(!Act.m_TargetX && !Act.m_TargetY)
			Act.m_TargetY = -1;
		Act.m_Hook = Hook ? 1 : 0;
		if(Jump)
			Act.m_Jump = (Act.m_Jump + 2) | 1;

		float Reward = 0.0f;
		Reward += StepPenalty;
		if(Hook)
		{
			vec2 HitPos;
			int TeleNr = 0;
			const int Hit = Collision()->IntersectLineTeleHook(
				Pos0,
				Pos0 +
					vec2(s_aAimCands[AimIndex].x, s_aAimCands[AimIndex].y) * HookLen,
				&HitPos, nullptr, &TeleNr);
			if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK)
				Reward -= 2.0f;
			else if(IsFreezeAt(HitPos))
				Reward -= 10.0f;
		}

		pChar->OnPredictedInput(&Act);
		m_FentBotTrainWorld.Tick();
		Simulated++;
		m_FentBotTrainSteps++;
		m_FentBotTrainEpStep++;

		// After Tick(), the character can disappear (death/freeze interactions), so
		// refetch.
		CCharacter *pCharAfter = m_FentBotTrainWorld.GetCharacterById(LocalId);
		if(!pCharAfter)
		{
			const float Reward = DeathPenalty;
			std::array<float, FENTBOT_OBS_DIM> NObs = Obs;
			SFentRolloutStep T;
			T.m_Obs = Obs;
			T.m_NObs = NObs;
			T.m_A = Action;
			T.m_R = Reward;
			T.m_D = true;
			T.m_V = V0;
			T.m_LogP = aLogPAll[Action];
			if(!m_vFentBotRollout.empty() &&
				m_FentBotRolloutPos < m_FentBotRolloutSize)
				m_vFentBotRollout[m_FentBotRolloutPos++] = T;
			m_FentBotTrainEpReturn += Reward;
			m_FentBotTrainEpisodesDone++;
			m_FentBotTrainEpisodesDeath++;
			m_FentBotTrainEpisodeStepsTotal += (uint64_t)m_FentBotTrainEpStep;
			m_FentBotTrainWorldInited = false;
			if(!StartNewEpisode())
				break;
			continue;
		}
		pChar = pCharAfter;

		const vec2 Pos1 = pChar->Core()->m_Pos;
		const vec2 Vel1 = pChar->Core()->m_Vel;
		const int Pos1Idx = Collision()->GetPureMapIndex(Pos1);
		const int Dist1 =
			(Pos1Idx >= 0 && Pos1Idx < (int)m_vFentBotTrainDist.size()) ? m_vFentBotTrainDist[Pos1Idx] : -1;
		const float D0 = distance(Pos0, Goal);
		const float D1 = distance(Pos1, Goal);
		float ProgressReward = 0.0f;
		// Prefer tile-distance potential shaping when available; euclidean shaping
		// can be misleading on maps with freeze traps.
		if(Dist0 >= 0 && Dist1 >= 0)
			ProgressReward = TileProgressScale * (float)(Dist0 - Dist1);
		else
			ProgressReward = EuclidProgressScale * (D0 - D1);
		ProgressReward =
			std::clamp(ProgressReward, -ProgressClipAbs, ProgressClipAbs);
		Reward += ProgressReward;

		s_DistSamples++;
		if(Dist0 >= 0 && Dist1 >= 0)
			s_DistValidSamples++;

		const float ProgressMetric = (Dist1 >= 0) ? (float)Dist1 : D1;
		if(ProgressMetric + 1.0f < m_FentBotTrainEpBestDist)
		{
			m_FentBotTrainEpBestDist = ProgressMetric;
			m_FentBotTrainEpStagnantSteps = 0;
		}
		else
		{
			m_FentBotTrainEpStagnantSteps++;
			if(m_FentBotTrainEpStagnantSteps >= StagnantLimit)
				Reward += StuckPenalty;
		}

		bool Done = false;
		if(IsFreezeAt(Pos1))
		{
			// Make freeze terminal clearly negative and not compensatable by shaping.
			Reward = FreezePenalty;
			Done = true;
		}
		if(distance(Pos1, Goal) < 32.0f)
		{
			Reward += GoalBonus;
			Done = true;
		}
		if(m_FentBotTrainEpStagnantSteps >= StagnantLimit)
			Done = true;
		if(m_FentBotTrainEpStep >= MaxSteps)
			Done = true;

		std::array<float, FENTBOT_OBS_DIM> NObs;
		BuildObs(Pos1, Vel1, Goal, NObs);
		SFentRolloutStep T;
		T.m_Obs = Obs;
		T.m_NObs = NObs;
		T.m_A = Action;
		T.m_R = Reward;
		T.m_D = Done;
		T.m_V = V0;
		T.m_LogP = aLogPAll[Action];
		if(!m_vFentBotRollout.empty() &&
			m_FentBotRolloutPos < m_FentBotRolloutSize)
			m_vFentBotRollout[m_FentBotRolloutPos++] = T;

		m_FentBotTrainEpReturn += Reward;

		if(m_FentBotRolloutSize > 0 &&
			m_FentBotRolloutPos >= m_FentBotRolloutSize)
		{
			std::vector<float> vAdv(m_FentBotRolloutSize);
			std::vector<float> vRet(m_FentBotRolloutSize);
			for(int i = m_FentBotRolloutSize - 1; i >= 0; i--)
			{
				const SFentRolloutStep &Samp = m_vFentBotRollout[i];
				float Vn = 0.0f;
				if(!Samp.m_D)
				{
					float aTmp[ActionCount];
					ForwardPpo(Samp.m_NObs, aTmp, Vn);
				}
				const float Delta = Samp.m_R + Gamma * Vn - Samp.m_V;
				const float NextAdv =
					(i + 1 < m_FentBotRolloutSize && !Samp.m_D) ? vAdv[i + 1] : 0.0f;
				vAdv[i] = Delta + Gamma * PpoLambda * NextAdv;
				vRet[i] = vAdv[i] + Samp.m_V;
			}
			float Mean = 0.0f;
			for(float a : vAdv)
				Mean += a;
			Mean /= (float)std::max(1, (int)vAdv.size());
			float Var = 0.0f;
			for(float a : vAdv)
			{
				const float d = a - Mean;
				Var += d * d;
			}
			Var /= (float)std::max(1, (int)vAdv.size());
			const float Std = sqrtf(std::max(1e-8f, Var));
			for(float &a : vAdv)
				a = (a - Mean) / Std;

			std::vector<int> vIdx(m_FentBotRolloutSize);
			for(int i = 0; i < m_FentBotRolloutSize; i++)
				vIdx[i] = i;
			for(int e = 0; e < PpoEpochs; e++)
			{
				std::shuffle(vIdx.begin(), vIdx.end(), s_Rng);
				for(int mb0 = 0; mb0 < m_FentBotRolloutSize; mb0 += Batch)
				{
					const int mb1 = std::min(m_FentBotRolloutSize, mb0 + Batch);
					const int Mb = mb1 - mb0;
					std::array<float, FENTBOT_HIDDEN * FENTBOT_OBS_DIM> gW1{};
					std::array<float, FENTBOT_HIDDEN> gB1{};
					std::array<float, ActionCount * FENTBOT_HIDDEN> gWpi{};
					std::array<float, ActionCount> gBpi{};
					std::array<float, FENTBOT_HIDDEN> gWv{};
					float gBv = 0.0f;

					for(int it = mb0; it < mb1; it++)
					{
						const int i = vIdx[it];
						const SFentRolloutStep &Samp = m_vFentBotRollout[i];
						float aZ[FENTBOT_HIDDEN];
						float aH[FENTBOT_HIDDEN];
						for(int h = 0; h < FENTBOT_HIDDEN; h++)
						{
							float v = m_aFentBotPpoB1[h];
							const int Base = h * FENTBOT_OBS_DIM;
							for(int j = 0; j < FENTBOT_OBS_DIM; j++)
								v += m_aFentBotPpoW1[Base + j] * Samp.m_Obs[j];
							aZ[h] = v;
							aH[h] = v > 0.0f ? v : 0.0f;
						}
						float aLogits2[ActionCount];
						for(int a = 0; a < ActionCount; a++)
						{
							float q = m_aFentBotPpoBpi[a];
							const int Base = a * FENTBOT_HIDDEN;
							for(int h = 0; h < FENTBOT_HIDDEN; h++)
								q += m_aFentBotPpoWpi[Base + h] * aH[h];
							aLogits2[a] = q;
						}
						float aLogP2[ActionCount];
						LogSoftmax(aLogits2, aLogP2);
						const float LogpNew = aLogP2[Samp.m_A];
						const float Ratio = expf(LogpNew - Samp.m_LogP);
						const float Adv = vAdv[i];
						bool UseGrad = true;
						if(Adv > 0.0f && Ratio > 1.0f + PpoClip)
							UseGrad = false;
						if(Adv < 0.0f && Ratio < 1.0f - PpoClip)
							UseGrad = false;
						const float dLogp = UseGrad ? (-(Adv)*Ratio) : 0.0f;
						float aGradLogits[ActionCount];
						float EntA = 0.0f;
						for(int a = 0; a < ActionCount; a++)
						{
							const float p = expf(aLogP2[a]);
							EntA += p * (aLogP2[a] + 1.0f);
						}
						for(int a = 0; a < ActionCount; a++)
						{
							const float p = expf(aLogP2[a]);
							const float one = (a == Samp.m_A) ? 1.0f : 0.0f;
							aGradLogits[a] = dLogp * (one - p);
							aGradLogits[a] += EntCoef * p * ((aLogP2[a] + 1.0f) - EntA);
						}
						float Vp = m_FentBotPpoBv;
						for(int h = 0; h < FENTBOT_HIDDEN; h++)
							Vp += m_aFentBotPpoWv[h] * aH[h];
						const float dV = VfCoef * (Vp - vRet[i]);

						float aGradH[FENTBOT_HIDDEN];
						for(int h = 0; h < FENTBOT_HIDDEN; h++)
							aGradH[h] = dV * m_aFentBotPpoWv[h];
						gBv += dV;
						for(int h = 0; h < FENTBOT_HIDDEN; h++)
							gWv[h] += dV * aH[h];

						for(int a = 0; a < ActionCount; a++)
						{
							gBpi[a] += aGradLogits[a];
							const int Base = a * FENTBOT_HIDDEN;
							for(int h = 0; h < FENTBOT_HIDDEN; h++)
							{
								gWpi[Base + h] += aGradLogits[a] * aH[h];
								aGradH[h] += m_aFentBotPpoWpi[Base + h] * aGradLogits[a];
							}
						}
						for(int h = 0; h < FENTBOT_HIDDEN; h++)
						{
							float gz = aZ[h] > 0.0f ? aGradH[h] : 0.0f;
							gB1[h] += gz;
							const int Base = h * FENTBOT_OBS_DIM;
							for(int j = 0; j < FENTBOT_OBS_DIM; j++)
								gW1[Base + j] += gz * Samp.m_Obs[j];
						}
					}
					const float invMb = 1.0f / (float)std::max(1, Mb);
					for(float &v : gW1)
						v *= invMb;
					for(float &v : gB1)
						v *= invMb;
					for(float &v : gWpi)
						v *= invMb;
					for(float &v : gBpi)
						v *= invMb;
					for(float &v : gWv)
						v *= invMb;
					gBv *= invMb;
					for(size_t k = 0; k < m_aFentBotPpoW1.size(); k++)
						m_aFentBotPpoW1[k] -= Lr * gW1[k];
					for(size_t k = 0; k < m_aFentBotPpoB1.size(); k++)
						m_aFentBotPpoB1[k] -= Lr * gB1[k];
					for(size_t k = 0; k < m_aFentBotPpoWpi.size(); k++)
						m_aFentBotPpoWpi[k] -= Lr * gWpi[k];
					for(size_t k = 0; k < m_aFentBotPpoBpi.size(); k++)
						m_aFentBotPpoBpi[k] -= Lr * gBpi[k];
					for(size_t k = 0; k < m_aFentBotPpoWv.size(); k++)
						m_aFentBotPpoWv[k] -= Lr * gWv[k];
					m_FentBotPpoBv -= Lr * gBv;
				}
			}
			m_FentBotRolloutPos = 0;
		}

		if(Done)
		{
			if(IsFreezeAt(Pos1))
				m_FentBotTrainEpisodesFreeze++;
			else if(distance(Pos1, Goal) < 32.0f)
				m_FentBotTrainEpisodesGoal++;
			else if(m_FentBotTrainEpStagnantSteps >= StagnantLimit)
				m_FentBotTrainEpisodesStuck++;
			else if(m_FentBotTrainEpStep >= MaxSteps)
				m_FentBotTrainEpisodesMaxSteps++;
			m_FentBotTrainEpisodeStepsTotal += (uint64_t)m_FentBotTrainEpStep;

			m_vFentBotTrainEpPos.push_back(Pos1);
			m_FentBotTrainEpisodesDone++;
			EpisodesFinishedThisFrame++;
			if(m_FentBotTrainEpReturn > m_FentBotTrainBestReturn)
			{
				m_FentBotTrainBestReturn = m_FentBotTrainEpReturn;
				m_FentBotBestPathLen = std::clamp((int)m_vFentBotTrainEpPos.size(), 0,
					(int)std::size(m_aFentBotBestPath));
				for(int i = 0; i < m_FentBotBestPathLen; i++)
					m_aFentBotBestPath[i] = m_vFentBotTrainEpPos[i];
			}
			m_FentBotTrainWorldInited = false;
			if(!StartNewEpisode())
				break;
		}
	}

	// Periodic progress log, even when episodes_per_batch is large /
	// ticks_per_frame is small.
	static int64_t s_LastLog = 0;
	const int64_t Now = time_get();
	if(Now - s_LastLog > time_freq())
	{
		s_LastLog = Now;
		char aMsg[512];
		const double Total =
			(double)std::max<uint64_t>(1, m_FentBotTrainActionsTotal);
		const double HookRate = 100.0 * (double)m_FentBotTrainActionsHook / Total;
		const double JumpRate = 100.0 * (double)m_FentBotTrainActionsJump / Total;
		const double EpCount =
			(double)std::max<uint64_t>(1, (uint64_t)m_FentBotTrainEpisodesDone);
		const double AvgLen = (double)m_FentBotTrainEpisodeStepsTotal / EpCount;
		const double DistValidPct = 100.0 * (double)s_DistValidSamples /
					    (double)std::max<uint64_t>(1, s_DistSamples);
		const double DistSeededPct = 100.0 * (double)s_DistSeededSamples /
					     (double)std::max<uint64_t>(1, s_DistSamples);
		const double DistGoalBlockedPct =
			100.0 * (double)s_DistGoalBlockedSamples /
			(double)std::max<uint64_t>(1, s_DistSamples);
		str_format(aMsg, sizeof(aMsg),
			"train: ep=%d step=%llu best=%.1f eps=%.2f hook=%.1f%% "
			"jump=%.1f%% avg_len=%.1f dist_ok=%.1f%% seeded=%.1f%% "
			"goal_blk=%.1f%% end(g=%llu f=%llu s=%llu d=%llu m=%llu)",
			m_FentBotTrainEpisodesDone,
			(unsigned long long)m_FentBotTrainSteps,
			m_FentBotTrainBestReturn, EpsTrain, (float)HookRate,
			(float)JumpRate, (float)AvgLen, (float)DistValidPct,
			(float)DistSeededPct, (float)DistGoalBlockedPct,
			(unsigned long long)m_FentBotTrainEpisodesGoal,
			(unsigned long long)m_FentBotTrainEpisodesFreeze,
			(unsigned long long)m_FentBotTrainEpisodesStuck,
			(unsigned long long)m_FentBotTrainEpisodesDeath,
			(unsigned long long)m_FentBotTrainEpisodesMaxSteps);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
	}
}

void CGameClient::OnDummySwap(int PreviousConn)
{
	const int PairConn = Client()->DummyPair();
	if(g_Config.m_ClDummyResetOnSwitch)
	{
		int PlayerOrDummy =
			(g_Config.m_ClDummyResetOnSwitch == 2) ? g_Config.m_ClDummy : PairConn;
		m_Controls.ResetInput(PlayerOrDummy);
		m_Controls.m_aInputData[PlayerOrDummy].m_Hook = 0;
	}
	// The legacy swap logic assumed exactly one dummy, so every switch was a
	// mutual main<->dummy swap. With two dummies, D1->D2 keeps main as the pair;
	// copying the old pair fire counter into D2 can drop or stick a shot. Only
	// exchange counters when the newly active connection really was the old
	// pair and the new pair really was the previously active connection.
	const bool MutualPairSwap =
		m_DummyInputConn == g_Config.m_ClDummy && PairConn == PreviousConn;
	const int PrevDummyFire = m_DummyInput.m_Fire;
	m_DummyInput = m_Controls.m_aInputData[PairConn];
	m_DummyInputConn = PairConn;
	if(MutualPairSwap)
		m_Controls.m_aInputData[g_Config.m_ClDummy].m_Fire = PrevDummyFire;
	else
	{
		m_HammerInput = m_DummyInput;
		m_DummyFire = 0;
	}
	m_IsDummySwapping = 1;
	m_DummySwapFrom = PreviousConn;
}

float CGameClient::AutoUnfreezeLearningBias(int DummyIndex, uint64_t Key,
	int Tick)
{
	if(DummyIndex < 0 || DummyIndex >= NUM_DUMMIES || Key == 0)
		return 0.0f;
	const int Start = (int)(Key & (AUTO_UNFREEZE_LEARNING_ENTRIES - 1));
	for(int Probe = 0; Probe < AUTO_UNFREEZE_LEARNING_ENTRIES; Probe++)
	{
		auto &Entry =
			m_aaAutoUnfreezeLearning[DummyIndex]
						[(Start + Probe) &
							(AUTO_UNFREEZE_LEARNING_ENTRIES - 1)];
		if(Entry.m_Key == 0)
			return 0.0f;
		if(Entry.m_Key != Key)
			continue;
		Entry.m_LastUsedTick = Tick;
		return Entry.m_Score;
	}
	return 0.0f;
}

void CGameClient::AutoUnfreezeApplyFeedback(int DummyIndex, uint64_t Key,
	int Result, int Tick)
{
	if(DummyIndex < 0 || DummyIndex >= NUM_DUMMIES || Key == 0)
		return;

	float Delta = 0.0f;
	bool Success = false;
	switch(Result)
	{
	case AUTO_UNFREEZE_OBS_HIT_WHILE_FROZEN:
		Delta = 24.0f;
		Success = true;
		break;
	case AUTO_UNFREEZE_OBS_HIT_BEFORE_FREEZE:
		Delta = -30.0f;
		break;
	case AUTO_UNFREEZE_OBS_STILL_FROZEN:
		Delta = -42.0f;
		break;
	case AUTO_UNFREEZE_OBS_TRAJECTORY_MISSED:
		Delta = -18.0f;
		break;
	default:
		// A shot rejected by the server is an input-delivery problem, not evidence
		// that the selected geometry was wrong.
		return;
	}

	SAutoUnfreezeLearningEntry *pEntry = nullptr;
	const int Start = (int)(Key & (AUTO_UNFREEZE_LEARNING_ENTRIES - 1));
	for(int Probe = 0; Probe < AUTO_UNFREEZE_LEARNING_ENTRIES; Probe++)
	{
		auto &Entry =
			m_aaAutoUnfreezeLearning[DummyIndex]
						[(Start + Probe) &
							(AUTO_UNFREEZE_LEARNING_ENTRIES - 1)];
		if(Entry.m_Key == Key)
		{
			pEntry = &Entry;
			break;
		}
		if(Entry.m_Key == 0)
		{
			pEntry = &Entry;
			break;
		}
	}
	if(!pEntry)
	{
		pEntry = &m_aaAutoUnfreezeLearning[DummyIndex][0];
		for(auto &Entry : m_aaAutoUnfreezeLearning[DummyIndex])
			if(Entry.m_LastUsedTick < pEntry->m_LastUsedTick)
				pEntry = &Entry;
	}
	if(pEntry->m_Key != Key)
	{
		*pEntry = {};
		pEntry->m_Key = Key;
	}

	// Repeated evidence matters, but a route can recover after movement or
	// server timing changes. The bounded score prevents permanent lockout.
	const int TotalBefore = pEntry->m_Successes + pEntry->m_Failures;
	const float Confidence = 1.0f - 0.35f / (float)maximum(1, TotalBefore + 1);
	pEntry->m_Score =
		std::clamp(pEntry->m_Score * Confidence + Delta, -120.0f, 60.0f);
	if(Success)
		pEntry->m_Successes++;
	else
		pEntry->m_Failures++;
	pEntry->m_LastUsedTick = Tick;
}

void CGameClient::AutoUnfreezeStartShotObservation(
	int DummyIndex, int LocalId, int PredTick, int GameTick,
	int EstimatedServerShotTick, int ExpectedHitTick, int ExpireTick,
	int AttackTick, const vec2 *pExpectedOrigins,
	const int *pExpectedPredictionLeads, int NumExpectedOrigins,
	const vec2 &Direction, uint64_t LearningKey, int Bounces, float Quality)
{
	if(DummyIndex < 0 || DummyIndex >= NUM_DUMMIES)
		return;
	SAutoUnfreezeShotObservation &Observation =
		m_aAutoUnfreezeShotObservation[DummyIndex];
	Observation = {};
	Observation.m_Active = true;
	Observation.m_LocalId = LocalId;
	Observation.m_SentPredTick = PredTick;
	Observation.m_SentGameTick = GameTick;
	Observation.m_EstimatedServerShotTick = EstimatedServerShotTick;
	Observation.m_ExpectedHitTick = ExpectedHitTick;
	Observation.m_ExpireTick = ExpireTick;
	Observation.m_AttackTickBefore = AttackTick;
	Observation.m_PlannedDirection = Direction;
	Observation.m_LearningKey = LearningKey;
	Observation.m_Bounces = Bounces;
	Observation.m_PlannedQuality = Quality;
	Observation.m_NumExpectedOrigins = std::clamp(
		NumExpectedOrigins, 0, (int)std::size(Observation.m_aExpectedOrigins));
	for(int Index = 0; Index < Observation.m_NumExpectedOrigins; Index++)
	{
		Observation.m_aExpectedOrigins[Index] = pExpectedOrigins[Index];
		Observation.m_aExpectedPredictionLeads[Index] =
			pExpectedPredictionLeads[Index];
	}
}

void CGameClient::AutoUnfreezeObserveRawLasers()
{
	for(int DummyIndex = 0; DummyIndex < NUM_DUMMIES; DummyIndex++)
	{
		const int LocalId = m_aLocalIds[DummyIndex];
		if(LocalId < 0 || LocalId >= MAX_CLIENTS ||
			!m_Snap.m_aCharacters[LocalId].m_Active)
			continue;

		const int SnapshotTick = Client()->GameTick(DummyIndex);
		const vec2 TeePos((float)m_Snap.m_aCharacters[LocalId].m_Cur.m_X,
			(float)m_Snap.m_aCharacters[LocalId].m_Cur.m_Y);
		const bool TeeFrozen = m_aClients[LocalId].m_FreezeEnd > SnapshotTick;
		SAutoUnfreezeTeeHistory &History =
			m_aaAutoUnfreezeTeeHistory[DummyIndex]
						  [m_aAutoUnfreezeTeeHistoryWrite[DummyIndex]];
		History.m_Tick = SnapshotTick;
		History.m_Pos = TeePos;
		History.m_Frozen = TeeFrozen;
		m_aAutoUnfreezeTeeHistoryWrite[DummyIndex] =
			(m_aAutoUnfreezeTeeHistoryWrite[DummyIndex] + 1) %
			AUTO_UNFREEZE_OBSERVER_HISTORY;

		SAutoUnfreezeShotObservation &Observation =
			m_aAutoUnfreezeShotObservation[DummyIndex];
		if(!Observation.m_Active || Observation.m_LocalId != LocalId)
			continue;
		Observation.m_FreezeSeen |= TeeFrozen;
		if(m_Snap.m_pLocalCharacter &&
			m_Snap.m_pLocalCharacter->m_AttackTick !=
				Observation.m_AttackTickBefore &&
			m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_LASER)
			Observation.m_AttackConfirmed = true;

		auto ClosestHistory = [&](int Tick) -> const SAutoUnfreezeTeeHistory * {
			const SAutoUnfreezeTeeHistory *pBest = nullptr;
			int BestDelta = std::numeric_limits<int>::max();
			for(const auto &Sample : m_aaAutoUnfreezeTeeHistory[DummyIndex])
			{
				if(Sample.m_Tick < 0)
					continue;
				const int Delta = absolute(Sample.m_Tick - Tick);
				if(Delta < BestDelta)
				{
					BestDelta = Delta;
					pBest = &Sample;
				}
			}
			return pBest;
		};
		auto SegmentDistance = [](const vec2 &Point, const vec2 &From,
					       const vec2 &To) {
			const vec2 Segment = To - From;
			const float LengthSq = dot(Segment, Segment);
			if(LengthSq < 0.001f)
				return distance(Point, From);
			const float Parameter =
				std::clamp(dot(Point - From, Segment) / LengthSq, 0.0f, 1.0f);
			return distance(Point, From + Segment * Parameter);
		};

		for(const CSnapEntities &Entity : SnapEntities())
		{
			if(Entity.m_Item.m_Type != NETOBJTYPE_LASER &&
				Entity.m_Item.m_Type != NETOBJTYPE_DDNETLASER)
				continue;
			const CLaserData Data =
				ExtractLaserInfo(Entity.m_Item.m_Type, Entity.m_Item.m_pData,
					&m_GameWorld, Entity.m_pDataEx);
			if(Data.m_ExtraInfo && Data.m_Owner >= 0 && Data.m_Owner != LocalId)
				continue;
			const vec2 Segment = Data.m_To - Data.m_From;
			if(length(Segment) < 0.001f)
				continue;

			if(!Observation.m_RawMatched)
			{
				if(Data.m_StartTick < Observation.m_SentGameTick - 2 ||
					Data.m_StartTick > Observation.m_ExpectedHitTick + 2)
					continue;
				float BestOriginDistance = std::numeric_limits<float>::infinity();
				int BestOriginIndex = -1;
				for(int OriginIndex = 0;
					OriginIndex < Observation.m_NumExpectedOrigins; OriginIndex++)
				{
					const float OriginDistance = distance(
						Data.m_From, Observation.m_aExpectedOrigins[OriginIndex]);
					if(OriginDistance < BestOriginDistance)
					{
						BestOriginDistance = OriginDistance;
						BestOriginIndex = OriginIndex;
					}
				}
				const bool OwnerMatches = Data.m_ExtraInfo && Data.m_Owner == LocalId;
				const bool DirectionMatches =
					dot(normalize(Segment), Observation.m_PlannedDirection) > 0.90f;
				if(!OwnerMatches && (BestOriginDistance > 96.0f || !DirectionMatches))
					continue;
				Observation.m_RawMatched = true;
				Observation.m_AttackConfirmed = true;
				Observation.m_RawItemId = Entity.m_Item.m_Id;
				Observation.m_FirstRawStartTick = Data.m_StartTick;
				Observation.m_FirstRawOrigin = Data.m_From;
				Observation.m_FirstRawTarget = Data.m_To;
				Observation.m_FirstObservedSegmentOutgoing =
					BestOriginIndex >= 0 && BestOriginDistance <= 64.0f &&
					DirectionMatches;

				// Only an outgoing segment is suitable for calibration. A first
				// observed reflected segment has a wall as its origin and would poison
				// the model.
				if(Observation.m_FirstObservedSegmentOutgoing)
				{
					const int PredictionLead =
						std::clamp(Observation.m_SentPredTick - Data.m_StartTick, 0,
							AUTO_UNFREEZE_MAX_PREDICTION_LEAD);
					Observation.m_ObservedPredictionLead = PredictionLead;
					m_aaAutoUnfreezeObservedPredictionLeadCount[DummyIndex]
										   [PredictionLead]++;
					int LeadSamples = 0;
					float LeadTotal = 0.0f;
					for(int Lead = 0; Lead <= AUTO_UNFREEZE_MAX_PREDICTION_LEAD;
						Lead++)
					{
						LeadSamples +=
							m_aaAutoUnfreezeObservedPredictionLeadCount[DummyIndex][Lead];
						LeadTotal +=
							(float)(Lead *
								m_aaAutoUnfreezeObservedPredictionLeadCount[DummyIndex]
													   [Lead]);
					}
					m_aAutoUnfreezeObservedPredictionLead[DummyIndex] =
						LeadSamples > 0 ? LeadTotal / (float)LeadSamples : 0.0f;

					vec2 OriginOffset =
						Data.m_From - Observation.m_aExpectedOrigins[BestOriginIndex];
					if(length(OriginOffset) <= 64.0f)
					{
						const int Samples = m_aAutoUnfreezeObservedSamples[DummyIndex];
						const float Weight =
							Samples < 6 ? 1.0f / (float)(Samples + 1) : 0.15f;
						m_aAutoUnfreezeObservedOriginOffset[DummyIndex] =
							mix(m_aAutoUnfreezeObservedOriginOffset[DummyIndex],
								OriginOffset, Weight);
						m_aAutoUnfreezeObservedSamples[DummyIndex] =
							minimum(Samples + 1, 1000000);
					}
				}
			}
			if(Entity.m_Item.m_Id != Observation.m_RawItemId)
				continue;

			bool Duplicate = false;
			for(int Index = 0; Index < Observation.m_NumSegments; Index++)
			{
				const auto &Old = Observation.m_aSegments[Index];
				if(Old.m_StartTick == Data.m_StartTick &&
					distance(Old.m_From, Data.m_From) < 0.5f &&
					distance(Old.m_To, Data.m_To) < 0.5f)
				{
					Duplicate = true;
					break;
				}
			}
			if(Duplicate ||
				Observation.m_NumSegments >= AUTO_UNFREEZE_OBSERVER_MAX_SEGMENTS)
				continue;

			SAutoUnfreezeObservedSegment &Observed =
				Observation.m_aSegments[Observation.m_NumSegments++];
			Observed.m_SegmentIndex = Observation.m_NumSegments - 1;
			Observed.m_SnapshotTick = SnapshotTick;
			Observed.m_ItemId = Entity.m_Item.m_Id;
			Observed.m_StartTick = Data.m_StartTick;
			Observed.m_Owner = Data.m_Owner;
			Observed.m_Type = Data.m_Type;
			Observed.m_From = Data.m_From;
			Observed.m_To = Data.m_To;
			Observed.m_Returning = Observed.m_SegmentIndex > 0 ||
					       !Observation.m_FirstObservedSegmentOutgoing;
			const SAutoUnfreezeTeeHistory *pTee = ClosestHistory(Data.m_StartTick);
			if(pTee)
			{
				Observed.m_TeeHistoryTick = pTee->m_Tick;
				Observed.m_TeeFrozen = pTee->m_Frozen;
				Observed.m_DistanceToTee =
					SegmentDistance(pTee->m_Pos, Data.m_From, Data.m_To);
				if(Observed.m_Returning &&
					(Observation.m_ClosestSegmentDistance < 0.0f ||
						Observed.m_DistanceToTee < Observation.m_ClosestSegmentDistance))
					Observation.m_ClosestSegmentDistance = Observed.m_DistanceToTee;
				if(Observed.m_Returning &&
					Observed.m_DistanceToTee <= CCharacterCore::PhysicalSize() + 2.0f)
				{
					Observed.m_CrossedWhileFrozen = pTee->m_Frozen;
					Observation.m_CrossedTeeWhileFrozen |= pTee->m_Frozen;
					Observation.m_CrossedTeeBeforeFreeze |= !pTee->m_Frozen;
				}
			}
			Observation.m_LastSeenSnapshotTick = SnapshotTick;
		}

		if(Observation.m_CrossedTeeWhileFrozen ||
			(Observation.m_FreezeSeen && !TeeFrozen &&
				Observation.m_AttackConfirmed))
			Observation.m_Result = AUTO_UNFREEZE_OBS_HIT_WHILE_FROZEN;
		else if(SnapshotTick > Observation.m_ExpireTick + 2)
		{
			if(!Observation.m_AttackConfirmed && !Observation.m_RawMatched)
				Observation.m_Result = AUTO_UNFREEZE_OBS_NO_SERVER_SHOT;
			else if(TeeFrozen || Observation.m_FreezeSeen)
				Observation.m_Result = AUTO_UNFREEZE_OBS_STILL_FROZEN;
			else if(Observation.m_CrossedTeeBeforeFreeze)
				Observation.m_Result = AUTO_UNFREEZE_OBS_HIT_BEFORE_FREEZE;
			else
				Observation.m_Result = AUTO_UNFREEZE_OBS_TRAJECTORY_MISSED;
		}

		if(Observation.m_Result != AUTO_UNFREEZE_OBS_PENDING &&
			!Observation.m_FeedbackApplied)
		{
			AutoUnfreezeApplyFeedback(DummyIndex, Observation.m_LearningKey,
				Observation.m_Result, SnapshotTick);
			Observation.m_FeedbackApplied = true;
			Observation.m_Active = false;
		}
	}
}

void CGameClient::AutoUnfreezeDebugUpdateRecording()
{
	const bool WantsFullRecording = g_Config.m_ClZzFreezeFullDebugRecord != 0;
	const bool WantsAnyRecording =
		g_Config.m_ClZzAutoUnfreezeDebugRecord != 0 || WantsFullRecording;
	if(!WantsAnyRecording)
	{
		AutoUnfreezeDebugStop();
		return;
	}
	if(m_pAutoUnfreezeDebugWriter &&
		m_AutoUnfreezeDebugFullRecording != WantsFullRecording)
		AutoUnfreezeDebugStop();
	if(m_pAutoUnfreezeDebugWriter)
		return;
	m_AutoUnfreezeDebugFullRecording = WantsFullRecording;

	if(!Storage()->CreateFolder("dumps", IStorage::TYPE_SAVE) ||
		!Storage()->CreateFolder("dumps/auto_unfreeze", IStorage::TYPE_SAVE))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auto_unfreeze",
			"Could not create dumps/auto_unfreeze");
		g_Config.m_ClZzAutoUnfreezeDebugRecord = 0;
		return;
	}

	char aTimestamp[64];
	str_timestamp(aTimestamp, sizeof(aTimestamp));
	str_format(m_aAutoUnfreezeDebugFilename, sizeof(m_aAutoUnfreezeDebugFilename),
		"dumps/auto_unfreeze/%s_%s_%lld.json",
		WantsFullRecording ? "freeze_session" : "auto_unfreeze",
		aTimestamp, (long long)(time_get() % 100000));
	IOHANDLE File = Storage()->OpenFile(m_aAutoUnfreezeDebugFilename,
		IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auto_unfreeze",
			"Could not open auto-unfreeze debug file");
		g_Config.m_ClZzAutoUnfreezeDebugRecord = 0;
		return;
	}

	m_pAutoUnfreezeDebugWriter = new CJsonFileWriter(File);
	m_pAutoUnfreezeDebugWriter->BeginObject();
	m_pAutoUnfreezeDebugWriter->WriteAttribute("format_version");
	m_pAutoUnfreezeDebugWriter->WriteIntValue(6);
	m_pAutoUnfreezeDebugWriter->WriteAttribute("full_freeze_session");
	m_pAutoUnfreezeDebugWriter->WriteBoolValue(WantsFullRecording);
	m_pAutoUnfreezeDebugWriter->WriteAttribute("started_at");
	m_pAutoUnfreezeDebugWriter->WriteStrValue(aTimestamp);
	m_pAutoUnfreezeDebugWriter->WriteAttribute("tick_speed");
	m_pAutoUnfreezeDebugWriter->WriteIntValue(SERVER_TICK_SPEED);
	m_pAutoUnfreezeDebugWriter->WriteAttribute("position_scale");
	m_pAutoUnfreezeDebugWriter->WriteIntValue(100);
	if(WantsFullRecording)
	{
		m_pAutoUnfreezeDebugWriter->WriteAttribute("tuning_x100");
		m_pAutoUnfreezeDebugWriter->BeginObject();
		const CTuningParams &Tuning = m_aTuning[g_Config.m_ClDummy];
		for(int Index = 0; Index < CTuningParams::Num(); Index++)
		{
			m_pAutoUnfreezeDebugWriter->WriteAttribute(CTuningParams::Name(Index));
			m_pAutoUnfreezeDebugWriter->WriteIntValue(Tuning.NetworkArray()[Index]);
		}
		m_pAutoUnfreezeDebugWriter->EndObject();

		m_pAutoUnfreezeDebugWriter->WriteAttribute("map_width");
		m_pAutoUnfreezeDebugWriter->WriteIntValue(Collision()->GetWidth());
		m_pAutoUnfreezeDebugWriter->WriteAttribute("map_height");
		m_pAutoUnfreezeDebugWriter->WriteIntValue(Collision()->GetHeight());
		m_pAutoUnfreezeDebugWriter->WriteAttribute("map_tiles");
		m_pAutoUnfreezeDebugWriter->BeginArray();
		const int MapTileCount = Collision()->GetWidth() * Collision()->GetHeight();
		for(int Index = 0; Index < MapTileCount; Index++)
		{
			const int GameTile = Collision()->GetTileIndex(Index);
			const int FrontTile = Collision()->GetFrontTileIndex(Index);
			const int GameFlags = Collision()->GetTileFlags(Index);
			const int FrontFlags = Collision()->GetFrontTileFlags(Index);
			const int Tele = Collision()->IsTeleport(Index);
			const int EvilTele = Collision()->IsEvilTeleport(Index);
			const int TeleWeapon = Collision()->IsTeleportWeapon(Index);
			const int TeleHook = Collision()->IsTeleportHook(Index);
			const int TeleCheckpoint = Collision()->IsTeleCheckpoint(Index);
			const int Tune = Collision()->IsTune(Index);
			const int SwitchType = Collision()->GetSwitchType(Index);
			const int SwitchNumber = Collision()->GetSwitchNumber(Index);
			const int SwitchDelay = Collision()->GetSwitchDelay(Index);
			vec2 SpeedDir(0.0f, 0.0f);
			int SpeedForce = 0;
			int SpeedMax = 0;
			int SpeedType = 0;
			Collision()->GetSpeedup(Index, &SpeedDir, &SpeedForce, &SpeedMax,
				&SpeedType);
			if(GameTile == 0 && FrontTile == 0 && GameFlags == 0 &&
				FrontFlags == 0 && Tele == 0 && EvilTele == 0 && TeleWeapon == 0 &&
				TeleHook == 0 && TeleCheckpoint == 0 && Tune == 0 &&
				SwitchType == 0 && SpeedForce == 0)
				continue;

			m_pAutoUnfreezeDebugWriter->BeginObject();
			m_pAutoUnfreezeDebugWriter->WriteAttribute("x");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(Index %
								  Collision()->GetWidth());
			m_pAutoUnfreezeDebugWriter->WriteAttribute("y");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(Index /
								  Collision()->GetWidth());
			m_pAutoUnfreezeDebugWriter->WriteAttribute("game");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(GameTile);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("front");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(FrontTile);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("game_flags");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(GameFlags);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("front_flags");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(FrontFlags);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("tele");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(Tele);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("evil_tele");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(EvilTele);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("tele_weapon");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(TeleWeapon);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("tele_hook");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(TeleHook);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("tele_checkpoint");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(TeleCheckpoint);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("tune");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(Tune);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("switch_type");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SwitchType);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("switch_number");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SwitchNumber);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("switch_delay");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SwitchDelay);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("speed_force");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SpeedForce);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("speed_max");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SpeedMax);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("speed_type");
			m_pAutoUnfreezeDebugWriter->WriteIntValue(SpeedType);
			m_pAutoUnfreezeDebugWriter->WriteAttribute("speed_dir_x1000");
			m_pAutoUnfreezeDebugWriter->BeginArray();
			m_pAutoUnfreezeDebugWriter->WriteIntValue(
				round_to_int(SpeedDir.x * 1000.0f));
			m_pAutoUnfreezeDebugWriter->WriteIntValue(
				round_to_int(SpeedDir.y * 1000.0f));
			m_pAutoUnfreezeDebugWriter->EndArray();
			m_pAutoUnfreezeDebugWriter->EndObject();
		}
		m_pAutoUnfreezeDebugWriter->EndArray();
	}
	m_pAutoUnfreezeDebugWriter->WriteAttribute("frames");
	m_pAutoUnfreezeDebugWriter->BeginArray();
	std::fill(std::begin(m_aAutoUnfreezeDebugLastTick),
		std::end(m_aAutoUnfreezeDebugLastTick), -1);
	std::fill(std::begin(m_aAutoUnfreezeDebugPrevFrozen),
		std::end(m_aAutoUnfreezeDebugPrevFrozen), false);
	std::fill(std::begin(m_aAutoUnfreezeDebugPrevInFlight),
		std::end(m_aAutoUnfreezeDebugPrevInFlight), false);

	char aMessage[640];
	str_format(aMessage, sizeof(aMessage), "Recording started: %s",
		m_aAutoUnfreezeDebugFilename);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auto_unfreeze", aMessage);
}

void CGameClient::AutoUnfreezeDebugStop()
{
	if(!m_pAutoUnfreezeDebugWriter)
		return;
	m_pAutoUnfreezeDebugWriter->EndArray();
	m_pAutoUnfreezeDebugWriter->EndObject();
	delete m_pAutoUnfreezeDebugWriter;
	m_pAutoUnfreezeDebugWriter = nullptr;
	m_AutoUnfreezeDebugFullRecording = false;

	char aMessage[640];
	str_format(aMessage, sizeof(aMessage), "Recording saved: %s",
		m_aAutoUnfreezeDebugFilename);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "auto_unfreeze", aMessage);
}

void CGameClient::UpdateFlyRide()
{
	const int PilotConn = g_Config.m_ClDummy;
	const int PairConn = Client()->DummyPair();
	auto ReleaseDummyInput = [&](int Conn) {
		if(Conn < 0 || Conn >= NUM_DUMMIES)
			return;
		int ReleasedFire = m_FlyRideDummyFire & INPUT_STATE_MASK;
		if((m_DummyInput.m_Fire & 1) != 0)
			ReleasedFire = m_DummyInput.m_Fire & INPUT_STATE_MASK;
		if((ReleasedFire & 1) != 0)
			ReleasedFire = (ReleasedFire + 1) & INPUT_STATE_MASK;
		m_FlyRideDummyFire = ReleasedFire;
		m_DummyInput.m_Fire = ReleasedFire;
		m_DummyInput.m_Hook = 0;
		m_DummyInput.m_WantedWeapon = 0;
		m_DummyInput.m_NextWeapon = 0;
		m_DummyInput.m_PrevWeapon = 0;
		m_Controls.m_aInputData[Conn].m_Fire = ReleasedFire;
		m_Controls.m_aInputData[Conn].m_Hook = 0;
		m_Controls.m_aInputData[Conn].m_WantedWeapon = 0;
		m_Controls.m_aInputData[Conn].m_NextWeapon = 0;
		m_Controls.m_aInputData[Conn].m_PrevWeapon = 0;
	};
	auto ResetFlyState = [&]() {
		m_FlyRideWasActive = false;
		m_FlyRidePilotDirection = 0;
		m_FlyRideDummyDirection = 0;
		m_FlyRidePilotJump = 0;
		m_FlyRideVerticalMode = 0;
		m_FlyRideLastVerticalIntent = 0;
		m_FlyRideLastUpdateTick = -1;
		m_FlyRideSmoothedVerticalSpeed = 0.0f;
		m_FlyRideBalanceRecovery = false;
		m_FlyRideEmergencyClimb = false;
		m_FlyRideFastClimb = false;
		m_FlyRideDescentBrake = false;
		m_FlyRidePilotHook = 0;
		m_FlyRideDummyHook = 0;
		m_FlyRideHookStateTick = -1;
		m_FlyRideHookDistance = 72.0f;
		m_FlyRideTargetConn = -1;
		m_FlyRidePilotHammer = false;
		m_FlyRideDummyHammer = false;
		m_FlyRideAledState = EFlyRideAledState::NONE;
		m_FlyRideAledDirection = vec2(0.0f, -1.0f);
		m_FlyRideAledEntryProjection = 0.0f;
		m_FlyRideAledExitProjection = 0.0f;
		m_FlyRideAledPilotLeads = true;
		m_FlyRideAledStateTick = -1;
		m_FlyRideAledAttemptTick = -1;
		m_FlyRideAledRetryTick = -1;
		m_FlyRideAledLastDiagTick = -1;
	};
	const bool UiBlocked = m_Chat.IsActive() || m_Menus.IsActive() ||
			       m_GameConsole.IsActive() || g_Config.m_ClZzClickGui;
	// The old implementation controlled a dummy pair. The setting is now used
	// by the local Hook Drive in OnSnapInput, so keep this legacy state inactive.
	const bool Enabled = false;
	if(m_FlyRideWasActive && m_FlyRideTargetConn >= 0 &&
		m_FlyRideTargetConn != PairConn)
	{
		ReleaseDummyInput(m_FlyRideTargetConn);
		ResetFlyState();
	}

	if(!Enabled)
	{
		// Release inputs once when Pilot Fly loses control. Repeating this every
		// frame while disabled overwrites fresh manual hook/jump/fire input.
		if(m_FlyRideWasActive || m_FlyRideTargetConn >= 0)
		{
			if(m_FlyRideTargetConn >= 0)
				ReleaseDummyInput(m_FlyRideTargetConn);
			if(PilotConn >= 0 && PilotConn < NUM_DUMMIES)
			{
				m_Controls.m_aInputData[PilotConn].m_Direction = 0;
				m_Controls.m_aInputData[PilotConn].m_Jump = 0;
				m_Controls.m_aInputData[PilotConn].m_Hook = 0;
			}
		}
		ResetFlyState();
		return;
	}

	const int PilotId = m_aLocalIds[PilotConn];
	const int DummyId = m_aLocalIds[PairConn];
	if(PilotId < 0 || PilotId >= MAX_CLIENTS || DummyId < 0 ||
		DummyId >= MAX_CLIENTS || !m_Snap.m_aCharacters[PilotId].m_Active ||
		!m_Snap.m_aCharacters[DummyId].m_Active)
	{
		if(m_FlyRideTargetConn >= 0)
			ReleaseDummyInput(m_FlyRideTargetConn);
		ResetFlyState();
		return;
	}

	m_FlyRidePilotPos = m_aClients[PilotId].m_RegularPredicted.m_Pos;
	m_FlyRideDummyPos = m_aClients[DummyId].m_RegularPredicted.m_Pos;
	if(!m_FlyRideWasActive)
	{
		m_FlyRideWasActive = true;
		m_FlyRidePilotHook = 0;
		m_FlyRideHookStateTick = -1;
		m_FlyRideHookDistance = 72.0f;
		m_FlyRideLastVerticalIntent = 0;
		m_FlyRideLastUpdateTick = -1;
		m_FlyRideSmoothedVerticalSpeed = 0.0f;
		m_FlyRideBalanceRecovery = false;
		m_FlyRideEmergencyClimb = false;
		m_FlyRideFastClimb = false;
		m_FlyRideDescentBrake = false;
		m_FlyRideDummyHook = 0;
		m_FlyRidePilotHammer = false;
		m_FlyRideDummyFire = m_DummyInput.m_Fire & INPUT_STATE_MASK;
		m_FlyRidePilotFire = m_Controls.m_aInputData[PilotConn].m_Fire & INPUT_STATE_MASK;
		m_FlyRidePilotLastFireTick = -1;
		m_FlyRideLastFireTick = -1;
		m_FlyRidePilotNextHitTick = -1;
		m_FlyRideDummyNextHitTick = -1;
		m_FlyRideAledState = EFlyRideAledState::NONE;
		m_FlyRideAledStateTick = -1;
		m_FlyRideAledAttemptTick = -1;
		m_FlyRideAledRetryTick = -1;
		m_FlyRideAledLastDiagTick = -1;
	}

	const vec2 ToMouse = m_Controls.m_aMousePos[PilotConn];
	m_FlyRideAnchor = m_Camera.m_Center + ToMouse;
	const int Tick = Client()->PredGameTick(PilotConn);
	auto ApplyPilotInput = [&]() {
		CNetObj_PlayerInput &PilotInput = m_Controls.m_aInputData[PilotConn];
		PilotInput.m_Direction = m_FlyRidePilotDirection;
		PilotInput.m_Jump = m_FlyRidePilotJump;
		PilotInput.m_Hook = m_FlyRidePilotHook;
		// Do not steal the manual cursor while Pilot Fly is neutral. In
		// particular, this keeps Aimbot=OFF from visibly snapping the weapon to
		// the dummy merely because another player entered hook range.
		if(m_FlyRidePilotHook || m_FlyRidePilotHammer ||
			m_FlyRideAledState != EFlyRideAledState::NONE)
		{
			vec2 HookAim = m_FlyRideDummyPos - m_FlyRidePilotPos;
			if(length(HookAim) > 0.001f)
				HookAim = normalize(HookAim) * 1000.0f;
			else
				HookAim = vec2(0.0f, 1.0f);
			PilotInput.m_TargetX = round_to_int(HookAim.x);
			PilotInput.m_TargetY = round_to_int(HookAim.y);
		}
	};
	if(Tick == m_FlyRideLastUpdateTick)
	{
		ApplyPilotInput();
		return;
	}
	m_FlyRideLastUpdateTick = Tick;

	const int PreviousVerticalIntent = m_FlyRideLastVerticalIntent;
	if(m_FlyRideVerticalMode < 0)
		m_FlyRideVerticalMode = ToMouse.y > 56.0f ? 1 :
							    (ToMouse.y > -24.0f ? 0 : -1);
	else if(m_FlyRideVerticalMode > 0)
		m_FlyRideVerticalMode = ToMouse.y < -56.0f ? -1 :
							     (ToMouse.y < 24.0f ? 0 : 1);
	else if(ToMouse.y < -56.0f)
		m_FlyRideVerticalMode = -1;
	else if(ToMouse.y > 56.0f)
		m_FlyRideVerticalMode = 1;
	const bool RecoveringFromDescent =
		m_FlyRideVerticalMode < 0 && PreviousVerticalIntent > 0;
	if(m_FlyRideVerticalMode != 0)
		m_FlyRideLastVerticalIntent = m_FlyRideVerticalMode;
	if(ToMouse.y < -144.0f)
		m_FlyRideFastClimb = true;
	else if(ToMouse.y > -96.0f)
		m_FlyRideFastClimb = false;

	const vec2 PilotVelocity = m_aClients[PilotId].m_RegularPredicted.m_Vel;
	const vec2 DummyVelocity = m_aClients[DummyId].m_RegularPredicted.m_Vel;
	const float VerticalSpeed = (PilotVelocity.y + DummyVelocity.y) * 0.5f;
	m_FlyRideSmoothedVerticalSpeed +=
		(VerticalSpeed - m_FlyRideSmoothedVerticalSpeed) * 0.22f;

	auto IsFreezeTile = [](int Tile) {
		return Tile == TILE_FREEZE || Tile == TILE_DFREEZE ||
		       Tile == TILE_LFREEZE;
	};
	CCharacter *pPilotCharacter =
		m_RegularPredictedWorld.GetCharacterById(PilotId);
	CCharacter *pDummyCharacter =
		m_RegularPredictedWorld.GetCharacterById(DummyId);
	const int Team = pPilotCharacter ? pPilotCharacter->Team() : 0;
	const auto &vSwitchers = m_RegularPredictedWorld.Switchers();
	auto IsHazardAt = [&](const vec2 &Pos) {
		const int MapIndex = Collision()->GetPureMapIndex(Pos);
		if(MapIndex < 0)
			return false;
		const int Tile = Collision()->GetTileIndex(MapIndex);
		const int FrontTile = Collision()->GetFrontTileIndex(MapIndex);
		if(Tile == TILE_DEATH || FrontTile == TILE_DEATH ||
			IsFreezeTile(Tile) || IsFreezeTile(FrontTile))
			return true;
		if(!IsFreezeTile(Collision()->GetSwitchType(MapIndex)))
			return false;
		const int Number = Collision()->GetSwitchNumber(MapIndex);
		return Number == 0 || (Number > 0 && Number < (int)vSwitchers.size() &&
					      Team >= 0 && Team < NUM_DDRACE_TEAMS &&
					      vSwitchers[Number].m_aStatus[Team]);
	};
	auto IsSwitchActive = [&](int MapIndex) {
		const int Number = Collision()->GetSwitchNumber(MapIndex);
		return Number == 0 || (Number > 0 && Number < (int)vSwitchers.size() &&
					      Team >= 0 && Team < NUM_DDRACE_TEAMS &&
					      vSwitchers[Number].m_aStatus[Team]);
	};
	auto IsNormalFreezeAt = [&](const vec2 &Pos) {
		const int MapIndex = Collision()->GetPureMapIndex(Pos);
		if(MapIndex < 0)
			return false;
		if(Collision()->GetTileIndex(MapIndex) == TILE_FREEZE ||
			Collision()->GetFrontTileIndex(MapIndex) == TILE_FREEZE)
			return true;
		return Collision()->GetSwitchType(MapIndex) == TILE_FREEZE &&
		       IsSwitchActive(MapIndex);
	};
	auto IsForbiddenAledAt = [&](const vec2 &Pos) {
		const int MapIndex = Collision()->GetPureMapIndex(Pos);
		if(MapIndex < 0)
			return true;
		const int Tile = Collision()->GetTileIndex(MapIndex);
		const int FrontTile = Collision()->GetFrontTileIndex(MapIndex);
		if(Tile == TILE_DEATH || FrontTile == TILE_DEATH ||
			Tile == TILE_DFREEZE || FrontTile == TILE_DFREEZE ||
			Tile == TILE_LFREEZE || FrontTile == TILE_LFREEZE)
			return true;
		const int SwitchType = Collision()->GetSwitchType(MapIndex);
		return (SwitchType == TILE_DFREEZE || SwitchType == TILE_LFREEZE) &&
		       IsSwitchActive(MapIndex);
	};
	auto ProbeAledBand = [&](const vec2 &Direction, float &EntryProjection,
				     float &ExitProjection) {
		const vec2 Center = (m_FlyRidePilotPos + m_FlyRideDummyPos) * 0.5f;
		const vec2 Perpendicular(-Direction.y, Direction.x);
		float EntryDistanceSum = 0.0f;
		float ExitDistanceSum = 0.0f;
		float MinEntryDistance = 1000000.0f;
		float MaxEntryDistance = -1000000.0f;
		int ValidRays = 0;
		bool CenterRayValid = false;
		const float aOffsets[] = {-12.0f, 0.0f, 12.0f};
		for(int Ray = 0; Ray < 3; Ray++)
		{
			const float Offset = aOffsets[Ray];
			bool InFreeze = false;
			bool RayBlocked = false;
			int Segments = 0;
			float SegmentStart = 0.0f;
			float SegmentEnd = 0.0f;
			float ClearAfter = 0.0f;
			for(float DistanceAhead = -48.0f; DistanceAhead <= 200.0f;
				DistanceAhead += 4.0f)
			{
				const vec2 Sample = Center + Perpendicular * Offset +
						    Direction * DistanceAhead;
				if(Collision()->CheckPoint(Sample) || IsForbiddenAledAt(Sample))
				{
					if(DistanceAhead < 0.0f && Segments == 0)
						continue;
					RayBlocked = true;
					break;
				}
				const bool Freeze = IsNormalFreezeAt(Sample);
				if(Freeze)
				{
					if(!InFreeze)
					{
						Segments++;
						SegmentStart = DistanceAhead;
					}
					InFreeze = true;
					ClearAfter = 0.0f;
				}
				else if(InFreeze)
				{
					InFreeze = false;
					SegmentEnd = DistanceAhead;
					ClearAfter = 4.0f;
				}
				else if(Segments > 0)
				{
					ClearAfter += 4.0f;
					if(ClearAfter >= 40.0f)
						break;
				}
			}
			if(RayBlocked || InFreeze || Segments != 1 || SegmentStart > 128.0f ||
				SegmentEnd < -8.0f ||
				SegmentEnd <= SegmentStart || SegmentEnd - SegmentStart > 40.0f ||
				ClearAfter < 40.0f)
				continue;
			EntryDistanceSum += SegmentStart;
			ExitDistanceSum += SegmentEnd;
			MinEntryDistance = minimum(MinEntryDistance, SegmentStart);
			MaxEntryDistance = maximum(MaxEntryDistance, SegmentStart);
			ValidRays++;
			if(Ray == 1)
				CenterRayValid = true;
		}
		if(!CenterRayValid || ValidRays < 1 ||
			MaxEntryDistance - MinEntryDistance > 24.0f)
			return false;
		EntryProjection = dot(Center, Direction) + EntryDistanceSum / ValidRays;
		ExitProjection = dot(Center, Direction) + ExitDistanceSum / ValidRays;
		return ExitProjection > EntryProjection;
	};
	const bool PilotFrozen = pPilotCharacter &&
				 (pPilotCharacter->m_FreezeTime > 0 || pPilotCharacter->Core()->m_IsInFreeze);
	const bool DummyFrozen = pDummyCharacter &&
				 (pDummyCharacter->m_FreezeTime > 0 || pDummyCharacter->Core()->m_IsInFreeze);
	bool FreezeBelow = false;
	const float LowerTeeY = maximum(m_FlyRidePilotPos.y, m_FlyRideDummyPos.y);
	const float ScanDistance = std::clamp(
		96.0f + maximum(0.0f, m_FlyRideSmoothedVerticalSpeed) * 28.0f,
		96.0f, 256.0f);
	for(float ScanY = 24.0f; ScanY <= ScanDistance && !FreezeBelow; ScanY += 16.0f)
	{
		const float Fraction = ScanY / ScanDistance;
		const vec2 aOrigins[] = {m_FlyRidePilotPos, m_FlyRideDummyPos};
		const vec2 aVelocities[] = {PilotVelocity, DummyVelocity};
		for(int Tee = 0; Tee < 2 && !FreezeBelow; Tee++)
		{
			for(float ScanX : {-12.0f, 0.0f, 12.0f})
			{
				const vec2 ScanPos(
					aOrigins[Tee].x + aVelocities[Tee].x * 10.0f * Fraction + ScanX,
					LowerTeeY + ScanY);
				vec2 SolidHit;
				if(Collision()->IntersectLine(aOrigins[Tee], ScanPos, &SolidHit, nullptr) == 0 &&
					IsHazardAt(ScanPos))
				{
					FreezeBelow = true;
					break;
				}
			}
		}
	}
	if(!m_FlyRideEmergencyClimb &&
		((RecoveringFromDescent && m_FlyRideSmoothedVerticalSpeed > 1.0f) ||
			(FreezeBelow && m_FlyRideSmoothedVerticalSpeed > 0.45f)))
		m_FlyRideEmergencyClimb = true;
	else if(m_FlyRideEmergencyClimb &&
		m_FlyRideSmoothedVerticalSpeed < -0.75f)
		m_FlyRideEmergencyClimb = false;
	const int FlightVerticalMode =
		m_FlyRideEmergencyClimb ? -1 : m_FlyRideVerticalMode;
	const bool FastClimb = FlightVerticalMode < 0 &&
			       (m_FlyRideEmergencyClimb || m_FlyRideFastClimb);

	float TargetHookDistance = 72.0f;
	if(FastClimb)
		TargetHookDistance = 66.0f;
	else if(FlightVerticalMode < 0)
		TargetHookDistance = 70.0f;
	else if(FlightVerticalMode > 0)
		TargetHookDistance = 74.0f;
	m_FlyRideHookDistance +=
		(TargetHookDistance - m_FlyRideHookDistance) * 0.16f;

	const float DeltaX = ToMouse.x;
	const float HorizontalDeadZone = FastClimb ? 64.0f : 32.0f;
	if(std::fabs(DeltaX) <= HorizontalDeadZone)
		m_FlyRidePilotDirection = 0;
	else
		m_FlyRidePilotDirection = DeltaX > 0.0f ? 1 : -1;

	const float DummyOffsetX = m_FlyRideDummyPos.x - m_FlyRidePilotPos.x;
	const float PredictedDummyOffsetX = DummyOffsetX +
					    (DummyVelocity.x - PilotVelocity.x) * 5.0f;
	const float VerticalGap = m_FlyRideDummyPos.y - m_FlyRidePilotPos.y;
	if(!m_FlyRideBalanceRecovery && VerticalGap < -18.0f)
		m_FlyRideBalanceRecovery = true;
	else if(m_FlyRideBalanceRecovery && VerticalGap > 42.0f)
		m_FlyRideBalanceRecovery = false;

	const float BalanceDeadZone = FastClimb ? 16.0f : 24.0f;
	if(PredictedDummyOffsetX > BalanceDeadZone)
		m_FlyRideDummyDirection = -1;
	else if(PredictedDummyOffsetX < -BalanceDeadZone)
		m_FlyRideDummyDirection = 1;
	else
		m_FlyRideDummyDirection = m_FlyRidePilotDirection;

	if(m_FlyRideBalanceRecovery && std::fabs(DummyOffsetX) < 52.0f)
	{
		const int SeparateDirection = DummyOffsetX >= 0.0f ? 1 : -1;
		m_FlyRideDummyDirection = SeparateDirection;
		if(m_FlyRideBalanceRecovery)
			m_FlyRidePilotDirection = -SeparateDirection;
	}
	else if(m_FlyRideBalanceRecovery)
	{
		m_FlyRidePilotDirection = 0;
		m_FlyRideDummyDirection = 0;
	}
	else if(m_FlyRideEmergencyClimb)
	{
		m_FlyRidePilotDirection = 0;
		if(std::fabs(PredictedDummyOffsetX) <= BalanceDeadZone)
			m_FlyRideDummyDirection = 0;
	}

	m_FlyRidePilotJump = pPilotCharacter && pPilotCharacter->IsGrounded() &&
			     FlightVerticalMode < 0;
	const float RelativeVerticalSpeed = DummyVelocity.y - PilotVelocity.y;
	const float PredictedVerticalGap =
		VerticalGap + RelativeVerticalSpeed * 4.0f;

	const float Distance = distance(m_FlyRidePilotPos, m_FlyRideDummyPos);
	const bool SnapshotCanCollide = !m_aClients[PilotId].m_Solo &&
					!m_aClients[DummyId].m_Solo && !m_aClients[PilotId].m_CollisionDisabled &&
					!m_aClients[DummyId].m_CollisionDisabled;
	const bool CanCollidePair = pPilotCharacter && pDummyCharacter ?
					    (pPilotCharacter->CanCollide(DummyId) && pDummyCharacter->CanCollide(PilotId)) :
					    SnapshotCanCollide;
	const bool HookingEnabled = CanCollidePair &&
				    !m_aClients[PilotId].m_HookHitDisabled &&
				    m_aTuning[PilotConn].m_PlayerHooking;
	bool HookPathClear = false;
	if(HookingEnabled)
	{
		vec2 HookDirection = m_FlyRideDummyPos - m_FlyRidePilotPos;
		const float HookDistance = length(HookDirection);
		if(HookDistance > 0.001f)
		{
			HookDirection *= 1.0f / HookDistance;
			const float TeeRadius = CCharacterCore::PhysicalSize();
			const vec2 HookStart =
				m_FlyRidePilotPos + HookDirection * TeeRadius * 1.5f;
			vec2 RayEnd = m_FlyRideDummyPos;
			int TeleNr = 0;
			Collision()->IntersectLineTeleHook(
				HookStart, m_FlyRideDummyPos, &RayEnd, nullptr, &TeleNr);
			const float ReachableDistance = distance(HookStart, RayEnd);
			const float RequiredDistance =
				maximum(0.0f, distance(HookStart, m_FlyRideDummyPos) -
						      TeeRadius - 2.0f);
			vec2 PlayerHitPos;
			CCharacter *pFirstHookTarget =
				m_RegularPredictedWorld.IntersectCharacter(
					HookStart, RayEnd, 2.0f, PlayerHitPos,
					pPilotCharacter, PilotId);
			HookPathClear = ReachableDistance + 0.5f >= RequiredDistance &&
					pFirstHookTarget && pFirstHookTarget->GetCid() == DummyId;
		}
	}
	const bool CanHookDummy = HookingEnabled && HookPathClear;
	const int ActualHookState = pPilotCharacter ?
					    pPilotCharacter->Core()->m_HookState :
					    HOOK_IDLE;
	const int ActualHookedPlayer = pPilotCharacter ?
					       pPilotCharacter->Core()->HookedPlayer() :
					       -1;
	const bool HookNeedsRelease = pPilotCharacter &&
				      (ActualHookState == HOOK_RETRACTED ||
					      (ActualHookState >= HOOK_RETRACT_START &&
						      ActualHookState <= HOOK_RETRACT_END) ||
					      (ActualHookState == HOOK_GRABBED && ActualHookedPlayer != DummyId));
	if(!CanHookDummy || HookNeedsRelease)
	{
		m_FlyRidePilotHook = 0;
		m_FlyRideHookStateTick = Tick;
	}
	else if(m_FlyRideBalanceRecovery)
	{
		const int RecoveryHook = VerticalGap < -18.0f && Distance > 52.0f;
		if(m_FlyRidePilotHook != RecoveryHook)
		{
			m_FlyRidePilotHook = RecoveryHook;
			m_FlyRideHookStateTick = Tick;
		}
	}
	else if(VerticalGap < 44.0f || PredictedVerticalGap < 50.0f)
	{
		if(m_FlyRidePilotHook)
		{
			m_FlyRidePilotHook = 0;
			m_FlyRideHookStateTick = Tick;
		}
	}
	else if(RecoveringFromDescent && VerticalGap > 58.0f &&
		PredictedVerticalGap > 56.0f)
	{
		m_FlyRidePilotHook = 1;
		m_FlyRideHookStateTick = Tick;
		m_FlyRideDummyNextHitTick = -1;
	}
	else if(Tick - m_FlyRideHookStateTick >= 4)
	{
		const float ReleaseGap = FastClimb ? 56.0f : FlightVerticalMode < 0 ? 58.0f :
										      (FlightVerticalMode > 0 ? 60.0f : 56.0f);
		const float AttachGap = FastClimb ? 76.0f : FlightVerticalMode < 0 ? 86.0f :
										     (FlightVerticalMode > 0 ? 84.0f : 82.0f);
		if(m_FlyRidePilotHook &&
			(VerticalGap <= ReleaseGap || PredictedVerticalGap <= ReleaseGap ||
				Distance <= m_FlyRideHookDistance - 10.0f))
		{
			m_FlyRidePilotHook = 0;
			m_FlyRideHookStateTick = Tick;
		}
		else if(!m_FlyRidePilotHook &&
			(VerticalGap >= AttachGap ||
				(VerticalGap > 52.0f &&
					Distance >= m_FlyRideHookDistance + (FastClimb ? 10.0f : 18.0f))))
		{
			m_FlyRidePilotHook = 1;
			m_FlyRideHookStateTick = Tick;
		}
	}
	const float MinHammerDistance = CCharacterCore::PhysicalSize() * 1.02f;
	const float MaxHammerDistance = CCharacterCore::PhysicalSize() * 2.23f;
	const bool DummyIsBelowPilot = VerticalGap >= 18.0f;
	if(FlightVerticalMode > 0)
	{
		if(!m_FlyRideDescentBrake &&
			(m_FlyRideSmoothedVerticalSpeed > 3.35f || VerticalSpeed > 4.05f))
			m_FlyRideDescentBrake = true;
		else if(m_FlyRideDescentBrake &&
			m_FlyRideSmoothedVerticalSpeed < 2.15f)
			m_FlyRideDescentBrake = false;
	}
	else
		m_FlyRideDescentBrake = false;
	const bool WantsHammer = FlightVerticalMode < 0 ||
				 (FlightVerticalMode == 0 && m_FlyRideSmoothedVerticalSpeed > 1.35f) ||
				 (FlightVerticalMode > 0 && m_FlyRideDescentBrake);
	m_FlyRidePilotHammer = false;
	m_FlyRideDummyHook = 0;
	m_FlyRideDummyHammer = CanCollidePair &&
			       !m_aClients[DummyId].m_HammerHitDisabled &&
			       !m_FlyRideBalanceRecovery && WantsHammer && DummyIsBelowPilot &&
			       Distance >= MinHammerDistance && Distance <= MaxHammerDistance;
	const int BasePilotDirection = m_FlyRidePilotDirection;
	const int BaseDummyDirection = m_FlyRideDummyDirection;
	const int BasePilotJump = m_FlyRidePilotJump;
	const int BasePilotHook = m_FlyRidePilotHook;
	const bool BaseDummyHammer = m_FlyRideDummyHammer;
	if(!g_Config.m_ClZzFlyRideFreezeAssist &&
		m_FlyRideAledState != EFlyRideAledState::NONE)
	{
		// Cancelling the assist must immediately hand control back to normal
		// Pilot Fly instead of leaving an old aled phase latched.
		m_FlyRideAledState = EFlyRideAledState::NONE;
		m_FlyRideAledStateTick = -1;
		m_FlyRideAledAttemptTick = -1;
		m_FlyRideAledRetryTick = -1;
		m_FlyRidePilotHook = BasePilotHook;
		m_FlyRideDummyHook = 0;
		m_FlyRidePilotHammer = false;
		m_FlyRideDummyHammer = BaseDummyHammer;
	}

	auto AledStateName = [](EFlyRideAledState State) {
		switch(State)
		{
		case EFlyRideAledState::NONE: return "none";
		case EFlyRideAledState::APPROACH: return "approach";
		case EFlyRideAledState::LEAD_ENTERS_FREEZE: return "lead_enters";
		case EFlyRideAledState::REAR_HITS_LEAD: return "rear_hits_lead";
		case EFlyRideAledState::REAR_ENTERS_FREEZE: return "rear_enters";
		case EFlyRideAledState::LEAD_HITS_REAR: return "lead_hits_rear";
		case EFlyRideAledState::EXIT: return "exit";
		case EFlyRideAledState::RETREAT: return "retreat";
		}
		return "unknown";
	};
	auto EnterAledState = [&](EFlyRideAledState State) {
		if(State != m_FlyRideAledState)
		{
			char aMessage[256];
			str_format(aMessage, sizeof(aMessage),
				"tick=%d %s->%s dist=%.1f pilot_frozen=%d dummy_frozen=%d",
				Tick, AledStateName(m_FlyRideAledState), AledStateName(State),
				Distance, PilotFrozen, DummyFrozen);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "pilot_aled", aMessage);
		}
		m_FlyRideAledState = State;
		m_FlyRideAledStateTick = Tick;
	};
	const int TickSpeed = maximum(1, Client()->GameTickSpeed());
	const int AledPhaseTimeout = maximum(1, (TickSpeed * 3) / 2);
	const int AledAttemptTimeout = maximum(1, TickSpeed * 4);
	const float MouseDistance = length(ToMouse);
	if(m_FlyRideAledState == EFlyRideAledState::NONE &&
		g_Config.m_ClZzFlyRideFreezeAssist && MouseDistance >= 72.0f &&
		Tick >= m_FlyRideAledRetryTick && !(PilotFrozen && DummyFrozen) &&
		CanCollidePair && !m_aClients[PilotId].m_HammerHitDisabled &&
		!m_aClients[DummyId].m_HammerHitDisabled)
	{
		const vec2 DesiredDirection = ToMouse / MouseDistance;
		float EntryProjection = 0.0f;
		float ExitProjection = 0.0f;
		const bool OneTeeFrozen = PilotFrozen != DummyFrozen;
		bool BandFound = ProbeAledBand(DesiredDirection, EntryProjection, ExitProjection);
		if(!BandFound && OneTeeFrozen &&
			Distance <= MaxHammerDistance + 24.0f)
		{
			const vec2 FrozenPos = PilotFrozen ? m_FlyRidePilotPos : m_FlyRideDummyPos;
			const float FrozenProjection = dot(FrozenPos, DesiredDirection);
			EntryProjection = FrozenProjection - 20.0f;
			ExitProjection = FrozenProjection + 20.0f;
			BandFound = true;
		}
		if(BandFound)
		{
			m_FlyRideAledDirection = DesiredDirection;
			m_FlyRideAledEntryProjection = EntryProjection;
			m_FlyRideAledExitProjection = ExitProjection;
			m_FlyRideAledPilotLeads = OneTeeFrozen ? PilotFrozen :
								 dot(m_FlyRidePilotPos, DesiredDirection) >=
									 dot(m_FlyRideDummyPos, DesiredDirection);
			m_FlyRideAledAttemptTick = Tick;
			m_FlyRideEmergencyClimb = false;
			m_FlyRideBalanceRecovery = false;
			EnterAledState(OneTeeFrozen ?
					       EFlyRideAledState::REAR_HITS_LEAD :
					       EFlyRideAledState::APPROACH);
		}
		else if(Tick - m_FlyRideAledLastDiagTick >= TickSpeed)
		{
			m_FlyRideAledLastDiagTick = Tick;
			char aMessage[256];
			str_format(aMessage, sizeof(aMessage),
				"tick=%d scan_miss mouse=%.1f dist=%.1f collide=%d pilot_frozen=%d dummy_frozen=%d",
				Tick, MouseDistance, Distance, CanCollidePair, PilotFrozen, DummyFrozen);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "pilot_aled", aMessage);
		}
	}

	if(m_FlyRideAledState != EFlyRideAledState::NONE)
	{
		const float PilotProjection = dot(m_FlyRidePilotPos, m_FlyRideAledDirection);
		const float DummyProjection = dot(m_FlyRideDummyPos, m_FlyRideAledDirection);
		const float LeadProjection = m_FlyRideAledPilotLeads ?
						     PilotProjection :
						     DummyProjection;
		const float RearProjection = m_FlyRideAledPilotLeads ?
						     DummyProjection :
						     PilotProjection;
		const float ProjectionGap = maximum(0.0f, LeadProjection - RearProjection);
		const bool HorizontalAled = std::fabs(m_FlyRideAledDirection.x) >= 0.55f;
		const bool LeadFrozen = m_FlyRideAledPilotLeads ? PilotFrozen : DummyFrozen;
		const bool RearFrozen = m_FlyRideAledPilotLeads ? DummyFrozen : PilotFrozen;
		const bool HammerInRange = Distance > 1.0f &&
					   Distance <= MaxHammerDistance;
		// Keep enough space for the rear tee to stay outside a single freeze tile,
		// while still leaving both tees safely inside hammer range. In particular,
		// do not over-compact vertical pairs: at ~44 px the rear tee reaches the
		// tile one prediction tick after the lead tee.
		const float AledReadyDistance = minimum(59.0f, MaxHammerDistance - 2.5f);
		const float AledMinimumDistance = minimum(
			HorizontalAled ? 50.0f : 54.0f, AledReadyDistance - 3.0f);
		const float AledMinimumProjectionGap = HorizontalAled ? 38.0f : 48.0f;
		const bool RearHammerReady = m_FlyRideAledPilotLeads ?
						     ((!pDummyCharacter || pDummyCharacter->GetReloadTimer() <= 0) &&
							     Tick >= m_FlyRideDummyNextHitTick) :
						     ((!pPilotCharacter || pPilotCharacter->GetReloadTimer() <= 0) &&
							     Tick >= m_FlyRidePilotNextHitTick);
		const bool DummyCanHookPilot = CanCollidePair &&
					       !m_aClients[DummyId].m_HookHitDisabled &&
					       m_aTuning[PairConn].m_PlayerHooking;
		const bool PhaseTimedOut = m_FlyRideAledStateTick >= 0 &&
					   Tick - m_FlyRideAledStateTick > AledPhaseTimeout;
		const bool AttemptTimedOut = m_FlyRideAledAttemptTick >= 0 &&
					     Tick - m_FlyRideAledAttemptTick > AledAttemptTimeout;
		auto RequestHammer = [&](bool PilotHammers) {
			if(!HammerInRange || !CanCollidePair)
				return;
			if(PilotHammers)
				m_FlyRidePilotHammer = !m_aClients[PilotId].m_HammerHitDisabled;
			else
				m_FlyRideDummyHammer = !m_aClients[DummyId].m_HammerHitDisabled;
		};
		auto RequestRearHammer = [&]() {
			RequestHammer(!m_FlyRideAledPilotLeads);
		};
		auto RequestLeadHammer = [&]() {
			RequestHammer(m_FlyRideAledPilotLeads);
		};
		auto SetLeadHook = [&]() {
			if(m_FlyRideAledPilotLeads)
				m_FlyRidePilotHook = CanHookDummy ? 1 : 0;
			else
				m_FlyRideDummyHook = DummyCanHookPilot ? 1 : 0;
		};
		auto SetRearHook = [&]() {
			if(m_FlyRideAledPilotLeads)
				m_FlyRideDummyHook = DummyCanHookPilot ? 1 : 0;
			else
				m_FlyRidePilotHook = CanHookDummy ? 1 : 0;
		};
		auto SetFormationDirections = [&](int LeadDirection, int RearDirection) {
			if(m_FlyRideAledPilotLeads)
			{
				m_FlyRidePilotDirection = LeadDirection;
				m_FlyRideDummyDirection = RearDirection;
			}
			else
			{
				m_FlyRideDummyDirection = LeadDirection;
				m_FlyRidePilotDirection = RearDirection;
			}
		};

		m_FlyRideEmergencyClimb = false;
		m_FlyRideBalanceRecovery = false;
		m_FlyRidePilotHammer = false;
		m_FlyRideDummyHammer = false;
		m_FlyRidePilotHook = 0;
		m_FlyRideDummyHook = 0;

		vec2 TravelDirection = m_FlyRideAledDirection;
		if(m_FlyRideAledState == EFlyRideAledState::RETREAT)
			TravelDirection = -TravelDirection;
		if(std::fabs(TravelDirection.x) > 0.20f)
		{
			const int TravelX = TravelDirection.x > 0.0f ? 1 : -1;
			m_FlyRidePilotDirection = TravelX;
			m_FlyRideDummyDirection = TravelX;
		}
		else
		{
			const float AledOffsetX = m_FlyRideDummyPos.x - m_FlyRidePilotPos.x;
			if(AledOffsetX > 14.0f)
			{
				m_FlyRidePilotDirection = 1;
				m_FlyRideDummyDirection = -1;
			}
			else if(AledOffsetX < -14.0f)
			{
				m_FlyRidePilotDirection = -1;
				m_FlyRideDummyDirection = 1;
			}
			else
			{
				m_FlyRidePilotDirection = 0;
				m_FlyRideDummyDirection = 0;
			}
		}
		m_FlyRidePilotJump = pPilotCharacter && pPilotCharacter->IsGrounded() &&
				     TravelDirection.y < -0.25f;

		if(m_FlyRideAledState != EFlyRideAledState::RETREAT &&
			(PilotFrozen && DummyFrozen || AttemptTimedOut))
			EnterAledState(EFlyRideAledState::RETREAT);

		switch(m_FlyRideAledState)
		{
		case EFlyRideAledState::NONE:
			break;
		case EFlyRideAledState::APPROACH:
			m_FlyRidePilotDirection = BasePilotDirection;
			m_FlyRideDummyDirection = BaseDummyDirection;
			m_FlyRidePilotJump = BasePilotJump;
			m_FlyRidePilotHook = BasePilotHook;
			m_FlyRideDummyHook = 0;
			m_FlyRidePilotHammer = false;
			m_FlyRideDummyHammer = BaseDummyHammer;
			if(Tick < m_FlyRideAledRetryTick)
				break;
			if(HorizontalAled &&
				LeadProjection >= m_FlyRideAledEntryProjection - 112.0f)
			{
				const int TravelX = m_FlyRideAledDirection.x > 0.0f ? 1 : -1;
				m_FlyRidePilotJump = 0;
				m_FlyRidePilotHook = 0;
				m_FlyRideDummyHook = 0;
				m_FlyRidePilotHammer = false;
				m_FlyRideDummyHammer = false;
				if(ProjectionGap < AledMinimumProjectionGap)
					SetFormationDirections(TravelX, -TravelX);
				else if(ProjectionGap > 50.0f)
					SetFormationDirections(0, TravelX);
				else if(Distance > AledReadyDistance)
				{
					SetFormationDirections(0, 0);
					SetLeadHook();
				}
				else if(Distance < AledMinimumDistance)
					SetFormationDirections(TravelX, -TravelX);
				else
					SetFormationDirections(TravelX, TravelX);
			}
			if(!HorizontalAled &&
				LeadProjection >= m_FlyRideAledEntryProjection - 48.0f)
			{
				m_FlyRidePilotHammer = false;
				m_FlyRideDummyHammer = false;
				if(Distance > AledReadyDistance ||
					Distance < AledMinimumDistance || !RearHammerReady)
				{
					m_FlyRidePilotDirection = 0;
					m_FlyRideDummyDirection = 0;
					m_FlyRidePilotJump = 0;
					m_FlyRidePilotHook = 0;
					m_FlyRideDummyHook = 0;
					// Only pull while the pair is too far apart. Continuing the hook
					// while waiting for reload used to collapse the vertical gap and
					// freeze both tees on consecutive ticks.
					if(Distance > AledReadyDistance)
						SetLeadHook();
				}
			}
			if(LeadFrozen ||
				(LeadProjection >= m_FlyRideAledEntryProjection - 14.0f &&
					ProjectionGap >= AledMinimumProjectionGap &&
					Distance >= AledMinimumDistance &&
					Distance <= AledReadyDistance && RearHammerReady))
				EnterAledState(EFlyRideAledState::LEAD_ENTERS_FREEZE);
			else if(AttemptTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::LEAD_ENTERS_FREEZE:
			if(LeadFrozen)
				EnterAledState(EFlyRideAledState::REAR_HITS_LEAD);
			else if(Distance > MaxHammerDistance)
				EnterAledState(EFlyRideAledState::RETREAT);
			else if(PhaseTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::REAR_HITS_LEAD:
			if(LeadFrozen)
			{
				if(HammerInRange)
					RequestRearHammer();
				else
					SetRearHook();
			}
			else if(LeadProjection >= m_FlyRideAledExitProjection + 6.0f)
				EnterAledState(EFlyRideAledState::REAR_ENTERS_FREEZE);
			else
				EnterAledState(EFlyRideAledState::LEAD_ENTERS_FREEZE);
			if(PhaseTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::REAR_ENTERS_FREEZE:
			// Pull in short pulses. Holding the hook through the whole second
			// crossing can overshoot beyond hammer range before freeze is seen.
			if(Distance > AledReadyDistance)
				SetLeadHook();
			if(RearFrozen)
				EnterAledState(EFlyRideAledState::LEAD_HITS_REAR);
			else if(RearProjection >= m_FlyRideAledExitProjection + 6.0f)
				EnterAledState(EFlyRideAledState::EXIT);
			else if(PhaseTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::LEAD_HITS_REAR:
			if(RearFrozen)
				RequestLeadHammer();
			else if(RearProjection >= m_FlyRideAledExitProjection + 6.0f)
				EnterAledState(EFlyRideAledState::EXIT);
			else
				EnterAledState(EFlyRideAledState::REAR_ENTERS_FREEZE);
			if(PhaseTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::EXIT:
			SetLeadHook();
			if(PilotFrozen != DummyFrozen)
				RequestHammer(DummyFrozen);
			if(!PilotFrozen && !DummyFrozen &&
				minimum(PilotProjection, DummyProjection) >=
					m_FlyRideAledExitProjection + 24.0f)
			{
				m_FlyRideAledState = EFlyRideAledState::NONE;
				m_FlyRideAledStateTick = -1;
				m_FlyRideAledAttemptTick = -1;
				m_FlyRideAledRetryTick = Tick + TickSpeed / 2;
				m_FlyRidePilotHook = 0;
				m_FlyRideDummyHook = 0;
			}
			else if(PhaseTimedOut)
				EnterAledState(EFlyRideAledState::RETREAT);
			break;
		case EFlyRideAledState::RETREAT:
			if(PilotFrozen != DummyFrozen)
				RequestHammer(DummyFrozen);
			else if(!PilotFrozen && !DummyFrozen)
			{
				const bool PilotRetreatLeads = PilotProjection <= DummyProjection;
				RequestHammer(!PilotRetreatLeads);
			}
			if(!PilotFrozen && !DummyFrozen &&
				maximum(PilotProjection, DummyProjection) <=
					m_FlyRideAledEntryProjection - 48.0f)
			{
				m_FlyRideAledPilotLeads =
					PilotProjection >= DummyProjection;
				m_FlyRideAledAttemptTick = Tick;
				m_FlyRideAledRetryTick = Tick + maximum(1, TickSpeed / 5);
				EnterAledState(EFlyRideAledState::APPROACH);
			}
			break;
		}

		// A state transition is evaluated from the latest predicted snapshot.
		// Request the rescue strike after the switch as well, so observing a
		// newly frozen tee produces fire in this very input tick instead of one
		// tick later. This is essential for vertical aled where the rear tee can
		// enter the tile on the following tick.
		if(PilotFrozen != DummyFrozen && HammerInRange)
			RequestHammer(DummyFrozen);
	}
	m_FlyRideTargetConn = PairConn;
	ApplyPilotInput();
}

bool CGameClient::ApplyAvoid(CNetObj_PlayerInput *pInput,
	int DummyIndex, int LocalId, bool &OverrodeInput)
{
	OverrodeInput = false;
	// Avoid caches the previous prediction so repeated SnapInput calls for one
	// tick stay cheap. Clear that cache while disabled: otherwise toggling Avoid
	// back on (or switching a dummy/local tee) could replay an old movement or
	// hook decision for one packet.
	static bool s_aWasEnabled[NUM_DUMMIES] = {};
	static int s_aLastLocalId[NUM_DUMMIES] = {-1, -1, -1};
	static bool s_aWasOverriding[NUM_DUMMIES] = {};
	static int s_aOverrideHook[NUM_DUMMIES] = {};
	static int s_aAnalyzedTick[NUM_DUMMIES] = {-1, -1, -1};
	static bool s_aCachedOverride[NUM_DUMMIES] = {};
	static CNetObj_PlayerInput s_aCachedInput[NUM_DUMMIES] = {};
	if(!g_Config.m_ClZzAvoidEnabled)
	{
		for(int Index = 0; Index < NUM_DUMMIES; ++Index)
		{
			s_aWasEnabled[Index] = false;
			s_aWasOverriding[Index] = false;
			s_aOverrideHook[Index] = 0;
			s_aAnalyzedTick[Index] = -1;
			s_aCachedOverride[Index] = false;
			s_aLastLocalId[Index] = -1;
		}
		return false;
	}
	// Returning true means Avoid owns this input packet. The unified final
	// application keeps the previous compatibility block from running in parallel.
	if(!pInput || DummyIndex < 0 || DummyIndex >= NUM_DUMMIES ||
		LocalId < 0 || LocalId >= MAX_CLIENTS)
		return true;

	const int NowTick = Client()->PredGameTick(DummyIndex);
	if(!s_aWasEnabled[DummyIndex] || s_aLastLocalId[DummyIndex] != LocalId ||
		NowTick < s_aAnalyzedTick[DummyIndex])
	{
		s_aWasOverriding[DummyIndex] = false;
		s_aOverrideHook[DummyIndex] = 0;
		s_aAnalyzedTick[DummyIndex] = -1;
		s_aCachedOverride[DummyIndex] = false;
	}
	s_aWasEnabled[DummyIndex] = true;
	s_aLastLocalId[DummyIndex] = LocalId;
	if(s_aAnalyzedTick[DummyIndex] == NowTick)
	{
		if(s_aCachedOverride[DummyIndex])
		{
			const CNetObj_PlayerInput &Cached = s_aCachedInput[DummyIndex];
			pInput->m_Direction = Cached.m_Direction;
			pInput->m_Jump = Cached.m_Jump;
			pInput->m_Hook = Cached.m_Hook;
			pInput->m_TargetX = Cached.m_TargetX;
			pInput->m_TargetY = Cached.m_TargetY;
			m_Controls.m_aInputData[DummyIndex].m_Direction = Cached.m_Direction;
			m_Controls.m_aInputData[DummyIndex].m_Jump = Cached.m_Jump;
			m_Controls.m_aInputData[DummyIndex].m_Hook = Cached.m_Hook;
			m_Controls.m_aInputData[DummyIndex].m_TargetX = Cached.m_TargetX;
			m_Controls.m_aInputData[DummyIndex].m_TargetY = Cached.m_TargetY;
			OverrodeInput = true;
		}
		return true;
	}
	s_aAnalyzedTick[DummyIndex] = NowTick;
	s_aCachedOverride[DummyIndex] = false;
	auto CacheOverride = [&]() {
		s_aCachedInput[DummyIndex] = *pInput;
		s_aCachedOverride[DummyIndex] = OverrodeInput;
	};
	CCharacter *pAvoidChar = m_PredictedWorld.GetCharacterById(LocalId);
	if(!pAvoidChar)
		return true;
	const int AvoidTeam = pAvoidChar->Team();
	const std::vector<SSwitchers> *pAvoidSwitchers =
		&m_PredictedWorld.Switchers();

	auto SimulateDangerExact = [&](const CNetObj_PlayerInput &DelayInput,
		int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks,
		bool AllowBraking) {
		CGameWorld SimWorld;
		// Kinetix runs the search from the current predicted world. Using the
		// regular-prediction copy here made the result lag behind the input that
		// is actually about to be sent, especially while moving or hooking.
		SimWorld.CopyWorldClean(&m_PredictedWorld);
		SimWorld.m_WorldConfig.m_IsDDRace = true;
		SimWorld.m_WorldConfig.m_IsVanilla = false;
		SimWorld.m_WorldConfig.m_PredictDDRace = true;
		SimWorld.m_WorldConfig.m_PredictTiles = true;
		SimWorld.m_WorldConfig.m_PredictFreeze = 1;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(ClientId == LocalId)
				continue;
			if(CCharacter *pOther = SimWorld.GetCharacterById(ClientId))
				delete pOther;
		}
		CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
		if(!pChar)
			return 0;
		const int StartTick = SimWorld.GameTick();
		vec2 PreviousPos = pChar->Core()->m_Pos;
		float PreviousFreezeClearance = DmdFreezeClearance(Collision(),
			PreviousPos, &SimWorld.Switchers(), pChar->Team());
		float MinimumFreezeClearance = PreviousFreezeClearance;
		int FirstMarginTick = 0;
		for(int Tick = 0; Tick < Ticks; ++Tick)
		{
			const CNetObj_PlayerInput &Input =
				Tick < Delay ? DelayInput : ComboInput;
			pChar->OnDirectInput(&Input);
			SimWorld.m_GameTick = StartTick + Tick + 1;
			pChar->OnPredictedInput(&Input);
			SimWorld.Tick();
			pChar = SimWorld.GetCharacterById(LocalId);
			if(!pChar)
				return g_Config.m_ClZzAvoidKill ? Tick + 1 : 0;
			const vec2 CurrentPos = pChar->Core()->m_Pos;
			const float CurrentFreezeClearance = DmdFreezeClearance(Collision(),
				CurrentPos, &SimWorld.Switchers(), pChar->Team());
			// Keep a full tile of clearance. If we are already inside that margin,
			// only motion farther into it is rejected, so Avoid never traps the tee
			// and still permits walking/jumping away from freeze.
			const bool ApproachesFreezeMargin =
				CurrentFreezeClearance <= 32.0f &&
				CurrentFreezeClearance + 0.05f < PreviousFreezeClearance;
			if(ApproachesFreezeMargin && FirstMarginTick == 0)
				FirstMarginTick = Tick + 1;
			MinimumFreezeClearance = minimum(MinimumFreezeClearance,
				CurrentFreezeClearance);
			if(g_Config.m_ClZzAvoidFreeze &&
				(pChar->m_FreezeTime > 0 || pChar->Core()->m_IsInFreeze ||
					pChar->Core()->m_DeepFrozen || pChar->Core()->m_LiveFrozen ||
					(ApproachesFreezeMargin && !AllowBraking) ||
					DmdSegmentTouchesFreeze(Collision(), PreviousPos, CurrentPos,
						&SimWorld.Switchers(), pChar->Team())))
				return Tick + 1;
			// Sample the complete swept segment. Checking only the final center
			// point skipped death/tele tiles at high horizontal or vertical speed.
			const int SegmentSteps = std::clamp(
				(int)ceilf(distance(PreviousPos, CurrentPos) / 8.0f), 1, 32);
			for(int SegmentStep = 0; SegmentStep <= SegmentSteps; ++SegmentStep)
			{
				const vec2 SamplePos = mix(PreviousPos, CurrentPos,
					(float)SegmentStep / (float)SegmentSteps);
				const int MapIndex = Collision()->GetPureMapIndex(SamplePos);
				if(MapIndex < 0)
					continue;
				if(g_Config.m_ClZzAvoidKill &&
					(Collision()->GetTileIndex(MapIndex) == TILE_DEATH ||
						Collision()->GetFrontTileIndex(MapIndex) == TILE_DEATH))
					return Tick + 1;
				if(g_Config.m_ClZzAvoidTele &&
					(Collision()->IsTeleport(MapIndex) ||
						Collision()->IsEvilTeleport(MapIndex) ||
						Collision()->IsTeleportHook(MapIndex)))
					return Tick + 1;
			}
			PreviousPos = CurrentPos;
			PreviousFreezeClearance = CurrentFreezeClearance;
		}
		if(g_Config.m_ClZzAvoidFreeze && AllowBraking && FirstMarginTick > 0 &&
			PreviousFreezeClearance < 32.0f &&
			PreviousFreezeClearance <= MinimumFreezeClearance + 0.25f)
			return FirstMarginTick;
		return 0;
	};

	auto SimulateDangerFast = [&](const CNetObj_PlayerInput &DelayInput,
		int Delay, const CNetObj_PlayerInput &ComboInput, int Ticks,
		bool AllowBraking) {
		CDmdFastCoreWorld FastWorld(m_PredictedWorld, Collision(), LocalId);
		CCharacterCore *pCore = FastWorld.LocalCore();
		if(!pCore)
			return 0;
		vec2 PreviousPos = pCore->m_Pos;
		float PreviousFreezeClearance = DmdFreezeClearance(Collision(),
			PreviousPos, pAvoidSwitchers, AvoidTeam);
		float MinimumFreezeClearance = PreviousFreezeClearance;
		int FirstMarginTick = 0;
		for(int Tick = 0; Tick < Ticks; ++Tick)
		{
			FastWorld.Step(Tick < Delay ? DelayInput : ComboInput);
			pCore = FastWorld.LocalCore();
			if(!pCore)
				return g_Config.m_ClZzAvoidKill ? Tick + 1 : 0;
			const vec2 CurrentPos = pCore->m_Pos;
			const float CurrentFreezeClearance = DmdFreezeClearance(Collision(),
				CurrentPos, pAvoidSwitchers, AvoidTeam);
			const bool ApproachesFreezeMargin =
				CurrentFreezeClearance <= 32.0f &&
				CurrentFreezeClearance + 0.05f < PreviousFreezeClearance;
			if(ApproachesFreezeMargin && FirstMarginTick == 0)
				FirstMarginTick = Tick + 1;
			MinimumFreezeClearance = minimum(MinimumFreezeClearance,
				CurrentFreezeClearance);
			if(g_Config.m_ClZzAvoidFreeze &&
				((ApproachesFreezeMargin && !AllowBraking) ||
					DmdSegmentTouchesFreeze(Collision(), PreviousPos, CurrentPos,
						pAvoidSwitchers, AvoidTeam)))
				return Tick + 1;
			const int SegmentSteps = std::clamp(
				(int)ceilf(distance(PreviousPos, CurrentPos) / 8.0f), 1, 32);
			for(int SegmentStep = 0; SegmentStep <= SegmentSteps; ++SegmentStep)
			{
				const vec2 SamplePos = mix(PreviousPos, CurrentPos,
					(float)SegmentStep / (float)SegmentSteps);
				const int MapIndex = Collision()->GetPureMapIndex(SamplePos);
				if(MapIndex < 0)
					continue;
				if(g_Config.m_ClZzAvoidKill &&
					(Collision()->GetTileIndex(MapIndex) == TILE_DEATH ||
						Collision()->GetFrontTileIndex(MapIndex) == TILE_DEATH))
					return Tick + 1;
				if(g_Config.m_ClZzAvoidTele &&
					(Collision()->IsTeleport(MapIndex) ||
						Collision()->IsEvilTeleport(MapIndex) ||
						Collision()->IsTeleportHook(MapIndex)))
					return Tick + 1;
			}
			PreviousPos = CurrentPos;
			PreviousFreezeClearance = CurrentFreezeClearance;
		}
		if(g_Config.m_ClZzAvoidFreeze && AllowBraking && FirstMarginTick > 0 &&
			PreviousFreezeClearance < 32.0f &&
			PreviousFreezeClearance <= MinimumFreezeClearance + 0.25f)
			return FirstMarginTick;
		return 0;
	};

	auto InputDiff = [](const CNetObj_PlayerInput &A,
		const CNetObj_PlayerInput &B) {
		int Diff = 0;
		Diff += A.m_Direction != B.m_Direction;
		Diff += (A.m_Jump != 0) != (B.m_Jump != 0);
		Diff += (A.m_Hook != 0) != (B.m_Hook != 0);
		Diff += A.m_TargetX != B.m_TargetX || A.m_TargetY != B.m_TargetY;
		return Diff;
	};

	const CNetObj_PlayerInput Current = *pInput;
	const int SimTicks = std::clamp(g_Config.m_ClZzAvoidTicks, 1, 20);
	// Kinetix checks the complete configured prediction horizon. Restricting
	// this initial check to the trigger window made Avoid wake up only two or
	// three ticks before impact, when walking/jumping could no longer change the
	// outcome. Keep the trigger value as an optional extension, never as a
	// shorter replacement for the real simulation horizon.
	const int DetectionTicks = maximum(SimTicks,
		std::clamp(g_Config.m_ClZzAvoidTriggerTicks, 1, 20));
	CNetObj_PlayerInput NoHook = Current;
	NoHook.m_Hook = 0;
	auto FinishSafe = [&]() {
		if(s_aWasOverriding[DummyIndex])
		{
			if(s_aOverrideHook[DummyIndex])
			{
				pInput->m_Hook = 0;
				m_Controls.m_aInputData[DummyIndex].m_Hook = 0;
				m_Controls.m_aFastInput[DummyIndex].m_Hook = 0;
				OverrodeInput = true;
				CacheOverride();
			}
			s_aWasOverriding[DummyIndex] = false;
		}
	};
	// The broad gate is core-only and allocation-free. In normal gameplay, far
	// from danger, Avoid now returns here without cloning a CGameWorld at all.
	const int FastDangerCurrent =
		SimulateDangerFast(Current, 0, Current, DetectionTicks, false);
	if(FastDangerCurrent == 0)
	{
		FinishSafe();
		return true;
	}
	const int DangerCurrent =
		SimulateDangerExact(Current, 0, Current, DetectionTicks, false);
	if(DangerCurrent == 0)
	{
		FinishSafe();
		return true;
	}
	const int DangerNoHook = Current.m_Hook == 0 ? DangerCurrent :
		SimulateDangerExact(NoHook, 0, NoHook, DetectionTicks, false);

	// Release a dangerous hook immediately. Re-testing "one more safe tick"
	// every frame caused a receding-horizon bug: the decision was postponed
	// forever until the tee had already entered freeze.
	if(DangerCurrent > 0 && DangerNoHook == 0)
	{
		pInput->m_Hook = 0;
		m_Controls.m_aInputData[DummyIndex].m_Hook = 0;
		m_Controls.m_aFastInput[DummyIndex].m_Hook = 0;
		s_aWasOverriding[DummyIndex] = true;
		s_aOverrideHook[DummyIndex] = 0;
		OverrodeInput = true;
		CacheOverride();
		return true;
	}

	std::vector<CNetObj_PlayerInput> vCandidates;
	vCandidates.reserve(g_Config.m_ClZzAvoidAim ? 160 : 12);
	auto AddCandidate = [&](const CNetObj_PlayerInput &Candidate) {
		for(const CNetObj_PlayerInput &Existing : vCandidates)
		{
			if(Existing.m_Direction == Candidate.m_Direction &&
				(Existing.m_Jump != 0) == (Candidate.m_Jump != 0) &&
				(Existing.m_Hook != 0) == (Candidate.m_Hook != 0) &&
				Existing.m_TargetX == Candidate.m_TargetX &&
				Existing.m_TargetY == Candidate.m_TargetY)
				return;
		}
		vCandidates.push_back(Candidate);
	};
	const int aDirections[] = {-1, 0, 1};
	const int DirectionVariants = g_Config.m_ClZzAvoidDirection ? 3 : 1;
	const int JumpVariants = g_Config.m_ClZzAvoidJump ? 2 : 1;
	const int HookVariants = g_Config.m_ClZzAvoidHookAssist ? 2 : 1;
	const float CurrentAimAngle = atan2f((float)Current.m_TargetY,
		(float)Current.m_TargetX);
	const float AimDistance = maximum(1.0f,
		sqrtf((float)Current.m_TargetX * Current.m_TargetX +
			(float)Current.m_TargetY * Current.m_TargetY));
	// A 144-angle Cartesian search used to tick thousands of candidates per
	// frame. Sixteen evenly spaced directions keep hook-assist coverage while
	// bounding the lightweight pass even with direction and jump enabled.
	const int ConfiguredAimVariants = g_Config.m_ClZzAvoidAim ?
		std::clamp(g_Config.m_ClZzAvoidAngles, 1, 16) : 1;
	const float AimFov = (float)std::clamp(g_Config.m_ClZzAvoidFov, 5, 360) *
		(pi / 180.0f);
	for(int DirectionIndex = 0; DirectionIndex < DirectionVariants;
		DirectionIndex++)
	{
		const int DirectionValue = g_Config.m_ClZzAvoidDirection ?
			aDirections[DirectionIndex] : Current.m_Direction;
		for(int JumpVariant = 0; JumpVariant < JumpVariants; ++JumpVariant)
		{
			for(int HookVariant = 0; HookVariant < HookVariants; ++HookVariant)
			{
				const int Hook = g_Config.m_ClZzAvoidHookAssist ? HookVariant :
					(Current.m_Hook != 0);
				// Aim only influences physics while hook is active. The previous
				// Cartesian product simulated up to 144 identical no-hook inputs for
				// every direction/jump pair, causing the large FPS loss.
				const int AimVariants = Hook ? ConfiguredAimVariants : 1;
				for(int AimVariant = 0; AimVariant < AimVariants; ++AimVariant)
				{
					CNetObj_PlayerInput Candidate = Current;
					Candidate.m_Direction = DirectionValue;
					Candidate.m_Jump = g_Config.m_ClZzAvoidJump ? JumpVariant :
						(Current.m_Jump != 0);
					Candidate.m_Hook = Hook;
					if(g_Config.m_ClZzAvoidAim && Hook)
					{
						const float Fraction = AimVariants == 1 ? 0.0f :
							(float)AimVariant / (float)(AimVariants - 1) - 0.5f;
						const float AimAngle = CurrentAimAngle + Fraction * AimFov;
						Candidate.m_TargetX =
							round_to_int(cosf(AimAngle) * AimDistance);
						Candidate.m_TargetY =
							round_to_int(sinf(AimAngle) * AimDistance);
					}
					AddCandidate(Candidate);
				}
			}
		}
	}

	// Rank every combination in the lightweight core sandbox, then validate only
	// the best few in the complete prediction world. This bounds the expensive
	// work near danger to a handful of worlds instead of up to 150 per tick.
	auto SurvivalTicks = [&](int Danger) {
		return Danger > 0 ? Danger - 1 : DetectionTicks;
	};
	const int CurrentSurvival = SurvivalTicks(DangerCurrent);
	auto SameInput = [](const CNetObj_PlayerInput &A,
		const CNetObj_PlayerInput &B) {
		return A.m_Direction == B.m_Direction &&
		       (A.m_Jump != 0) == (B.m_Jump != 0) &&
		       (A.m_Hook != 0) == (B.m_Hook != 0) &&
		       A.m_TargetX == B.m_TargetX && A.m_TargetY == B.m_TargetY;
	};
	struct SAvoidCandidate
	{
		CNetObj_PlayerInput m_Input;
		int m_FastSurvival;
		int m_Diff;
	};
	std::vector<SAvoidCandidate> vRanked;
	vRanked.reserve(vCandidates.size());
	for(const CNetObj_PlayerInput &Candidate : vCandidates)
	{
		const int FastDanger = SameInput(Candidate, Current) ? FastDangerCurrent :
			SimulateDangerFast(Candidate, 0, Candidate, DetectionTicks, true);
		vRanked.push_back({Candidate, SurvivalTicks(FastDanger),
			InputDiff(Current, Candidate)});
	}
	std::stable_sort(vRanked.begin(), vRanked.end(),
		[](const SAvoidCandidate &A, const SAvoidCandidate &B) {
			if(A.m_FastSurvival != B.m_FastSurvival)
				return A.m_FastSurvival > B.m_FastSurvival;
			return A.m_Diff < B.m_Diff;
		});

	int BestSurvival = CurrentSurvival;
	int BestDiff = 0;
	CNetObj_PlayerInput BestInput = Current;
	int ExactChecks = 0;
	constexpr int MaxExactCandidates = 5;
	for(const SAvoidCandidate &Ranked : vRanked)
	{
		const CNetObj_PlayerInput &Candidate = Ranked.m_Input;
		if(SameInput(Candidate, Current))
			continue;
		const int Danger = SimulateDangerExact(Candidate, 0, Candidate,
			DetectionTicks, true);
		ExactChecks++;
		const int Survival = SurvivalTicks(Danger);
		const int Diff = Ranked.m_Diff;
		if(Survival > BestSurvival ||
			(Survival == BestSurvival && Survival > CurrentSurvival &&
				Diff < BestDiff))
		{
			BestSurvival = Survival;
			BestDiff = Diff;
			BestInput = Candidate;
		}
		if(ExactChecks >= MaxExactCandidates)
			break;
	}
	if(BestSurvival <= CurrentSurvival)
		return true;

	pInput->m_Direction = BestInput.m_Direction;
	pInput->m_Jump = BestInput.m_Jump;
	pInput->m_Hook = BestInput.m_Hook;
	pInput->m_TargetX = BestInput.m_TargetX;
	pInput->m_TargetY = BestInput.m_TargetY;
	if(!g_Config.m_ClZzAvoidSilent && g_Config.m_ClZzAvoidAim)
		m_Controls.m_aMousePos[DummyIndex] =
			vec2((float)pInput->m_TargetX, (float)pInput->m_TargetY);
	m_Controls.m_aInputData[DummyIndex].m_Direction = pInput->m_Direction;
	m_Controls.m_aInputData[DummyIndex].m_Jump = pInput->m_Jump;
	m_Controls.m_aInputData[DummyIndex].m_Hook = pInput->m_Hook;
	m_Controls.m_aInputData[DummyIndex].m_TargetX = pInput->m_TargetX;
	m_Controls.m_aInputData[DummyIndex].m_TargetY = pInput->m_TargetY;
	m_Controls.m_aFastInput[DummyIndex].m_Direction = pInput->m_Direction;
	m_Controls.m_aFastInput[DummyIndex].m_Jump = pInput->m_Jump;
	m_Controls.m_aFastInput[DummyIndex].m_Hook = pInput->m_Hook;
	m_Controls.m_aFastInput[DummyIndex].m_TargetX = pInput->m_TargetX;
	m_Controls.m_aFastInput[DummyIndex].m_TargetY = pInput->m_TargetY;
	s_aWasOverriding[DummyIndex] = true;
	s_aOverrideHook[DummyIndex] = pInput->m_Hook;
	OverrodeInput = true;
	CacheOverride();
	return true;
}

void CGameClient::ResetKinetixLaserUnfreeze()
{
	for(auto &State : m_aKinetixLaserUnfreezeState)
		State = {};
	m_Controls.m_LaserUnfreezeAimActive = false;
	m_Controls.m_LaserUnfreezeAimOffset = vec2(0.0f, 0.0f);
	m_vKinetixLaserUnfreezePath.clear();
	m_KinetixLaserUnfreezePathTime = 0;
}

bool CGameClient::ApplyKinetixLaserUnfreeze(CNetObj_PlayerInput *pInput,
	int DummyIndex, int LocalId)
{
	// Safe KernelNet port of Kinetix Laser Unfreeze. The original component
	// keeps a raw CLaser pointer after CGameWorld::Tick, although that tick may
	// destroy the entity. This version uses the same predictive trigger,
	// bounce timing and latest movement path without retaining dangling entity
	// pointers, which is required for ClickGUI/map crash stability.
	// Keep the generated input edge stable when OnSnapInput is called more than
	// once in a predicted tick. The state is owned by CGameClient so reconnects,
	// map changes and dummy changes can reset it deterministically.
	if(DummyIndex < 0 || DummyIndex >= NUM_DUMMIES || !pInput)
		return false;
	SKinetixLaserUnfreezeState &State =
		m_aKinetixLaserUnfreezeState[DummyIndex];
	const int NowTick = Client()->PredGameTick(DummyIndex);

	auto ReleaseInjectedFire = [&]() {
		m_Controls.m_LaserUnfreezeAimActive = false;
		const bool ChangedInput = State.m_WasFiring && State.m_ReleaseRequired;
		if(ChangedInput)
		{
			pInput->m_Fire =
				(State.m_InjectedFire + 1) & INPUT_STATE_MASK;
			m_Controls.m_aInputData[DummyIndex].m_Fire = pInput->m_Fire;
			m_Controls.m_aFastInput[DummyIndex].m_Fire = pInput->m_Fire;
		}
		State.m_WasFiring = false;
		State.m_ReleaseRequired = false;
		State.m_InjectedFire = -1;
		State.m_ShotAimOffset = vec2(0.0f, -1.0f);
		return ChangedInput;
	};
	auto ApplyStoredShot = [&]() {
		pInput->m_TargetX = round_to_int(State.m_ShotAimOffset.x);
		pInput->m_TargetY = round_to_int(State.m_ShotAimOffset.y);
		if(!pInput->m_TargetX && !pInput->m_TargetY)
			pInput->m_TargetY = 1;
		pInput->m_Fire = State.m_InjectedFire;
		if(g_Config.m_ClZzAutoUnfreezeAutoLaser)
		{
			pInput->m_WantedWeapon = WEAPON_LASER + 1;
			m_Controls.m_aInputData[DummyIndex].m_WantedWeapon =
				WEAPON_LASER + 1;
			m_Controls.m_aFastInput[DummyIndex].m_WantedWeapon =
				WEAPON_LASER + 1;
		}
		return true;
	};

	if(!g_Config.m_ClZzAutoUnfreeze || LocalId < 0 || LocalId >= MAX_CLIENTS)
	{
		const bool Released = ReleaseInjectedFire();
		State.m_AnalyzedTick = -1;
		State.m_LocalId = -1;
		m_vKinetixLaserUnfreezePath.clear();
		return Released;
	}
	// A lower prediction tick or a different local id means a new prediction
	// session. Never replay an input edge from the previous session.
	if((State.m_LocalId >= 0 && State.m_LocalId != LocalId) ||
		(State.m_AnalyzedTick >= 0 && NowTick < State.m_AnalyzedTick))
	{
		State = {};
		m_Controls.m_LaserUnfreezeAimActive = false;
	}
	State.m_LocalId = LocalId;

	CCharacter *pPredChar = m_PredictedWorld.GetCharacterById(LocalId);
	if(!pPredChar)
	{
		const bool Released = ReleaseInjectedFire();
		State.m_AnalyzedTick = -1;
		m_vKinetixLaserUnfreezePath.clear();
		return Released;
	}

	if(pPredChar->m_FreezeTime > 0 || pPredChar->Core()->m_DeepFrozen ||
		pPredChar->Core()->m_LiveFrozen)
	{
		return ReleaseInjectedFire();
	}
	// OnSnapInput may be queried repeatedly for the same predicted tick. A
	// failed angle search is deterministic, so running all copied worlds again
	// only wastes CPU and was visible as a sharp FPS drop near freeze.
	if(State.m_AnalyzedTick == NowTick)
	{
		if(!State.m_WasFiring)
			return false;
		return ApplyStoredShot();
	}
	State.m_AnalyzedTick = NowTick;
	constexpr int MaxSimulationTicks = 50;
	constexpr int MaxInputLeadTicks = 10;
	constexpr int MaxTriggerTicks = 15;
	const int SnapshotLatencyMs = m_Snap.m_pLocalInfo ?
		std::clamp(m_Snap.m_pLocalInfo->m_Latency, 0, 400) :
		0;
	const int PredictionTimeMs =
		std::clamp(Client()->GetPredictionTime(), 0, 400);
	// Use a capped one-way input lead. The exact world simulation below still
	// has to validate a returning laser, so the larger high-ping warning window
	// cannot by itself generate an unsafe shot.
	const int InputLeadTicks = std::clamp(
		(maximum(SnapshotLatencyMs, PredictionTimeMs) *
			Client()->GameTickSpeed() +
			1999) /
			2000,
		0, MaxInputLeadTicks);
	const int TriggerTicks = std::clamp(
		std::clamp(g_Config.m_ClZzAutoUnfreezeTriggerTicks, 1, 5) +
			InputLeadTicks,
		1, MaxTriggerTicks);
	float LaserReach = pPredChar->Core()->m_Tuning.m_LaserReach;
	if(LaserReach <= 0.0f)
		LaserReach = 800.0f;
	CNetObj_PlayerInput MovementInput = *pInput;
	if(MovementInput.m_Fire & 1)
		MovementInput.m_Fire = (MovementInput.m_Fire + 1) & INPUT_STATE_MASK;
	MovementInput.m_WantedWeapon = 0;
	MovementInput.m_NextWeapon = 0;
	MovementInput.m_PrevWeapon = 0;

	// Cheap broad-phase: character-core movement plus map freeze samples. Most
	// ticks are nowhere near freeze, so avoid allocating a complete CGameWorld
	// there. Hooked movement falls back to the exact world because it depends on
	// another character and cannot be represented by the isolated core.
	constexpr int MaxQuickTicks = MaxTriggerTicks + 2;
	vec2 aQuickPos[MaxQuickTicks];
	bool aQuickTouchesFreeze[MaxQuickTicks] = {};
	const int QuickTicks = minimum(MaxQuickTicks, TriggerTicks + 2);
	CCharacterCore QuickCore = *pPredChar->Core();
	QuickCore.m_Input = MovementInput;
	QuickCore.m_Direction = MovementInput.m_Direction;
	DmdPredictCharacterPath(&QuickCore, QuickTicks, aQuickPos,
		Collision(), false, aQuickTouchesFreeze, &m_PredictedWorld.Switchers(),
		pPredChar->Team());
	bool QuickFreezeRisk = pPredChar->Core()->m_HookState != HOOK_IDLE ||
		pPredChar->Core()->HookedPlayer() >= 0;
	for(int Tick = 0; Tick < QuickTicks && !QuickFreezeRisk; ++Tick)
	{
		QuickFreezeRisk = aQuickTouchesFreeze[Tick] ||
			DmdIsFreezeNearTee(Collision(), aQuickPos[Tick],
				&m_PredictedWorld.Switchers(), pPredChar->Team());
	}
	if(!QuickFreezeRisk)
		return ReleaseInjectedFire();

	auto RemoveCopiedLasers = [](CGameWorld &World) {
		for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_LASER);
			pEntity;)
		{
			CEntity *pNext = pEntity->TypeNext();
			delete pEntity;
			pEntity = pNext;
		}
	};
	auto ConfigureUnfreezeWorld = [](CGameWorld &World) {
		// Some custom servers expose freeze tiles without advertising client-side
		// tile/freeze prediction. Enable only those prediction switches inside the
		// disposable sandbox while preserving the server's remaining physics mode.
		World.m_WorldConfig.m_PredictDDRace = true;
		World.m_WorldConfig.m_PredictTiles = true;
		World.m_WorldConfig.m_PredictFreeze = 1;
	};
	auto FindShotLaser = [&](CGameWorld &World) -> CLaser * {
		for(CEntity *pEntity = World.FindFirst(CGameWorld::ENTTYPE_LASER);
			pEntity; pEntity = pEntity->TypeNext())
		{
			auto *pLaser = static_cast<CLaser *>(pEntity);
			if(pLaser->GetOwner() == LocalId)
				return pLaser;
		}
		return nullptr;
	};

	// Kinetix throttles phase 1 every five OnUpdate calls. DDNet normally calls
	// OnUpdate several times per predicted game tick. Throttling by predicted
	// tick instead created an uncovered 5-tick gap, so run this once per new
	// predicted tick (the analyzed-tick guard above still prevents FPS waste).
	bool WillFreezeSoon = false;
	int PredictedFreezeStep = -1;
	CGameWorld TriggerWorld;
	// CopyWorld establishes live parent/child prediction links and must not be
	// used for a disposable sandbox. CopyWorldClean keeps the source prediction
	// world untouched when temporary entities are removed or destroyed.
	TriggerWorld.CopyWorldClean(&m_PredictedWorld);
	ConfigureUnfreezeWorld(TriggerWorld);
	CCharacter *pTriggerChar = TriggerWorld.GetCharacterById(LocalId);
	if(!pTriggerChar)
		return ReleaseInjectedFire();
	CNetObj_PlayerInput TriggerInput = MovementInput;
	const int TriggerStartTick = TriggerWorld.GameTick();
	for(int Tick = 0; Tick < TriggerTicks; ++Tick)
	{
		pTriggerChar->OnDirectInput(&TriggerInput);
		TriggerWorld.m_GameTick = TriggerStartTick + Tick + 1;
		pTriggerChar->OnPredictedInput(&TriggerInput);
		TriggerWorld.Tick();
		pTriggerChar = TriggerWorld.GetCharacterById(LocalId);
		if(!pTriggerChar)
			break;
		if(pTriggerChar->m_FreezeTime > 0 ||
			pTriggerChar->Core()->m_DeepFrozen ||
			pTriggerChar->Core()->m_LiveFrozen)
		{
			WillFreezeSoon = true;
			PredictedFreezeStep = Tick + 1;
			break;
		}
	}
	if(!WillFreezeSoon)
		return ReleaseInjectedFire();
	const double BounceDelayMs = maximum(
		0.0, (double)pPredChar->Core()->m_Tuning.m_LaserBounceDelay);
	const int BounceDelayTicks = maximum(1,
		(int)floor((double)Client()->GameTickSpeed() *
			(BounceDelayMs / 1000.0) +
			1e-6) +
			1);
	// Always simulate long enough to observe at least one delayed return after
	// the predicted freeze entry, even when the configured value is too short
	// for a server's laser-bounce tuning.
	const int SimulationTicks = std::clamp(
		maximum(std::clamp(g_Config.m_ClZzAutoUnfreezeTicks, 1,
				MaxSimulationTicks),
			PredictedFreezeStep + BounceDelayTicks + 2),
		1, MaxSimulationTicks);

	// Once the shot edge was generated, keep the exact same input until the
	// danger disappears. Re-running all ray worlds while that laser is already
	// in flight caused the visible micro-freezes near freeze tiles.
	if(State.m_WasFiring)
		return ApplyStoredShot();

	// Do not spend time searching angles, and never consume a fire edge, while
	// the laser cannot actually be fired. Direct weapon selection is processed
	// before FireWeapon by DDNet, so a ready owned laser may switch and fire in
	// the same packet when Auto laser is enabled.
	const bool AutoLaser = g_Config.m_ClZzAutoUnfreezeAutoLaser != 0;
	const bool HasLaser = pPredChar->GetWeaponGot(WEAPON_LASER) &&
		pPredChar->GetWeaponAmmo(WEAPON_LASER) != 0;
	const bool CorrectWeapon =
		pPredChar->GetActiveWeapon() == WEAPON_LASER || AutoLaser;
	if(!HasLaser || !CorrectWeapon || pPredChar->GetReloadTimer() > 0)
		return false;
	if(AutoLaser)
	{
		pInput->m_WantedWeapon = WEAPON_LASER + 1;
		m_Controls.m_aInputData[DummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
		m_Controls.m_aFastInput[DummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
	}

	const vec2 InputAim((float)pInput->m_TargetX, (float)pInput->m_TargetY);
	const vec2 CurrentAim = length(InputAim) > 0.001f ?
					normalize(InputAim) :
					vec2(0.0f, -1.0f);
	const float CurrentAngle = angle(CurrentAim);
	const float ScanWidth =
		(float)std::clamp(g_Config.m_ClZzAutoUnfreezeFov, 5, 360) *
		(pi / 180.0f);
	const int ConfiguredAngles =
		std::clamp(g_Config.m_ClZzAutoUnfreezeAngles, 1, 144);
	const int NumAngles = ConfiguredAngles;
	const bool FullCircle =
		std::clamp(g_Config.m_ClZzAutoUnfreezeFov, 5, 360) == 360;
	const float AngleStep = NumAngles > 1 ?
		ScanWidth / (float)(FullCircle ? NumAngles : NumAngles - 1) :
		0.0f;
	const float StartAngle = NumAngles > 1 ?
		CurrentAngle - ScanWidth * 0.5f :
		CurrentAngle;
	const vec2 MyPos = pPredChar->m_Pos;
	int BestScore = std::numeric_limits<int>::max();
	vec2 BestBouncePos(0.0f, 0.0f);
	std::vector<vec2> BestPath;
	bool FoundCandidate = false;

	// Kinetix phase 2: replay one complete predicted world per configured
	// angle, spawn the real prediction CLaser and rank the first reflected ray
	// for which the copied character is unfrozen. Keep other characters: they
	// affect hooked movement and can intercept the laser on the real server.
	for(int AngleIndex = 0; AngleIndex < NumAngles; ++AngleIndex)
	{
		const float TestAngle = StartAngle + AngleStep * (float)AngleIndex;
		const vec2 LaserDir = direction(TestAngle);
		// A self-unfreeze requires at least one real wall bounce. Reject open
		// directions before allocating and simulating a complete copied world.
		vec2 FirstCollision;
		vec2 BeforeFirstCollision;
		if(Collision()->IntersectLineTeleWeapon(MyPos,
			MyPos + LaserDir * LaserReach, &FirstCollision,
			&BeforeFirstCollision) == 0)
			continue;
		CGameWorld RayWorld;
		RayWorld.CopyWorldClean(&m_PredictedWorld);
		ConfigureUnfreezeWorld(RayWorld);
		RemoveCopiedLasers(RayWorld);
		CCharacter *pRayChar = RayWorld.GetCharacterById(LocalId);
		if(!pRayChar)
			continue;

		CLaser *pSpawnedLaser = new CLaser(&RayWorld, MyPos, LaserDir,
			LaserReach, LocalId, WEAPON_LASER);
		CNetObj_PlayerInput RayInput = MovementInput;
		const int StartTick = RayWorld.GameTick();
		int Score = std::numeric_limits<int>::max();
		std::vector<vec2> LaserPath;
		LaserPath.push_back(MyPos);
		// CLaser performs its first trace in the constructor. Store that wall
		// immediately, otherwise a zero/short bounce delay can destroy the laser
		// before the old tracker ever observes its first reflection.
		const vec2 InitialBouncePos = pSpawnedLaser->GetPos();
		LaserPath.push_back(InitialBouncePos);
		vec2 PreviousFrom = InitialBouncePos;
		bool GotBounce = true;

		for(int Tick = 0; Tick < SimulationTicks; ++Tick)
		{
			pRayChar->OnDirectInput(&RayInput);
			RayWorld.m_GameTick = StartTick + Tick + 1;
			pRayChar->OnPredictedInput(&RayInput);
			RayWorld.Tick();
			pRayChar = RayWorld.GetCharacterById(LocalId);
			if(!pRayChar)
				break;

			// RayWorld::Tick may destroy the laser. Reacquiring it preserves
			// Kinetix's path logic without dereferencing its dangling pointer.
			CLaser *pShotLaser = FindShotLaser(RayWorld);
			if(pShotLaser)
			{
				const vec2 LaserFrom = pShotLaser->GetFrom();
				if(distance(LaserFrom, PreviousFrom) > 1.0f)
				{
					LaserPath.push_back(LaserFrom);
					PreviousFrom = LaserFrom;
					GotBounce = true;
				}
			}

			// The original Kinetix code accepted an angle while the tee was
			// still unfrozen, before reaching the predicted freeze tick. Require
			// the ray to keep/unfreeze the tee at the actual danger step instead.
			if(Tick + 1 >= PredictedFreezeStep &&
				pRayChar->m_FreezeTime == 0 &&
				!pRayChar->Core()->m_DeepFrozen &&
				!pRayChar->Core()->m_LiveFrozen)
			{
				Score = Tick + 1;
				if(pShotLaser)
					LaserPath.push_back(pShotLaser->GetPos());
				break;
			}
		}

		if(Score < BestScore && GotBounce && LaserPath.size() >= 2)
		{
			BestScore = Score;
			BestBouncePos = LaserPath[1];
			BestPath = std::move(LaserPath);
			FoundCandidate = true;
		}
		// No later angle can beat the first valid tick accepted above. The old
		// loop still copied and simulated every remaining world in this case.
		if(BestScore == PredictedFreezeStep)
			break;
	}
	if(!FoundCandidate)
		return false;

	// Kinetix phase 3 aims at the first wall bounce, not at a normalized ray.
	// That distinction is important on close walls and tele-laser surfaces.
	const vec2 AimOffset = BestBouncePos - MyPos;
	if(g_Config.m_ClZzAutoUnfreezeAutoLaser &&
		pPredChar->GetActiveWeapon() != WEAPON_LASER)
	{
		pInput->m_WantedWeapon = WEAPON_LASER + 1;
		m_Controls.m_aInputData[DummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
		m_Controls.m_aFastInput[DummyIndex].m_WantedWeapon = WEAPON_LASER + 1;
	}
	pInput->m_TargetX = round_to_int(AimOffset.x);
	pInput->m_TargetY = round_to_int(AimOffset.y);
	if(!pInput->m_TargetX && !pInput->m_TargetY)
		pInput->m_TargetY = 1;
	if(g_Config.m_ClZzAutoUnfreezeSilent)
	{
		// Analysis now runs on the final packet input. The packet already contains
		// the hidden aim, so leaving the old deferred channel active would replay
		// this direction on the next tick.
		m_Controls.m_LaserUnfreezeAimActive = false;
	}
	else
	{
		m_Controls.m_aMousePos[DummyIndex] = AimOffset;
		m_Controls.m_aInputData[DummyIndex].m_TargetX = pInput->m_TargetX;
		m_Controls.m_aInputData[DummyIndex].m_TargetY = pInput->m_TargetY;
	}
	m_Controls.m_aFastInput[DummyIndex].m_TargetX = pInput->m_TargetX;
	m_Controls.m_aFastInput[DummyIndex].m_TargetY = pInput->m_TargetY;
	const bool UserAlreadyFiring = (pInput->m_Fire & 1) != 0;
	if(UserAlreadyFiring)
		pInput->m_Fire = ((pInput->m_Fire + 2) | 1) & INPUT_STATE_MASK;
	else
		pInput->m_Fire = (pInput->m_Fire + 1) & INPUT_STATE_MASK;
	m_Controls.m_aInputData[DummyIndex].m_Fire = pInput->m_Fire;
	m_Controls.m_aFastInput[DummyIndex].m_Fire = pInput->m_Fire;
	State.m_ShotAimOffset = AimOffset;
	State.m_WasFiring = true;
	State.m_ReleaseRequired = !UserAlreadyFiring;
	State.m_InjectedFire = pInput->m_Fire;
	if(g_Config.m_ClZzAutoUnfreezeShowAttempt && BestPath.size() >= 2)
	{
		m_vKinetixLaserUnfreezePath = BestPath;
		m_KinetixLaserUnfreezePathTime = time_get();
	}
	return true;
}

void CGameClient::RenderKinetixLaserUnfreezeAttempt()
{
	if(!g_Config.m_ClZzAutoUnfreezeShowAttempt)
	{
		m_vKinetixLaserUnfreezePath.clear();
		return;
	}
	if(m_vKinetixLaserUnfreezePath.size() < 2)
		return;
	constexpr float FadeDuration = 5.0f;
	const float Elapsed =
		(float)(time_get() - m_KinetixLaserUnfreezePathTime) / (float)time_freq();
	if(Elapsed >= FadeDuration)
	{
		m_vKinetixLaserUnfreezePath.clear();
		return;
	}

	float aPoints[4];
	Graphics()->MapScreenToWorld(m_Camera.m_Center.x, m_Camera.m_Center.y, 100.0f,
		100.0f, 100.0f, 0.0f, 0.0f, Graphics()->ScreenAspect(),
		m_Camera.m_Zoom, aPoints);
	Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);
	Graphics()->TextureClear();
	Graphics()->BlendNormal();
	Graphics()->LinesBegin();
	Graphics()->SetColor(0.20f, 0.70f, 1.0f,
		0.85f * (1.0f - Elapsed / FadeDuration));
	for(size_t Index = 1; Index < m_vKinetixLaserUnfreezePath.size(); Index++)
	{
		const IGraphics::CLineItem Line(
			m_vKinetixLaserUnfreezePath[Index - 1].x,
			m_vKinetixLaserUnfreezePath[Index - 1].y,
			m_vKinetixLaserUnfreezePath[Index].x,
			m_vKinetixLaserUnfreezePath[Index].y);
		Graphics()->LinesDraw(&Line, 1);
	}
	Graphics()->LinesEnd();
}



int CGameClient::OnSnapInput(int *pData, int Conn, bool Force)
{
	if(Conn == g_Config.m_ClDummy)
	{
		const int Size = m_Controls.SnapInput(pData);
		if(Size <= 0)
			return Size;

		auto *pInput = (CNetObj_PlayerInput *)pData;
		// Chat, the regular menus, and ClickGUI have already produced a released
		// input in CControls::SnapInput. Keep sending that neutral input for ACK
		// freshness, but do not let gameplay automations reapply actions while a
		// UI owns the keyboard and mouse.
		if((pInput->m_PlayerFlags & PLAYERFLAG_PLAYING) == 0)
			return Size;

		AutoUnfreezeDebugUpdateRecording();
		const CNetObj_PlayerInput OrigInput = *pInput;
		bool AvoidAllowed = true;
		if(m_Snap.m_pLocalCharacter)
		{
			const int LocalId = m_Snap.m_LocalClientId;
			const vec2 LocalPos = m_aClients[LocalId].m_RegularPredicted.m_Pos;
			if(g_Config.m_ClZzFlyRide && m_FlyRideWasActive &&
				m_FlyRideTargetConn == Client()->DummyPair())
			{
				AvoidAllowed = false;
				pInput->m_Direction = m_FlyRidePilotDirection;
				pInput->m_Jump = m_FlyRidePilotJump;
				pInput->m_Hook = m_FlyRidePilotHook;
				const bool AledActive =
					m_FlyRideAledState != EFlyRideAledState::NONE;
				if(m_FlyRidePilotHook || m_FlyRidePilotHammer || AledActive)
				{
					vec2 Aim = m_FlyRideDummyPos - LocalPos;
					if(length(Aim) > 0.001f)
						Aim = normalize(Aim) * 1000.0f;
					else
						Aim = vec2(0.0f, 1.0f);
					pInput->m_TargetX = round_to_int(Aim.x);
					pInput->m_TargetY = round_to_int(Aim.y);
				}
				const int Tick = Client()->PredGameTick(g_Config.m_ClDummy);
				CCharacter *pPilotCharacter =
					m_RegularPredictedWorld.GetCharacterById(LocalId);
				const bool HammerReady = !pPilotCharacter ||
							 (pPilotCharacter->GetReloadTimer() <= 0 &&
								 !pPilotCharacter->HammerHitDisabled());
				if(AledActive)
				{
					pInput->m_WantedWeapon = WEAPON_HAMMER + 1;
					pInput->m_NextWeapon = 0;
					pInput->m_PrevWeapon = 0;
					if(Tick != m_FlyRidePilotLastFireTick)
					{
						m_FlyRidePilotLastFireTick = Tick;
						const bool Strike = m_FlyRidePilotHammer && HammerReady &&
								    Tick >= m_FlyRidePilotNextHitTick;
						if(Strike)
						{
							if((m_FlyRidePilotFire & 1) != 0)
								m_FlyRidePilotFire++;
							m_FlyRidePilotFire++;
							const int HammerDelay = maximum(1,
								(int)ceilf(m_aTuning[g_Config.m_ClDummy]
										   .GetWeaponFireDelay(WEAPON_HAMMER) *
									   Client()->GameTickSpeed()));
							m_FlyRidePilotNextHitTick = Tick + HammerDelay;
						}
						else if((m_FlyRidePilotFire & 1) != 0)
							m_FlyRidePilotFire++;
						m_FlyRidePilotFire &= INPUT_STATE_MASK;
					}
					pInput->m_Fire = m_FlyRidePilotFire;
				}
				else if((m_FlyRidePilotFire & 1) != 0)
				{
					m_FlyRidePilotFire = (m_FlyRidePilotFire + 1) & INPUT_STATE_MASK;
					pInput->m_Fire = m_FlyRidePilotFire;
				}
				else
					m_FlyRidePilotFire = pInput->m_Fire & INPUT_STATE_MASK;
			}
			else
			{
			bool ZzAutoUnfreezeOverridesInput = false;
			bool ZzFentBotOverridesInput = false;
			bool ZzAvoidOverridesInput = false;
			bool HookAimApplied = false;
			vec2 CurAim((float)pInput->m_TargetX, (float)pInput->m_TargetY);
			if(length(CurAim) < 0.001f)
				CurAim = vec2(1.0f, 0.0f);
			CurAim = normalize(CurAim);
			const bool SaveInBlockActive =
				m_Controls.m_aSaveInBlock[g_Config.m_ClDummy] != 0;
			const int DummyIndex = g_Config.m_ClDummy;
			auto RayHitsAnyPlayer = [&](const vec2 &From, const vec2 &To) -> bool {
				const vec2 Seg = To - From;
				const float SegLenSq = dot(Seg, Seg);
				if(SegLenSq < 0.001f)
					return false;
				const float Radius = CCharacterCore::PhysicalSize() + 2.0f;
				const float RadiusSq = Radius * Radius;
				CCharacter *pLocalHookChar =
					m_RegularPredictedWorld.GetCharacterById(LocalId);
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(i == LocalId)
						continue;
					if(!m_Snap.m_aCharacters[i].m_Active)
						continue;
					if(pLocalHookChar && !pLocalHookChar->CanCollide(i))
						continue;
					const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
					const vec2 AP = P - From;
					float T = dot(AP, Seg) / SegLenSq;
					T = std::clamp(T, 0.0f, 1.0f);
					const vec2 Closest = From + Seg * T;
					const vec2 Diff = P - Closest;
					if(dot(Diff, Diff) <= RadiusSq)
						return true;
				}
				return false;
			};
			if(m_pKoGAIController && m_pKoGAIController->ProcessInput(
							 pInput, OrigInput, SaveInBlockActive))
			{
				ZzFentBotOverridesInput = true;
				ZzAutoUnfreezeOverridesInput = true;
			}

			m_FentBotActive = false;
			m_FentBotPlannedPathLen = 0;
			if((m_FentBotRecordActive && m_FentBotRecordHandle) ||
				(m_FentBotClipActive && m_FentBotClipHandle) ||
				(m_FentBotSavePlrActive && m_FentBotSavePlrHandle))
			{
				static const vec2 s_aAimCands[] = {
					vec2(0.0f, -1.0f),
					normalize(vec2(-0.4f, -1.0f)),
					normalize(vec2(0.4f, -1.0f)),
					vec2(-1.0f, 0.0f),
					vec2(1.0f, 0.0f),
				};
				auto EncodeTile = [&](int T) -> float {
					if(T == TILE_FREEZE || T == TILE_DFREEZE || T == TILE_LFREEZE)
						return 2.0f / 3.0f;
					if(T == TILE_DEATH)
						return 1.0f;
					if(T == TILE_SOLID || T == TILE_NOHOOK)
						return 1.0f / 3.0f;
					return 0.0f;
				};
				vec2 Aim((float)OrigInput.m_TargetX, (float)OrigInput.m_TargetY);
				if(length(Aim) < 0.001f)
					Aim = vec2(1.0f, 0.0f);
				Aim = normalize(Aim);
				int AimIndex = 0;
				float BestAimD = 1e18f;
				for(int i = 0; i < (int)std::size(s_aAimCands); i++)
				{
					const float d = distance(Aim, s_aAimCands[i]);
					if(d < BestAimD)
					{
						BestAimD = d;
						AimIndex = i;
					}
				}
				const int Dir = std::clamp(OrigInput.m_Direction, -1, 1);
				const bool Jump = (OrigInput.m_Jump & 1) != 0;
				const bool Hook = OrigInput.m_Hook != 0;
				const int DirIndex = Dir + 1;
				const int ActionId =
					(((DirIndex * 2 + (Jump ? 1 : 0)) * 2 + (Hook ? 1 : 0)) *
						(int)std::size(s_aAimCands)) +
					AimIndex;

				auto AppendNearestPlayers = [&](std::array<float, FENTBOT_OBS_DIM> &Obs,
								    int &o) {
					struct SCand
					{
						float m_D2;
						int m_Id;
					};
					SCand aCands[MAX_CLIENTS];
					int Num = 0;
					const float Radius = (float)(FENTBOT_PLAYER_RADIUS_TILES * 32);
					const float R2 = Radius * Radius;
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(i == LocalId)
							continue;
						if(!m_Snap.m_aCharacters[i].m_Active)
							continue;
						const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
						const vec2 d = P - LocalPos;
						const float d2 = dot(d, d);
						if(d2 > R2)
							continue;
						aCands[Num++] = {d2, i};
					}
					std::sort(aCands, aCands + Num, [](const SCand &A, const SCand &B) {
						return A.m_D2 < B.m_D2;
					});
					for(int k = 0; k < FENTBOT_PLAYERS_K; k++)
					{
						if(k < Num)
						{
							const int Id = aCands[k].m_Id;
							const vec2 P = m_aClients[Id].m_RegularPredicted.m_Pos;
							const vec2 V = m_aClients[Id].m_RegularPredicted.m_Vel;
							const vec2 d = P - LocalPos;
							Obs[o++] = d.x / 1000.0f;
							Obs[o++] = d.y / 1000.0f;
							Obs[o++] = V.x / 1000.0f;
							Obs[o++] = V.y / 1000.0f;
						}
						else
						{
							Obs[o++] = 0.0f;
							Obs[o++] = 0.0f;
							Obs[o++] = 0.0f;
							Obs[o++] = 0.0f;
						}
					}
				};
				std::array<float, FENTBOT_OBS_DIM> Obs;
				const vec2 Goal =
					m_FentBotHasWaypoint ? (m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint) : LocalPos;
				const vec2 Vel = m_aClients[LocalId].m_RegularPredicted.m_Vel;
				const vec2 dd = Goal - LocalPos;
				Obs[0] = dd.x / 1000.0f;
				Obs[1] = dd.y / 1000.0f;
				Obs[2] = Vel.x / 1000.0f;
				Obs[3] = Vel.y / 1000.0f;
				const int W = Collision()->GetWidth();
				const int H = Collision()->GetHeight();
				const int Center = Collision()->GetPureMapIndex(LocalPos);
				int cx = 0;
				int cy = 0;
				if(Center >= 0)
				{
					cx = Center % W;
					cy = Center / W;
				}
				int o = 4;
				for(int dy = -FENTBOT_GRID_R; dy <= FENTBOT_GRID_R; dy++)
				{
					for(int dx = -FENTBOT_GRID_R; dx <= FENTBOT_GRID_R; dx++)
					{
						const int tx = std::clamp(cx + dx, 0, std::max(0, W - 1));
						const int ty = std::clamp(cy + dy, 0, std::max(0, H - 1));
						const int Idx = ty * W + tx;
						Obs[o++] = EncodeTile(Collision()->GetTileIndex(Idx));
						Obs[o++] = EncodeTile(Collision()->GetFrontTileIndex(Idx));
					}
				}
				AppendNearestPlayers(Obs, o);

				if(m_vFentBotRecent.empty())
				{
					const int Tps = std::max(1, Client()->GameTickSpeed());
					const int Cap = std::clamp(Tps * 10, 100, 4000);
					m_vFentBotRecent.resize(Cap);
					m_FentBotRecentHead = 0;
					m_FentBotRecentCount = 0;
				}
				m_vFentBotRecent[m_FentBotRecentHead].m_Action = ActionId;
				m_vFentBotRecent[m_FentBotRecentHead].m_Obs = Obs;
				m_FentBotRecentHead =
					(m_FentBotRecentHead + 1) % (int)m_vFentBotRecent.size();
				m_FentBotRecentCount =
					std::min((int)m_vFentBotRecent.size(), m_FentBotRecentCount + 1);

				if(m_FentBotRecordActive && m_FentBotRecordHandle)
				{
					io_write(m_FentBotRecordHandle, &ActionId, (int)sizeof(ActionId));
					io_write(m_FentBotRecordHandle, Obs.data(),
						(int)(Obs.size() * sizeof(float)));
					m_FentBotRecordWritten++;
				}
				if(m_FentBotClipActive && m_FentBotClipHandle)
				{
					io_write(m_FentBotClipHandle, &ActionId, (int)sizeof(ActionId));
					io_write(m_FentBotClipHandle, Obs.data(),
						(int)(Obs.size() * sizeof(float)));
					m_FentBotClipWritten++;
				}
				if(m_FentBotSavePlrActive && m_FentBotSavePlrHandle)
				{
					io_write(m_FentBotSavePlrHandle, &ActionId, (int)sizeof(ActionId));
					io_write(m_FentBotSavePlrHandle, Obs.data(),
						(int)(Obs.size() * sizeof(float)));
					m_FentBotSavePlrWritten++;
				}
			}
			if(g_Config.m_ClZzFentBotEnabled && g_Config.m_ClZzFentBotPyInfer &&
				!SaveInBlockActive && !Client()->DummyConnected())
			{
				bool FrozenNow = Client()->m_IsLocalFrozen;
				if(CCharacter *pRegPredChar =
						m_RegularPredictedWorld.GetCharacterById(LocalId))
					FrozenNow = pRegPredChar->Core()->m_IsInFreeze ||
						    pRegPredChar->m_FreezeTime > 0;
				if(!FrozenNow && m_FentBotHasWaypoint)
				{
					const vec2 Goal =
						(m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint);
					const vec2 Vel = m_aClients[LocalId].m_RegularPredicted.m_Vel;
					int ActionId = -1;
					if(FentBotPyInferAction(DummyIndex, LocalPos, Vel, Goal, ActionId))
					{
						static const vec2 s_aAimCands5[] = {
							vec2(0.0f, -1.0f),
							normalize(vec2(-0.4f, -1.0f)),
							normalize(vec2(0.4f, -1.0f)),
							vec2(-1.0f, 0.0f),
							vec2(1.0f, 0.0f),
						};
						const int AimCount = (int)std::size(s_aAimCands5);
						const int AimIndex = ActionId % AimCount;
						int Tmp = ActionId / AimCount;
						const int HookBit = Tmp & 1;
						Tmp >>= 1;
						const int JumpBit = Tmp & 1;
						Tmp >>= 1;
						const int DirIndex = Tmp;
						const int Dir = std::clamp(DirIndex - 1, -1, 1);

						pInput->m_Direction = Dir;
						const vec2 OutAim = s_aAimCands5[AimIndex];
						pInput->m_TargetX = (int)round_to_int(OutAim.x * 1000.0f);
						pInput->m_TargetY = (int)round_to_int(OutAim.y * 1000.0f);
						if(!pInput->m_TargetX && !pInput->m_TargetY)
							pInput->m_TargetY = -1;
						pInput->m_Hook = HookBit ? 1 : 0;
						if(JumpBit)
							pInput->m_Jump = (pInput->m_Jump + 2) | 1;

						ZzFentBotOverridesInput = true;
						m_FentBotActive = true;
					}
				}
			}
			if(g_Config.m_ClZzFentBotEnabled && !SaveInBlockActive)
			{
				auto IsFreezeTile = [&](int Tile) {
					return Tile == TILE_FREEZE || Tile == TILE_DFREEZE ||
					       Tile == TILE_LFREEZE;
				};
				auto IsFreezeAt = [&](const vec2 &Pos) {
					const int PureIndex = Collision()->GetPureMapIndex(Pos);
					if(PureIndex < 0)
						return false;
					const int Tile = Collision()->GetTileIndex(PureIndex);
					const int FTile = Collision()->GetFrontTileIndex(PureIndex);
					return IsFreezeTile(Tile) || IsFreezeTile(FTile);
				};

				bool FrozenNow = Client()->m_IsLocalFrozen;
				if(CCharacter *pRegPredChar =
						m_RegularPredictedWorld.GetCharacterById(LocalId))
					FrozenNow = pRegPredChar->Core()->m_IsInFreeze ||
						    pRegPredChar->m_FreezeTime > 0;

				// Don't try to drive while already frozen.
				if(!FrozenNow)
				{
					const int NowTick = Client()->GameTick(DummyIndex);
					const int Horizon =
						std::clamp(g_Config.m_ClZzFentBotHorizonTicks, 8, 80);
					const float HookLen = (float)m_aTuning[DummyIndex].m_HookLength;
					static int s_aHookCommitTicks[NUM_DUMMIES] = {0, 0, 0};
					static vec2 s_aHookCommitDir[NUM_DUMMIES] = {
						vec2(0.0f, -1.0f), vec2(0.0f, -1.0f), vec2(0.0f, -1.0f)};

					if(g_Config.m_ClZzFentBotAimFollow)
					{
						static int s_LastFollowWpTick = -1000000;
						static vec2 s_LastFollowWp = vec2(0.0f, 0.0f);
						const int WpDebounceTicks = 5;
						const float WpMinMove = 24.0f;
						// Make the bot goal follow the view direction.
						const float AimDist = 2000.0f;
						const vec2 Desired = LocalPos + CurAim * AimDist;
						vec2 HitPos;
						vec2 Goal = Desired;
						if(Collision()->IntersectLine(LocalPos, Desired, &HitPos,
							   nullptr) != 0)
							Goal = HitPos;

						// Step back if the goal would be on freeze/death/solid.
						for(int Back = 0; Back < 16; Back++)
						{
							const int PureIndex = Collision()->GetPureMapIndex(Goal);
							if(PureIndex < 0)
								break;
							const int Tile = Collision()->GetTileIndex(PureIndex);
							const int FTile = Collision()->GetFrontTileIndex(PureIndex);
							const bool Bad = Tile == TILE_SOLID || Tile == TILE_NOHOOK ||
									 Tile == TILE_DEATH || IsFreezeTile(Tile) ||
									 FTile == TILE_SOLID || FTile == TILE_NOHOOK ||
									 FTile == TILE_DEATH || IsFreezeTile(FTile);
							if(!Bad)
								break;
							Goal -= CurAim * 16.0f;
						}

						const bool EnoughTimePassed =
							(NowTick - s_LastFollowWpTick) >= WpDebounceTicks;
						const bool MovedEnough =
							distance(s_LastFollowWp, Goal) >= WpMinMove;
						if(!m_FentBotHasWaypoint || (EnoughTimePassed && MovedEnough))
						{
							s_LastFollowWpTick = NowTick;
							s_LastFollowWp = Goal;
							m_FentBotWaypoint = Goal;
							m_FentBotHasWaypoint = true;
							m_FentBotPlanDirty = true;
							m_FentBotDistValid = false;
						}
					}

					if(m_FentBotHasWaypoint)
						FentBotUpdatePlan(LocalId, DummyIndex);

					// Hook commitment logic: if a hookable direction yields a clear
					// dist-field improvement, keep hooking that way for a few ticks to
					// avoid dithering and falling.
					vec2 WantedHookDir = vec2(0.0f, -1.0f);
					bool WantHookCommit = false;
					if(m_FentBotDistValid &&
						m_FentBotDistGoalIndex ==
							Collision()->GetPureMapIndex(m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint))
					{
						const int StartIdx = Collision()->GetPureMapIndex(LocalPos);
						int StartD =
							(StartIdx >= 0 && StartIdx < (int)m_vFentBotDist.size()) ? m_vFentBotDist[StartIdx] : -1;
						if(StartD >= 0)
						{
							int BestD = StartD;
							vec2 BestDir = vec2(0.0f, -1.0f);
							static const int HOOK_DIRS = 32;
							for(int i = 0; i < HOOK_DIRS; i++)
							{
								const float Ang = (2.0f * pi) * ((float)i / (float)HOOK_DIRS);
								const vec2 Dir = normalize(vec2(cosf(Ang), sinf(Ang)));
								vec2 HitPos;
								int TeleNr = 0;
								const int Hit = Collision()->IntersectLineTeleHook(
									LocalPos, LocalPos + Dir * HookLen, &HitPos, nullptr,
									&TeleNr);
								if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK)
									continue;
								const vec2 DestPos = HitPos - Dir * 32.0f;
								const int DestIdx = Collision()->GetPureMapIndex(DestPos);
								if(DestIdx < 0 || DestIdx >= (int)m_vFentBotDist.size())
									continue;
								const int D = m_vFentBotDist[DestIdx];
								if(D < 0)
									continue;
								if(D + 2 < BestD)
								{
									BestD = D;
									BestDir = Dir;
								}
							}
							if(BestD + 2 < StartD)
							{
								WantedHookDir = BestDir;
								WantHookCommit = true;
							}
						}
					}
					if(WantHookCommit)
					{
						s_aHookCommitTicks[DummyIndex] = 10;
						s_aHookCommitDir[DummyIndex] = WantedHookDir;
					}
					else if(s_aHookCommitTicks[DummyIndex] > 0)
					{
						s_aHookCommitTicks[DummyIndex]--;
					}

					int DesiredMove = 0;
					if(pInput->m_Direction != 0)
						DesiredMove = pInput->m_Direction;
					else if(g_Config.m_ClZzFentBotAimFollow)
					{
						if(CurAim.x > 0.15f)
							DesiredMove = 1;
						else if(CurAim.x < -0.15f)
							DesiredMove = -1;
					}

					struct SCandidate
					{
						int m_Direction;
						bool m_Jump;
						bool m_Hook;
						vec2 m_Aim;
					};
					static const vec2 s_aAimCands[] = {
						vec2(0.0f, -1.0f),
						normalize(vec2(-0.4f, -1.0f)),
						normalize(vec2(0.4f, -1.0f)),
						vec2(-1.0f, 0.0f),
						vec2(1.0f, 0.0f),
						vec2(0.0f, 1.0f),
						normalize(vec2(-0.35f, 1.0f)),
						normalize(vec2(0.35f, 1.0f)),
						normalize(vec2(-0.7f, 1.0f)),
						normalize(vec2(0.7f, 1.0f)),
					};

					SCandidate Best{};
					float BestScore = -1e18f;
					bool Found = false;

					for(int DirCand = -1; DirCand <= 1; DirCand++)
					{
						for(int JumpCand = 0; JumpCand <= 1; JumpCand++)
						{
							for(int HookCand = 0; HookCand <= 1; HookCand++)
							{
								for(const vec2 &AimCand : s_aAimCands)
								{
									SCandidate Cand;
									Cand.m_Direction = DirCand;
									Cand.m_Jump = JumpCand != 0;
									Cand.m_Hook = HookCand != 0;
									Cand.m_Aim = AimCand;

									CGameWorld SimWorld;
									SimWorld.CopyWorldClean(&m_RegularPredictedWorld);
									SimWorld.m_GameTick = NowTick;
									CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
									if(!pChar)
										continue;

									CNetObj_PlayerInput In = *pInput;
									In.m_Direction = Cand.m_Direction;
									vec2 UseAim = Cand.m_Aim;
									if(Cand.m_Hook && s_aHookCommitTicks[DummyIndex] > 0)
										UseAim = s_aHookCommitDir[DummyIndex];
									In.m_TargetX = (int)round_to_int(UseAim.x * 1000.0f);
									In.m_TargetY = (int)round_to_int(UseAim.y * 1000.0f);
									if(!In.m_TargetX && !In.m_TargetY)
										In.m_TargetY = -1;
									In.m_Hook =
										(Cand.m_Hook || s_aHookCommitTicks[DummyIndex] > 0) ? 1 : 0;

									if(m_FentBotBcInfer && m_FentBotAiModelLoaded)
									{
										const vec2 Goal = (m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint);
										const vec2 Vel =
											m_aClients[LocalId].m_RegularPredicted.m_Vel;
										auto EncodeTile = [&](int T) -> float {
											if(T == TILE_FREEZE || T == TILE_DFREEZE ||
												T == TILE_LFREEZE)
												return 2.0f / 3.0f;
											if(T == TILE_DEATH)
												return 1.0f;
											if(T == TILE_SOLID || T == TILE_NOHOOK)
												return 1.0f / 3.0f;
											return 0.0f;
										};
										std::array<float, FENTBOT_OBS_DIM> Obs;
										const vec2 dd = Goal - LocalPos;
										Obs[0] = dd.x / 1000.0f;
										Obs[1] = dd.y / 1000.0f;
										Obs[2] = Vel.x / 1000.0f;
										Obs[3] = Vel.y / 1000.0f;
										const int W = Collision()->GetWidth();
										const int H = Collision()->GetHeight();
										const int Center = Collision()->GetPureMapIndex(LocalPos);
										int cx = 0;
										int cy = 0;
										if(Center >= 0)
										{
											cx = Center % W;
											cy = Center / W;
										}
										int o = 4;
										for(int dy = -FENTBOT_GRID_R; dy <= FENTBOT_GRID_R; dy++)
										{
											for(int dx = -FENTBOT_GRID_R; dx <= FENTBOT_GRID_R;
												dx++)
											{
												const int tx =
													std::clamp(cx + dx, 0, std::max(0, W - 1));
												const int ty =
													std::clamp(cy + dy, 0, std::max(0, H - 1));
												const int Idx = ty * W + tx;
												Obs[o++] = EncodeTile(Collision()->GetTileIndex(Idx));
												Obs[o++] =
													EncodeTile(Collision()->GetFrontTileIndex(Idx));
											}
										}
										{
											struct SCand
											{
												float m_D2;
												int m_Id;
											};
											SCand aCands[MAX_CLIENTS];
											int Num = 0;
											const float Radius =
												(float)(FENTBOT_PLAYER_RADIUS_TILES * 32);
											const float R2 = Radius * Radius;
											for(int i = 0; i < MAX_CLIENTS; i++)
											{
												if(i == LocalId)
													continue;
												if(!m_Snap.m_aCharacters[i].m_Active)
													continue;
												const vec2 P = m_aClients[i].m_RegularPredicted.m_Pos;
												const vec2 d = P - LocalPos;
												const float d2 = dot(d, d);
												if(d2 > R2)
													continue;
												aCands[Num++] = {d2, i};
											}
											std::sort(aCands, aCands + Num,
												[](const SCand &A, const SCand &B) {
													return A.m_D2 < B.m_D2;
												});
											for(int k = 0; k < FENTBOT_PLAYERS_K; k++)
											{
												if(k < Num)
												{
													const int Id = aCands[k].m_Id;
													const vec2 P =
														m_aClients[Id].m_RegularPredicted.m_Pos;
													const vec2 V =
														m_aClients[Id].m_RegularPredicted.m_Vel;
													const vec2 d = P - LocalPos;
													Obs[o++] = d.x / 1000.0f;
													Obs[o++] = d.y / 1000.0f;
													Obs[o++] = V.x / 1000.0f;
													Obs[o++] = V.y / 1000.0f;
												}
												else
												{
													Obs[o++] = 0.0f;
													Obs[o++] = 0.0f;
													Obs[o++] = 0.0f;
													Obs[o++] = 0.0f;
												}
											}
										}

										float aZ[FENTBOT_HIDDEN];
										float aH[FENTBOT_HIDDEN];
										for(int i = 0; i < FENTBOT_HIDDEN; i++)
										{
											float v = m_aFentBotPpoB1[i];
											const int Base = i * FENTBOT_OBS_DIM;
											for(int j = 0; j < FENTBOT_OBS_DIM; j++)
												v += m_aFentBotPpoW1[Base + j] * Obs[j];
											aZ[i] = v;
											aH[i] = v > 0.0f ? v : 0.0f;
										}
										float aLogits[FENTBOT_ACTIONS];
										for(int a = 0; a < FENTBOT_ACTIONS; a++)
										{
											float q = m_aFentBotPpoBpi[a];
											const int Base = a * FENTBOT_HIDDEN;
											for(int i = 0; i < FENTBOT_HIDDEN; i++)
												q += m_aFentBotPpoWpi[Base + i] * aH[i];
											aLogits[a] = q;
										}

										int AimIndex = 0;
										for(int i = 0; i < (int)std::size(s_aAimCands); i++)
										{
											if(distance(Cand.m_Aim, s_aAimCands[i]) < 0.0001f)
											{
												AimIndex = i;
												break;
											}
										}
										const int DirIndex = Cand.m_Direction + 1;
										const int ActionId =
											(((DirIndex * 2 + (Cand.m_Jump ? 1 : 0)) * 2 +
												 (Cand.m_Hook ? 1 : 0)) *
												(int)std::size(s_aAimCands)) +
											AimIndex;
										const float Score = aLogits[ActionId];
										if(!Found || Score > BestScore)
										{
											Found = true;
											BestScore = Score;
											Best = Cand;
										}
										continue;
									}
									float Score = 0.0f;
									if(Cand.m_Jump)
										In.m_Jump = (In.m_Jump + 2) | 1;
									if(Cand.m_Hook)
									{
										vec2 HitPos;
										int TeleNr = 0;
										const int Hit = Collision()->IntersectLineTeleHook(
											LocalPos, LocalPos + Cand.m_Aim * HookLen, &HitPos,
											nullptr, &TeleNr);
										if(Hit == 0 || Hit == TILE_NOHOOK ||
											Hit == TILE_TELEINHOOK)
											Score -= 1000.0f;
										else
											Score += 0.25f;
									}
									if(Cand.m_Direction == 0)
										Score -= 0.01f;
									if(Cand.m_Jump)
										Score -= 0.02f;

									auto IsDeathAt = [&](const vec2 &Pos) -> bool {
										const float r = CCharacterCore::PhysicalSize() / 3.f;
										const vec2 aPts[4] = {
											vec2(Pos.x + r, Pos.y - r), vec2(Pos.x + r, Pos.y + r),
											vec2(Pos.x - r, Pos.y - r), vec2(Pos.x - r, Pos.y + r)};
										for(const vec2 &P : aPts)
										{
											if(Collision()->GetCollisionAt(P.x, P.y) == TILE_DEATH ||
												Collision()->GetFrontCollisionAt(P.x, P.y) ==
													TILE_DEATH)
												return true;
										}
										return false;
									};

									const vec2 Goal = (m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint);
									float StartDist = distance(LocalPos, Goal);
									bool HasDistField = false;
									if(m_FentBotDistValid &&
										m_FentBotDistW == Collision()->GetWidth() &&
										m_FentBotDistH == Collision()->GetHeight() &&
										m_FentBotDistGoalIndex ==
											Collision()->GetPureMapIndex(Goal))
									{
										const int StartIdx = Collision()->GetPureMapIndex(LocalPos);
										if(StartIdx >= 0 &&
											StartIdx < (int)m_vFentBotDist.size() &&
											m_vFentBotDist[StartIdx] >= 0)
										{
											StartDist = (float)m_vFentBotDist[StartIdx];
											HasDistField = true;
										}
									}
									bool Unsafe = false;
									float BestDist = StartDist;
									vec2 PrevPos = pChar->Core()->m_Pos;
									for(int t = 1; t <= Horizon; t++)
									{
										pChar->OnPredictedInput(&In);
										SimWorld.Tick();
										const vec2 P = pChar->Core()->m_Pos;
										const bool Grounded = pChar->IsGrounded();
										const bool FreezeBelow =
											Grounded && IsFreezeAt(P + vec2(0.0f, 16.0f));
										if(IsFreezeAt(P) || FreezeBelow || IsDeathAt(P))
										{
											Unsafe = true;
											break;
										}
										if(!Grounded && Cand.m_Hook)
											Score += 0.02f;
										if(HasDistField)
										{
											const int Idx = Collision()->GetPureMapIndex(P);
											if(Idx < 0 || Idx >= (int)m_vFentBotDist.size() ||
												m_vFentBotDist[Idx] < 0)
											{
												// Dist-field doesn't cover this area (e.g. early-stop
												// margin). Fallback to Euclidean for this candidate.
												HasDistField = false;
												BestDist = std::min(BestDist, distance(P, Goal));
											}
											else
												BestDist =
													std::min(BestDist, (float)m_vFentBotDist[Idx]);
										}
										else
										{
											BestDist = std::min(BestDist, distance(P, Goal));
										}
										if(t % 6 == 0 && distance(P, PrevPos) < 0.25f)
											Score -= 0.05f;
										PrevPos = P;
									}
									if(Unsafe)
										Score -= 1000000.0f;
									else
									{
										float EndDist = distance(pChar->Core()->m_Pos, Goal);
										if(HasDistField)
										{
											const int EndIdx =
												Collision()->GetPureMapIndex(pChar->Core()->m_Pos);
											if(EndIdx < 0 || EndIdx >= (int)m_vFentBotDist.size() ||
												m_vFentBotDist[EndIdx] < 0)
											{
												// Dist-field missing here: fallback to Euclidean for
												// this candidate.
												HasDistField = false;
											}
											else
												EndDist = (float)m_vFentBotDist[EndIdx];
										}
										if(Unsafe)
											Score -= 1000000.0f;
										else
										{
											Score += (StartDist - EndDist) * 0.65f;
											Score += (StartDist - BestDist) * 0.15f;
											Score -= EndDist * 0.01f;
										}
									}
									if(!Found || Score > BestScore)
									{
										Found = true;
										BestScore = Score;
										Best = Cand;
									}
								}
							}
						}
					}
					if(Found)
					{
						pInput->m_Direction = Best.m_Direction;
						vec2 OutAim = Best.m_Aim;
						if(s_aHookCommitTicks[DummyIndex] > 0)
							OutAim = s_aHookCommitDir[DummyIndex];
						pInput->m_TargetX = (int)round_to_int(OutAim.x * 1000.0f);
						pInput->m_TargetY = (int)round_to_int(OutAim.y * 1000.0f);
						if(!pInput->m_TargetX && !pInput->m_TargetY)
							pInput->m_TargetY = -1;
						pInput->m_Hook =
							(Best.m_Hook || s_aHookCommitTicks[DummyIndex] > 0) ? 1 : 0;
						if(Best.m_Jump)
							pInput->m_Jump = (pInput->m_Jump + 2) | 1;

						ZzFentBotOverridesInput = true;
						m_FentBotActive = true;

						if(g_Config.m_ClZzFentBotShowPath)
						{
							CGameWorld SimWorld;
							SimWorld.CopyWorldClean(&m_RegularPredictedWorld);
							SimWorld.m_GameTick = NowTick;
							CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
							if(pChar)
							{
								CNetObj_PlayerInput In = *pInput;
								if(In.m_TargetX == 0 && In.m_TargetY == 0)
									In.m_TargetY = -1;
								m_aFentBotPlannedPath[0] = pChar->Core()->m_Pos;
								m_FentBotPlannedPathLen = 1;
								for(int t = 1; t < FENTBOT_MAX_PATH_POINTS && t <= Horizon;
									t++)
								{
									pChar->OnPredictedInput(&In);
									SimWorld.Tick();
									m_aFentBotPlannedPath[t] = pChar->Core()->m_Pos;
									m_FentBotPlannedPathLen = t + 1;
								}
							}
						}
					}
				}
			}

			// KernelNet: tile aimbot (hold). A hookable tee is always tested first;
			// only if no tee can be reached do we acquire a wall point.
			static vec2 s_aLockedTilePos[NUM_DUMMIES] = {
				vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(0.0f, 0.0f)};
			static int s_aLockedTileIndex[NUM_DUMMIES] = {-1, -1, -1};
			static bool s_aHasLockedTile[NUM_DUMMIES] = {false, false, false};
			static bool s_aTileAimWasActive[NUM_DUMMIES] = {false, false, false};
			static int s_aTileAimReleaseTick[NUM_DUMMIES] = {-1000, -1000, -1000};
			static int s_aTileAimLaunchTick[NUM_DUMMIES] = {-1000, -1000, -1000};
			static bool s_aTileAimUsingTee[NUM_DUMMIES] = {false, false, false};
			static int s_aTileAimTeeTarget[NUM_DUMMIES] = {-1, -1, -1};
			if(SaveInBlockActive)
			{
				const float HookLen = (float)m_aTuning[DummyIndex].m_HookLength;
				const float TeeRadius = CCharacterCore::PhysicalSize();
				CNetObj_PlayerInput TeeInput = *pInput;
				TeeInput.m_Hook = 1;
				const vec2 TileUserMouse = m_Controls.m_aMousePos[DummyIndex];
				if(!ZzFentBotOverridesInput &&
					m_ZZHookAimbot.Apply(&TeeInput, DummyIndex, LocalPos, CurAim,
						false, true))
				{
					*pInput = TeeInput;
					const int TeeTarget = m_ZZHookAimbot.LockedTarget(DummyIndex);
					CCharacter *pLocalPred =
						m_RegularPredictedWorld.GetCharacterById(LocalId);
					const int HookState = pLocalPred ?
								      pLocalPred->Core()->m_HookState :
								      HOOK_IDLE;
					const int HookedPlayer = pLocalPred ?
									 pLocalPred->Core()->HookedPlayer() :
									 -1;
					const int NowTick = Client()->PredGameTick(DummyIndex);
					const bool SameTee = s_aTileAimUsingTee[DummyIndex] &&
							     s_aTileAimTeeTarget[DummyIndex] == TeeTarget;
					const bool AlreadyHookedToTee = HookState == HOOK_GRABBED &&
									HookedPlayer == TeeTarget;
					const bool SwitchingToTee = !SameTee &&
								    HookState != HOOK_IDLE && !AlreadyHookedToTee;
					const bool MissedTee = SameTee && HookState == HOOK_RETRACTED &&
							       NowTick > s_aTileAimLaunchTick[DummyIndex] + 2 &&
							       s_aTileAimReleaseTick[DummyIndex] < NowTick - 1;
					if((SwitchingToTee || MissedTee) &&
						s_aTileAimReleaseTick[DummyIndex] != NowTick)
						s_aTileAimReleaseTick[DummyIndex] = NowTick;
					const bool ReleaseForTee =
						s_aTileAimReleaseTick[DummyIndex] == NowTick;
					pInput->m_Hook = ReleaseForTee ? 0 : 1;
					if(!ReleaseForTee &&
						(s_aTileAimReleaseTick[DummyIndex] == NowTick - 1 ||
							(!SameTee && HookState == HOOK_IDLE)))
						s_aTileAimLaunchTick[DummyIndex] = NowTick;
					HookAimApplied = true;
					ZzAutoUnfreezeOverridesInput = true;
					s_aHasLockedTile[DummyIndex] = false;
					s_aLockedTileIndex[DummyIndex] = -1;
					s_aTileAimUsingTee[DummyIndex] = true;
					s_aTileAimTeeTarget[DummyIndex] = TeeTarget;
					s_aTileAimWasActive[DummyIndex] = true;
					// Tee priority is packet-only. Never leak the selected direction to
					// the visible cursor or to the persistent Controls input.
					m_Controls.m_aInputData[DummyIndex].m_TargetX = OrigInput.m_TargetX;
					m_Controls.m_aInputData[DummyIndex].m_TargetY = OrigInput.m_TargetY;
					m_Controls.m_aMousePos[DummyIndex] = TileUserMouse;
				}

				auto TraceHookableWall = [&](const vec2 &Direction, vec2 *pHitPos,
								 int *pHitIndex) {
					if(length(Direction) < 0.001f)
						return false;
					const vec2 Dir = normalize(Direction);
					const vec2 Start = LocalPos + Dir * TeeRadius * 1.5f;
					const vec2 End = LocalPos + Dir * HookLen;
					vec2 HitPos;
					int TeleNr = 0;
					const int Hit = Collision()->IntersectLineTeleHook(
						Start, End, &HitPos, nullptr, &TeleNr);
					if(Hit != TILE_SOLID ||
						TeleNr != 0 || distance(LocalPos, HitPos) > HookLen + 0.5f ||
						RayHitsAnyPlayer(Start, HitPos))
						return false;

					// IntersectLineTeleHook returns the first collision sample. Move a
					// little into the surface so rounding identifies the actual solid tile.
					const int HitIndex = Collision()->GetPureMapIndex(HitPos + Dir * 4.0f);
					if(HitIndex < 0 || Collision()->GetTileIndex(HitIndex) != TILE_SOLID)
						return false;
					if(pHitPos)
						*pHitPos = HitPos;
					if(pHitIndex)
						*pHitIndex = HitIndex;
					return true;
				};

				auto BuildCenteredWallTarget = [&](const vec2 &SearchDirection,
								       vec2 *pAimPos, vec2 *pHitPos, int *pHitIndex) {
					vec2 SearchHitPos;
					int SearchHitIndex = -1;
					if(!TraceHookableWall(SearchDirection, &SearchHitPos, &SearchHitIndex))
						return false;

					// Aim at the middle of the face actually hit by the search ray. Using
					// only the tee-to-center direction can select an internal face of a
					// continuous wall and incorrectly discard a reachable surface.
					const vec2 TileCenter = Collision()->GetPos(SearchHitIndex);
					vec2 FaceCenter = TileCenter;
					const float LeftDistance =
						fabsf(SearchHitPos.x - (TileCenter.x - 16.0f));
					const float RightDistance =
						fabsf(SearchHitPos.x - (TileCenter.x + 16.0f));
					const float TopDistance =
						fabsf(SearchHitPos.y - (TileCenter.y - 16.0f));
					const float BottomDistance =
						fabsf(SearchHitPos.y - (TileCenter.y + 16.0f));
					const float MinFaceDistance = minimum(
						minimum(LeftDistance, RightDistance),
						minimum(TopDistance, BottomDistance));
					if(MinFaceDistance == LeftDistance)
						FaceCenter.x -= 16.0f;
					else if(MinFaceDistance == RightDistance)
						FaceCenter.x += 16.0f;
					else if(MinFaceDistance == TopDistance)
						FaceCenter.y -= 16.0f;
					else
						FaceCenter.y += 16.0f;
					vec2 CenterHitPos;
					int CenterHitIndex = -1;
					vec2 AimPos = FaceCenter;
					if(!TraceHookableWall(FaceCenter - LocalPos, &CenterHitPos,
						   &CenterHitIndex) ||
						CenterHitIndex != SearchHitIndex)
					{
						// An exact face center can be shadowed by a corner or a through
						// tile. The original search ray is already verified hookable, so
						// retain it instead of losing the whole surface.
						AimPos = SearchHitPos;
						CenterHitPos = SearchHitPos;
						CenterHitIndex = SearchHitIndex;
					}

					if(pAimPos)
						*pAimPos = AimPos;
					if(pHitPos)
						*pHitPos = CenterHitPos;
					if(pHitIndex)
						*pHitIndex = CenterHitIndex;
					return true;
				};

				if(!HookAimApplied && s_aHasLockedTile[DummyIndex])
				{
					const int LockedIndex = s_aLockedTileIndex[DummyIndex];
					vec2 AimPos;
					vec2 HitPos;
					int HitIndex = -1;
					if(LockedIndex < 0 ||
						!BuildCenteredWallTarget(s_aLockedTilePos[DummyIndex] - LocalPos,
							&AimPos, &HitPos,
							&HitIndex) ||
						HitIndex != LockedIndex)
					{
						s_aHasLockedTile[DummyIndex] = false;
						s_aLockedTileIndex[DummyIndex] = -1;
					}
					else
						s_aLockedTilePos[DummyIndex] = AimPos;
				}

				if(!HookAimApplied && !s_aHasLockedTile[DummyIndex])
				{
					vec2 BestAimPos;
					float BestScore = 1e18f;
					int BestHitIndex = -1;
					bool Found = false;

					const float HalfFovRad =
						std::clamp((float)g_Config.m_ClZzAimbotFov, 1.0f, 180.0f) *
						(pi / 360.0f);
					const float AimAngle = angle(CurAim);
					auto ConsiderDirection = [&](const vec2 &SearchDirection,
									 int ExpectedTileIndex) {
						if(length(SearchDirection) < 0.001f)
							return;
						const vec2 SearchDir = normalize(SearchDirection);
						const float SearchAngle = acosf(std::clamp(dot(CurAim, SearchDir),
							-1.0f, 1.0f));
						if(SearchAngle > HalfFovRad + 0.001f)
							return;
						vec2 AimPos;
						vec2 HitPos;
						int HitIndex = -1;
						if(!BuildCenteredWallTarget(SearchDir, &AimPos, &HitPos, &HitIndex) ||
							(ExpectedTileIndex >= 0 && HitIndex != ExpectedTileIndex))
							return;
						const vec2 CenterDir = normalize(AimPos - LocalPos);
						const float CenterAngle = acosf(std::clamp(dot(CurAim, CenterDir),
							-1.0f, 1.0f));
						if(CenterAngle > HalfFovRad + 0.001f)
							return;
						const float Score = distance(LocalPos, HitPos) +
								    CenterAngle * 48.0f + distance(AimPos, HitPos) * 0.08f;
						if(Score < BestScore)
						{
							BestScore = Score;
							BestAimPos = AimPos;
							BestHitIndex = HitIndex;
							Found = true;
						}
					};

					// Enumerate the exposed faces, including three safe points on every
					// face. This cannot miss a narrow hookable block between angular rays.
					const int TileRadius = (int)ceilf(HookLen / 32.0f) + 1;
					const int LocalTileX = std::clamp(round_to_int(LocalPos.x) / 32,
						0, Collision()->GetWidth() - 1);
					const int LocalTileY = std::clamp(round_to_int(LocalPos.y) / 32,
						0, Collision()->GetHeight() - 1);
					const vec2 aFaceNormals[] = {
						vec2(-1.0f, 0.0f), vec2(1.0f, 0.0f),
						vec2(0.0f, -1.0f), vec2(0.0f, 1.0f)};
					for(int TileY = maximum(0, LocalTileY - TileRadius);
						TileY <= minimum(Collision()->GetHeight() - 1,
								 LocalTileY + TileRadius);
						++TileY)
					{
						for(int TileX = maximum(0, LocalTileX - TileRadius);
							TileX <= minimum(Collision()->GetWidth() - 1,
									 LocalTileX + TileRadius);
							++TileX)
						{
							const int TileIndex = TileY * Collision()->GetWidth() + TileX;
							if(Collision()->GetTileIndex(TileIndex) != TILE_SOLID)
								continue;
							const vec2 Center = Collision()->GetPos(TileIndex);
							if(distance(LocalPos, Center) > HookLen + 24.0f)
								continue;
							for(const vec2 &Normal : aFaceNormals)
							{
								if(Collision()->CheckPoint(Center + Normal * 32.0f))
									continue;
								const vec2 Tangent(-Normal.y, Normal.x);
								for(float Offset : {-7.0f, 0.0f, 7.0f})
									ConsiderDirection(Center + Normal * 16.0f +
												  Tangent * Offset - LocalPos,
										TileIndex);
							}
						}
					}

					// Fallback for unusual through/front-layer geometry.
					constexpr int Samples = 361;
					const float Step = (HalfFovRad * 2.0f) / (float)(Samples - 1);
					for(int i = 0; i < Samples; ++i)
						ConsiderDirection(direction(AimAngle - HalfFovRad +
									    Step * (float)i),
							-1);

					if(Found)
					{
						s_aLockedTilePos[DummyIndex] = BestAimPos;
						s_aLockedTileIndex[DummyIndex] = BestHitIndex;
						s_aHasLockedTile[DummyIndex] = true;
					}
				}

				if(!HookAimApplied && s_aHasLockedTile[DummyIndex])
				{
					const bool SwitchingFromTee = s_aTileAimUsingTee[DummyIndex];
					const vec2 Dir = normalize(s_aLockedTilePos[DummyIndex] - LocalPos);
					const bool Silent = g_Config.m_ClZzSaveInBlockSilent != 0;
					pInput->m_TargetX = (int)round_to_int(Dir.x * 1000.0f);
					pInput->m_TargetY = (int)round_to_int(Dir.y * 1000.0f);
					if(!pInput->m_TargetX && !pInput->m_TargetY)
						pInput->m_TargetY = 1;
					if(!Silent)
						m_Controls.m_aMousePos[DummyIndex] = Dir * 1000.0f;

					CCharacter *pLocalPred =
						m_RegularPredictedWorld.GetCharacterById(LocalId);
					const int HookState = pLocalPred ?
								      pLocalPred->Core()->m_HookState :
								      HOOK_IDLE;
					const int NowTick = Client()->PredGameTick(DummyIndex);
					const bool JustActivated = !s_aTileAimWasActive[DummyIndex];
					const bool WrongExistingHook = (JustActivated || SwitchingFromTee) &&
								       HookState != HOOK_IDLE;
					// HOOK_RETRACTED is -1, not part of the positive retract range.
					// Keep release for the whole predicted tick, then press once on the
					// following tick. A short launch grace prevents stale prediction from
					// producing an alternating release/press loop.
					const bool MissedHook = HookState == HOOK_RETRACTED &&
								NowTick > s_aTileAimLaunchTick[DummyIndex] + 2 &&
								s_aTileAimReleaseTick[DummyIndex] < NowTick - 1;
					if((WrongExistingHook || MissedHook) &&
						s_aTileAimReleaseTick[DummyIndex] != NowTick)
						s_aTileAimReleaseTick[DummyIndex] = NowTick;
					const bool ReleaseNow = s_aTileAimReleaseTick[DummyIndex] == NowTick;
					if(ReleaseNow)
						pInput->m_Hook = 0;
					else
					{
						pInput->m_Hook = 1;
						if(s_aTileAimReleaseTick[DummyIndex] == NowTick - 1 ||
							(JustActivated && HookState == HOOK_IDLE))
							s_aTileAimLaunchTick[DummyIndex] = NowTick;
					}
					if(Silent)
					{
						m_Controls.m_aInputData[DummyIndex].m_TargetX = OrigInput.m_TargetX;
						m_Controls.m_aInputData[DummyIndex].m_TargetY = OrigInput.m_TargetY;
					}
					ZzAutoUnfreezeOverridesInput = true;
					s_aTileAimWasActive[DummyIndex] = true;
					s_aTileAimUsingTee[DummyIndex] = false;
					s_aTileAimTeeTarget[DummyIndex] = -1;
				}
			}
			else
			{
				s_aHasLockedTile[DummyIndex] = false;
				s_aLockedTileIndex[DummyIndex] = -1;
				// On bind release: auto-release hook from tile aimbot (old convenient
				// behavior).
				if(s_aTileAimWasActive[DummyIndex])
				{
					pInput->m_Hook = 0;
					m_Controls.m_aInputData[DummyIndex].m_Hook = 0;
					m_Controls.m_aFastInput[DummyIndex].m_Hook = 0;
				}
				s_aTileAimWasActive[DummyIndex] = false;
				s_aTileAimReleaseTick[DummyIndex] = -1000;
				s_aTileAimLaunchTick[DummyIndex] = -1000;
				s_aTileAimUsingTee[DummyIndex] = false;
				s_aTileAimTeeTarget[DummyIndex] = -1;
			}

			static bool s_aAvoidAssistHookHeld[NUM_DUMMIES] = {};
			static int s_aAvoidAssistHookStartTick[NUM_DUMMIES] = {-1, -1, -1};
			static int s_aAvoidAssistTargetX[NUM_DUMMIES] = {};
			static int s_aAvoidAssistTargetY[NUM_DUMMIES] = {};
			if(!g_Config.m_ClZzAvoidEnabled || !g_Config.m_ClZzAvoidHookAssist)
			{
				s_aAvoidAssistHookHeld[DummyIndex] = false;
				s_aAvoidAssistHookStartTick[DummyIndex] = -1;
			}
			// Avoid is applied once at the very end of this input function, after
			// every aimbot/movement helper. Keep the compatibility block disabled
			// here so it cannot compete with that final override.
			const bool AvoidHandledAtFinalInput =
				g_Config.m_ClZzAvoidEnabled != 0;
			if(g_Config.m_ClZzAvoidEnabled && !ZzFentBotOverridesInput &&
				!AvoidHandledAtFinalInput)
			{
				auto IsTeleTile = [&](int Tile) {
					return Tile == TILE_TELEIN || Tile == TILE_TELEINEVIL ||
					       Tile == TILE_TELEINHOOK || Tile == TILE_TELEINWEAPON;
				};
				auto IsFreezeTile = [&](int Tile) {
					return Tile == TILE_FREEZE || Tile == TILE_DFREEZE ||
					       Tile == TILE_LFREEZE;
				};
				auto IsFreezeAt = [&](const vec2 &Pos) {
					const int PureIndex = Collision()->GetPureMapIndex(Pos);
					if(PureIndex < 0)
						return false;
					const int Tile = Collision()->GetTileIndex(PureIndex);
					const int FTile = Collision()->GetFrontTileIndex(PureIndex);
					return IsFreezeTile(Tile) || IsFreezeTile(FTile);
				};
				auto IsFreezeNearTee = [&](const vec2 &Pos) {
					const float Radius = CCharacterCore::PhysicalSize() * 0.65f;
					const float Diagonal = Radius * 0.70710678f;
					const vec2 aOffsets[] = {
						vec2(0.0f, 0.0f), vec2(Radius, 0.0f), vec2(-Radius, 0.0f),
						vec2(0.0f, Radius), vec2(0.0f, -Radius),
						vec2(Diagonal, Diagonal), vec2(Diagonal, -Diagonal),
						vec2(-Diagonal, Diagonal), vec2(-Diagonal, -Diagonal)};
					for(const vec2 &Offset : aOffsets)
					{
						if(IsFreezeAt(Pos + Offset))
							return true;
					}
					return false;
				};
				auto SegmentTouchesFreeze = [&](const vec2 &From, const vec2 &To) {
					const int Steps = std::clamp(
						(int)ceilf(distance(From, To) / 8.0f), 1, 16);
					for(int Step = 1; Step <= Steps; Step++)
					{
						if(IsFreezeNearTee(mix(From, To, (float)Step / (float)Steps)))
							return true;
					}
					return false;
				};
				auto IsHazardAt = [&](const vec2 &Pos) {
					if(g_Config.m_ClZzAvoidFreeze && IsFreezeNearTee(Pos))
						return true;
					const int PureIndex = Collision()->GetPureMapIndex(Pos);
					if(PureIndex < 0)
						return false;
					const int Tile = Collision()->GetTileIndex(PureIndex);
					const int FTile = Collision()->GetFrontTileIndex(PureIndex);
					if(g_Config.m_ClZzAvoidTele &&
						(IsTeleTile(Tile) || IsTeleTile(FTile)))
						return true;
					if(g_Config.m_ClZzAvoidKill)
					{
						const float r = CCharacterCore::PhysicalSize() / 3.f;
						const vec2 aPts[4] = {
							vec2(Pos.x + r, Pos.y - r), vec2(Pos.x + r, Pos.y + r),
							vec2(Pos.x - r, Pos.y - r), vec2(Pos.x - r, Pos.y + r)};
						for(const vec2 &P : aPts)
						{
							if(Collision()->GetCollisionAt(P.x, P.y) == TILE_DEATH ||
								Collision()->GetFrontCollisionAt(P.x, P.y) == TILE_DEATH)
								return true;
						}
					}
					return false;
				};

				const float Look =
					(float)std::clamp(g_Config.m_ClZzAvoidLookahead, 4, 128);
				const float FreezeLook = minimum(Look, 32.0f);
				const vec2 Vel = m_aClients[LocalId].m_RegularPredicted.m_Vel;
				const float AbsVelX = fabsf(Vel.x);
				const int AvoidTick = Client()->PredGameTick(DummyIndex);
				const int CurrentHookState =
					m_aClients[LocalId].m_RegularPredicted.m_HookState;
				if(s_aAvoidAssistHookHeld[DummyIndex])
				{
					const int HeldTicks = maximum(
						0, AvoidTick - s_aAvoidAssistHookStartTick[DummyIndex]);
					const bool PullingAwayFromDanger =
						CurrentHookState == HOOK_GRABBED && HeldTicks >= 4 &&
						Vel.y < -0.65f;
					const bool HookFailed = HeldTicks >= 3 &&
								(CurrentHookState == HOOK_RETRACTED ||
									(CurrentHookState >= HOOK_RETRACT_START &&
										CurrentHookState <= HOOK_RETRACT_END));
					if(PullingAwayFromDanger || HookFailed || HeldTicks >= 24)
					{
						s_aAvoidAssistHookHeld[DummyIndex] = false;
						s_aAvoidAssistHookStartTick[DummyIndex] = -1;
						pInput->m_Hook = 0;
						m_Controls.m_aFastInput[DummyIndex].m_Hook = 0;
						ZzAvoidOverridesInput = true;
					}
					else
					{
						// Keep the generated hook edge alive across input packets. Do not
						// touch m_aMousePos/m_aInputData targets, so the visible cursor
						// remains completely user-controlled.
						pInput->m_Hook = 1;
						pInput->m_TargetX = s_aAvoidAssistTargetX[DummyIndex];
						pInput->m_TargetY = s_aAvoidAssistTargetY[DummyIndex];
						m_Controls.m_aFastInput[DummyIndex].m_Hook = 1;
						m_Controls.m_aFastInput[DummyIndex].m_TargetX = pInput->m_TargetX;
						m_Controls.m_aFastInput[DummyIndex].m_TargetY = pInput->m_TargetY;
						ZzAvoidOverridesInput = true;
					}
				}
				const vec2 NextPos =
					LocalPos + vec2((float)pInput->m_Direction * FreezeLook, 0.0f);
				const vec2 NextVelPos =
					(AbsVelX > 0.5f) ? (LocalPos +
								   vec2((Vel.x > 0.0f ? 1.0f : -1.0f) * FreezeLook, 0.0f)) :
							   LocalPos;
				const bool NextIsHazard =
					((pInput->m_Direction != 0) && IsHazardAt(NextPos)) ||
					((AbsVelX > 0.5f) && IsHazardAt(NextVelPos));

				struct SAvoidPrediction
				{
					int m_HazardTick;
					vec2 m_EndPos;
					vec2 m_EndVel;
				};

				const bool HookActive =
					pInput->m_Hook != 0 ||
					m_aClients[LocalId].m_RegularPredicted.m_HookState == HOOK_FLYING ||
					m_aClients[LocalId].m_RegularPredicted.m_HookState == HOOK_GRABBED;
				const int HookPredictionBonus = g_Config.m_ClZzAvoidHookAssist ?
									8 + (HookActive ? 4 : 0) :
									0;
				const int PredictionTicks = std::clamp(
					6 + g_Config.m_ClZzAvoidLookahead / 6 +
						round_to_int(length(Vel) * 0.20f) + HookPredictionBonus,
					8, 40);
				auto PredictAvoidInput =
					[&](const CNetObj_PlayerInput &CandidateInput) {
						SAvoidPrediction Result{PredictionTicks + 1, LocalPos, Vel};
						CGameWorld SimWorld;
						SimWorld.CopyWorldClean(&m_RegularPredictedWorld);
						SimWorld.m_WorldConfig.m_PredictFreeze = 1;
						CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
						if(!pChar)
						{
							Result.m_HazardTick = 0;
							return Result;
						}

						CNetObj_PlayerInput In = CandidateInput;
						if(In.m_TargetX == 0 && In.m_TargetY == 0)
							In.m_TargetY = -1;
						vec2 PreviousPos = pChar->Core()->m_Pos;
						for(int Tick = 1; Tick <= PredictionTicks; Tick++)
						{
							pChar->OnPredictedInput(&In);
							SimWorld.Tick();
							pChar = SimWorld.GetCharacterById(LocalId);
							if(!pChar)
							{
								Result.m_HazardTick = Tick;
								break;
							}
							Result.m_EndPos = pChar->Core()->m_Pos;
							Result.m_EndVel = pChar->Core()->m_Vel;
							const bool EntersFreeze = g_Config.m_ClZzAvoidFreeze &&
										  (pChar->Core()->m_IsInFreeze ||
											  IsFreezeNearTee(Result.m_EndPos) ||
											  SegmentTouchesFreeze(PreviousPos, Result.m_EndPos));
							const bool EntersTele = g_Config.m_ClZzAvoidTele &&
										(IsTeleTile(pChar->m_TileIndex) ||
											IsTeleTile(pChar->m_TileFIndex));
							const bool EntersOtherHazard =
								EntersTele || IsHazardAt(Result.m_EndPos);
							if(EntersFreeze || EntersOtherHazard)
							{
								Result.m_HazardTick = Tick;
								break;
							}
							PreviousPos = Result.m_EndPos;
						}
						return Result;
					};

				const CNetObj_PlayerInput BaselineInput = *pInput;
				const SAvoidPrediction Baseline = PredictAvoidInput(BaselineInput);
				// Look far enough to evaluate alternatives, but only take control when
				// the danger is close enough that input latency and current momentum
				// leave very little manual reaction time. The old vertical scan started
				// hook assist up to eight tiles early.
				const int InputLeadTicks = std::clamp(
					(Client()->GetPredictionTime() * Client()->GameTickSpeed() + 999) /
						1000,
					0, 4);
				const int MomentumTicks = std::clamp(
					round_to_int(length(Vel) * 0.12f), 0, 3);
				const int UrgencyTicks = std::clamp(
					3 + InputLeadTicks + MomentumTicks, 3, 9);
				if(Baseline.m_HazardTick <= minimum(PredictionTicks, UrgencyTicks))
				{
					CNetObj_PlayerInput BestInput = BaselineInput;
					SAvoidPrediction BestPrediction = Baseline;
					float BestCost = 0.0f;
					const int CurrentDirection =
						std::clamp(BaselineInput.m_Direction, -1, 1);
					const int BrakeDirection =
						Vel.x > 0.25f ? -1 : (Vel.x < -0.25f ? 1 : 0);
					const int aDirections[] = {CurrentDirection, 0, BrakeDirection,
						-CurrentDirection, -1, 1};
					const bool CanReleaseHook =
						g_Config.m_ClZzAvoidHookAssist && HookActive;
					const bool CanStartHook =
						g_Config.m_ClZzAvoidHookAssist && !HookActive;
					const bool CanInjectJump =
						g_Config.m_ClZzAvoidJump && (BaselineInput.m_Jump & 1) == 0;
					struct SAvoidHookVariant
					{
						int m_Hook;
						int m_TargetX;
						int m_TargetY;
						float m_Cost;
					};
					std::array<SAvoidHookVariant, 32> aHookVariants{};
					int NumHookVariants = 0;
					aHookVariants[NumHookVariants++] = {
						BaselineInput.m_Hook, BaselineInput.m_TargetX,
						BaselineInput.m_TargetY, 0.0f};
					if(CanReleaseHook)
					{
						aHookVariants[NumHookVariants++] = {
							0, BaselineInput.m_TargetX, BaselineInput.m_TargetY, 1.0f};
					}
					if(CanStartHook)
					{
						const float HookScanRange = minimum(
							(float)m_aTuning[DummyIndex].m_HookLength,
							(float)std::clamp(maximum(
										  g_Config.m_ClZzAvoidHookAssistRange, 256),
								16, 512));
						const vec2 OppositeVelocity = length(Vel) > 0.1f ?
										      normalize(-Vel) :
										      vec2(0.0f, -1.0f);
						// A dense upper hemisphere finds ceilings and side walls that the
						// old seven fixed rays routinely skipped between tiles.
						for(int Ray = 0; Ray <= 16; ++Ray)
						{
							const float RayAngle = -pi + (float)Ray * pi / 16.0f;
							const vec2 HookDirection = direction(RayAngle);
							vec2 HitPos;
							int TeleNumber = 0;
							const int Hit = Collision()->IntersectLineTeleHook(
								LocalPos, LocalPos + HookDirection * HookScanRange,
								&HitPos, nullptr, &TeleNumber);
							if(Hit == 0 || Hit == TILE_NOHOOK ||
								Hit == TILE_TELEINHOOK || IsHazardAt(HitPos))
								continue;
							const float RescueAlignment =
								dot(HookDirection, OppositeVelocity);
							aHookVariants[NumHookVariants++] = {
								1, round_to_int(HookDirection.x * 1000.0f),
								round_to_int(HookDirection.y * 1000.0f),
								2.1f - RescueAlignment * 0.9f};
						}
					}

					for(int DirectionIndex = 0;
						DirectionIndex < (int)std::size(aDirections); DirectionIndex++)
					{
						const int CandidateDirection = aDirections[DirectionIndex];
						bool DuplicateDirection = false;
						for(int Previous = 0; Previous < DirectionIndex; Previous++)
							DuplicateDirection |= aDirections[Previous] == CandidateDirection;
						if(DuplicateDirection)
							continue;

						const int JumpVariants = CanInjectJump ? 2 : 1;
						for(int HookVariant = 0; HookVariant < NumHookVariants;
							HookVariant++)
						{
							for(int JumpVariant = 0; JumpVariant < JumpVariants;
								JumpVariant++)
							{
								CNetObj_PlayerInput Candidate = BaselineInput;
								Candidate.m_Direction = CandidateDirection;
								Candidate.m_Hook = aHookVariants[HookVariant].m_Hook;
								Candidate.m_TargetX =
									aHookVariants[HookVariant].m_TargetX;
								Candidate.m_TargetY =
									aHookVariants[HookVariant].m_TargetY;
								if(JumpVariant == 1)
									Candidate.m_Jump =
										((Candidate.m_Jump + 2) | 1) & INPUT_STATE_MASK;

								const SAvoidPrediction Prediction =
									PredictAvoidInput(Candidate);
								float Cost =
									(float)abs(CandidateDirection - CurrentDirection) * 2.0f;
								if(CandidateDirection == 0 && CurrentDirection != 0)
									Cost -= 0.5f;
								Cost += aHookVariants[HookVariant].m_Cost;
								if(JumpVariant == 1)
									Cost += 1.5f;
								Cost += fabsf(Prediction.m_EndVel.x) * 0.02f;
								if(Prediction.m_HazardTick > BestPrediction.m_HazardTick ||
									(Prediction.m_HazardTick == BestPrediction.m_HazardTick &&
										Prediction.m_HazardTick > Baseline.m_HazardTick &&
										Cost < BestCost))
								{
									BestInput = Candidate;
									BestPrediction = Prediction;
									BestCost = Cost;
								}
							}
						}
					}

					if(BestPrediction.m_HazardTick > Baseline.m_HazardTick)
					{
						pInput->m_Direction = BestInput.m_Direction;
						pInput->m_Hook = BestInput.m_Hook;
						pInput->m_Jump = BestInput.m_Jump;
						pInput->m_TargetX = BestInput.m_TargetX;
						pInput->m_TargetY = BestInput.m_TargetY;
						m_Controls.m_aFastInput[DummyIndex].m_Direction =
							BestInput.m_Direction;
						m_Controls.m_aFastInput[DummyIndex].m_Hook = BestInput.m_Hook;
						m_Controls.m_aFastInput[DummyIndex].m_Jump = BestInput.m_Jump;
						m_Controls.m_aFastInput[DummyIndex].m_TargetX =
							BestInput.m_TargetX;
						m_Controls.m_aFastInput[DummyIndex].m_TargetY =
							BestInput.m_TargetY;
						if(g_Config.m_ClZzAvoidHookAssist && BestInput.m_Hook &&
							!HookActive)
						{
							s_aAvoidAssistHookHeld[DummyIndex] = true;
							s_aAvoidAssistHookStartTick[DummyIndex] = AvoidTick;
							s_aAvoidAssistTargetX[DummyIndex] = BestInput.m_TargetX;
							s_aAvoidAssistTargetY[DummyIndex] = BestInput.m_TargetY;
						}
						else if(!BestInput.m_Hook && s_aAvoidAssistHookHeld[DummyIndex])
						{
							s_aAvoidAssistHookHeld[DummyIndex] = false;
							s_aAvoidAssistHookStartTick[DummyIndex] = -1;
						}
						ZzAvoidOverridesInput = true;
					}
				}

				// Generic avoid (tele/kill/etc): keep old behavior.
				if(!ZzAvoidOverridesInput && NextIsHazard &&
					!(g_Config.m_ClZzAvoidFreeze &&
						(((pInput->m_Direction != 0) && IsFreezeAt(NextPos)) ||
							((length(Vel) > 0.5f) && IsFreezeAt(NextVelPos)))))
				{
					pInput->m_Direction = 0;
					m_Controls.m_aFastInput[DummyIndex].m_Direction = 0;
					ZzAvoidOverridesInput = true;
				}
			}

			// Independent jump-based avoid-freeze (doesn't require Avoid enabled).
			if(g_Config.m_ClZzAvoidJump && !g_Config.m_ClZzAvoidEnabled &&
				!ZzFentBotOverridesInput)
			{
				auto IsFreezeTile = [&](int Tile) {
					return Tile == TILE_FREEZE || Tile == TILE_DFREEZE ||
					       Tile == TILE_LFREEZE;
				};
				auto IsFreezeAt = [&](const vec2 &Pos) {
					const int PureIndex = Collision()->GetPureMapIndex(Pos);
					if(PureIndex < 0)
						return false;
					const int Tile = Collision()->GetTileIndex(PureIndex);
					const int FTile = Collision()->GetFrontTileIndex(PureIndex);
					return IsFreezeTile(Tile) || IsFreezeTile(FTile);
				};

				const vec2 Vel = m_aClients[LocalId].m_RegularPredicted.m_Vel;
				// Only try to auto-jump when actually falling fast enough.
				if(Vel.y > 1.0f && (pInput->m_Jump & 1) == 0)
				{
					const int NowTick = Client()->GameTick(g_Config.m_ClDummy);
					CGameWorld SimWorld;
					SimWorld.CopyWorldClean(&m_RegularPredictedWorld);
					SimWorld.m_GameTick = NowTick;
					CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
					if(pChar)
					{
						CNetObj_PlayerInput In = *pInput;
						// Ensure non-zero aim.
						if(In.m_TargetX == 0 && In.m_TargetY == 0)
							In.m_TargetY = -1;
						bool WillEnterFreeze = false;
						vec2 PrevPos = pChar->Core()->m_Pos;
						for(int t = 1; t <= 12; t++)
						{
							pChar->OnPredictedInput(&In);
							SimWorld.Tick();
							const vec2 P = pChar->Core()->m_Pos;
							if(P.y > PrevPos.y + 0.5f && IsFreezeAt(P))
							{
								WillEnterFreeze = true;
								break;
							}
							PrevPos = P;
						}
						if(WillEnterFreeze)
							pInput->m_Jump = (pInput->m_Jump + 2) | 1;
					}
				}
			}

			// Keep the local tee centered over a collidable player below it. Only the
			// horizontal direction is assisted; every other input remains user-owned.
			{
				static int s_aBalanceTarget[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aBalanceDirection[NUM_DUMMIES] = {0, 0, 0};
				static int s_aBalanceDecisionTick[NUM_DUMMIES] = {-1, -1, -1};
				static float s_aBalanceTargetVelX[NUM_DUMMIES] = {0.0f, 0.0f, 0.0f};
				static float s_aBalanceTargetAccelX[NUM_DUMMIES] = {0.0f, 0.0f, 0.0f};
				static int s_aBalanceFire[NUM_DUMMIES] = {0, 0, 0};
				static int s_aBalanceFireTick[NUM_DUMMIES] = {-1, -1, -1};
				static bool s_aBalanceFireHeld[NUM_DUMMIES] = {false, false, false};
				int &BalanceTarget = s_aBalanceTarget[DummyIndex];
				int &BalanceDirection = s_aBalanceDirection[DummyIndex];
				const int UnassistedDirection = pInput->m_Direction;
				if(!g_Config.m_ClZzBalanceBot || ZzFentBotOverridesInput ||
					ZzAvoidOverridesInput)
				{
					BalanceTarget = -1;
					BalanceDirection = 0;
					s_aBalanceDecisionTick[DummyIndex] = -1;
					s_aBalanceTargetAccelX[DummyIndex] = 0.0f;
					s_aBalanceFire[DummyIndex] = pInput->m_Fire;
					s_aBalanceFireTick[DummyIndex] = -1;
					s_aBalanceFireHeld[DummyIndex] = false;
					m_Controls.m_aFastInput[DummyIndex].m_Direction = UnassistedDirection;
				}
				else
				{
					CCharacter *pBalanceChar =
						m_RegularPredictedWorld.GetCharacterById(LocalId);
					const float TeeSize = CCharacterCore::PhysicalSize();
					auto IsSupportCandidate = [&](int ClientId, bool Retain) {
						if(ClientId < 0 || ClientId >= MAX_CLIENTS ||
							ClientId == LocalId || !m_Snap.m_aCharacters[ClientId].m_Active)
							return false;
						if(pBalanceChar && !pBalanceChar->CanCollide(ClientId))
							return false;
						const vec2 Delta =
							m_aClients[ClientId].m_RegularPredicted.m_Pos - LocalPos;
						const float MaxHorizontal = TeeSize * (Retain ? 1.75f : 1.35f);
						const float MinVertical = TeeSize * (Retain ? 0.35f : 0.50f);
						const float MaxVertical = TeeSize * (Retain ? 1.90f : 1.65f);
						return Delta.y >= MinVertical && Delta.y <= MaxVertical &&
						       fabsf(Delta.x) <= MaxHorizontal;
					};

					const int PreviousTarget = BalanceTarget;
					if(!IsSupportCandidate(BalanceTarget, true))
						BalanceTarget = -1;
					if(BalanceTarget < 0 &&
						m_aClients[LocalId].m_RegularPredicted.m_Vel.y >= -1.5f)
					{
						float BestScore = std::numeric_limits<float>::infinity();
						for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
						{
							if(!IsSupportCandidate(ClientId, false))
								continue;
							const vec2 Delta =
								m_aClients[ClientId].m_RegularPredicted.m_Pos - LocalPos;
							const float Score =
								fabsf(Delta.x) + fabsf(Delta.y - TeeSize) * 0.45f;
							if(Score < BestScore)
							{
								BestScore = Score;
								BalanceTarget = ClientId;
							}
						}
					}

					if(BalanceTarget < 0)
					{
						BalanceDirection = 0;
						s_aBalanceDecisionTick[DummyIndex] = -1;
						pInput->m_Direction = UnassistedDirection;
						m_Controls.m_aFastInput[DummyIndex].m_Direction =
							UnassistedDirection;
					}
					else
					{
						const vec2 TargetPos =
							m_aClients[BalanceTarget].m_RegularPredicted.m_Pos;
						const vec2 TargetVel =
							m_aClients[BalanceTarget].m_RegularPredicted.m_Vel;
						const vec2 LocalVel = m_aClients[LocalId].m_RegularPredicted.m_Vel;
						const float CenterError = LocalPos.x - TargetPos.x;
						const float RelativeVelX = LocalVel.x - TargetVel.x;
						const int DecisionTick = Client()->PredGameTick(DummyIndex);
						if(BalanceTarget != PreviousTarget)
						{
							BalanceDirection = 0;
							s_aBalanceDecisionTick[DummyIndex] = -1;
							s_aBalanceTargetVelX[DummyIndex] = TargetVel.x;
							s_aBalanceTargetAccelX[DummyIndex] = 0.0f;
						}
						if(s_aBalanceDecisionTick[DummyIndex] != DecisionTick)
						{
							s_aBalanceDecisionTick[DummyIndex] = DecisionTick;
							const float AirMaxSpeed = maximum(
								0.01f, (float)m_aTuning[DummyIndex].m_AirControlSpeed);
							const float AirAccel = maximum(
								0.01f, (float)m_aTuning[DummyIndex].m_AirControlAccel);
							const float AirFriction = std::clamp(
								(float)m_aTuning[DummyIndex].m_AirFriction, 0.0f, 1.0f);
							const float ObservedTargetAccel =
								std::clamp(TargetVel.x - s_aBalanceTargetVelX[DummyIndex],
									-AirAccel * 1.5f, AirAccel * 1.5f);
							s_aBalanceTargetAccelX[DummyIndex] =
								mix(s_aBalanceTargetAccelX[DummyIndex], ObservedTargetAccel,
									0.55f);
							s_aBalanceTargetVelX[DummyIndex] = TargetVel.x;

							const int PredictionHorizon = std::clamp(
								5 + round_to_int(fabsf(RelativeVelX) * 0.65f), 5, 11);
							float BestScore = std::numeric_limits<float>::infinity();
							int BestDirection = 0;
							for(int CandidateDirection = -1; CandidateDirection <= 1;
								CandidateDirection++)
							{
								float SimLocalX = LocalPos.x;
								float SimTargetX = TargetPos.x;
								float SimLocalVelX = LocalVel.x;
								float SimTargetVelX = TargetVel.x;
								float SimTargetAccelX = s_aBalanceTargetAccelX[DummyIndex];
								float Score =
									CandidateDirection == BalanceDirection ? 0.0f : 1.2f;
								for(int Tick = 1; Tick <= PredictionHorizon; Tick++)
								{
									if(CandidateDirection < 0)
										SimLocalVelX = SaturatedAdd(-AirMaxSpeed, AirMaxSpeed,
											SimLocalVelX, -AirAccel);
									else if(CandidateDirection > 0)
										SimLocalVelX = SaturatedAdd(-AirMaxSpeed, AirMaxSpeed,
											SimLocalVelX, AirAccel);
									else
										SimLocalVelX *= AirFriction;

									SimTargetVelX += SimTargetAccelX;
									SimTargetAccelX *= 0.72f;
									SimLocalX += SimLocalVelX;
									SimTargetX += SimTargetVelX;
									const float Error = SimLocalX - SimTargetX;
									const float SimRelativeVel = SimLocalVelX - SimTargetVelX;
									const float TickWeight = 1.0f + (float)Tick * 0.28f;
									Score += Error * Error * TickWeight +
										 SimRelativeVel * SimRelativeVel * 3.5f;
									const float EdgeOverflow =
										maximum(0.0f, fabsf(Error) - TeeSize * 0.42f);
									Score += EdgeOverflow * EdgeOverflow * 90.0f;
								}
								const float TerminalError = SimLocalX - SimTargetX;
								Score += TerminalError * TerminalError * 5.0f;
								if(Score < BestScore)
								{
									BestScore = Score;
									BestDirection = CandidateDirection;
								}
							}

							// Near a stable center, prefer neutral input over tiny
							// alternating corrections.
							if(fabsf(CenterError) < 0.8f && fabsf(RelativeVelX) < 0.22f)
								BestDirection = 0;
							BalanceDirection = BestDirection;
						}
						pInput->m_Direction = BalanceDirection;
						m_Controls.m_aFastInput[DummyIndex].m_Direction = BalanceDirection;
					}

					const bool UserHoldingFire = (OrigInput.m_Fire & 1) != 0;
					const int ActiveWeapon = pBalanceChar ? pBalanceChar->GetActiveWeapon() : m_Snap.m_pLocalCharacter->m_Weapon;
					const bool GunNeedsRepeat =
						ActiveWeapon == WEAPON_GUN &&
						(!pBalanceChar || !pBalanceChar->Core()->m_Jetpack);
					const bool RepeatableWeapon =
						ActiveWeapon == WEAPON_HAMMER || GunNeedsRepeat;
					const bool AmmoAvailable = ActiveWeapon == WEAPON_HAMMER ||
								   m_Snap.m_pLocalCharacter->m_AmmoCount != 0;
					const bool WeaponReady =
						!pBalanceChar || pBalanceChar->GetReloadTimer() <= 0;
					const int FireTick = Client()->PredGameTick(DummyIndex);
					if(BalanceTarget >= 0 && UserHoldingFire && RepeatableWeapon &&
						AmmoAvailable)
					{
						if(!s_aBalanceFireHeld[DummyIndex])
						{
							s_aBalanceFire[DummyIndex] = pInput->m_Fire;
							s_aBalanceFireTick[DummyIndex] = FireTick;
							s_aBalanceFireHeld[DummyIndex] = true;
						}
						else if(WeaponReady &&
							s_aBalanceFireTick[DummyIndex] != FireTick)
						{
							s_aBalanceFire[DummyIndex] =
								((s_aBalanceFire[DummyIndex] + 2) | 1) & INPUT_STATE_MASK;
							s_aBalanceFireTick[DummyIndex] = FireTick;
						}
						pInput->m_Fire = s_aBalanceFire[DummyIndex];
						m_Controls.m_aFastInput[DummyIndex].m_Fire = pInput->m_Fire;
					}
					else
					{
						s_aBalanceFire[DummyIndex] = pInput->m_Fire;
						s_aBalanceFireTick[DummyIndex] = -1;
						s_aBalanceFireHeld[DummyIndex] = false;
						m_Controls.m_aFastInput[DummyIndex].m_Fire = pInput->m_Fire;
					}
				}
			}

			// KernelNet: manual auto-shotgun bounce-to-self. The predictive
			// expensive candidate search unless manual auto-shotgun is enabled.
			if(g_Config.m_ClZzAutoShotgun)
			{
				constexpr int AutoUnfreezeMaxPredictTicks = 96;
				constexpr int AutoUnfreezeTriggerTicks = 40;
				constexpr int AutoUnfreezeDebugMaxCandidates = 1024;
				vec2 aAutoUnfreezeDebugCandidateDir[AutoUnfreezeDebugMaxCandidates];
				int aAutoUnfreezeDebugCandidateDelay[AutoUnfreezeDebugMaxCandidates] =
					{};
				float
					aAutoUnfreezeDebugCandidateQuality[AutoUnfreezeDebugMaxCandidates] =
						{};
				int AutoUnfreezeDebugCandidateCount = 0;
				int AutoUnfreezeDebugExactTested = 0;
				int AutoUnfreezeDebugExactAccepted = -1;
				int AutoUnfreezeDebugPhase0HitDelay = -1;
				int AutoUnfreezeDebugPhase1HitDelay = -1;
				int AutoUnfreezeDebugPhase2HitDelay = -1;
				int aAutoUnfreezeDebugPredictionLeads[3] = {-1, -1, -1};
				int AutoUnfreezeDebugPredictionLeadCount = 0;
				static int s_aPrevUserFire[NUM_DUMMIES] = {0, 0, 0};
				static bool s_aAutoUnfreezeReleasePending[NUM_DUMMIES] = {false, false,
					false};
				static int s_aAutoUnfreezeInjectedFire[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezeInjectedTick[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezeAnalysisTick[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezePredictionTicks[NUM_DUMMIES] = {0, 0, 0};
				static bool s_aAutoUnfreezePathValid[NUM_DUMMIES] = {false, false,
					false};
				static bool s_aAutoUnfreezeNearFreeze[NUM_DUMMIES] = {false, false,
					false};
				static bool s_aAutoUnfreezeWillFreezeSoon[NUM_DUMMIES] = {false, false,
					false};
				static int s_aAutoUnfreezeFirstFreezeDelay[NUM_DUMMIES] = {
					AutoUnfreezeMaxPredictTicks, AutoUnfreezeMaxPredictTicks,
					AutoUnfreezeMaxPredictTicks};
				static vec2 s_aaAutoUnfreezePredPos[NUM_DUMMIES]
								   [AutoUnfreezeMaxPredictTicks];
				static bool
					s_aaAutoUnfreezePredCanUnfreeze[NUM_DUMMIES]
								       [AutoUnfreezeMaxPredictTicks] = {};
				static vec2 s_aaaAutoUnfreezePredPlayerPos[NUM_DUMMIES]
									  [AutoUnfreezeMaxPredictTicks]
									  [MAX_CLIENTS];
				static bool s_aaaAutoUnfreezePredPlayerActive
					[NUM_DUMMIES][AutoUnfreezeMaxPredictTicks][MAX_CLIENTS] = {};
				static int s_aAutoUnfreezePingSampleTick[NUM_DUMMIES] = {-1, -1, -1};
				static float s_aAutoUnfreezeSmoothedPingTicks[NUM_DUMMIES] = {
					0.0f, 0.0f, 0.0f};
				static int s_aAutoUnfreezeSolutionValidatedTick[NUM_DUMMIES] = {-1, -1,
					-1};
				static bool s_aAutoUnfreezeSolutionFound[NUM_DUMMIES] = {false, false,
					false};
				static int s_aAutoUnfreezeSolutionHitDelay[NUM_DUMMIES] = {0, 0, 0};
				static int s_aAutoUnfreezeSolutionBounces[NUM_DUMMIES] = {0, 0, 0};
				static vec2 s_aAutoUnfreezeSolutionDir[NUM_DUMMIES] = {
					vec2(0.0f, -1.0f), vec2(0.0f, -1.0f), vec2(0.0f, -1.0f)};
				static bool s_aAutoUnfreezeLaserInFlight[NUM_DUMMIES] = {false, false,
					false};
				static int s_aAutoUnfreezeLaserShotTick[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezeLaserExpectedHitTick[NUM_DUMMIES] = {-1, -1,
					-1};
				static int s_aAutoUnfreezeLaserExpireTick[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezeAttackTickAtShot[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aAutoUnfreezeConfirmDeadlineTick[NUM_DUMMIES] = {-1, -1,
					-1};
				static bool s_aAutoUnfreezeLaserConfirmed[NUM_DUMMIES] = {false, false,
					false};
				static vec2 s_aAutoUnfreezeLaserDir[NUM_DUMMIES] = {
					vec2(0.0f, -1.0f), vec2(0.0f, -1.0f), vec2(0.0f, -1.0f)};
				static int s_aAutoUnfreezeEstimatedServerShotTick[NUM_DUMMIES] = {
					-1, -1, -1};
				static int s_aAutoUnfreezePredictionLead[NUM_DUMMIES] = {0, 0, 0};

				const int UserPresses =
					CountInput(s_aPrevUserFire[DummyIndex], OrigInput.m_Fire).m_Presses;
				s_aPrevUserFire[DummyIndex] = OrigInput.m_Fire;
				const bool ManualAutoShotgun =
					g_Config.m_ClZzAutoShotgun && UserPresses > 0;

				CCharacter *pLocalPredChar =
					m_RegularPredictedWorld.GetCharacterById(LocalId);
				// The regular predicted world can already be inside freeze while the
				// outgoing input is still able to reach the server before that tick.
				// Only the last authoritative snapshot may suppress a new shot.
				const bool FrozenNow =
					m_aClients[LocalId].m_FreezeEnd > Client()->GameTick(DummyIndex);

				const bool HardFrozen =
					(pLocalPredChar && pLocalPredChar->Core()->m_DeepFrozen) ||
					m_aClients[LocalId].m_DeepFrozen ||
					m_aClients[LocalId].m_LiveFrozen;
				vec2 *pAutoUnfreezePredPos = s_aaAutoUnfreezePredPos[DummyIndex];
				bool *pAutoUnfreezePredCanUnfreeze =
					s_aaAutoUnfreezePredCanUnfreeze[DummyIndex];
				const int MaxBounces =
					std::clamp((int)m_aTuning[DummyIndex].m_LaserBounceNum, 0, 32);
				const double DelayMs = (double)m_aTuning[DummyIndex].m_LaserBounceDelay;
				const int DelayTicks =
					(int)floor((double)SERVER_TICK_SPEED * (DelayMs / 1000.0) + 1e-6) +
					1;
				// Inputs are tagged with PredGameTick and RegularPredicted is already
				// at that same tick. Using the snapshot tick here compensated latency
				// twice.
				const int AnalysisTick = Client()->PredGameTick(DummyIndex);
				s_aAutoUnfreezePredictionLead[DummyIndex] =
					std::clamp(AnalysisTick - Client()->GameTick(DummyIndex), 0,
						AUTO_UNFREEZE_MAX_PREDICTION_LEAD);
				s_aAutoUnfreezeEstimatedServerShotTick[DummyIndex] =
					AnalysisTick - s_aAutoUnfreezePredictionLead[DummyIndex];
				const int SnapshotPingMs =
					m_Snap.m_pLocalInfo ? std::clamp(m_Snap.m_pLocalInfo->m_Latency, 0, 400) : 0;
				const int PredictionTimeMs =
					std::clamp(Client()->GetPredictionTime(), 0, 400);
				const int RawPingCompTicks = std::clamp(
					(maximum(SnapshotPingMs, PredictionTimeMs) * SERVER_TICK_SPEED +
						1999) /
						2000,
					0, 10);
				if(s_aAutoUnfreezePingSampleTick[DummyIndex] != AnalysisTick)
				{
					s_aAutoUnfreezePingSampleTick[DummyIndex] = AnalysisTick;
					if((float)RawPingCompTicks >
						s_aAutoUnfreezeSmoothedPingTicks[DummyIndex])
						s_aAutoUnfreezeSmoothedPingTicks[DummyIndex] =
							(float)RawPingCompTicks;
					else
						s_aAutoUnfreezeSmoothedPingTicks[DummyIndex] =
							mix(s_aAutoUnfreezeSmoothedPingTicks[DummyIndex],
								(float)RawPingCompTicks, 0.12f);
				}
				const int PingCompTicks = std::clamp(
					round_to_int(s_aAutoUnfreezeSmoothedPingTicks[DummyIndex]), 0, 10);
				const int PingUncertaintyTicks =
					maximum(SnapshotPingMs, PredictionTimeMs) >= 140 ? 2 : 1;
				const int PredictionTicks =
					std::clamp(maximum(AutoUnfreezeTriggerTicks + PingCompTicks,
							   PingCompTicks + MaxBounces * DelayTicks + 2),
						1, AutoUnfreezeMaxPredictTicks);
				const vec2 SnapshotPos((float)m_Snap.m_pLocalCharacter->m_X,
					(float)m_Snap.m_pLocalCharacter->m_Y);
				const float PredictionError =
					pLocalPredChar ? distance(pLocalPredChar->Core()->m_Pos, SnapshotPos) : std::numeric_limits<float>::infinity();
				const float MaxPredictionError =
					96.0f + (pLocalPredChar ? length(pLocalPredChar->Core()->m_Vel) *
									  (float)maximum(1, PingCompTicks) :
								  0.0f);
				const bool PredictionCoherent =
					pLocalPredChar && PredictionError <= MaxPredictionError;
				if(!PredictionCoherent)
				{
					s_aAutoUnfreezePathValid[DummyIndex] = false;
					s_aAutoUnfreezeNearFreeze[DummyIndex] = false;
					s_aAutoUnfreezeWillFreezeSoon[DummyIndex] = false;
					s_aAutoUnfreezeFirstFreezeDelay[DummyIndex] =
						AutoUnfreezeMaxPredictTicks;
					s_aAutoUnfreezeSolutionFound[DummyIndex] = false;
					s_aAutoUnfreezeSolutionValidatedTick[DummyIndex] = -1;
				}

				if(s_aAutoUnfreezeLaserInFlight[DummyIndex] &&
					!s_aAutoUnfreezeLaserConfirmed[DummyIndex] &&
					m_Snap.m_pLocalCharacter)
				{
					if(m_Snap.m_pLocalCharacter->m_AttackTick !=
							s_aAutoUnfreezeAttackTickAtShot[DummyIndex] &&
						m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_LASER)
						s_aAutoUnfreezeLaserConfirmed[DummyIndex] = true;
					else if(Client()->GameTick(DummyIndex) >
						s_aAutoUnfreezeConfirmDeadlineTick[DummyIndex])
						s_aAutoUnfreezeLaserInFlight[DummyIndex] = false;
				}

				if(true ||
					AnalysisTick < s_aAutoUnfreezeLaserShotTick[DummyIndex] ||
					AnalysisTick > s_aAutoUnfreezeLaserExpireTick[DummyIndex])
				{
					s_aAutoUnfreezeLaserInFlight[DummyIndex] = false;
					s_aAutoUnfreezeLaserShotTick[DummyIndex] = -1;
					s_aAutoUnfreezeLaserExpectedHitTick[DummyIndex] = -1;
					s_aAutoUnfreezeLaserExpireTick[DummyIndex] = -1;
					s_aAutoUnfreezeAttackTickAtShot[DummyIndex] = -1;
					s_aAutoUnfreezeConfirmDeadlineTick[DummyIndex] = -1;
					s_aAutoUnfreezeLaserConfirmed[DummyIndex] = false;
				}

				// A self-unfreeze shot must be fired before entering freeze. Predict
				// the character and remember the exact ticks on which a returning laser
				// may hit.
				if(s_aAutoUnfreezeAnalysisTick[DummyIndex] != AnalysisTick)
				{
					s_aAutoUnfreezeAnalysisTick[DummyIndex] = AnalysisTick;
					s_aAutoUnfreezePredictionTicks[DummyIndex] = 0;
					s_aAutoUnfreezePathValid[DummyIndex] = false;
					s_aAutoUnfreezeNearFreeze[DummyIndex] = false;
					s_aAutoUnfreezeWillFreezeSoon[DummyIndex] = false;
					s_aAutoUnfreezeFirstFreezeDelay[DummyIndex] =
						AutoUnfreezeMaxPredictTicks;
					if(false &&
						!s_aAutoUnfreezeLaserInFlight[DummyIndex] && PredictionCoherent &&
						!FrozenNow && !HardFrozen && MaxBounces > 0 &&
						pLocalPredChar->GetWeaponGot(WEAPON_LASER))
					{
						std::fill_n(pAutoUnfreezePredCanUnfreeze, PredictionTicks, false);
						for(int Tick = 0; Tick < PredictionTicks; Tick++)
							std::fill_n(s_aaaAutoUnfreezePredPlayerActive[DummyIndex][Tick],
								MAX_CLIENTS, false);

						CGameWorld SimWorld;
						SimWorld.CopyWorldClean(&m_RegularPredictedWorld);
						// Custom/FNG servers do not always advertise DDRace tile
						// prediction. Auto-unfreeze still needs the actual map tiles to
						// forecast freeze.
						SimWorld.m_WorldConfig.m_IsDDRace = true;
						SimWorld.m_WorldConfig.m_IsVanilla = false;
						SimWorld.m_WorldConfig.m_PredictDDRace = true;
						SimWorld.m_WorldConfig.m_PredictTiles = true;
						SimWorld.m_WorldConfig.m_PredictFreeze = 1;
						SimWorld.m_GameTick = AnalysisTick;
						CCharacter *pSimChar = SimWorld.GetCharacterById(LocalId);
						CNetObj_PlayerInput SimInput = *pInput;
						if(SimInput.m_Fire & 1)
							SimInput.m_Fire = (SimInput.m_Fire + 1) & INPUT_STATE_MASK;
						SimInput.m_WantedWeapon = 0;
						SimInput.m_NextWeapon = 0;
						SimInput.m_PrevWeapon = 0;

						bool WasFrozen = false;
						bool ExactPathValid = pSimChar != nullptr;
						int SimulatedTicks = 0;
						for(int Tick = 0; Tick < PredictionTicks && ExactPathValid;
							Tick++)
						{
							SimulatedTicks = Tick + 1;
							pAutoUnfreezePredPos[Tick] = pSimChar->Core()->m_Pos;
							for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
							{
								CCharacter *pSimPlayer = SimWorld.GetCharacterById(ClientId);
								if(!pSimPlayer)
									continue;
								s_aaaAutoUnfreezePredPlayerActive[DummyIndex][Tick][ClientId] =
									true;
								s_aaaAutoUnfreezePredPlayerPos[DummyIndex][Tick][ClientId] =
									pSimPlayer->Core()->m_Pos;
							}

							const bool TouchingFreeze =
								DmdIsFreezeAtPoint(Collision(), pSimChar->Core()->m_Pos,
									&SimWorld.Switchers(), pSimChar->Team());
							const bool ApproachingFreeze =
								length(pSimChar->Core()->m_Vel) > 0.05f &&
								DmdIsFreezeNearTee(Collision(), pSimChar->Core()->m_Pos,
									&SimWorld.Switchers(), pSimChar->Team());
							const bool PredFrozen =
								pSimChar->m_FreezeTime > 0 || pSimChar->Core()->m_IsInFreeze;
							const bool PredHardFrozen = pSimChar->Core()->m_DeepFrozen ||
										    pSimChar->Core()->m_LiveFrozen;
							WasFrozen |= PredFrozen || TouchingFreeze;
							pAutoUnfreezePredCanUnfreeze[Tick] =
								WasFrozen && pSimChar->m_FreezeTime > 0 && !TouchingFreeze &&
								!PredHardFrozen;
							if(Tick < AutoUnfreezeTriggerTicks + PingCompTicks &&
								ApproachingFreeze)
								s_aAutoUnfreezeNearFreeze[DummyIndex] = true;
							if(Tick < AutoUnfreezeTriggerTicks + PingCompTicks &&
								(PredFrozen || TouchingFreeze))
							{
								s_aAutoUnfreezeWillFreezeSoon[DummyIndex] = true;
								s_aAutoUnfreezeFirstFreezeDelay[DummyIndex] =
									minimum(s_aAutoUnfreezeFirstFreezeDelay[DummyIndex], Tick);
							}
							if(PredHardFrozen)
							{
								ExactPathValid = false;
								break;
							}
							if(Tick + 1 >=
									minimum(PredictionTicks,
										AutoUnfreezeTriggerTicks + PingCompTicks) &&
								!s_aAutoUnfreezeWillFreezeSoon[DummyIndex])
								break;

							if(Tick + 1 < PredictionTicks)
							{
								pSimChar->OnPredictedInput(&SimInput);
								SimWorld.m_GameTick = AnalysisTick + Tick + 1;
								SimWorld.Tick();
								pSimChar = SimWorld.GetCharacterById(LocalId);
								ExactPathValid = pSimChar != nullptr;
							}
						}
						if(ExactPathValid)
						{
							s_aAutoUnfreezePredictionTicks[DummyIndex] = SimulatedTicks;
							s_aAutoUnfreezePathValid[DummyIndex] = true;
						}
					}
				}

				if(ManualAutoShotgun && !s_aAutoUnfreezePathValid[DummyIndex])
				{
					DmdPredictCharacterPath(&m_aClients[LocalId].m_RegularPredicted,
						PredictionTicks, pAutoUnfreezePredPos,
						Collision());
					s_aAutoUnfreezePredictionTicks[DummyIndex] = PredictionTicks;
				}

				// A generated press always needs its matching release, even if that
				// press already removed the freeze on the previous prediction tick.
				bool ReleasedAutoUnfreezeThisTick = false;
				if(s_aAutoUnfreezeReleasePending[DummyIndex])
				{
					if(AnalysisTick != s_aAutoUnfreezeInjectedTick[DummyIndex])
					{
						pInput->m_Fire = (s_aAutoUnfreezeInjectedFire[DummyIndex] + 1) &
								 INPUT_STATE_MASK;
						s_aAutoUnfreezeReleasePending[DummyIndex] = false;
						s_aAutoUnfreezeInjectedFire[DummyIndex] = -1;
						s_aAutoUnfreezeInjectedTick[DummyIndex] = -1;
						ReleasedAutoUnfreezeThisTick = true;
					}
					else
					{
						// OnSnapInput can run repeatedly during one game tick. Keep the
						// generated press and direction stable until the server receives
						// them.
						pInput->m_Fire = s_aAutoUnfreezeInjectedFire[DummyIndex];
						pInput->m_TargetX =
							round_to_int(s_aAutoUnfreezeLaserDir[DummyIndex].x * 1000.0f);
						pInput->m_TargetY =
							round_to_int(s_aAutoUnfreezeLaserDir[DummyIndex].y * 1000.0f);
						if(!pInput->m_TargetX && !pInput->m_TargetY)
							pInput->m_TargetY = 1;
					}
					ZzAutoUnfreezeOverridesInput = true;
				}

				const bool AutoUnfreezeActive =
					false &&
					!s_aAutoUnfreezeLaserInFlight[DummyIndex] && PredictionCoherent &&
					s_aAutoUnfreezePathValid[DummyIndex] && !FrozenNow && !HardFrozen &&
					s_aAutoUnfreezeWillFreezeSoon[DummyIndex];
				if((ManualAutoShotgun || AutoUnfreezeActive) &&
					!ReleasedAutoUnfreezeThisTick && !ZzFentBotOverridesInput &&
					!ZzAutoUnfreezeOverridesInput && m_Snap.m_pLocalCharacter)
				{
					const int ActiveWeapon = pLocalPredChar ? pLocalPredChar->GetActiveWeapon() : m_Snap.m_pLocalCharacter->m_Weapon;
					const int RequiredWeapon =
						AutoUnfreezeActive ? WEAPON_LASER : WEAPON_SHOTGUN;
					const bool WeaponReady =
						!pLocalPredChar || pLocalPredChar->GetReloadTimer() <= 0;
					const bool HasRequiredWeapon =
						!pLocalPredChar || pLocalPredChar->GetWeaponGot(RequiredWeapon);
					const bool RequestsRequiredWeapon =
						ActiveWeapon != RequiredWeapon && HasRequiredWeapon;
					const bool SwitchesRequiredWeaponNow =
						RequestsRequiredWeapon && WeaponReady;
					if(RequestsRequiredWeapon)
					{
						pInput->m_WantedWeapon = RequiredWeapon + 1;
						ZzAutoUnfreezeOverridesInput = true;
					}

					// DDNet handles direct weapon selection before FireWeapon, so a ready
					// weapon can be selected and fired by the same input packet.
					if((ManualAutoShotgun || AutoUnfreezeActive) &&
						(ActiveWeapon == RequiredWeapon || SwitchesRequiredWeaponNow) &&
						WeaponReady && HasRequiredWeapon)
					{
						float LaserReach = (float)m_aTuning[DummyIndex].m_LaserReach;
						if(m_RegularPredictedWorld.m_WorldConfig.m_IsFNG &&
							LaserReach < 10.0f)
							LaserReach = 800.0f;
						const float BounceCost =
							(float)m_aTuning[DummyIndex].m_LaserBounceCost;
						const float HitRadius = CCharacterCore::PhysicalSize();
						const float BaseAim = angle(CurAim);
						const int TracePredictionTicks =
							s_aAutoUnfreezePredictionTicks[DummyIndex];
						int LeadSampleCount = 0;
						int DominantPredictionLead = 0;
						for(int Lead = 0; Lead <= AUTO_UNFREEZE_MAX_PREDICTION_LEAD;
							Lead++)
						{
							LeadSampleCount +=
								m_aaAutoUnfreezeObservedPredictionLeadCount[DummyIndex][Lead];
							if(m_aaAutoUnfreezeObservedPredictionLeadCount[DummyIndex]
												      [Lead] >
								m_aaAutoUnfreezeObservedPredictionLeadCount
									[DummyIndex][DominantPredictionLead])
								DominantPredictionLead = Lead;
						}
						const bool UseLearnedPredictionLead =
							LeadSampleCount >= 4 &&
							m_aaAutoUnfreezeObservedPredictionLeadCount
										[DummyIndex][DominantPredictionLead] *
									10 >=
								LeadSampleCount * 7;
						const int CurrentPredictionLead =
							std::clamp(AnalysisTick - Client()->GameTick(DummyIndex), 0,
								AUTO_UNFREEZE_MAX_PREDICTION_LEAD);
						const int CentralPredictionLead = UseLearnedPredictionLead ? DominantPredictionLead : CurrentPredictionLead;
						int aArrivalLags[3] = {CentralPredictionLead, CentralPredictionLead,
							CentralPredictionLead};
						int NumArrivalLags = 1;
						if(AutoUnfreezeActive && !UseLearnedPredictionLead)
						{
							const int EarlierLead = maximum(0, CentralPredictionLead - 1);
							const int LaterLead = minimum(AUTO_UNFREEZE_MAX_PREDICTION_LEAD,
								CentralPredictionLead + 1);
							if(EarlierLead != CentralPredictionLead)
								aArrivalLags[NumArrivalLags++] = EarlierLead;
							if(LaterLead != CentralPredictionLead &&
								LaterLead != EarlierLead)
								aArrivalLags[NumArrivalLags++] = LaterLead;
						}
						auto PredictAuthoritativeShotOrigin = [&](int PredictionLead,
											      vec2 *pOrigin) {
							const int EstimatedServerShotTick = AnalysisTick - PredictionLead;
							CGameWorld OriginWorld;
							OriginWorld.CopyWorldClean(&m_GameWorld);
							OriginWorld.m_WorldConfig.m_IsDDRace = true;
							OriginWorld.m_WorldConfig.m_IsVanilla = false;
							OriginWorld.m_WorldConfig.m_PredictDDRace = true;
							OriginWorld.m_WorldConfig.m_PredictTiles = true;
							OriginWorld.m_WorldConfig.m_PredictFreeze = 1;
							const int BaseTick = OriginWorld.GameTick();
							if(EstimatedServerShotTick < BaseTick)
								return false;
							CCharacter *pOriginChar = OriginWorld.GetCharacterById(LocalId);
							if(!pOriginChar)
								return false;
							CNetObj_PlayerInput OriginInput = *pInput;
							if(OriginInput.m_Fire & 1)
								OriginInput.m_Fire =
									(OriginInput.m_Fire + 1) & INPUT_STATE_MASK;
							OriginInput.m_WantedWeapon = 0;
							OriginInput.m_NextWeapon = 0;
							OriginInput.m_PrevWeapon = 0;
							for(int Tick = BaseTick + 1; Tick <= EstimatedServerShotTick;
								Tick++)
							{
								pOriginChar->OnPredictedInput(&OriginInput);
								OriginWorld.m_GameTick = Tick;
								OriginWorld.Tick();
								pOriginChar = OriginWorld.GetCharacterById(LocalId);
								if(!pOriginChar)
									return false;
							}
							*pOrigin = pOriginChar->Core()->m_Pos;
							return true;
						};
						vec2 aExpectedShotOrigins[3];
						int NumValidArrivalLags = 0;
						for(int Scenario = 0; Scenario < NumArrivalLags; Scenario++)
						{
							vec2 Origin;
							if(!AutoUnfreezeActive || PredictAuthoritativeShotOrigin(
											  aArrivalLags[Scenario], &Origin))
							{
								aArrivalLags[NumValidArrivalLags] = aArrivalLags[Scenario];
								aExpectedShotOrigins[NumValidArrivalLags] =
									AutoUnfreezeActive ? Origin : LocalPos;
								NumValidArrivalLags++;
							}
						}
						NumArrivalLags = NumValidArrivalLags;
						if(NumArrivalLags == 0)
						{
							aArrivalLags[0] = CentralPredictionLead;
							aExpectedShotOrigins[0] = LocalPos;
							NumArrivalLags = 1;
						}
						AutoUnfreezeDebugPredictionLeadCount = NumArrivalLags;
						for(int Scenario = 0; Scenario < NumArrivalLags; Scenario++)
							aAutoUnfreezeDebugPredictionLeads[Scenario] =
								aArrivalLags[Scenario];
						s_aAutoUnfreezePredictionLead[DummyIndex] = aArrivalLags[0];
						s_aAutoUnfreezeEstimatedServerShotTick[DummyIndex] =
							AnalysisTick - aArrivalLags[0];
						vec2 CalibratedOriginOffset(0.0f, 0.0f);
						if(AutoUnfreezeActive &&
							m_aAutoUnfreezeObservedSamples[DummyIndex] >= 2)
						{
							CalibratedOriginOffset =
								m_aAutoUnfreezeObservedOriginOffset[DummyIndex];
							if(length(CalibratedOriginOffset) > 24.0f)
								CalibratedOriginOffset =
									normalize(CalibratedOriginOffset) * 24.0f;
						}
						vec2 aScenarioOrigins[3];
						for(int Scenario = 0; Scenario < NumArrivalLags; Scenario++)
							aScenarioOrigins[Scenario] =
								aExpectedShotOrigins[Scenario] + CalibratedOriginOffset;
						const vec2 ShotOrigin = aScenarioOrigins[0];
						auto LearningKeyFor = [&](const vec2 &Direction, int HitDelay,
									      int Bounces) {
							uint64_t Key = 1469598103934665603ULL;
							auto MixKey = [&](int Value) {
								Key ^= (uint32_t)Value;
								Key *= 1099511628211ULL;
							};
							const vec2 Velocity = pLocalPredChar ? pLocalPredChar->Core()->m_Vel : vec2(0.0f, 0.0f);
							float DirectionAngle = angle(Direction);
							if(DirectionAngle < 0.0f)
								DirectionAngle += 2.0f * pi;
							MixKey(round_to_int(ShotOrigin.x / 32.0f));
							MixKey(round_to_int(ShotOrigin.y / 32.0f));
							MixKey(round_to_int(DirectionAngle * 72.0f / (2.0f * pi)) % 72);
							MixKey(std::clamp(round_to_int(Velocity.x / 2.0f), -16, 16));
							MixKey(std::clamp(round_to_int(Velocity.y / 2.0f), -16, 16));
							MixKey(std::clamp(HitDelay / 2, 0, 63));
							MixKey(std::clamp(Bounces, 0, 32));
							MixKey(std::clamp(PingCompTicks, 0, 10));
							return Key == 0 ? 1ULL : Key;
						};
						auto SegmentHitsBlockingPlayer = [&](const vec2 &From,
											 const vec2 &To,
											 float MaxParameter,
											 int PredictionIndex) {
							const vec2 Segment = To - From;
							const float SegmentLengthSq = dot(Segment, Segment);
							if(SegmentLengthSq < 0.001f)
								return false;
							const float RadiusSq = HitRadius * HitRadius;
							for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
							{
								if(ClientId == LocalId)
									continue;
								if(pLocalPredChar && !pLocalPredChar->CanCollide(ClientId))
									continue;
								vec2 PlayerPos;
								if(AutoUnfreezeActive && PredictionIndex >= 0 &&
									PredictionIndex < TracePredictionTicks)
								{
									if(!s_aaaAutoUnfreezePredPlayerActive[DummyIndex]
													     [PredictionIndex]
													     [ClientId])
										continue;
									PlayerPos =
										s_aaaAutoUnfreezePredPlayerPos[DummyIndex]
													      [PredictionIndex][ClientId];
								}
								else
								{
									if(!m_Snap.m_aCharacters[ClientId].m_Active)
										continue;
									PlayerPos = m_aClients[ClientId].m_RegularPredicted.m_Pos;
								}
								const float Parameter =
									dot(PlayerPos - From, Segment) / SegmentLengthSq;
								if(Parameter < 0.0f || Parameter > MaxParameter)
									continue;
								const vec2 Delta = PlayerPos - (From + Segment * Parameter);
								if(dot(Delta, Delta) <= RadiusSq)
									return true;
							}
							return false;
						};

						auto TraceDirectionScenario = [&](const vec2 &InitialDir,
										      const vec2 &ScenarioOrigin,
										      int ScenarioLagTicks,
										      float *pHitQuality,
										      int *pHitBounces) {
							if(pHitQuality)
								*pHitQuality = -std::numeric_limits<float>::infinity();
							if(pHitBounces)
								*pHitBounces = 0;
							vec2 Dir = InitialDir;
							vec2 Pos = ScenarioOrigin;
							float Energy = LaserReach;
							int EvalTick = 0;
							bool ZeroEnergyBounceInLastTick = false;
							vec2 aSegmentFrom[33];
							vec2 aSegmentTo[33];
							int aSegmentPredictionIndex[33];
							int SegmentCount = 0;
							float BestNearQuality = -std::numeric_limits<float>::infinity();
							int BestNearBounces = 0;

							for(int Bounce = 0; Bounce <= MaxBounces && Energy > 0.0f &&
									    EvalTick < TracePredictionTicks;
								Bounce++)
							{
								vec2 ColTile;
								vec2 To = Pos + Dir * Energy;
								const vec2 From = Pos;
								const int Res = Collision()->IntersectLineTeleWeapon(
									Pos, To, &ColTile, &To);
								aSegmentFrom[SegmentCount] = From;
								aSegmentTo[SegmentCount] = To;
								aSegmentPredictionIndex[SegmentCount] =
									Bounce == 0 ? 0 : std::clamp(EvalTick - 1 - ScenarioLagTicks, 0, TracePredictionTicks - 1);
								SegmentCount++;

								// Tick zero is the outgoing beam, which cannot hit its owner.
								// Every later segment exists on one exact bounce tick, not for
								// its lifetime. Server lasers tick before characters. A
								// direct-input shot made between ticks therefore sees the
								// character after EvalTick - 1 movement steps when the
								// reflected segment is evaluated.
								const int PredIndex = EvalTick - 1 - ScenarioLagTicks;
								const bool CanHitFrozenSelf =
									Bounce > 0 && PredIndex >= 0 &&
									PredIndex < TracePredictionTicks &&
									(!AutoUnfreezeActive ||
										pAutoUnfreezePredCanUnfreeze[PredIndex]);
								auto HitClearance = [&](int PositionIndex) {
									if(PositionIndex < 0 ||
										PositionIndex >= TracePredictionTicks ||
										(AutoUnfreezeActive &&
											!pAutoUnfreezePredCanUnfreeze[PositionIndex]))
										return -HitRadius;
									const vec2 Segment = To - Pos;
									const float SegmentLengthSq = dot(Segment, Segment);
									if(SegmentLengthSq < 0.001f)
										return -HitRadius;
									const float Parameter = std::clamp(
										dot(pAutoUnfreezePredPos[PositionIndex] - Pos, Segment) /
											SegmentLengthSq,
										0.0f, 1.0f);
									return HitRadius -
									       distance(pAutoUnfreezePredPos[PositionIndex],
										       Pos + Segment * Parameter);
								};
								const float NominalClearance =
									CanHitFrozenSelf ? HitClearance(PredIndex) : -HitRadius;
								if(CanHitFrozenSelf)
								{
									const float NearQuality =
										NominalClearance * 2.0f -
										80.0f * (float)maximum(0, Bounce - 1) -
										0.04f * (float)EvalTick;
									if(NearQuality > BestNearQuality)
									{
										BestNearQuality = NearQuality;
										BestNearBounces = Bounce;
									}
								}
								if(CanHitFrozenSelf && NominalClearance >= 2.0f)
								{
									bool BlockedByPlayer = false;
									for(int SegmentIndex = 0;
										SegmentIndex < SegmentCount && !BlockedByPlayer;
										SegmentIndex++)
									{
										float MaxParameter = 1.0f;
										if(SegmentIndex == SegmentCount - 1)
										{
											const vec2 Segment =
												aSegmentTo[SegmentIndex] - aSegmentFrom[SegmentIndex];
											const float SegmentLengthSq = dot(Segment, Segment);
											if(SegmentLengthSq > 0.001f)
												MaxParameter =
													std::clamp(dot(pAutoUnfreezePredPos[PredIndex] -
																   aSegmentFrom[SegmentIndex],
															   Segment) /
															   SegmentLengthSq,
														0.0f, 1.0f);
										}
										BlockedByPlayer = SegmentHitsBlockingPlayer(
											aSegmentFrom[SegmentIndex], aSegmentTo[SegmentIndex],
											MaxParameter, aSegmentPredictionIndex[SegmentIndex]);
									}
									if(!BlockedByPlayer)
									{
										float Quality = NominalClearance * 2.0f;
										for(int Offset = 1; Offset <= PingUncertaintyTicks;
											Offset++)
											Quality += 0.35f * (HitClearance(PredIndex - Offset) +
														   HitClearance(PredIndex + Offset));
										if(pHitQuality)
											*pHitQuality = Quality;
										if(pHitBounces)
											*pHitBounces = Bounce;
										return EvalTick;
									}
								}

								if(!Res)
									break;
								if(Bounce >= MaxBounces)
									break;

								vec2 TempPos = To;
								vec2 TempDir = Dir * 4.0f;
								int OldTile = 0;
								if(Res == -1)
								{
									OldTile = Collision()->GetTile(round_to_int(ColTile.x),
										round_to_int(ColTile.y));
									Collision()->SetCollisionAt(round_to_int(ColTile.x),
										round_to_int(ColTile.y),
										TILE_SOLID);
								}
								Collision()->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
								if(Res == -1)
									Collision()->SetCollisionAt(round_to_int(ColTile.x),
										round_to_int(ColTile.y), OldTile);
								Pos = TempPos;
								if(length(TempDir) <= 0.001f)
									break;
								Dir = normalize(TempDir);
								const float Dist = distance(From, Pos);
								if(Dist == 0.0f && ZeroEnergyBounceInLastTick)
									break;
								Energy -= Dist + BounceCost;
								ZeroEnergyBounceInLastTick = Dist == 0.0f;
								EvalTick += DelayTicks;
							}
							if(pHitQuality)
								*pHitQuality = BestNearQuality;
							if(pHitBounces)
								*pHitBounces = BestNearBounces;
							return -1;
						};

						auto TraceDirection = [&](const vec2 &InitialDir,
									      float *pHitQuality, int *pHitBounces) {
							if(pHitBounces)
								*pHitBounces = 0;
							float WorstQuality = std::numeric_limits<float>::infinity();
							int ExpectedHitDelay = -1;
							int MaxHitDelay = 0;
							int MaxHitBounces = 0;
							int SuccessfulScenarios = 0;
							for(int Scenario = 0; Scenario < NumArrivalLags; Scenario++)
							{
								const int ArrivalLag = aArrivalLags[Scenario];
								const vec2 ScenarioOrigin = aScenarioOrigins[Scenario];
								float ScenarioQuality;
								int ScenarioBounces;
								const int ScenarioHitDelay = TraceDirectionScenario(
									InitialDir, ScenarioOrigin, ArrivalLag, &ScenarioQuality,
									&ScenarioBounces);
								if(ScenarioHitDelay < 0)
								{
									// The central timing estimate must always work. High-ping
									// edge scenarios are a robustness vote instead of an absolute
									// veto.
									if(Scenario == 0)
									{
										if(pHitQuality)
											*pHitQuality = ScenarioQuality;
										if(pHitBounces)
											*pHitBounces = ScenarioBounces;
										return -1;
									}
									continue;
								}
								SuccessfulScenarios++;
								if(Scenario == 0)
									ExpectedHitDelay = ScenarioHitDelay;
								WorstQuality = minimum(WorstQuality, ScenarioQuality);
								MaxHitDelay = maximum(MaxHitDelay, ScenarioHitDelay);
								MaxHitBounces = maximum(MaxHitBounces, ScenarioBounces);
							}
							const int RequiredScenarios = NumArrivalLags == 1 ? 1 : 2;
							if(SuccessfulScenarios < RequiredScenarios)
							{
								if(pHitQuality)
									*pHitQuality = -std::numeric_limits<float>::infinity();
								return -1;
							}
							if(pHitQuality)
							{
								const float BaseQuality =
									WorstQuality -
									80.0f * (float)maximum(0, MaxHitBounces - 1) -
									8.0f * (float)(NumArrivalLags - SuccessfulScenarios) -
									0.04f * (float)MaxHitDelay;
								const uint64_t LearningKey =
									LearningKeyFor(InitialDir, ExpectedHitDelay, MaxHitBounces);
								*pHitQuality = BaseQuality +
									       (AutoUnfreezeActive ? AutoUnfreezeLearningBias(
													     DummyIndex, LearningKey, AnalysisTick) :
												     0.0f);
							}
							if(pHitBounces)
								*pHitBounces = MaxHitBounces;
							return ExpectedHitDelay;
						};

						const bool UseTickCache =
							AutoUnfreezeActive &&
							s_aAutoUnfreezeSolutionValidatedTick[DummyIndex] ==
								AnalysisTick;
						bool Found =
							UseTickCache && s_aAutoUnfreezeSolutionFound[DummyIndex];
						vec2 BestDir = s_aAutoUnfreezeSolutionFound[DummyIndex] ? s_aAutoUnfreezeSolutionDir[DummyIndex] : CurAim;
						int BestHitDelay = UseTickCache && Found ? s_aAutoUnfreezeSolutionHitDelay[DummyIndex] : -1;
						int BestHitBounces =
							UseTickCache && Found ? s_aAutoUnfreezeSolutionBounces[DummyIndex] : 0;
						float BestHitQuality = -std::numeric_limits<float>::infinity();
						struct SAutoUnfreezeCandidate
						{
							vec2 m_Dir;
							int m_HitDelay;
							int m_Bounces;
							float m_Quality;
						};
						constexpr int MaxExactCandidates = 6;
						constexpr int MaxNearCandidates = 8;
						SAutoUnfreezeCandidate aExactCandidates[MaxExactCandidates];
						SAutoUnfreezeCandidate aNearCandidates[MaxNearCandidates];
						int NumExactCandidates = 0;
						int NumNearCandidates = 0;
						auto RememberExactCandidate = [&](const vec2 &Dir, int HitDelay,
										      int Bounces, float Quality) {
							if(!AutoUnfreezeActive)
								return;
							if(m_AutoUnfreezeDebugFullRecording &&
								AutoUnfreezeDebugCandidateCount <
									AutoUnfreezeDebugMaxCandidates)
							{
								aAutoUnfreezeDebugCandidateDir
									[AutoUnfreezeDebugCandidateCount] = Dir;
								aAutoUnfreezeDebugCandidateDelay
									[AutoUnfreezeDebugCandidateCount] = HitDelay;
								aAutoUnfreezeDebugCandidateQuality
									[AutoUnfreezeDebugCandidateCount] = Quality;
								AutoUnfreezeDebugCandidateCount++;
							}
							if(HitDelay < 0)
							{
								if(!is_finite(Quality))
									return;
								for(int Index = 0; Index < NumNearCandidates; Index++)
								{
									if(dot(aNearCandidates[Index].m_Dir, Dir) < 0.9998f)
										continue;
									if(Quality <= aNearCandidates[Index].m_Quality)
										return;
									for(int Move = Index; Move + 1 < NumNearCandidates; Move++)
										aNearCandidates[Move] = aNearCandidates[Move + 1];
									NumNearCandidates--;
									break;
								}
								int InsertAt = 0;
								while(InsertAt < NumNearCandidates &&
									aNearCandidates[InsertAt].m_Quality >= Quality)
									InsertAt++;
								if(InsertAt >= MaxNearCandidates)
									return;
								const int NewCount =
									minimum(NumNearCandidates + 1, MaxNearCandidates);
								for(int Move = NewCount - 1; Move > InsertAt; Move--)
									aNearCandidates[Move] = aNearCandidates[Move - 1];
								aNearCandidates[InsertAt] = {Dir, -1, Bounces, Quality};
								NumNearCandidates = NewCount;
								return;
							}

							// Keep genuinely different rays. Otherwise a half-degree search
							// can fill the entire shortlist with six quantizations of the
							// same grazing hit.
							for(int Index = 0; Index < NumExactCandidates; Index++)
							{
								if(dot(aExactCandidates[Index].m_Dir, Dir) < 0.9994f)
									continue;
								if(Quality <= aExactCandidates[Index].m_Quality)
									return;
								for(int Move = Index; Move + 1 < NumExactCandidates; Move++)
									aExactCandidates[Move] = aExactCandidates[Move + 1];
								NumExactCandidates--;
								break;
							}

							int InsertAt = 0;
							while(InsertAt < NumExactCandidates &&
								aExactCandidates[InsertAt].m_Quality >= Quality)
								InsertAt++;
							if(InsertAt >= MaxExactCandidates)
								return;
							const int NewCount =
								minimum(NumExactCandidates + 1, MaxExactCandidates);
							for(int Move = NewCount - 1; Move > InsertAt; Move--)
								aExactCandidates[Move] = aExactCandidates[Move - 1];
							aExactCandidates[InsertAt] = {Dir, HitDelay, Bounces, Quality};
							NumExactCandidates = NewCount;
						};

						if(!UseTickCache || ManualAutoShotgun)
						{
							// Expensive global work is only needed when the cached and local
							// corrections did not produce an exact solution on this tick.
							const bool RunFullSearch =
								ManualAutoShotgun ||
								s_aAutoUnfreezeFirstFreezeDelay[DummyIndex] <= 8 ||
								(AnalysisTick & 1) == 0;
							const bool HadSolution = AutoUnfreezeActive &&
										 s_aAutoUnfreezeSolutionFound[DummyIndex];
							BestHitDelay = HadSolution ? TraceDirection(BestDir, &BestHitQuality,
											     &BestHitBounces) :
										     -1;
							Found = BestHitDelay >= 0;
							if(Found)
								RememberExactCandidate(BestDir, BestHitDelay, BestHitBounces,
									BestHitQuality);

							// A moving tee usually only needs a tiny correction to the
							// previous solution. Keep this cheap path ahead of the global
							// search.
							if(HadSolution)
							{
								const float CachedAngle = angle(BestDir);
								for(int StepIndex = 1; StepIndex <= 16; StepIndex++)
								{
									const float Offset = (pi / 720.0f) * (float)StepIndex;
									for(int Sign : {-1, 1})
									{
										const vec2 Candidate =
											direction(CachedAngle + Offset * (float)Sign);
										float CandidateQuality;
										int CandidateBounces = 0;
										const int CandidateHitDelay = TraceDirection(
											Candidate, &CandidateQuality, &CandidateBounces);
										RememberExactCandidate(Candidate, CandidateHitDelay,
											CandidateBounces, CandidateQuality);
										if(CandidateHitDelay >= 0 &&
											(!Found || CandidateQuality > BestHitQuality))
										{
											BestDir = Candidate;
											BestHitDelay = CandidateHitDelay;
											BestHitBounces = CandidateBounces;
											BestHitQuality = CandidateQuality;
											Found = true;
										}
									}
								}
							}

							// Mirror every future hittable tee position that lines up with a
							// laser bounce tick. Large freeze fields often cannot be left by
							// the first bounce, so limiting this to one delay loses valid
							// rays.
							if(AutoUnfreezeActive && MaxBounces > 0 && RunFullSearch &&
								!Found)
							{
								const int MaxWallSteps =
									std::clamp((int)ceilf(LaserReach / 32.0f), 1, 32);
								const float LeftBoundary = floorf(ShotOrigin.x / 32.0f) * 32.0f;
								const float RightBoundary = LeftBoundary + 32.0f;
								const float TopBoundary = floorf(ShotOrigin.y / 32.0f) * 32.0f;
								const float BottomBoundary = TopBoundary + 32.0f;
								auto TryReflectedTarget = [&](const vec2 &ReflectedTarget) {
									const vec2 Delta = ReflectedTarget - ShotOrigin;
									if(length(Delta) < 0.001f)
										return;
									const vec2 Candidate = normalize(Delta);
									float CandidateQuality;
									int CandidateBounces = 0;
									const int CandidateHitDelay = TraceDirection(
										Candidate, &CandidateQuality, &CandidateBounces);
									RememberExactCandidate(Candidate, CandidateHitDelay,
										CandidateBounces, CandidateQuality);
									if(CandidateHitDelay < 0)
										return;
									if(!Found || CandidateQuality > BestHitQuality)
									{
										BestDir = Candidate;
										BestHitDelay = CandidateHitDelay;
										BestHitBounces = CandidateBounces;
										BestHitQuality = CandidateQuality;
										Found = true;
									}
								};
								for(int ReturnBounce = 1; ReturnBounce <= MaxBounces;
									ReturnBounce++)
								{
									const int TargetIndex =
										ReturnBounce * DelayTicks - 1 - aArrivalLags[0];
									if(TargetIndex >= TracePredictionTicks)
										break;
									if(TargetIndex < 0 ||
										!pAutoUnfreezePredCanUnfreeze[TargetIndex])
										continue;
									const vec2 Target = pAutoUnfreezePredPos[TargetIndex];
									for(int WallStep = 0; WallStep <= MaxWallSteps; WallStep++)
									{
										const float Offset = (float)WallStep * 32.0f;
										const float aWallX[] = {LeftBoundary - Offset,
											RightBoundary + Offset};
										const float aWallY[] = {TopBoundary - Offset,
											BottomBoundary + Offset};
										for(float WallX : aWallX)
										{
											if(fabsf(WallX - ShotOrigin.x) > 1.0f)
												TryReflectedTarget(
													vec2(2.0f * WallX - Target.x, Target.y));
										}
										for(float WallY : aWallY)
										{
											if(fabsf(WallY - ShotOrigin.y) > 1.0f)
												TryReflectedTarget(
													vec2(Target.x, 2.0f * WallY - Target.y));
										}
									}
								}
							}

							// Compare the geometric solution with a complete angular search.
							// Search every other tick while there is time, then every tick
							// during the final eight ticks before freeze.
							if(RunFullSearch && !Found)
							{
								constexpr int SearchSamples = 480;
								const float SearchBaseAngle =
									ManualAutoShotgun ? BaseAim : 0.0f;
								for(int Sample = 0; Sample < SearchSamples; Sample++)
								{
									const vec2 Candidate = direction(
										SearchBaseAngle +
										(2.0f * pi / (float)SearchSamples) * (float)Sample);
									float CandidateQuality;
									int CandidateBounces = 0;
									const int CandidateHitDelay = TraceDirection(
										Candidate, &CandidateQuality, &CandidateBounces);
									RememberExactCandidate(Candidate, CandidateHitDelay,
										CandidateBounces, CandidateQuality);
									if(CandidateHitDelay >= 0 &&
										(!Found || CandidateQuality > BestHitQuality))
									{
										BestDir = Candidate;
										BestHitDelay = CandidateHitDelay;
										BestHitBounces = CandidateBounces;
										BestHitQuality = CandidateQuality;
										Found = true;
									}
								}
							}

							// A coarse ray can pass only a few pixels outside the tee after
							// several reflections. Refine the closest misses instead of
							// losing narrow but fully valid ricochet corridors between 0.5
							// degree samples.
							if(RunFullSearch && !Found && NumNearCandidates > 0)
							{
								constexpr int NearFineSteps = 24;
								constexpr float NearFineAngleStep = pi / 7200.0f;
								vec2 aNearRefineDirs[MaxNearCandidates];
								const int NumNearRefineDirs = NumNearCandidates;
								for(int NearIndex = 0; NearIndex < NumNearRefineDirs;
									NearIndex++)
									aNearRefineDirs[NearIndex] = aNearCandidates[NearIndex].m_Dir;
								for(int NearIndex = 0; NearIndex < NumNearRefineDirs;
									NearIndex++)
								{
									const float NearAngle = angle(aNearRefineDirs[NearIndex]);
									for(int StepIndex = 1; StepIndex <= NearFineSteps;
										StepIndex++)
									{
										for(int Sign : {-1, 1})
										{
											const vec2 Candidate =
												direction(NearAngle + NearFineAngleStep *
															      (float)(StepIndex * Sign));
											float CandidateQuality;
											int CandidateBounces = 0;
											const int CandidateHitDelay = TraceDirection(
												Candidate, &CandidateQuality, &CandidateBounces);
											RememberExactCandidate(Candidate, CandidateHitDelay,
												CandidateBounces,
												CandidateQuality);
											if(CandidateHitDelay >= 0 &&
												(!Found || CandidateQuality > BestHitQuality))
											{
												BestDir = Candidate;
												BestHitDelay = CandidateHitDelay;
												BestHitBounces = CandidateBounces;
												BestHitQuality = CandidateQuality;
												Found = true;
											}
										}
									}
								}
							}

							// Sub-degree refinement maximizes clearance instead of accepting
							// a grazing hit that can miss after one quantized movement step.
							if(Found && RunFullSearch)
							{
								const float BestAngle = angle(BestDir);
								constexpr int FineSteps = 40;
								constexpr float FineAngleStep = pi / 3600.0f;
								for(int StepIndex = 1; StepIndex <= FineSteps; StepIndex++)
								{
									for(int Sign : {-1, 1})
									{
										const vec2 Candidate = direction(
											BestAngle + FineAngleStep * (float)(StepIndex * Sign));
										float CandidateQuality;
										int CandidateBounces = 0;
										const int CandidateHitDelay = TraceDirection(
											Candidate, &CandidateQuality, &CandidateBounces);
										RememberExactCandidate(Candidate, CandidateHitDelay,
											CandidateBounces, CandidateQuality);
										if(CandidateHitDelay >= 0 &&
											CandidateQuality > BestHitQuality)
										{
											BestDir = Candidate;
											BestHitDelay = CandidateHitDelay;
											BestHitBounces = CandidateBounces;
											BestHitQuality = CandidateQuality;
										}
									}
								}
							}

							if(AutoUnfreezeActive && Found)
							{
								RememberExactCandidate(BestDir, BestHitDelay, BestHitBounces,
									BestHitQuality);
								auto ValidatePredictedLaser =
									[&](const SAutoUnfreezeCandidate &Candidate,
										int PredictionLead, vec2 *pValidatedDir) {
										vec2 QuantizedDir(
											(float)round_to_int(Candidate.m_Dir.x * 1000.0f),
											(float)round_to_int(Candidate.m_Dir.y * 1000.0f));
										if(length(QuantizedDir) < 0.001f)
											return -1;
										QuantizedDir = normalize(QuantizedDir);

										CGameWorld ShotWorld;
										ShotWorld.CopyWorldClean(&m_GameWorld);
										ShotWorld.m_WorldConfig.m_IsDDRace = true;
										ShotWorld.m_WorldConfig.m_IsVanilla = false;
										ShotWorld.m_WorldConfig.m_PredictDDRace = true;
										ShotWorld.m_WorldConfig.m_PredictTiles = true;
										ShotWorld.m_WorldConfig.m_PredictFreeze = 1;
										const int EstimatedServerShotTick =
											AnalysisTick - PredictionLead;
										const int BaseTick = ShotWorld.GameTick();
										if(EstimatedServerShotTick < BaseTick)
											return -1;

										// Existing snapshot lasers must not be allowed to make a
										// candidate look successful. This validation is
										// exclusively for the proposed shot.
										while(CEntity *pExistingLaser =
												ShotWorld.FindFirst(CGameWorld::ENTTYPE_LASER))
											pExistingLaser->Destroy();

										CCharacter *pShotChar =
											ShotWorld.GetCharacterById(LocalId);
										if(!pShotChar)
											return -1;

										CNetObj_PlayerInput ShotInput = *pInput;
										if(ShotInput.m_Fire & 1)
											ShotInput.m_Fire =
												(ShotInput.m_Fire + 1) & INPUT_STATE_MASK;
										ShotInput.m_WantedWeapon = 0;
										ShotInput.m_NextWeapon = 0;
										ShotInput.m_PrevWeapon = 0;
										for(int Tick = BaseTick + 1;
											Tick <= EstimatedServerShotTick; Tick++)
										{
											pShotChar->OnPredictedInput(&ShotInput);
											ShotWorld.m_GameTick = Tick;
											ShotWorld.Tick();
											pShotChar = ShotWorld.GetCharacterById(LocalId);
											if(!pShotChar)
												return -1;
										}
										const bool FrozenBeforeShot =
											pShotChar->m_FreezeTime > 0 ||
											pShotChar->Core()->m_IsInFreeze ||
											DmdIsFreezeAtPoint(
												Collision(), pShotChar->Core()->m_Pos,
												&ShotWorld.Switchers(), pShotChar->Team());
										if(FrozenBeforeShot || pShotChar->Core()->m_DeepFrozen ||
											pShotChar->Core()->m_LiveFrozen)
											return -1;

										const int TuneZone = pShotChar->GetOverriddenTuneZone();
										const float ExactLaserReach =
											ShotWorld.GetTuning(TuneZone)->m_LaserReach;
										new CLaser(
											&ShotWorld,
											pShotChar->Core()->m_Pos + CalibratedOriginOffset,
											QuantizedDir, ExactLaserReach, LocalId, WEAPON_LASER);

										bool WasFrozen = false;
										const int ValidationTicks =
											minimum(TracePredictionTicks,
												maximum(1, Candidate.m_HitDelay + 4));
										for(int Step = 1; Step <= ValidationTicks; Step++)
										{
											pShotChar->OnPredictedInput(&ShotInput);
											ShotWorld.m_GameTick = EstimatedServerShotTick + Step;
											ShotWorld.Tick();
											pShotChar = ShotWorld.GetCharacterById(LocalId);
											if(!pShotChar)
												return -1;

											const bool TouchingFreeze = DmdIsFreezeAtPoint(
												Collision(), pShotChar->Core()->m_Pos,
												&ShotWorld.Switchers(), pShotChar->Team());
											const bool Frozen = pShotChar->m_FreezeTime > 0 ||
													    pShotChar->Core()->m_IsInFreeze;
											const bool HardFrozen =
												pShotChar->Core()->m_DeepFrozen ||
												pShotChar->Core()->m_LiveFrozen;
											WasFrozen |= Frozen || TouchingFreeze;
											if(WasFrozen && !Frozen && !TouchingFreeze &&
												!HardFrozen)
											{
												*pValidatedDir = QuantizedDir;
												return Step;
											}
											if(HardFrozen)
												return -1;
										}
										return -1;
									};

								Found = false;
								for(int CandidateIndex = 0;
									CandidateIndex < NumExactCandidates; CandidateIndex++)
								{
									AutoUnfreezeDebugExactTested++;
									vec2 aLeadDir[3];
									int aLeadHitDelay[3] = {-1, -1, -1};
									int SuccessfulLeadScenarios = 0;
									for(int Scenario = 0; Scenario < NumArrivalLags;
										Scenario++)
									{
										aLeadHitDelay[Scenario] = ValidatePredictedLaser(
											aExactCandidates[CandidateIndex],
											aArrivalLags[Scenario], &aLeadDir[Scenario]);
										if(aLeadHitDelay[Scenario] >= 0)
											SuccessfulLeadScenarios++;
									}
									const int RequiredLeadScenarios = NumArrivalLags == 1 ? 1 : 2;
									if(aLeadHitDelay[0] < 0 ||
										SuccessfulLeadScenarios < RequiredLeadScenarios)
										continue;
									BestDir = aLeadDir[0];
									BestHitDelay = aLeadHitDelay[0];
									for(int Scenario = 1; Scenario < NumArrivalLags; Scenario++)
										BestHitDelay =
											maximum(BestHitDelay, aLeadHitDelay[Scenario]);
									BestHitBounces = aExactCandidates[CandidateIndex].m_Bounces;
									BestHitQuality = aExactCandidates[CandidateIndex].m_Quality;
									Found = true;
									AutoUnfreezeDebugExactAccepted = CandidateIndex;
									AutoUnfreezeDebugPhase0HitDelay = aLeadHitDelay[0];
									AutoUnfreezeDebugPhase1HitDelay = aLeadHitDelay[1];
									AutoUnfreezeDebugPhase2HitDelay = aLeadHitDelay[2];
									break;
								}
							}

							if(!m_AutoUnfreezeDebugFullRecording)
							{
								AutoUnfreezeDebugCandidateCount =
									minimum(NumExactCandidates, AutoUnfreezeDebugMaxCandidates);
								for(int CandidateIndex = 0;
									CandidateIndex < AutoUnfreezeDebugCandidateCount;
									CandidateIndex++)
								{
									aAutoUnfreezeDebugCandidateDir[CandidateIndex] =
										aExactCandidates[CandidateIndex].m_Dir;
									aAutoUnfreezeDebugCandidateDelay[CandidateIndex] =
										aExactCandidates[CandidateIndex].m_HitDelay;
									aAutoUnfreezeDebugCandidateQuality[CandidateIndex] =
										aExactCandidates[CandidateIndex].m_Quality;
								}
							}

							if(AutoUnfreezeActive)
							{
								s_aAutoUnfreezeSolutionValidatedTick[DummyIndex] = AnalysisTick;
								s_aAutoUnfreezeSolutionFound[DummyIndex] = Found;
								if(Found)
								{
									s_aAutoUnfreezeSolutionDir[DummyIndex] = BestDir;
									s_aAutoUnfreezeSolutionHitDelay[DummyIndex] = BestHitDelay;
									s_aAutoUnfreezeSolutionBounces[DummyIndex] = BestHitBounces;
								}
							}
						}

						if(Found)
						{
							pInput->m_TargetX = (int)round_to_int(BestDir.x * 1000.0f);
							pInput->m_TargetY = (int)round_to_int(BestDir.y * 1000.0f);
							if(!pInput->m_TargetX && !pInput->m_TargetY)
								pInput->m_TargetY = 1;

							if(AutoUnfreezeActive)
							{
								if((pInput->m_Fire & 1) != 0)
									pInput->m_Fire =
										((pInput->m_Fire + 2) | 1) & INPUT_STATE_MASK;
								else
									pInput->m_Fire = (pInput->m_Fire + 1) & INPUT_STATE_MASK;
								s_aAutoUnfreezeReleasePending[DummyIndex] = true;
								s_aAutoUnfreezeInjectedFire[DummyIndex] = pInput->m_Fire;
								s_aAutoUnfreezeInjectedTick[DummyIndex] = AnalysisTick;
								s_aAutoUnfreezeLaserInFlight[DummyIndex] = true;
								s_aAutoUnfreezeLaserShotTick[DummyIndex] = AnalysisTick;
								s_aAutoUnfreezeLaserExpectedHitTick[DummyIndex] =
									AnalysisTick + BestHitDelay;
								s_aAutoUnfreezeLaserExpireTick[DummyIndex] =
									s_aAutoUnfreezeLaserExpectedHitTick[DummyIndex] +
									PingUncertaintyTicks + 2;
								s_aAutoUnfreezeAttackTickAtShot[DummyIndex] =
									m_Snap.m_pLocalCharacter->m_AttackTick;
								s_aAutoUnfreezeConfirmDeadlineTick[DummyIndex] =
									Client()->GameTick(DummyIndex) +
									maximum(6, PingCompTicks * 2 + 4);
								s_aAutoUnfreezeLaserConfirmed[DummyIndex] = false;
								s_aAutoUnfreezeLaserDir[DummyIndex] = BestDir;
								const int SentGameTick = Client()->GameTick(DummyIndex);
								const int EstimatedServerShotTick =
									AnalysisTick - aArrivalLags[0];
								AutoUnfreezeStartShotObservation(
									DummyIndex, LocalId, AnalysisTick, SentGameTick,
									EstimatedServerShotTick,
									EstimatedServerShotTick + BestHitDelay,
									EstimatedServerShotTick + BestHitDelay +
										PingUncertaintyTicks + 2,
									m_Snap.m_pLocalCharacter->m_AttackTick,
									aExpectedShotOrigins, aArrivalLags, NumArrivalLags, BestDir,
									LearningKeyFor(BestDir, BestHitDelay, BestHitBounces),
									BestHitBounces, BestHitQuality);
							}

							ZzAutoUnfreezeOverridesInput = true;
						}
					}
				}

				if(m_pAutoUnfreezeDebugWriter &&
					m_aAutoUnfreezeDebugLastTick[DummyIndex] != AnalysisTick)
				{
					m_aAutoUnfreezeDebugLastTick[DummyIndex] = AnalysisTick;
					CJsonFileWriter &Writer = *m_pAutoUnfreezeDebugWriter;
					auto WriteVec100 = [&](const char *pName, const vec2 &Value) {
						Writer.WriteAttribute(pName);
						Writer.BeginArray();
						Writer.WriteIntValue(round_to_int(Value.x * 100.0f));
						Writer.WriteIntValue(round_to_int(Value.y * 100.0f));
						Writer.EndArray();
					};
					auto WriteInput = [&](const char *pName,
								  const CNetObj_PlayerInput &Input) {
						Writer.WriteAttribute(pName);
						Writer.BeginObject();
						Writer.WriteAttribute("direction");
						Writer.WriteIntValue(Input.m_Direction);
						Writer.WriteAttribute("jump");
						Writer.WriteIntValue(Input.m_Jump);
						Writer.WriteAttribute("hook");
						Writer.WriteIntValue(Input.m_Hook);
						Writer.WriteAttribute("fire");
						Writer.WriteIntValue(Input.m_Fire);
						Writer.WriteAttribute("target");
						Writer.BeginArray();
						Writer.WriteIntValue(Input.m_TargetX);
						Writer.WriteIntValue(Input.m_TargetY);
						Writer.EndArray();
						Writer.WriteAttribute("wanted_weapon");
						Writer.WriteIntValue(Input.m_WantedWeapon);
						Writer.EndObject();
					};

					const bool PredFrozenNow =
						pLocalPredChar && (pLocalPredChar->m_FreezeTime > 0 ||
									  pLocalPredChar->Core()->m_IsInFreeze);
					const bool ShotThisTick =
						s_aAutoUnfreezeLaserShotTick[DummyIndex] == AnalysisTick;
					const bool FreezeEntered =
						FrozenNow && !m_aAutoUnfreezeDebugPrevFrozen[DummyIndex];
					const bool FreezeExited =
						!FrozenNow && m_aAutoUnfreezeDebugPrevFrozen[DummyIndex];
					const bool FlightEnded = !s_aAutoUnfreezeLaserInFlight[DummyIndex] &&
								 m_aAutoUnfreezeDebugPrevInFlight[DummyIndex];
					const char *pEvent = ShotThisTick ? "shot" : FreezeExited ? "freeze_exit" :
									     FreezeEntered        ? "freeze_enter" :
									     FlightEnded          ? (FrozenNow ? "flight_end_frozen" : "flight_end_clear") :
												    "tick";

					Writer.BeginObject();
					Writer.WriteAttribute("tick");
					Writer.WriteIntValue(Client()->GameTick(DummyIndex));
					Writer.WriteAttribute("pred_tick");
					Writer.WriteIntValue(AnalysisTick);
					Writer.WriteAttribute("dummy");
					Writer.WriteIntValue(DummyIndex);
					Writer.WriteAttribute("event");
					Writer.WriteStrValue(pEvent);
					Writer.WriteAttribute("ping_ms");
					Writer.WriteIntValue(SnapshotPingMs);
					Writer.WriteAttribute("prediction_ms");
					Writer.WriteIntValue(PredictionTimeMs);
					Writer.WriteAttribute("prediction_margin_ms");
					Writer.WriteIntValue(g_Config.m_ClPredictionMargin);
					Writer.WriteAttribute("fast_input");
					Writer.WriteBoolValue(g_Config.m_TcFastInput != 0);
					Writer.WriteAttribute("fast_input_amount_ms");
					Writer.WriteIntValue(g_Config.m_TcFastInputAmount);
					Writer.WriteAttribute("prediction_error_x100");
					Writer.WriteIntValue(
						pLocalPredChar ? round_to_int(PredictionError * 100.0f) : -1);
					Writer.WriteAttribute("prediction_error_limit_x100");
					Writer.WriteIntValue(round_to_int(MaxPredictionError * 100.0f));
					Writer.WriteAttribute("prediction_coherent");
					Writer.WriteBoolValue(PredictionCoherent);
					WriteVec100("snap_pos_x100",
						vec2((float)m_Snap.m_pLocalCharacter->m_X,
							(float)m_Snap.m_pLocalCharacter->m_Y));
					WriteVec100("snap_vel_x100",
						vec2((float)m_Snap.m_pLocalCharacter->m_VelX,
							(float)m_Snap.m_pLocalCharacter->m_VelY));
					WriteVec100("pred_pos_x100", pLocalPredChar ? pLocalPredChar->Core()->m_Pos : LocalPos);
					WriteVec100("pred_vel_x100",
						pLocalPredChar ? pLocalPredChar->Core()->m_Vel : m_aClients[LocalId].m_RegularPredicted.m_Vel);
					WriteInput("original_input", OrigInput);
					WriteInput("sent_input", *pInput);
					const char *pDecisionBlockReason = "none";
					if(!g_Config.m_ClZzAutoUnfreeze)
						pDecisionBlockReason = "disabled";
					else if(!PredictionCoherent)
						pDecisionBlockReason = "prediction_incoherent";
					else if(FrozenNow || HardFrozen)
						pDecisionBlockReason = "already_frozen";
					else if(s_aAutoUnfreezeLaserInFlight[DummyIndex])
						pDecisionBlockReason = "laser_in_flight";
					else if(s_aAutoUnfreezeWillFreezeSoon[DummyIndex] &&
						ZzFentBotOverridesInput)
						pDecisionBlockReason = "input_override";
					else if(s_aAutoUnfreezeWillFreezeSoon[DummyIndex] &&
						pLocalPredChar && pLocalPredChar->GetReloadTimer() > 0)
						pDecisionBlockReason = "reload";
					else if(s_aAutoUnfreezeWillFreezeSoon[DummyIndex] &&
						!s_aAutoUnfreezeSolutionFound[DummyIndex])
						pDecisionBlockReason = AutoUnfreezeDebugExactTested > 0 ? "exact_rejected" : "no_candidate";
					Writer.WriteAttribute("state");
					Writer.BeginObject();
					Writer.WriteAttribute("auto_enabled");
					Writer.WriteBoolValue(g_Config.m_ClZzAutoUnfreeze != 0);
					Writer.WriteAttribute("frozen_snap");
					Writer.WriteBoolValue(FrozenNow);
					Writer.WriteAttribute("frozen_pred");
					Writer.WriteBoolValue(PredFrozenNow);
					Writer.WriteAttribute("hard_frozen");
					Writer.WriteBoolValue(HardFrozen);
					Writer.WriteAttribute("path_valid");
					Writer.WriteBoolValue(s_aAutoUnfreezePathValid[DummyIndex]);
					Writer.WriteAttribute("will_freeze_soon");
					Writer.WriteBoolValue(s_aAutoUnfreezeWillFreezeSoon[DummyIndex]);
					Writer.WriteAttribute("laser_in_flight");
					Writer.WriteBoolValue(s_aAutoUnfreezeLaserInFlight[DummyIndex]);
					Writer.WriteAttribute("laser_confirmed");
					Writer.WriteBoolValue(s_aAutoUnfreezeLaserConfirmed[DummyIndex]);
					Writer.WriteAttribute("weapon");
					Writer.WriteIntValue(pLocalPredChar ? pLocalPredChar->GetActiveWeapon() : m_Snap.m_pLocalCharacter->m_Weapon);
					Writer.WriteAttribute("reload_ticks");
					Writer.WriteIntValue(pLocalPredChar ? pLocalPredChar->GetReloadTimer() : -1);
					Writer.WriteAttribute("decision_block_reason");
					Writer.WriteStrValue(pDecisionBlockReason);
					Writer.EndObject();

					Writer.WriteAttribute("solution");
					Writer.BeginObject();
					Writer.WriteAttribute("found");
					Writer.WriteBoolValue(s_aAutoUnfreezeSolutionFound[DummyIndex]);
					Writer.WriteAttribute("dir_target");
					Writer.BeginArray();
					Writer.WriteIntValue(
						round_to_int(s_aAutoUnfreezeSolutionDir[DummyIndex].x * 1000.0f));
					Writer.WriteIntValue(
						round_to_int(s_aAutoUnfreezeSolutionDir[DummyIndex].y * 1000.0f));
					Writer.EndArray();
					Writer.WriteAttribute("hit_delay_ticks");
					Writer.WriteIntValue(s_aAutoUnfreezeSolutionHitDelay[DummyIndex]);
					Writer.WriteAttribute("shot_tick");
					Writer.WriteIntValue(s_aAutoUnfreezeLaserShotTick[DummyIndex]);
					Writer.WriteAttribute("expected_hit_tick");
					Writer.WriteIntValue(s_aAutoUnfreezeLaserExpectedHitTick[DummyIndex]);
					Writer.WriteAttribute("expire_tick");
					Writer.WriteIntValue(s_aAutoUnfreezeLaserExpireTick[DummyIndex]);
					Writer.WriteAttribute("estimated_server_shot_tick");
					Writer.WriteIntValue(
						s_aAutoUnfreezeEstimatedServerShotTick[DummyIndex]);
					Writer.WriteAttribute("prediction_lead_ticks");
					Writer.WriteIntValue(s_aAutoUnfreezePredictionLead[DummyIndex]);
					Writer.WriteAttribute("tested_prediction_leads");
					Writer.BeginArray();
					for(int Scenario = 0;
						Scenario < AutoUnfreezeDebugPredictionLeadCount; Scenario++)
						Writer.WriteIntValue(aAutoUnfreezeDebugPredictionLeads[Scenario]);
					Writer.EndArray();
					Writer.WriteAttribute("exact_tested");
					Writer.WriteIntValue(AutoUnfreezeDebugExactTested);
					Writer.WriteAttribute("exact_accepted_index");
					Writer.WriteIntValue(AutoUnfreezeDebugExactAccepted);
					Writer.WriteAttribute("lead0_hit_delay_ticks");
					Writer.WriteIntValue(AutoUnfreezeDebugPhase0HitDelay);
					Writer.WriteAttribute("lead1_hit_delay_ticks");
					Writer.WriteIntValue(AutoUnfreezeDebugPhase1HitDelay);
					Writer.WriteAttribute("lead2_hit_delay_ticks");
					Writer.WriteIntValue(AutoUnfreezeDebugPhase2HitDelay);
					Writer.EndObject();

					const SAutoUnfreezeShotObservation &Observation =
						m_aAutoUnfreezeShotObservation[DummyIndex];
					const char *pObservationResult = "pending";
					switch(Observation.m_Result)
					{
					case AUTO_UNFREEZE_OBS_NO_SERVER_SHOT:
						pObservationResult = "no_server_shot";
						break;
					case AUTO_UNFREEZE_OBS_TRAJECTORY_MISSED:
						pObservationResult = "trajectory_missed";
						break;
					case AUTO_UNFREEZE_OBS_HIT_BEFORE_FREEZE:
						pObservationResult = "hit_before_freeze";
						break;
					case AUTO_UNFREEZE_OBS_HIT_WHILE_FROZEN:
						pObservationResult = "success";
						break;
					case AUTO_UNFREEZE_OBS_STILL_FROZEN:
						pObservationResult = "still_frozen";
						break;
					default:
						break;
					}
					Writer.WriteAttribute("shot_observation");
					Writer.BeginObject();
					Writer.WriteAttribute("active");
					Writer.WriteBoolValue(Observation.m_Active);
					Writer.WriteAttribute("result");
					Writer.WriteIntValue(Observation.m_Result);
					Writer.WriteAttribute("result_name");
					Writer.WriteStrValue(pObservationResult);
					Writer.WriteAttribute("raw_matched");
					Writer.WriteBoolValue(Observation.m_RawMatched);
					Writer.WriteAttribute("attack_confirmed");
					Writer.WriteBoolValue(Observation.m_AttackConfirmed);
					Writer.WriteAttribute("estimated_server_shot_tick");
					Writer.WriteIntValue(Observation.m_EstimatedServerShotTick);
					Writer.WriteAttribute("observed_prediction_lead_ticks");
					Writer.WriteIntValue(Observation.m_ObservedPredictionLead);
					Writer.WriteAttribute("first_segment_outgoing");
					Writer.WriteBoolValue(Observation.m_FirstObservedSegmentOutgoing);
					Writer.WriteAttribute("freeze_seen");
					Writer.WriteBoolValue(Observation.m_FreezeSeen);
					Writer.WriteAttribute("crossed_before_freeze");
					Writer.WriteBoolValue(Observation.m_CrossedTeeBeforeFreeze);
					Writer.WriteAttribute("crossed_while_frozen");
					Writer.WriteBoolValue(Observation.m_CrossedTeeWhileFrozen);
					Writer.WriteAttribute("closest_segment_distance_x100");
					Writer.WriteIntValue(
						Observation.m_ClosestSegmentDistance < 0.0f ? -1 : round_to_int(Observation.m_ClosestSegmentDistance * 100.0f));
					Writer.WriteAttribute("observed_segments");
					Writer.WriteIntValue(Observation.m_NumSegments);
					Writer.WriteAttribute("planned_bounces");
					Writer.WriteIntValue(Observation.m_Bounces);
					Writer.WriteAttribute("planned_quality_x100");
					Writer.WriteIntValue(
						is_finite(Observation.m_PlannedQuality) ? round_to_int(Observation.m_PlannedQuality * 100.0f) : std::numeric_limits<int>::min());
					Writer.WriteAttribute("learning_bias_x100");
					Writer.WriteIntValue(round_to_int(
						AutoUnfreezeLearningBias(DummyIndex, Observation.m_LearningKey,
							AnalysisTick) *
						100.0f));
					Writer.WriteAttribute("calibrated_prediction_lead_x100");
					Writer.WriteIntValue(round_to_int(
						m_aAutoUnfreezeObservedPredictionLead[DummyIndex] * 100.0f));
					Writer.WriteAttribute("calibration_samples");
					Writer.WriteIntValue(m_aAutoUnfreezeObservedSamples[DummyIndex]);
					WriteVec100("calibrated_origin_offset_x100",
						m_aAutoUnfreezeObservedOriginOffset[DummyIndex]);
					Writer.WriteAttribute("segments");
					Writer.BeginArray();
					for(int SegmentIndex = 0; SegmentIndex < Observation.m_NumSegments;
						SegmentIndex++)
					{
						const SAutoUnfreezeObservedSegment &Segment =
							Observation.m_aSegments[SegmentIndex];
						Writer.BeginObject();
						Writer.WriteAttribute("index");
						Writer.WriteIntValue(Segment.m_SegmentIndex);
						Writer.WriteAttribute("start_tick");
						Writer.WriteIntValue(Segment.m_StartTick);
						Writer.WriteAttribute("returning");
						Writer.WriteBoolValue(Segment.m_Returning);
						Writer.WriteAttribute("tee_frozen");
						Writer.WriteBoolValue(Segment.m_TeeFrozen);
						Writer.WriteAttribute("crossed_while_frozen");
						Writer.WriteBoolValue(Segment.m_CrossedWhileFrozen);
						Writer.WriteAttribute("distance_to_tee_x100");
						Writer.WriteIntValue(
							Segment.m_DistanceToTee < 0.0f ? -1 : round_to_int(Segment.m_DistanceToTee * 100.0f));
						Writer.EndObject();
					}
					Writer.EndArray();
					Writer.EndObject();

					Writer.WriteAttribute("candidates");
					Writer.BeginArray();
					for(int CandidateIndex = 0;
						CandidateIndex < AutoUnfreezeDebugCandidateCount;
						CandidateIndex++)
					{
						Writer.BeginObject();
						Writer.WriteAttribute("dir_target");
						Writer.BeginArray();
						Writer.WriteIntValue(round_to_int(
							aAutoUnfreezeDebugCandidateDir[CandidateIndex].x * 1000.0f));
						Writer.WriteIntValue(round_to_int(
							aAutoUnfreezeDebugCandidateDir[CandidateIndex].y * 1000.0f));
						Writer.EndArray();
						Writer.WriteAttribute("hit_delay_ticks");
						Writer.WriteIntValue(
							aAutoUnfreezeDebugCandidateDelay[CandidateIndex]);
						Writer.WriteAttribute("quality_x100");
						Writer.WriteIntValue(
							is_finite(
								aAutoUnfreezeDebugCandidateQuality[CandidateIndex]) ?
								round_to_int(
									aAutoUnfreezeDebugCandidateQuality[CandidateIndex] *
									100.0f) :
								std::numeric_limits<int>::min());
						Writer.EndObject();
					}
					Writer.EndArray();

					Writer.WriteAttribute("predicted_path");
					Writer.BeginArray();
					if(m_AutoUnfreezeDebugFullRecording)
					{
						CGameWorld DebugWorld;
						DebugWorld.CopyWorldClean(&m_RegularPredictedWorld);
						DebugWorld.m_WorldConfig.m_IsDDRace = true;
						DebugWorld.m_WorldConfig.m_IsVanilla = false;
						DebugWorld.m_WorldConfig.m_PredictDDRace = true;
						DebugWorld.m_WorldConfig.m_PredictTiles = true;
						DebugWorld.m_WorldConfig.m_PredictFreeze = 1;
						DebugWorld.m_GameTick = AnalysisTick;
						CCharacter *pDebugChar = DebugWorld.GetCharacterById(LocalId);
						CNetObj_PlayerInput DebugInput = *pInput;
						if(DebugInput.m_Fire & 1)
							DebugInput.m_Fire = (DebugInput.m_Fire + 1) & INPUT_STATE_MASK;
						DebugInput.m_WantedWeapon = 0;
						DebugInput.m_NextWeapon = 0;
						DebugInput.m_PrevWeapon = 0;
						for(int Tick = 0; Tick < AutoUnfreezeMaxPredictTicks && pDebugChar;
							Tick++)
						{
							const bool TouchingFreeze = DmdIsFreezeAtPoint(
								Collision(), pDebugChar->Core()->m_Pos,
								&DebugWorld.Switchers(), pDebugChar->Team());
							const bool Frozen = pDebugChar->m_FreezeTime > 0 ||
									    pDebugChar->Core()->m_IsInFreeze;
							Writer.BeginObject();
							Writer.WriteAttribute("dt");
							Writer.WriteIntValue(Tick);
							WriteVec100("pos_x100", pDebugChar->Core()->m_Pos);
							WriteVec100("vel_x100", pDebugChar->Core()->m_Vel);
							Writer.WriteAttribute("frozen");
							Writer.WriteBoolValue(Frozen);
							Writer.WriteAttribute("touching_freeze");
							Writer.WriteBoolValue(TouchingFreeze);
							Writer.WriteAttribute("freeze_time");
							Writer.WriteIntValue(pDebugChar->m_FreezeTime);
							Writer.WriteAttribute("deep_frozen");
							Writer.WriteBoolValue(pDebugChar->Core()->m_DeepFrozen);
							Writer.WriteAttribute("live_frozen");
							Writer.WriteBoolValue(pDebugChar->Core()->m_LiveFrozen);
							Writer.WriteAttribute("weapon");
							Writer.WriteIntValue(pDebugChar->GetActiveWeapon());
							Writer.WriteAttribute("reload_ticks");
							Writer.WriteIntValue(pDebugChar->GetReloadTimer());
							Writer.WriteAttribute("hook_state");
							Writer.WriteIntValue(pDebugChar->Core()->m_HookState);
							Writer.WriteAttribute("hooked_player");
							Writer.WriteIntValue(pDebugChar->Core()->HookedPlayer());
							WriteVec100("hook_pos_x100", pDebugChar->Core()->m_HookPos);
							Writer.EndObject();

							pDebugChar->OnPredictedInput(&DebugInput);
							DebugWorld.m_GameTick = AnalysisTick + Tick + 1;
							DebugWorld.Tick();
							pDebugChar = DebugWorld.GetCharacterById(LocalId);
						}
					}
					else
					{
						const int DebugPathTicks =
							(ShotThisTick || AutoUnfreezeDebugCandidateCount > 0) ? minimum(s_aAutoUnfreezePredictionTicks[DummyIndex], 48) : 0;
						for(int Tick = 0; Tick < DebugPathTicks; Tick++)
						{
							Writer.BeginObject();
							Writer.WriteAttribute("dt");
							Writer.WriteIntValue(Tick);
							WriteVec100("pos_x100", pAutoUnfreezePredPos[Tick]);
							Writer.WriteAttribute("can_unfreeze");
							Writer.WriteBoolValue(pAutoUnfreezePredCanUnfreeze[Tick]);
							Writer.EndObject();
						}
					}
					Writer.EndArray();

					Writer.WriteAttribute("players");
					Writer.BeginArray();
					if(m_AutoUnfreezeDebugFullRecording)
					{
						for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
						{
							const bool SnapActive = m_Snap.m_aCharacters[ClientId].m_Active;
							CCharacter *pWorldChar =
								m_RegularPredictedWorld.GetCharacterById(ClientId);
							if(!SnapActive && !pWorldChar)
								continue;
							Writer.BeginObject();
							Writer.WriteAttribute("id");
							Writer.WriteIntValue(ClientId);
							Writer.WriteAttribute("snap_active");
							Writer.WriteBoolValue(SnapActive);
							Writer.WriteAttribute("pred_active");
							Writer.WriteBoolValue(pWorldChar != nullptr);
							if(SnapActive)
							{
								WriteVec100(
									"snap_pos_x100",
									vec2((float)m_Snap.m_aCharacters[ClientId].m_Cur.m_X,
										(float)m_Snap.m_aCharacters[ClientId].m_Cur.m_Y));
								WriteVec100(
									"snap_vel_x100",
									vec2((float)m_Snap.m_aCharacters[ClientId].m_Cur.m_VelX,
										(float)m_Snap.m_aCharacters[ClientId].m_Cur.m_VelY));
								Writer.WriteAttribute("snap_weapon");
								Writer.WriteIntValue(
									m_Snap.m_aCharacters[ClientId].m_Cur.m_Weapon);
								Writer.WriteAttribute("snap_attack_tick");
								Writer.WriteIntValue(
									m_Snap.m_aCharacters[ClientId].m_Cur.m_AttackTick);
							}
							if(pWorldChar)
							{
								WriteVec100("pred_pos_x100", pWorldChar->Core()->m_Pos);
								WriteVec100("pred_vel_x100", pWorldChar->Core()->m_Vel);
								Writer.WriteAttribute("team");
								Writer.WriteIntValue(pWorldChar->Team());
								Writer.WriteAttribute("weapon");
								Writer.WriteIntValue(pWorldChar->GetActiveWeapon());
								Writer.WriteAttribute("reload_ticks");
								Writer.WriteIntValue(pWorldChar->GetReloadTimer());
								Writer.WriteAttribute("freeze_time");
								Writer.WriteIntValue(pWorldChar->m_FreezeTime);
								Writer.WriteAttribute("in_freeze");
								Writer.WriteBoolValue(pWorldChar->Core()->m_IsInFreeze);
								Writer.WriteAttribute("deep_frozen");
								Writer.WriteBoolValue(pWorldChar->Core()->m_DeepFrozen);
								Writer.WriteAttribute("live_frozen");
								Writer.WriteBoolValue(pWorldChar->Core()->m_LiveFrozen);
								Writer.WriteAttribute("hook_state");
								Writer.WriteIntValue(pWorldChar->Core()->m_HookState);
								Writer.WriteAttribute("hooked_player");
								Writer.WriteIntValue(pWorldChar->Core()->HookedPlayer());
							}
							Writer.WriteAttribute("client_freeze_end");
							Writer.WriteIntValue(m_aClients[ClientId].m_FreezeEnd);
							Writer.WriteAttribute("solo");
							Writer.WriteBoolValue(m_aClients[ClientId].m_Solo);
							Writer.WriteAttribute("collision_disabled");
							Writer.WriteBoolValue(m_aClients[ClientId].m_CollisionDisabled);
							Writer.EndObject();
						}
					}
					Writer.EndArray();

					Writer.WriteAttribute("world_entities");
					Writer.BeginArray();
					if(m_AutoUnfreezeDebugFullRecording)
					{
						for(int EntityType = 0; EntityType < CGameWorld::NUM_ENTTYPES;
							EntityType++)
						{
							for(CEntity *pEntity =
									m_RegularPredictedWorld.FindFirst(EntityType);
								pEntity; pEntity = pEntity->TypeNext())
							{
								Writer.BeginObject();
								Writer.WriteAttribute("type");
								Writer.WriteIntValue(EntityType);
								Writer.WriteAttribute("id");
								Writer.WriteIntValue(pEntity->GetId());
								WriteVec100("pos_x100", pEntity->GetPos());
								Writer.EndObject();
							}
						}
					}
					Writer.EndArray();

					Writer.WriteAttribute("switchers_local_team");
					Writer.BeginArray();
					if(m_AutoUnfreezeDebugFullRecording && pLocalPredChar)
					{
						const int LocalTeam =
							std::clamp(pLocalPredChar->Team(), 0, NUM_DDRACE_TEAMS - 1);
						const auto &vSwitchers = m_RegularPredictedWorld.Switchers();
						for(size_t SwitchIndex = 0; SwitchIndex < vSwitchers.size();
							SwitchIndex++)
						{
							const SSwitchers &Switcher = vSwitchers[SwitchIndex];
							Writer.BeginObject();
							Writer.WriteAttribute("number");
							Writer.WriteIntValue((int)SwitchIndex);
							Writer.WriteAttribute("status");
							Writer.WriteBoolValue(Switcher.m_aStatus[LocalTeam]);
							Writer.WriteAttribute("end_tick");
							Writer.WriteIntValue(Switcher.m_aEndTick[LocalTeam]);
							Writer.WriteAttribute("type");
							Writer.WriteIntValue(Switcher.m_aType[LocalTeam]);
							Writer.WriteAttribute("last_update_tick");
							Writer.WriteIntValue(Switcher.m_aLastUpdateTick[LocalTeam]);
							Writer.EndObject();
						}
					}
					Writer.EndArray();

					Writer.WriteAttribute("lasers");
					Writer.BeginArray();
					int LaserCount = 0;
					for(CEntity *pEntity =
							m_RegularPredictedWorld.FindFirst(CGameWorld::ENTTYPE_LASER);
						pEntity && LaserCount < 16;
						pEntity = pEntity->TypeNext(), LaserCount++)
					{
						const CLaser *pLaser = static_cast<const CLaser *>(pEntity);
						Writer.BeginObject();
						Writer.WriteAttribute("owner");
						Writer.WriteIntValue(pLaser->GetOwner());
						Writer.WriteAttribute("eval_tick");
						Writer.WriteIntValue(pLaser->GetEvalTick());
						WriteVec100("from_x100", pLaser->GetFrom());
						WriteVec100("to_x100", pLaser->GetPos());
						Writer.EndObject();
					}
					Writer.EndArray();
					Writer.EndObject();

					m_aAutoUnfreezeDebugPrevFrozen[DummyIndex] = FrozenNow;
					m_aAutoUnfreezeDebugPrevInFlight[DummyIndex] =
						s_aAutoUnfreezeLaserInFlight[DummyIndex];
				}
			}

			// KernelNet: hammer-gun
			if(g_Config.m_ClZzHammerGun && !ZzFentBotOverridesInput &&
				!ZzAutoUnfreezeOverridesInput)
			{
				const int LocalId = m_Snap.m_LocalClientId;
				CCharacter *pRegPredChar =
					m_RegularPredictedWorld.GetCharacterById(LocalId);
				bool HasExtraWeapons = false;
				if(pRegPredChar)
				{
					for(int w = 0; w < NUM_WEAPONS; w++)
					{
						if(w == WEAPON_HAMMER || w == WEAPON_GUN)
							continue;
						if(pRegPredChar->GetWeaponGot(w))
						{
							HasExtraWeapons = true;
							break;
						}
					}
				}
				if(!(g_Config.m_ClZzHammerGunAutoDisableExtraWeapons &&
					   HasExtraWeapons))
				{
					static int s_aPrevUserFire[NUM_DUMMIES] = {0};
					static int s_aHammerGunFire[NUM_DUMMIES] = {0};
					static int s_aHammerGunStartTick[NUM_DUMMIES] = {-1, -1, -1};
					static int s_aHammerGunCooldownUntil[NUM_DUMMIES] = {-1, -1, -1};
					const int DummyIndex = g_Config.m_ClDummy;
					const int NowTick = Client()->GameTick(DummyIndex);
					const int PrevFire = s_aPrevUserFire[DummyIndex];
					const int CurFire = pInput->m_Fire;
					const int Presses = CountInput(PrevFire, CurFire).m_Presses;
					const bool Pressed = Presses > 0;
					const bool Cooldown =
						s_aHammerGunCooldownUntil[DummyIndex] >= NowTick;

					// Default: keep gun in hand and do not force firing.
					pInput->m_WantedWeapon = WEAPON_GUN + 1;

					// Start sequence only on press (not on hold) and not during cooldown.
					if(Pressed && !Cooldown)
					{
						s_aHammerGunStartTick[DummyIndex] = NowTick;
						// Short cooldown so holding doesn't spam.
						s_aHammerGunCooldownUntil[DummyIndex] = NowTick + 12;
					}

					// State machine:
					// 0) switch-to-hammer tick (suppress fire)
					// 1) switch-to-hammer tick (suppress fire)
					// 2) strike tick (fire with hammer)
					// 3) restore gun (handled by default)
					if(s_aHammerGunStartTick[DummyIndex] >= 0)
					{
						const int PhaseTick = NowTick - s_aHammerGunStartTick[DummyIndex];
						if(PhaseTick <= 1)
						{
							// Switch tick(s): force hammer + CANCEL user +fire so the server
							// never sees gun firing.
							pInput->m_WantedWeapon = WEAPON_HAMMER + 1;
							pInput->m_Fire = PrevFire & INPUT_STATE_MASK;
							ZzAutoUnfreezeOverridesInput = true;
						}
						else if(PhaseTick == 2)
						{
							// Strike tick
							pInput->m_WantedWeapon = WEAPON_HAMMER + 1;
							int Fire = s_aHammerGunFire[DummyIndex];
							Fire = (Fire + 2) | 1;
							Fire &= INPUT_STATE_MASK;
							s_aHammerGunFire[DummyIndex] = Fire;
							pInput->m_Fire = Fire;
							ZzAutoUnfreezeOverridesInput = true;
						}
						else
						{
							s_aHammerGunStartTick[DummyIndex] = -1;
						}
					}
					// Keep CountInput consistent with the final (possibly overridden)
					// fire value.
					s_aPrevUserFire[DummyIndex] = pInput->m_Fire;
				}
			}

			if(!HookAimApplied)
				HookAimApplied = m_ZZHookAimbot.Apply(pInput, DummyIndex,
					LocalPos, CurAim, ZzAutoUnfreezeOverridesInput);
			if(HookAimApplied)
			{
				ZzAutoUnfreezeOverridesInput = true;
				if(g_Config.m_ClZzAimbotSilent)
				{
					// Only the outgoing packet receives silent aim. Keep the local
					// input/cursor untouched to prevent a one-frame mirrored snap.
					m_Controls.m_aInputData[DummyIndex].m_TargetX = OrigInput.m_TargetX;
					m_Controls.m_aInputData[DummyIndex].m_TargetY = OrigInput.m_TargetY;
				}
			}

			// Hook Drive: while the physical hook key is held, rapidly release and
			// re-fire at the nearest hookable block. A tee selected by Aimbot always
			// has priority. Direction/jump input is deliberately left untouched.
			if(g_Config.m_ClZzFlyRide && OrigInput.m_Hook && !HookAimApplied &&
				!SaveInBlockActive && !ZzAutoUnfreezeOverridesInput)
			{
				const float HookLength = (float)m_aTuning[DummyIndex].m_HookLength;
				vec2 BestDirection;
				float BestDistance = 1e18f;
				constexpr int DRIVE_RAYS = 192;
				for(int Ray = 0; Ray < DRIVE_RAYS; ++Ray)
				{
					const float Angle = 2.0f * pi * (float)Ray / (float)DRIVE_RAYS;
					const vec2 Direction = direction(Angle);
					const vec2 Start = LocalPos + Direction *
									      CCharacterCore::PhysicalSize() * 1.5f;
					const vec2 End = LocalPos + Direction * HookLength;
					vec2 HitPos;
					int TeleNr = 0;
					const int Hit = Collision()->IntersectLineTeleHook(Start, End,
						&HitPos, nullptr, &TeleNr);
					if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK)
						continue;
					const float HitDistance = distance(LocalPos, HitPos);
					if(HitDistance < BestDistance)
					{
						BestDistance = HitDistance;
						BestDirection = Direction;
					}
				}
				if(BestDistance < 1e18f)
				{
					const int Tick = Client()->PredGameTick(DummyIndex);
					pInput->m_Hook = (Tick & 1) == 0 ? 0 : 1;
					pInput->m_TargetX = round_to_int(BestDirection.x * 1000.0f);
					pInput->m_TargetY = round_to_int(BestDirection.y * 1000.0f);
					if(!pInput->m_TargetX && !pInput->m_TargetY)
						pInput->m_TargetX = 1;
					m_Controls.m_aInputData[DummyIndex].m_TargetX = OrigInput.m_TargetX;
					m_Controls.m_aInputData[DummyIndex].m_TargetY = OrigInput.m_TargetY;
					ZzAutoUnfreezeOverridesInput = true;
				}
			}

			if(g_Config.m_ClKnGrenadeSave && !ZzAutoUnfreezeOverridesInput)
			{
				CCharacter *pLocalPred = m_RegularPredictedWorld.GetCharacterById(LocalId);
				static int s_aKnGrenadeAnalysisTick[NUM_DUMMIES] = {-1, -1, -1};
				static bool s_aKnGrenadeSolutionValid[NUM_DUMMIES] = {false, false, false};
				static vec2 s_aKnGrenadeAim[NUM_DUMMIES] = {
					vec2(0.0f, -1.0f), vec2(0.0f, -1.0f), vec2(0.0f, -1.0f)};
				static int s_aKnGrenadeFire[NUM_DUMMIES] = {0, 0, 0};
				static bool s_aKnGrenadeFireInitialized[NUM_DUMMIES] = {false, false, false};
				static bool s_aKnGrenadeInjected[NUM_DUMMIES] = {false, false, false};
				static int s_aKnGrenadeInjectedTick[NUM_DUMMIES] = {-1, -1, -1};
				static int s_aKnGrenadeNoHazardTicks[NUM_DUMMIES] = {0};
				static bool s_aKnGrenadeShotForHazard[NUM_DUMMIES] = {false, false, false};
				const int NowTick = Client()->PredGameTick(DummyIndex);
				if(!s_aKnGrenadeFireInitialized[DummyIndex])
				{
					s_aKnGrenadeFire[DummyIndex] = pInput->m_Fire & INPUT_STATE_MASK;
					s_aKnGrenadeFireInitialized[DummyIndex] = true;
				}

				bool GrenadePacketOwned = false;
				if(s_aKnGrenadeInjected[DummyIndex])
				{
					if(NowTick == s_aKnGrenadeInjectedTick[DummyIndex])
					{
						pInput->m_Fire = s_aKnGrenadeFire[DummyIndex];
						pInput->m_WantedWeapon = WEAPON_GRENADE + 1;
						pInput->m_TargetX = round_to_int(
							s_aKnGrenadeAim[DummyIndex].x * 1000.0f);
						pInput->m_TargetY = round_to_int(
							s_aKnGrenadeAim[DummyIndex].y * 1000.0f);
					}
					else
					{
						s_aKnGrenadeFire[DummyIndex] =
							(s_aKnGrenadeFire[DummyIndex] + 1) & INPUT_STATE_MASK;
						pInput->m_Fire = s_aKnGrenadeFire[DummyIndex];
						s_aKnGrenadeInjected[DummyIndex] = false;
						s_aKnGrenadeInjectedTick[DummyIndex] = -1;
					}
					GrenadePacketOwned = true;
					ZzAutoUnfreezeOverridesInput = true;
				}

				const bool AlreadyFrozen = pLocalPred &&
							   (pLocalPred->Core()->m_IsInFreeze || pLocalPred->m_FreezeTime > 0);
				if(s_aKnGrenadeAnalysisTick[DummyIndex] != NowTick)
				{
					s_aKnGrenadeAnalysisTick[DummyIndex] = NowTick;
					s_aKnGrenadeSolutionValid[DummyIndex] = false;
					bool WillFreeze = false;
					int FreezeTick = 0;
					vec2 FreezeEntry = LocalPos;
					constexpr int MaxSimulationTicks = 48;
					vec2 aBaselinePos[MaxSimulationTicks + 1];
					aBaselinePos[0] = LocalPos;

					auto ConfigureSaveWorld = [&](CGameWorld &World) {
						World.m_WorldConfig.m_IsDDRace = true;
						World.m_WorldConfig.m_IsVanilla = false;
						World.m_WorldConfig.m_PredictDDRace = true;
						World.m_WorldConfig.m_PredictTiles = true;
						World.m_WorldConfig.m_PredictFreeze = 1;
						World.m_GameTick = NowTick;
					};
					auto PrepareMovementInput = [&](CNetObj_PlayerInput Input) {
						if(Input.m_Fire & 1)
							Input.m_Fire = (Input.m_Fire + 1) & INPUT_STATE_MASK;
						Input.m_WantedWeapon = 0;
						Input.m_NextWeapon = 0;
						Input.m_PrevWeapon = 0;
						return Input;
					};
					auto IsFrozenInWorld = [&](CGameWorld &World, CCharacter *pCharacter) {
						return !pCharacter || pCharacter->m_FreezeTime > 0 ||
						       pCharacter->Core()->m_IsInFreeze ||
						       DmdIsFreezeNearTee(Collision(), pCharacter->Core()->m_Pos,
							       &World.Switchers(), pCharacter->Team());
					};

					if(pLocalPred && !AlreadyFrozen)
					{
						CGameWorld BaselineWorld;
						BaselineWorld.CopyWorldClean(&m_RegularPredictedWorld);
						ConfigureSaveWorld(BaselineWorld);
						CCharacter *pBaselineChar =
							BaselineWorld.GetCharacterById(LocalId);
						CNetObj_PlayerInput BaselineInput = PrepareMovementInput(*pInput);
						for(int Tick = 1; pBaselineChar && Tick <= MaxSimulationTicks;
							Tick++)
						{
							pBaselineChar->OnPredictedInput(&BaselineInput);
							BaselineWorld.m_GameTick = NowTick + Tick;
							BaselineWorld.Tick();
							pBaselineChar = BaselineWorld.GetCharacterById(LocalId);
							if(!pBaselineChar)
								break;
							aBaselinePos[Tick] = pBaselineChar->Core()->m_Pos;
							if(IsFrozenInWorld(BaselineWorld, pBaselineChar))
							{
								WillFreeze = true;
								FreezeTick = Tick;
								FreezeEntry = pBaselineChar->Core()->m_Pos;
								break;
							}
						}
					}

					bool MovementCanSave = false;
					const int InputLeadTicks = std::clamp(
						(maximum(m_Snap.m_pLocalInfo ? m_Snap.m_pLocalInfo->m_Latency : 0,
							 Client()->GetPredictionTime()) *
								Client()->GameTickSpeed() +
							1999) /
							2000,
						0, 10);
					const int TriggerTicks = 12 + InputLeadTicks;
					if(WillFreeze && FreezeTick <= TriggerTicks)
					{
						const int MovementTicks = minimum(MaxSimulationTicks,
							FreezeTick + 16);
						for(int Direction = -1; Direction <= 1 && !MovementCanSave;
							Direction++)
						{
							for(int Jump = 0; Jump <= 1 && !MovementCanSave; Jump++)
							{
								CNetObj_PlayerInput MoveInput =
									PrepareMovementInput(*pInput);
								MoveInput.m_Direction = Direction;
								if(Jump)
								{
									if(MoveInput.m_Jump & 1)
										MoveInput.m_Jump =
											(MoveInput.m_Jump + 1) & INPUT_STATE_MASK;
									MoveInput.m_Jump =
										(MoveInput.m_Jump + 1) & INPUT_STATE_MASK;
								}
								CGameWorld MoveWorld;
								MoveWorld.CopyWorldClean(&m_RegularPredictedWorld);
								ConfigureSaveWorld(MoveWorld);
								CCharacter *pMoveChar = MoveWorld.GetCharacterById(LocalId);
								bool Safe = pMoveChar != nullptr;
								for(int Tick = 1; Safe && Tick <= MovementTicks; Tick++)
								{
									pMoveChar->OnPredictedInput(&MoveInput);
									MoveWorld.m_GameTick = NowTick + Tick;
									MoveWorld.Tick();
									pMoveChar = MoveWorld.GetCharacterById(LocalId);
									Safe = pMoveChar &&
									       !IsFrozenInWorld(MoveWorld, pMoveChar);
								}
								MovementCanSave = Safe;
							}
						}
					}

					if(WillFreeze && FreezeTick <= TriggerTicks && !MovementCanSave &&
						pLocalPred && pLocalPred->GetWeaponGot(WEAPON_GRENADE) &&
						pLocalPred->GetWeaponAmmo(WEAPON_GRENADE) != 0)
					{
						const CTuningParams &Tuning = m_aTuning[DummyIndex];
						const float Curvature = (float)Tuning.m_GrenadeCurvature;
						const float Speed = (float)Tuning.m_GrenadeSpeed;
						const vec2 Movement =
							m_aClients[LocalId].m_RegularPredicted.m_Vel;
						const vec2 MovementDir = length(Movement) > 0.25f ?
										 normalize(Movement) :
										 CurAim;
						float BestScore = std::numeric_limits<float>::infinity();
						vec2 BestDirection(0.0f, -1.0f);
						constexpr int DirectionSamples = 96;
						for(int Sample = 0; Sample < DirectionSamples; Sample++)
						{
							const vec2 Direction = direction(
								2.0f * pi * (float)Sample / (float)DirectionSamples);
							const vec2 ShotOrigin = LocalPos + Direction *
												   CCharacterCore::PhysicalSize() * 0.75f;
							vec2 ImpactPos;
							int ImpactTick = -1;
							vec2 Previous = ShotOrigin;
							for(int Tick = 1; Tick <= FreezeTick; Tick++)
							{
								const float Time =
									(float)Tick / (float)Client()->GameTickSpeed();
								const vec2 Next = CalcPos(ShotOrigin, Direction,
									Curvature, Speed, Time);
								vec2 BeforeImpact;
								if(Collision()->IntersectLine(Previous, Next, &ImpactPos,
									   &BeforeImpact) != 0)
								{
									ImpactTick = Tick;
									break;
								}
								Previous = Next;
							}
							if(ImpactTick < 1)
								continue;

							const vec2 ImpactTeePos =
								aBaselinePos[minimum(ImpactTick, FreezeTick)];
							if(distance(ImpactTeePos, ImpactPos) > 135.0f)
								continue;
							vec2 AwayFromFreeze = ImpactTeePos - FreezeEntry;
							if(length(AwayFromFreeze) < 0.001f)
								AwayFromFreeze = -MovementDir;
							else
								AwayFromFreeze = normalize(AwayFromFreeze);
							vec2 ExplosionForce = ImpactTeePos - ImpactPos;
							if(length(ExplosionForce) < 0.001f)
								continue;
							ExplosionForce = normalize(ExplosionForce);
							const float ForceSafety = dot(ExplosionForce, AwayFromFreeze);
							if(ForceSafety <= 0.05f)
								continue;

							CGameWorld ShotWorld;
							ShotWorld.CopyWorldClean(&m_RegularPredictedWorld);
							ConfigureSaveWorld(ShotWorld);
							for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
							{
								if(ClientId == LocalId)
									continue;
								if(CCharacter *pOther =
										ShotWorld.GetCharacterById(ClientId))
									delete pOther;
							}
							CCharacter *pShotChar = ShotWorld.GetCharacterById(LocalId);
							if(!pShotChar)
								continue;
							const int GrenadeLife = maximum(1, (int)((float)Tuning.m_GrenadeLifetime *
												   Client()->GameTickSpeed()));
							new CProjectile(&ShotWorld, WEAPON_GRENADE, LocalId,
								ShotOrigin, Direction, GrenadeLife, false, true,
								SOUND_GRENADE_EXPLODE);
							CNetObj_PlayerInput ShotInput = PrepareMovementInput(*pInput);
							const int ShotSimulationTicks = minimum(MaxSimulationTicks,
								FreezeTick + 18);
							bool Safe = true;
							vec2 EndPos = LocalPos;
							for(int Tick = 1; Safe && Tick <= ShotSimulationTicks; Tick++)
							{
								pShotChar->OnPredictedInput(&ShotInput);
								ShotWorld.m_GameTick = NowTick + Tick;
								ShotWorld.Tick();
								pShotChar = ShotWorld.GetCharacterById(LocalId);
								Safe = pShotChar &&
								       !IsFrozenInWorld(ShotWorld, pShotChar);
								if(pShotChar)
									EndPos = pShotChar->Core()->m_Pos;
							}
							if(!Safe)
								continue;

							const vec2 SurfaceDelta = ImpactPos - ImpactTeePos;
							int SurfacePriority = 2;
							if(SurfaceDelta.y < -8.0f)
								SurfacePriority = 0;
							else if(dot(normalize(SurfaceDelta), MovementDir) > 0.35f)
								SurfacePriority = 1;
							else if(SurfaceDelta.y > 8.0f)
								SurfacePriority = 3;
							const float Score = -ForceSafety * 5000.0f +
									    SurfacePriority * 500.0f + ImpactTick * 18.0f -
									    distance(EndPos, FreezeEntry) * 0.5f;
							if(Score < BestScore)
							{
								BestScore = Score;
								BestDirection = Direction;
							}
						}
						if(BestScore < std::numeric_limits<float>::infinity())
						{
							s_aKnGrenadeSolutionValid[DummyIndex] = true;
							s_aKnGrenadeAim[DummyIndex] = BestDirection;
						}
					}

					if(WillFreeze)
						s_aKnGrenadeNoHazardTicks[DummyIndex] = 0;
					else if(++s_aKnGrenadeNoHazardTicks[DummyIndex] >= 6)
						s_aKnGrenadeShotForHazard[DummyIndex] = false;
				}

				if(!GrenadePacketOwned && !AlreadyFrozen && pLocalPred &&
					s_aKnGrenadeSolutionValid[DummyIndex] &&
					!s_aKnGrenadeShotForHazard[DummyIndex] &&
					pLocalPred->GetReloadTimer() <= 0)
				{
					pInput->m_WantedWeapon = WEAPON_GRENADE + 1;
					pInput->m_TargetX = round_to_int(
						s_aKnGrenadeAim[DummyIndex].x * 1000.0f);
					pInput->m_TargetY = round_to_int(
						s_aKnGrenadeAim[DummyIndex].y * 1000.0f);
					if(!pInput->m_TargetX && !pInput->m_TargetY)
						pInput->m_TargetY = 1;
					int Fire = s_aKnGrenadeFire[DummyIndex] & INPUT_STATE_MASK;
					if(Fire & 1)
						Fire = (Fire + 1) & INPUT_STATE_MASK;
					Fire = (Fire + 1) & INPUT_STATE_MASK;
					s_aKnGrenadeFire[DummyIndex] = Fire;
					pInput->m_Fire = Fire;
					s_aKnGrenadeInjected[DummyIndex] = true;
					s_aKnGrenadeInjectedTick[DummyIndex] = NowTick;
					s_aKnGrenadeShotForHazard[DummyIndex] = true;
					m_Controls.m_aInputData[DummyIndex].m_TargetX = OrigInput.m_TargetX;
					m_Controls.m_aInputData[DummyIndex].m_TargetY = OrigInput.m_TargetY;
					ZzAutoUnfreezeOverridesInput = true;
				}
			}

			if(g_Config.m_ClKnIdentitySteal)
			{
				g_Config.m_ClKnIdentitySteal = 0;
				int BestId = -1;
				float BestDistance = 1e18f;
				for(int Id = 0; Id < MAX_CLIENTS; ++Id)
				{
					if(Id == LocalId || !m_Snap.m_aCharacters[Id].m_Active)
						continue;
					const float Dist = distance(LocalPos, m_aClients[Id].m_RegularPredicted.m_Pos);
					if(Dist < BestDistance)
					{
						BestDistance = Dist;
						BestId = Id;
					}
				}
				if(BestId >= 0)
				{
					char aName[MAX_NAME_LENGTH];
					str_format(aName, sizeof(aName), "%s.", m_aClients[BestId].m_aName);
					str_copy(g_Config.m_PlayerName, aName);
					str_copy(g_Config.m_ClPlayerSkin, m_aClients[BestId].m_aSkinName);
					g_Config.m_ClPlayerUseCustomColor = m_aClients[BestId].m_UseCustomColor;
					g_Config.m_ClPlayerColorBody = m_aClients[BestId].m_ColorBody;
					g_Config.m_ClPlayerColorFeet = m_aClients[BestId].m_ColorFeet;
					SendInfo(false);
				}
			}

			// Auto-hit / auto-aled: hammer aim assist
			// Auto ALED is a one-tick action and must be allowed to take priority over
			// hook/tile aim. Those helpers also set ZzAutoUnfreezeOverridesInput even
			// though they do not own the fire button, which previously disabled ALED
			// completely while a hook helper was active.
			if(g_Config.m_ClZzAutoAled ||
				(g_Config.m_ClZzHammerAssistEnabled && !ZzAutoUnfreezeOverridesInput))
			{
				auto IsFreezeTile = [&](int Tile) {
					return Tile == TILE_FREEZE || Tile == TILE_DFREEZE ||
					       Tile == TILE_LFREEZE;
				};
				auto IsFreezeAt = [&](const vec2 &Pos) {
					const int PureIndex = Collision()->GetPureMapIndex(Pos);
					if(PureIndex < 0)
						return false;
					const int Tile = Collision()->GetTileIndex(PureIndex);
					const int FTile = Collision()->GetFrontTileIndex(PureIndex);
					return IsFreezeTile(Tile) || IsFreezeTile(FTile);
				};
				auto IsNormalFreezeAt = [&](const vec2 &Pos, int Team) {
					const int MapIndex = Collision()->GetPureMapIndex(Pos);
					if(MapIndex < 0)
						return false;
					if(Collision()->GetTileIndex(MapIndex) == TILE_FREEZE ||
						Collision()->GetFrontTileIndex(MapIndex) == TILE_FREEZE)
						return true;
					if(Collision()->GetSwitchType(MapIndex) != TILE_FREEZE)
						return false;
					const int Number = Collision()->GetSwitchNumber(MapIndex);
					const auto &vSwitchers = m_RegularPredictedWorld.Switchers();
					return Number == 0 ||
					       (Number > 0 && Number < (int)vSwitchers.size() && Team >= 0 &&
						       Team < NUM_DDRACE_TEAMS && vSwitchers[Number].m_aStatus[Team]);
				};
				auto HasNormalFreezeBetween = [&](const vec2 &From, const vec2 &To,
								      int Team) {
					const float Dist = distance(From, To);
					if(Dist < 1.0f)
						return false;
					const int Steps = std::clamp((int)ceilf(Dist / 3.0f), 2, 128);
					for(int Step = 1; Step < Steps; Step++)
						if(IsNormalFreezeAt(mix(From, To, (float)Step / (float)Steps), Team))
							return true;
					return false;
				};
				auto HasFreezeBetween = [&](const vec2 &From, const vec2 &To) {
					const float Dist = distance(From, To);
					if(Dist < 1.0f)
						return false;
					const int Steps = std::clamp((int)round_to_int(Dist / 8.0f), 2, 256);
					for(int s = 1; s < Steps; s++)
					{
						const float a = (float)s / (float)Steps;
						const vec2 P = mix(From, To, a);
						if(IsFreezeAt(P))
							return true;
					}
					return false;
				};
				auto HammerCanHit = [&](CCharacter *pPredChar, const vec2 &FromPos,
							    int TargetId, const vec2 &TargetPos,
							    vec2 *pOutDir = nullptr) {
					if(!pPredChar || TargetId < 0 || TargetId >= MAX_CLIENTS)
						return false;
					const float Prox = CCharacterCore::PhysicalSize();
					vec2 Dir = TargetPos - FromPos;
					const float Dist = length(Dir);
					if(Dist < 0.001f)
						return false;
					Dir *= 1.0f / Dist;
					const vec2 ProjStartPos = FromPos + Dir * Prox * 0.75f;
					CEntity *apEnts[MAX_CLIENTS];
					const int Num = m_RegularPredictedWorld.FindEntities(
						ProjStartPos, Prox * 0.5f, apEnts, MAX_CLIENTS,
						CGameWorld::ENTTYPE_CHARACTER);
					bool FoundTarget = false;
					for(int n = 0; n < Num; n++)
					{
						auto *pTarget = static_cast<CCharacter *>(apEnts[n]);
						if(!pTarget)
							continue;
						if(pTarget->GetCid() != TargetId)
							continue;
						if(pTarget == pPredChar || !pPredChar->CanCollide(TargetId))
							continue;
						FoundTarget = true;
						break;
					}
					if(!FoundTarget)
						return false;
					if(pOutDir)
						*pOutDir = Dir;
					return true;
				};
				auto HammerCanHitAny = [&](CCharacter *pPredChar, const vec2 &FromPos,
							       int TargetId, const vec2 &TargetPos,
							       vec2 *pOutDir = nullptr) {
					const float Prox = CCharacterCore::PhysicalSize();
					const vec2 aAimPoints[] = {
						TargetPos,
						TargetPos + vec2(0.0f, Prox * 0.5f),
						TargetPos - vec2(0.0f, Prox * 0.5f),
						TargetPos + vec2(Prox * 0.35f, 0.0f),
						TargetPos - vec2(Prox * 0.35f, 0.0f),
						TargetPos + vec2(Prox * 0.22f, Prox * 0.35f),
						TargetPos + vec2(-Prox * 0.22f, Prox * 0.35f),
						TargetPos + vec2(Prox * 0.22f, -Prox * 0.35f),
						TargetPos + vec2(-Prox * 0.22f, -Prox * 0.35f),
					};
					for(const vec2 &AimPoint : aAimPoints)
					{
						vec2 Dir;
						if(HammerCanHit(pPredChar, FromPos, TargetId, AimPoint, &Dir))
						{
							if(pOutDir)
								*pOutDir = Dir;
							return true;
						}
					}
					return false;
				};
				auto *pLocalPredChar =
					m_RegularPredictedWorld.GetCharacterById(LocalId);
				const int HammerReloadTicks =
					pLocalPredChar ? pLocalPredChar->GetReloadTimer() : 0;
				const bool HammerReady = !pLocalPredChar || HammerReloadTicks <= 0;
				const bool HammerAllowed =
					!pLocalPredChar || !pLocalPredChar->HammerHitDisabled();
				const bool LocalSoloPart = LocalId >= 0 && m_aClients[LocalId].m_Solo;

				// Keep our own fire counter so we can generate actual presses every
				// tick. Otherwise, Controls::SnapInput would re-create the same m_Fire
				// value every tick while holding, and our packet modification would not
				// advance across ticks.
				static int s_aAutoHitFire[NUM_DUMMIES] = {0};
				static bool s_aAutoHitFireInitialized[NUM_DUMMIES] = {false, false, false};
				static int s_aAutoAledCooldownUntil[NUM_DUMMIES] = {0};
				static bool s_aAutoAledInjected[NUM_DUMMIES] = {false, false, false};
				static int s_aAutoAledInjectedTick[NUM_DUMMIES] = {-1, -1, -1};
				static vec2 s_aAutoAledInjectedDir[NUM_DUMMIES] = {
					vec2(1.0f, 0.0f), vec2(1.0f, 0.0f), vec2(1.0f, 0.0f)};
				static int s_aAutoAledLockedTarget[NUM_DUMMIES] = {-1, -1, -1};
				const int DummyIndex = g_Config.m_ClDummy;
				const bool WantsAutoHit = g_Config.m_ClZzHammerAssistEnabled != 0;
				const bool WantsAutoAled = g_Config.m_ClZzAutoAled != 0;
				const bool UserHoldingFire = (pInput->m_Fire & 1) != 0;
				const int HookedCid = m_Snap.m_pLocalCharacter ? m_Snap.m_pLocalCharacter->m_HookedPlayer : -1;
				const int SnapshotTick = Client()->GameTick(DummyIndex);
				const int PredictedTick = Client()->PredGameTick(DummyIndex);
				const bool LocalFrozen = m_aClients[LocalId].m_DeepFrozen ||
							 m_aClients[LocalId].m_LiveFrozen ||
							 m_aClients[LocalId].m_FreezeEnd > SnapshotTick ||
							 (pLocalPredChar && pLocalPredChar->m_FreezeTime > 0);
				bool AutoAledPacketOwned = false;
				if(!s_aAutoHitFireInitialized[DummyIndex])
				{
					s_aAutoHitFire[DummyIndex] = pInput->m_Fire & INPUT_STATE_MASK;
					s_aAutoHitFireInitialized[DummyIndex] = true;
				}
				if(s_aAutoAledInjected[DummyIndex])
				{
					if(PredictedTick == s_aAutoAledInjectedTick[DummyIndex])
					{
						pInput->m_Fire = s_aAutoHitFire[DummyIndex];
						pInput->m_WantedWeapon = WEAPON_HAMMER + 1;
						pInput->m_TargetX = round_to_int(
							s_aAutoAledInjectedDir[DummyIndex].x * 1000.0f);
						pInput->m_TargetY = round_to_int(
							s_aAutoAledInjectedDir[DummyIndex].y * 1000.0f);
						AutoAledPacketOwned = true;
					}
					else
					{
						// Send and persist the matching release on the following input tick.
						// Keeping the press odd for one full tick is understood both by DDNet's
						// CountInput implementation and by older/custom 0.6 servers that only
						// inspect the current fire state.
						int Fire = s_aAutoHitFire[DummyIndex] & INPUT_STATE_MASK;
						if(Fire & 1)
							Fire = (Fire + 1) & INPUT_STATE_MASK;
						s_aAutoHitFire[DummyIndex] = Fire;
						pInput->m_Fire = Fire;
						m_Controls.m_aInputData[DummyIndex].m_Fire = Fire;
						s_aAutoAledInjected[DummyIndex] = false;
						s_aAutoAledInjectedTick[DummyIndex] = -1;
						AutoAledPacketOwned = true;
					}
				}

				auto TryFindAutoAledTarget = [&](vec2 &OutDir) -> bool {
					if(!HammerAllowed || LocalFrozen)
						return false;

					float BestScore = 1e18f;
					vec2 BestDir = CurAim;
					int BestTarget = -1;
					bool Found = false;
					const float Prox = CCharacterCore::PhysicalSize();
					auto CanHammerPredicted = [&](const vec2 &FromPos, int TargetId,
									  const vec2 &TargetPos,
									  vec2 &OutHitDir) {
						if(pLocalPredChar && !pLocalPredChar->CanCollide(TargetId))
							return false;
						vec2 Dir = TargetPos - FromPos;
						const float Dist = length(Dir);
						if(Dist < 0.001f)
							return false;
						Dir *= 1.0f / Dist;
						const vec2 ProjStartPos = FromPos + Dir * Prox * 0.75f;
						// Exact server hammer reach: FindEntities checks the query radius
						// plus the target tee's proximity radius with a strict '<'.
						if(distance(TargetPos, ProjStartPos) >= Prox * 1.5f)
							return false;
						OutHitDir = Dir;
						return true;
					};

					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(i == LocalId)
							continue;
						if(!m_Snap.m_aCharacters[i].m_Active)
							continue;
						// Auto ALED is DDNet wall-hammering, not the FNG thaw mechanic.
						// A finite running timer proves that the tee is frozen and can be
						// unfrozen by a hammer. Deep/live freeze must never be selected.
						if(m_aClients[i].m_DeepFrozen || m_aClients[i].m_LiveFrozen)
							continue;
						CCharacter *pTargetPred =
							m_RegularPredictedWorld.GetCharacterById(i);
						const bool SnapshotTimerRunning =
							m_aClients[i].m_FreezeEnd > SnapshotTick;
						const bool PredictedTimerRunning =
							pTargetPred && pTargetPred->m_FreezeTime > 0;
						if(!SnapshotTimerRunning && !PredictedTimerRunning)
							continue;
						const vec2 TargetPos = m_aClients[i].m_RegularPredicted.m_Pos;
						const int TargetTeam = pTargetPred ? pTargetPred->Team() : m_Teams.Team(i);
						// HandleTiles applies normal freeze at the character's map index.
						// Requiring the whole tee body to clear the tile makes the valid
						// hammer window impossible on many one-tile FNG freeze bands.
						if(IsNormalFreezeAt(TargetPos, TargetTeam))
							continue;
						if(!HasNormalFreezeBetween(LocalPos, TargetPos, TargetTeam))
							continue;

						vec2 To;
						if(!CanHammerPredicted(LocalPos, i, TargetPos, To))
							continue;
						const float Angle =
							acosf(std::clamp(dot(CurAim, To), -1.0f, 1.0f));
						float Score = distance(LocalPos, TargetPos) + Angle * 40.0f;
						if(i == s_aAutoAledLockedTarget[DummyIndex])
							Score -= 10000.0f;
						if(Score < BestScore)
						{
							BestScore = Score;
							BestDir = To;
							BestTarget = i;
							Found = true;
						}
					}
					if(Found)
					{
						OutDir = BestDir;
						s_aAutoAledLockedTarget[DummyIndex] = BestTarget;
					}
					else
						s_aAutoAledLockedTarget[DummyIndex] = -1;
					return Found;
				};

				auto TryFindAutoHitTarget = [&](vec2 &OutDir) -> bool {
					float BestScore = 1e18f;
					vec2 BestDir = CurAim;
					bool Found = false;
					const float Prox = CCharacterCore::PhysicalSize();
					const float AimOffset = Prox * 0.5f;
					const float HitRange = Prox * 1.5f - 2.0f;
					if(LocalSoloPart)
						return false;
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(i == LocalId)
							continue;
						if(!m_Snap.m_aCharacters[i].m_Active)
							continue;
						if(!m_Teams.SameTeam(LocalId, i))
							continue;
						if(m_aClients[i].m_Solo)
							continue;
						if(g_Config.m_ClZzHammerAssistIgnoreFriends &&
							m_aClients[i].m_Friend)
							continue;

						const vec2 SnapTargetPos =
							vec2((float)m_Snap.m_aCharacters[i].m_Cur.m_X,
								(float)m_Snap.m_aCharacters[i].m_Cur.m_Y);
						const vec2 PredTargetPos = m_aClients[i].m_RegularPredicted.m_Pos;
						const vec2 PredTargetVel = m_aClients[i].m_RegularPredicted.m_Vel;
						const vec2 aAimPoints[] = {
							SnapTargetPos,
							SnapTargetPos + vec2(0.0f, AimOffset),
							SnapTargetPos - vec2(0.0f, AimOffset),
							SnapTargetPos + vec2(AimOffset * 0.35f, 0.0f),
							SnapTargetPos - vec2(AimOffset * 0.35f, 0.0f),
							SnapTargetPos + vec2(0.0f, AimOffset * 0.8f) +
								vec2(AimOffset * 0.25f, 0.0f),
							SnapTargetPos + vec2(0.0f, AimOffset * 0.8f) -
								vec2(AimOffset * 0.25f, 0.0f),
							SnapTargetPos - vec2(0.0f, AimOffset * 0.8f) +
								vec2(AimOffset * 0.25f, 0.0f),
							SnapTargetPos - vec2(0.0f, AimOffset * 0.8f) -
								vec2(AimOffset * 0.25f, 0.0f),
							PredTargetPos,
							PredTargetPos + vec2(0.0f, AimOffset),
							PredTargetPos - vec2(0.0f, AimOffset),
							PredTargetPos + PredTargetVel * 1.0f,
							PredTargetPos + PredTargetVel * 1.0f +
								vec2(0.0f, AimOffset * 0.75f),
							PredTargetPos + PredTargetVel * 1.0f -
								vec2(0.0f, AimOffset * 0.75f),
							PredTargetPos + PredTargetVel * 2.0f,
							PredTargetPos + PredTargetVel * 2.0f +
								vec2(0.0f, AimOffset * 0.65f),
							PredTargetPos + PredTargetVel * 2.0f -
								vec2(0.0f, AimOffset * 0.65f),
							PredTargetPos + PredTargetVel * 3.0f,
						};

						for(const vec2 &AimPoint : aAimPoints)
						{
							vec2 To = AimPoint - LocalPos;
							const float Dist = length(To);
							if(Dist < 0.001f)
								continue;
							To *= 1.0f / Dist;
							const vec2 ProjStartPos = LocalPos + To * Prox * 0.75f;
							if(distance(AimPoint, ProjStartPos) > HitRange)
								continue;
							vec2 HitPos;
							if(Collision()->IntersectLine(ProjStartPos, AimPoint, &HitPos,
								   nullptr) != 0)
								continue;
							if(HasFreezeBetween(LocalPos, AimPoint))
								continue;
							if(pLocalPredChar && !pLocalPredChar->CanCollide(i))
								continue;
							vec2 ExactTo;
							if(!HammerCanHitAny(pLocalPredChar, LocalPos, i, AimPoint,
								   &ExactTo))
								continue;

							const float Angle =
								acosf(std::clamp(dot(CurAim, ExactTo), -1.0f, 1.0f));
							const float AimPenalty = distance(PredTargetPos, AimPoint) * 2.0f;
							float Score = Angle * 1000.0f + Dist + AimPenalty;
							if(distance(AimPoint, SnapTargetPos) < 0.001f)
								Score -= 24.0f;
							if(i == HookedCid)
								Score -= 1500.0f;
							if(Score < BestScore)
							{
								BestScore = Score;
								BestDir = ExactTo;
								Found = true;
							}
						}
					}
					if(Found)
						OutDir = BestDir;
					return Found;
				};

				bool Found = false;
				bool FoundByAutoAled = false;
				vec2 BestDir = CurAim;
				if(!AutoAledPacketOwned && WantsAutoAled && PredictedTick >= s_aAutoAledCooldownUntil[DummyIndex])
				{
					Found = TryFindAutoAledTarget(BestDir);
					FoundByAutoAled = Found;
				}
				if(!AutoAledPacketOwned && !Found && WantsAutoHit && UserHoldingFire &&
					!ZzAutoUnfreezeOverridesInput &&
					m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER)
					Found = TryFindAutoHitTarget(BestDir);

				if(Found)
				{
					pInput->m_TargetX = (int)round_to_int(BestDir.x * 1000.0f);
					pInput->m_TargetY = (int)round_to_int(BestDir.y * 1000.0f);
					if(!pInput->m_TargetX && !pInput->m_TargetY)
						pInput->m_TargetX = 1;
					// Auto-ALED should always remain silent (no camera/mouse snap).
					if(!g_Config.m_ClZzHammerAssistSilent && !FoundByAutoAled)
						m_Controls.m_aMousePos[DummyIndex] = BestDir * 1000.0f;
					if(FoundByAutoAled)
					{
						// Arm the hammer as soon as a valid tee is behind the freeze band.
						// This also handles servers where switching and firing in the same
						// packet is less reliable than stock DDNet.
						pInput->m_WantedWeapon = WEAPON_HAMMER + 1;
						pInput->m_NextWeapon = 0;
						pInput->m_PrevWeapon = 0;
					}

					if(HammerReady && HammerAllowed)
					{
						int Fire = s_aAutoHitFire[DummyIndex] & INPUT_STATE_MASK;
						// Generate a canonical released->pressed transition and keep it
						// pressed for one complete input tick. The next tick sends release.
						if(Fire & 1)
							Fire = (Fire + 1) & INPUT_STATE_MASK;
						Fire = (Fire + 1) & INPUT_STATE_MASK;
						s_aAutoHitFire[DummyIndex] = Fire;
						pInput->m_Fire = Fire;
						if(FoundByAutoAled)
						{
							m_Controls.m_aInputData[DummyIndex].m_Fire = Fire;
							s_aAutoAledInjected[DummyIndex] = true;
							s_aAutoAledInjectedTick[DummyIndex] = PredictedTick;
							s_aAutoAledInjectedDir[DummyIndex] = BestDir;
							const int HammerDelayTicks = maximum(
								1, (int)ceilf(m_aTuning[DummyIndex].GetWeaponFireDelay(
										      WEAPON_HAMMER) *
									      Client()->GameTickSpeed()));
							s_aAutoAledCooldownUntil[DummyIndex] =
								Client()->PredGameTick(DummyIndex) + HammerDelayTicks;
						}
					}
					else
					{
						int Fire = s_aAutoHitFire[DummyIndex];
						if((Fire & 1) != 0)
							Fire++;
						Fire &= INPUT_STATE_MASK;
						s_aAutoHitFire[DummyIndex] = Fire;
						pInput->m_Fire = Fire;
					}
				}
				else if(!AutoAledPacketOwned)
				{
					s_aAutoHitFire[DummyIndex] = pInput->m_Fire;
				}
			}
		}
		}

		// Kinetix input ownership is evaluated exactly once, on the final packet.
		// CControls has assembled current input and all KernelNet helpers above have
		// finished, so prediction cannot be cached from a stale pre-helper input.
		if(!m_FentBotActive)
		{
			const int AvoidDummyIndex = g_Config.m_ClDummy;
			const int AvoidLocalId = m_Snap.m_pLocalCharacter ?
				m_Snap.m_LocalClientId :
				-1;
			if(AvoidLocalId >= 0 && AvoidAllowed)
			{
				bool FinalAvoidOverride = false;
				ApplyAvoid(pInput, AvoidDummyIndex, AvoidLocalId,
					FinalAvoidOverride);
			}
			ApplyKinetixLaserUnfreeze(pInput, AvoidDummyIndex,
				AvoidLocalId);
		}
		else
		{
			ResetKinetixLaserUnfreeze();
		}
		return Size;
	}
	const int PairConn = Client()->DummyPair();
	if(Conn != PairConn)
	{
		const CNetObj_PlayerInput &InactiveInput = m_Controls.m_aInputData[Conn];
		if(!Force && !InactiveInput.m_Direction && !InactiveInput.m_Jump &&
			!InactiveInput.m_Hook && !(InactiveInput.m_Fire & 1))
			return 0;
		mem_copy(pData, &InactiveInput, sizeof(InactiveInput));
		return sizeof(InactiveInput);
	}

	if(m_aLocalIds[PairConn] < 0)
	{
		return 0;
	}
	m_DummyInputConn = PairConn;

	if(g_Config.m_ClZzFlyRide && m_FlyRideWasActive &&
		Conn == m_FlyRideTargetConn)
	{
		CNetObj_PlayerInput Out = m_DummyInput;
		Out.m_Direction = m_FlyRideDummyDirection;
		Out.m_Jump = 0;
		Out.m_Hook = m_FlyRideDummyHook;
		Out.m_NextWeapon = 0;
		Out.m_PrevWeapon = 0;
		// Both local connections are sent with the active connection's predicted
		// tick. The inactive dummy's own PredGameTick can remain unchanged, which
		// would otherwise stop the hammer state machine after its first update.
		const int Tick = Client()->PredGameTick(g_Config.m_ClDummy);
		CCharacter *pDummyCharacter =
			m_RegularPredictedWorld.GetCharacterById(m_aLocalIds[Conn]);
		const bool HammerReady = !pDummyCharacter ||
					 (pDummyCharacter->GetReloadTimer() <= 0 &&
						 !pDummyCharacter->HammerHitDisabled());
		if(Tick != m_FlyRideLastFireTick)
		{
			m_FlyRideLastFireTick = Tick;
			const bool Strike = m_FlyRideDummyHammer && HammerReady &&
					    Tick >= m_FlyRideDummyNextHitTick;
			if(Strike)
			{
				if((m_FlyRideDummyFire & 1) != 0)
					m_FlyRideDummyFire++;
				m_FlyRideDummyFire++;
				const int HammerDelay = maximum(
					1, (int)ceilf(m_aTuning[Conn].GetWeaponFireDelay(WEAPON_HAMMER) *
						      Client()->GameTickSpeed()));
				m_FlyRideDummyNextHitTick = Tick + HammerDelay;
			}
			else if((m_FlyRideDummyFire & 1) != 0)
				m_FlyRideDummyFire++;
			m_FlyRideDummyFire &= INPUT_STATE_MASK;
		}
		Out.m_Fire = m_FlyRideDummyFire;
		Out.m_WantedWeapon = WEAPON_HAMMER + 1;
		const vec2 Aim = m_FlyRidePilotPos - m_FlyRideDummyPos;
		Out.m_TargetX = (int)Aim.x;
		Out.m_TargetY = (int)Aim.y;
		m_DummyInput = Out;
		m_Controls.m_aInputData[Conn] = Out;
		mem_copy(pData, &Out, sizeof(Out));
		return sizeof(Out);
	}

	if(g_Config.m_ClZzCompanionEnabled && g_Config.m_ClZzCompanionPyInfer &&
		Client()->DummyConnected())
	{
		static int64_t s_CompanionSnapDbg = 0;
		auto SnapDbg = [&](const char *pMsg) {
			const int64_t Now = time_get();
			if(s_CompanionSnapDbg != 0 && Now - s_CompanionSnapDbg < time_freq())
				return;
			s_CompanionSnapDbg = Now;
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/companion", pMsg);
		};

		const int DummyTee = PairConn;
		const int LocalTee = g_Config.m_ClDummy;
		if(m_aLocalIds[DummyTee] >= 0 && m_aLocalIds[LocalTee] >= 0)
		{
			const int DummyCid = m_aLocalIds[DummyTee];
			const int OwnerCid = m_aLocalIds[LocalTee];
			const vec2 DummyPos = m_aClients[DummyCid].m_RegularPredicted.m_Pos;
			const vec2 DummyVel = m_aClients[DummyCid].m_RegularPredicted.m_Vel;
			const vec2 OwnerPos = m_aClients[OwnerCid].m_RegularPredicted.m_Pos;
			const float FollowR =
				(float)std::clamp(g_Config.m_ClZzCompanionFollowTiles, 0, 30) * 32.0f;
			vec2 Goal = DummyPos;
			const float Dist = distance(DummyPos, OwnerPos);
			if(Dist > FollowR)
				Goal = OwnerPos;
			char aDbg[256];
			str_format(aDbg, sizeof(aDbg),
				"DummyConnected=1 cl_dummy=%d localIdMain=%d localIdDummy=%d "
				"dist=%.1f goalToOwner=%d",
				g_Config.m_ClDummy, m_aLocalIds[LocalTee],
				m_aLocalIds[DummyTee], Dist, Dist > FollowR ? 1 : 0);
			SnapDbg(aDbg);

			int ActionId = -1;
			if(CompanionPyInferAction(g_Config.m_ClDummy, DummyPos, DummyVel, Goal,
				   ActionId))
			{
				char aDbg2[128];
				str_format(aDbg2, sizeof(aDbg2), "inference ok action=%d", ActionId);
				SnapDbg(aDbg2);
				static const vec2 s_aAimCands5[] = {
					vec2(0.0f, -1.0f),
					normalize(vec2(-0.4f, -1.0f)),
					normalize(vec2(0.4f, -1.0f)),
					vec2(-1.0f, 0.0f),
					vec2(1.0f, 0.0f),
				};
				const int AimCount = (int)std::size(s_aAimCands5);
				const int AimIndex = ActionId % AimCount;
				int Tmp = ActionId / AimCount;
				const int HookBit = Tmp & 1;
				Tmp >>= 1;
				const int JumpBit = Tmp & 1;
				Tmp >>= 1;
				const int DirIndex = Tmp;
				const int Dir = std::clamp(DirIndex - 1, -1, 1);

				CNetObj_PlayerInput Out = m_DummyInput;
				Out.m_PlayerFlags = m_DummyInput.m_PlayerFlags;
				Out.m_Direction = Dir;
				const vec2 OutAim = s_aAimCands5[AimIndex];
				Out.m_TargetX = (int)round_to_int(OutAim.x * 1000.0f);
				Out.m_TargetY = (int)round_to_int(OutAim.y * 1000.0f);
				if(!Out.m_TargetX && !Out.m_TargetY)
					Out.m_TargetY = -1;
				Out.m_Hook = HookBit ? 1 : 0;
				if(JumpBit)
					Out.m_Jump = (Out.m_Jump + 2) | 1;
				mem_copy(pData, &Out, sizeof(Out));
				return sizeof(Out);
			}
			else
			{
				SnapDbg("inference failed");
			}
		}
		else
		{
			SnapDbg("companion: local ids not ready");
		}
	}

	static vec2 s_aFlyStrikeDir[NUM_DUMMIES] = {
		vec2(0.0f, -1.0f), vec2(0.0f, -1.0f), vec2(0.0f, -1.0f)};
	static int s_aFlyCooldownCalls[NUM_DUMMIES] = {0, 0, 0};
	static int s_aFlyFire[NUM_DUMMIES] = {0, 0, 0};
	static bool s_aFlyWasDummyHammer[NUM_DUMMIES] = {false, false, false};
	static bool s_aHookFlyHooking[NUM_DUMMIES] = {false, false, false};
	const int ActiveDummy = g_Config.m_ClDummy;
	const bool DummyHammerMode =
		!g_Config.m_ClZzFlyRide && g_Config.m_ClDummyHammer != 0 && !m_IsDummySwapping;
	const bool FlyHelper =
		!g_Config.m_ClZzFlyRide && g_Config.m_ClZzFlyHelperEnabled != 0 &&
		!m_IsDummySwapping;
	const bool HookFlyOn = !g_Config.m_ClZzFlyRide &&
			       g_Config.m_ClZzHookFly != 0 && !m_IsDummySwapping;
	const bool TripleFlyOn =
		!g_Config.m_ClZzFlyRide && g_Config.m_ClZzTripleFly != 0 && DummyHammerMode;
	if(!HookFlyOn)
		s_aHookFlyHooking[ActiveDummy] = false;

	// Hook fly from the supplied OnSnapInput: while the dummy is above the
	// local tee it hooks the local tee, and releases once it falls below. Keep
	// the previous state inside the 8 px dead zone to avoid hook flicker.
	if(HookFlyOn)
	{
		const int MainCid = m_aLocalIds[g_Config.m_ClDummy];
		const int DummyCid = m_aLocalIds[PairConn];
		if(MainCid >= 0 && MainCid < MAX_CLIENTS && DummyCid >= 0 &&
			DummyCid < MAX_CLIENTS && m_Snap.m_aCharacters[MainCid].m_Active &&
			m_Snap.m_aCharacters[DummyCid].m_Active)
		{
			const vec2 MainPos = m_aClients[MainCid].m_RegularPredicted.m_Pos;
			const vec2 DummyPos = m_aClients[DummyCid].m_RegularPredicted.m_Pos;
			if(DummyPos.y < MainPos.y - 4.0f)
				s_aHookFlyHooking[ActiveDummy] = true;
			else if(DummyPos.y > MainPos.y + 4.0f)
				s_aHookFlyHooking[ActiveDummy] = false;

			CNetObj_PlayerInput Out = m_DummyInput;
			if((Out.m_Fire & 1) != 0)
				Out.m_Fire++;
			Out.m_Fire &= INPUT_STATE_MASK;
			Out.m_Hook = s_aHookFlyHooking[ActiveDummy] ? 1 : 0;
			if(Out.m_Hook)
			{
				vec2 ToMain = MainPos - DummyPos;
				if(length(ToMain) > 0.001f)
					ToMain = normalize(ToMain) * 1000.0f;
				Out.m_TargetX = (int)ToMain.x;
				Out.m_TargetY = (int)ToMain.y;
				if(!Out.m_TargetX && !Out.m_TargetY)
					Out.m_TargetY = 1;
			}
			if(!Force && !Out.m_Direction && !Out.m_Jump && !Out.m_Hook &&
				!Out.m_Fire)
				return 0;
			mem_copy(pData, &Out, sizeof(Out));
			return sizeof(Out);
		}
	}
	if(!DummyHammerMode && !FlyHelper)
	{
		s_aFlyWasDummyHammer[ActiveDummy] = false;
		s_aFlyCooldownCalls[ActiveDummy] = 0;
		s_aFlyFire[ActiveDummy] = m_DummyInput.m_Fire & INPUT_STATE_MASK;
		if(m_DummyFire != 0)
		{
			m_DummyInput.m_Fire = (m_HammerInput.m_Fire + 1) & ~1;
			m_DummyFire = 0;
		}

		if(!Force && (!m_DummyInput.m_Direction && !m_DummyInput.m_Jump &&
				     !m_DummyInput.m_Hook))
		{
			return 0;
		}

		mem_copy(pData, &m_DummyInput, sizeof(m_DummyInput));
		return sizeof(m_DummyInput);
	}
	else
	{
		bool InRange = false;
		bool DoStrikeNow = false;
		const int DummyTee = PairConn;
		const int LocalTee = g_Config.m_ClDummy;
		const int DummyCid = m_aLocalIds[DummyTee];
		const int LocalCid = m_aLocalIds[LocalTee];
		const float FlyHitRange =
			(float)std::clamp(g_Config.m_ClZzFlyHelperHitTiles, 1, 80) * 32.0f /
			10.0f;
		CCharacter *pDummyPredChar =
			m_RegularPredictedWorld.GetCharacterById(m_aLocalIds[DummyTee]);
		const bool DummyHammerAllowed =
			!pDummyPredChar || !pDummyPredChar->HammerHitDisabled();
		auto HammerDelayCalls = [&]() {
			const int HitDelayMs = std::clamp(g_Config.m_ClZzFlyHelperHitMs, 0, 1000);
			if(HitDelayMs > 0)
				return maximum(1,
					(HitDelayMs * Client()->GameTickSpeed() + 999) / 1000);
			return maximum(
				1, (int)ceilf(m_aTuning[DummyTee].GetWeaponFireDelay(WEAPON_HAMMER) *
					      Client()->GameTickSpeed()));
		};
		auto HammerCanHit = [&](CCharacter *pPredChar, const vec2 &FromPos,
					    int TargetId, const vec2 &TargetPos,
					    vec2 *pOutDir = nullptr) {
			if(TargetId < 0 || TargetId >= MAX_CLIENTS)
				return false;
			if(pPredChar &&
				(pPredChar->HammerHitDisabled() || !pPredChar->CanCollide(TargetId)))
				return false;
			const float Prox = CCharacterCore::PhysicalSize();
			vec2 Dir = TargetPos - FromPos;
			const float Dist = length(Dir);
			if(Dist < 0.001f)
				return false;
			Dir *= 1.0f / Dist;
			const vec2 ProjStartPos = FromPos + Dir * Prox * 0.75f;
			const CCharacter *pTargetPredChar =
				m_RegularPredictedWorld.GetCharacterById(TargetId);
			const float TargetRadius =
				pTargetPredChar ? pTargetPredChar->GetProximityRadius() : Prox;
			const float HitRadius =
				maximum(Prox * 0.5f + TargetRadius +
						(g_Config.m_ClZzFlyHelperRelaxed ? 4.0f : 1.5f),
					FlyHitRange);
			if(distance(TargetPos, ProjStartPos) > HitRadius)
				return false;
			vec2 HitPos;
			if(Collision()->IntersectLine(ProjStartPos, TargetPos, &HitPos,
				   nullptr) != 0)
				return false;
			if(pOutDir)
				*pOutDir = Dir;
			return true;
		};
		if(m_DummyFire == 0)
			s_aFlyWasDummyHammer[ActiveDummy] = false;
		if(!s_aFlyWasDummyHammer[ActiveDummy])
		{
			s_aFlyCooldownCalls[ActiveDummy] = 0;
			s_aFlyFire[ActiveDummy] = m_DummyInput.m_Fire & INPUT_STATE_MASK;
		}
		s_aFlyWasDummyHammer[ActiveDummy] = true;
		if(s_aFlyCooldownCalls[ActiveDummy] > 0)
			s_aFlyCooldownCalls[ActiveDummy]--;
		if(DummyHammerMode && !FlyHelper && DummyCid >= 0 && LocalCid >= 0)
		{
			const vec2 DummyPos = m_aClients[DummyCid].m_RegularPredicted.m_Pos;
			const vec2 LocalPos = m_aClients[LocalCid].m_RegularPredicted.m_Pos;
			const vec2 ToLocal = LocalPos - DummyPos;
			if(length(ToLocal) > 0.001f)
				s_aFlyStrikeDir[ActiveDummy] = normalize(ToLocal);

			DoStrikeNow = DummyHammerAllowed && s_aFlyCooldownCalls[ActiveDummy] <= 0;
			if(DoStrikeNow)
				s_aFlyCooldownCalls[ActiveDummy] = 25;
		}
		if(FlyHelper)
		{
			if(DummyCid >= 0 && LocalCid >= 0 &&
				(DummyHammerAllowed || g_Config.m_ClZzFlyHelperRelaxed))
			{
				const vec2 DummyPos = m_aClients[DummyCid].m_RegularPredicted.m_Pos;
				const vec2 DummyVel = m_aClients[DummyCid].m_RegularPredicted.m_Vel;
				float BestScore = 1e18f;
				vec2 BestDir = s_aFlyStrikeDir[ActiveDummy];
				auto ConsiderLocalTee = [&](int LeadTicks, const vec2 &PredDummyPos) {
					if(!m_Snap.m_aCharacters[LocalCid].m_Active &&
						!m_RegularPredictedWorld.GetCharacterById(LocalCid))
						return;
					const vec2 TargetPos = m_aClients[LocalCid].m_RegularPredicted.m_Pos;
					const vec2 TargetVel = m_aClients[LocalCid].m_RegularPredicted.m_Vel;
					const vec2 PredTargetPos = TargetPos + TargetVel * (float)LeadTicks;
					vec2 HitDir;
					if(!HammerCanHit(pDummyPredChar, PredDummyPos, LocalCid,
						   PredTargetPos, &HitDir))
						return;
					const float RelativeSpeed = length(TargetVel - DummyVel);
					const float Score = LeadTicks * 14.0f +
							    distance(PredDummyPos, PredTargetPos) -
							    RelativeSpeed * 0.2f;
					if(Score < BestScore)
					{
						BestScore = Score;
						BestDir = HitDir;
						InRange = true;
					}
				};
				for(int LeadTicks = 0; LeadTicks <= 3; LeadTicks++)
				{
					const vec2 PredDummyPos = DummyPos + DummyVel * (float)LeadTicks;
					ConsiderLocalTee(LeadTicks, PredDummyPos);
				}
				if(InRange)
					s_aFlyStrikeDir[ActiveDummy] = BestDir;
				DoStrikeNow = InRange &&
					      (DummyHammerAllowed || g_Config.m_ClZzFlyHelperRelaxed) &&
					      s_aFlyCooldownCalls[ActiveDummy] <= 0;
				if(DoStrikeNow)
				{
					s_aFlyCooldownCalls[ActiveDummy] = HammerDelayCalls();
				}
			}
		}
		m_HammerInput.m_Direction = m_DummyInput.m_Direction;
		m_HammerInput.m_Jump = m_DummyInput.m_Jump;
		m_HammerInput.m_Hook = m_DummyInput.m_Hook;
		m_HammerInput.m_PlayerFlags = m_DummyInput.m_PlayerFlags;
		m_HammerInput.m_TargetX = m_DummyInput.m_TargetX;
		m_HammerInput.m_TargetY = m_DummyInput.m_TargetY;
		if(!m_HammerInput.m_TargetX && !m_HammerInput.m_TargetY)
			m_HammerInput.m_TargetY = -1;
		m_HammerInput.m_NextWeapon = m_DummyInput.m_NextWeapon;
		m_HammerInput.m_PrevWeapon = m_DummyInput.m_PrevWeapon;
		int FlyFire = s_aFlyFire[ActiveDummy];
		if(!DoStrikeNow)
		{
			if((FlyFire & 1) != 0)
				FlyFire++;
		}
		else
		{
			if((FlyFire & 1) != 0)
				FlyFire++;
			FlyFire++;
		}
		FlyFire &= INPUT_STATE_MASK;
		s_aFlyFire[ActiveDummy] = FlyFire;
		m_HammerInput.m_Fire = FlyFire;

		m_HammerInput.m_WantedWeapon = WEAPON_HAMMER + 1;

		// Only aim at local while actually striking.
		if(DoStrikeNow)
		{
			const vec2 Dir = s_aFlyStrikeDir[ActiveDummy] * 1000.0f;
			m_HammerInput.m_TargetX = (int)Dir.x;
			m_HammerInput.m_TargetY = (int)Dir.y;
		}

		// Keep sending while hook is held or while striking.
		const bool NeedSend = Force || FlyHelper || DummyHammerMode ||
				      m_HammerInput.m_Hook != 0 || DoStrikeNow;
		if(NeedSend)
		{
			m_DummyFire++;
		}
		else
		{
			if(m_DummyFire % 25 != 0)
			{
				m_DummyFire++;
				return 0;
			}
			m_DummyFire++;
		}

		// Prefer copy-moves (dummy input) as the baseline. Only override aim for 1
		// tick when striking.
		CNetObj_PlayerInput Out = m_DummyInput;
		Out.m_PlayerFlags = m_DummyInput.m_PlayerFlags;
		Out.m_Fire = m_HammerInput.m_Fire;
		if(DummyHammerMode || FlyHelper)
			Out.m_WantedWeapon = WEAPON_HAMMER + 1;
		if(DummyHammerMode || DoStrikeNow)
		{
			Out.m_TargetX = m_HammerInput.m_TargetX;
			Out.m_TargetY = m_HammerInput.m_TargetY;
		}

		if(!g_Config.m_ClDummyHammer && g_Config.m_ClZzFentBotEnabled &&
			g_Config.m_ClZzFentBotPyInfer && m_aLocalIds[DummyTee] >= 0 &&
			m_aLocalIds[LocalTee] >= 0)
		{
			const int DummyCid = m_aLocalIds[DummyTee];
			const int OwnerCid = m_aLocalIds[LocalTee];
			const vec2 DummyPos = m_aClients[DummyCid].m_RegularPredicted.m_Pos;
			const vec2 DummyVel = m_aClients[DummyCid].m_RegularPredicted.m_Vel;
			const vec2 OwnerPos = m_aClients[OwnerCid].m_RegularPredicted.m_Pos;
			const float FollowR = 30.0f * 32.0f;
			vec2 Goal = DummyPos;
			if(distance(DummyPos, OwnerPos) > FollowR)
				Goal = OwnerPos;
			else if(m_FentBotHasWaypoint)
				Goal = (m_FentBotHasSubgoal ? m_FentBotSubgoal : m_FentBotWaypoint);

			int ActionId = -1;
			if(FentBotPyInferAction(g_Config.m_ClDummy, DummyPos, DummyVel, Goal,
				   ActionId))
			{
				static const vec2 s_aAimCands5[] = {
					vec2(0.0f, -1.0f),
					normalize(vec2(-0.4f, -1.0f)),
					normalize(vec2(0.4f, -1.0f)),
					vec2(-1.0f, 0.0f),
					vec2(1.0f, 0.0f),
				};
				const int AimCount = (int)std::size(s_aAimCands5);
				const int AimIndex = ActionId % AimCount;
				int Tmp = ActionId / AimCount;
				const int HookBit = Tmp & 1;
				Tmp >>= 1;
				const int JumpBit = Tmp & 1;
				Tmp >>= 1;
				const int DirIndex = Tmp;
				const int Dir = std::clamp(DirIndex - 1, -1, 1);

				Out.m_Direction = Dir;
				const vec2 OutAim = s_aAimCands5[AimIndex];
				Out.m_TargetX = (int)round_to_int(OutAim.x * 1000.0f);
				Out.m_TargetY = (int)round_to_int(OutAim.y * 1000.0f);
				if(!Out.m_TargetX && !Out.m_TargetY)
					Out.m_TargetY = -1;
				Out.m_Hook = HookBit ? 1 : 0;
				if(JumpBit)
					Out.m_Jump = (Out.m_Jump + 2) | 1;

				mem_copy(pData, &Out, sizeof(Out));
				return sizeof(Out);
			}
		}
		const bool UserWantsHook = m_DummyInput.m_Hook != 0;
		if(UserWantsHook && DoStrikeNow)
		{
			// Keep the hook attached while switching to hammer and striking.
			Out.m_WantedWeapon = WEAPON_HAMMER + 1;
			Out.m_Fire = m_HammerInput.m_Fire;
			Out.m_TargetX = m_HammerInput.m_TargetX;
			Out.m_TargetY = m_HammerInput.m_TargetY;
		}
		else if(DoStrikeNow)
		{
			// Strike: very short pulse, otherwise keep copy-moves camera/aim
			// untouched.
			Out.m_WantedWeapon = WEAPON_HAMMER + 1;
			Out.m_Fire = m_HammerInput.m_Fire;
			Out.m_TargetX = m_HammerInput.m_TargetX;
			Out.m_TargetY = m_HammerInput.m_TargetY;
		}
		else
		{
			// Release fire between swings without changing copy-moves aim or hook.
			if((Out.m_Fire & 1) != 0)
				Out.m_Fire++;
			Out.m_Fire &= INPUT_STATE_MASK;
		}

		// Triple fly from the supplied OnSnapInput. Between hammer strikes the
		// dummy holds hook on the closest other tee below it, then releases hook
		// for the strike. Never select the local tee or the dummy itself.
		if(TripleFlyOn)
		{
			if(DoStrikeNow)
			{
				Out.m_Hook = 0;
			}
			else
			{
				constexpr float SearchRadius = 400.0f;
				constexpr float HoldRadius = 250.0f;
				const int DummyCid = m_aLocalIds[PairConn];
				const int MainCid = m_aLocalIds[g_Config.m_ClDummy];
				int TargetCid = -1;
				float ClosestDist = SearchRadius;
				if(DummyCid >= 0 && DummyCid < MAX_CLIENTS)
				{
					const vec2 DummyPos =
						m_aClients[DummyCid].m_RegularPredicted.m_Pos;
					CCharacter *pTripleDummyChar =
						m_RegularPredictedWorld.GetCharacterById(DummyCid);
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(i == DummyCid || i == MainCid ||
							!m_Snap.m_aCharacters[i].m_Active)
							continue;
						if(pTripleDummyChar && !pTripleDummyChar->CanCollide(i))
							continue;
						const vec2 ToTarget =
							m_aClients[i].m_RegularPredicted.m_Pos - DummyPos;
						if(ToTarget.y <= 0.0f)
							continue;
						const float Dist = length(ToTarget);
						if(Dist > 0.001f)
						{
							const vec2 HookStart = DummyPos +
									       normalize(ToTarget) * CCharacterCore::PhysicalSize() * 1.5f;
							vec2 CollisionPos;
							int TeleNr = 0;
							Collision()->IntersectLineTeleHook(HookStart,
								DummyPos + ToTarget, &CollisionPos, nullptr, &TeleNr);
							if(distance(HookStart, CollisionPos) +
									CCharacterCore::PhysicalSize() <
								distance(HookStart, DummyPos + ToTarget))
								continue;
						}
						if(Dist <= ClosestDist)
						{
							ClosestDist = Dist;
							TargetCid = i;
						}
					}
					if(TargetCid >= 0)
					{
						vec2 TargetDir =
							m_aClients[TargetCid].m_RegularPredicted.m_Pos - DummyPos;
						if(length(TargetDir) > 0.001f)
							TargetDir = normalize(TargetDir) * 1000.0f;
						else
							TargetDir = vec2(0.0f, 1000.0f);
						Out.m_TargetX = (int)TargetDir.x;
						Out.m_TargetY = (int)TargetDir.y;
						Out.m_Hook = ClosestDist <= HoldRadius ? 1 : 0;
					}
					else
					{
						Out.m_Hook = 0;
					}
				}
			}
		}
		mem_copy(pData, &Out, sizeof(Out));
		return sizeof(Out);
	}
}

void CGameClient::OnConnected()
{
	const char *pConnectCaption = DemoPlayer()->IsPlaying() ? Localize("Preparing demo playback") : Localize("Connected");
	const char *pLoadMapContent = Localize("Initializing map logic");
	// render loading before skip is calculated
	m_Menus.RenderLoading(pConnectCaption, pLoadMapContent, 0);
	m_Layers.Init(Map(), false);
	m_Collision.Init(Layers());
	m_GameWorld.m_Core.InitSwitchers(m_Collision.m_HighestSwitchNumber);
	m_GameWorld.m_PredictedEvents.clear();
	m_RaceHelper.Init(this);

	// render loading before going through all components
	m_Menus.RenderLoading(pConnectCaption, pLoadMapContent, 0);
	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnMapLoad();
		pComponent->OnReset();
	}

	ConfigManager()->ResetGameSettings();
	LoadMapSettings();

	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		Client()->SetLoadingStateDetail(
			IClient::LOADING_STATE_DETAIL_GETTING_READY);
		m_Menus.RenderLoading(pConnectCaption,
			Localize("Sending initial client info"), 0);

		// send the initial info
		SendInfo(true);
		// we should keep this in for now, because otherwise you can't spectate
		// people at start as the other info 64 packet is only sent after the first
		// snap
		Client()->Rcon("crashmeplx");

		m_LocalServer.RconAuthIfPossible();
	}
}

void CGameClient::OnReset()
{
	m_FlyRideWasActive = false;
	m_FlyRidePilotDirection = 0;
	m_FlyRideDummyDirection = 0;
	m_FlyRidePilotJump = 0;
	m_FlyRideVerticalMode = 0;
	m_FlyRideLastVerticalIntent = 0;
	m_FlyRideLastUpdateTick = -1;
	m_FlyRideSmoothedVerticalSpeed = 0.0f;
	m_FlyRideBalanceRecovery = false;
	m_FlyRideEmergencyClimb = false;
	m_FlyRideFastClimb = false;
	m_FlyRideDescentBrake = false;
	m_FlyRidePilotHook = 0;
	m_FlyRideDummyHook = 0;
	m_FlyRideHookStateTick = -1;
	m_FlyRideHookDistance = 90.0f;
	m_FlyRideTargetConn = -1;
	m_FlyRidePilotHammer = false;
	m_FlyRideDummyHammer = false;
	m_FlyRidePilotFire = 0;
	m_FlyRideDummyFire = 0;
	m_FlyRidePilotLastFireTick = -1;
	m_FlyRideLastFireTick = -1;
	m_FlyRidePilotNextHitTick = -1;
	m_FlyRideDummyNextHitTick = -1;
	m_FlyRideAledState = EFlyRideAledState::NONE;
	m_FlyRideAledDirection = vec2(0.0f, -1.0f);
	m_FlyRideAledEntryProjection = 0.0f;
	m_FlyRideAledExitProjection = 0.0f;
	m_FlyRideAledPilotLeads = true;
	m_FlyRideAledStateTick = -1;
	m_FlyRideAledAttemptTick = -1;
	m_FlyRideAledRetryTick = -1;
	m_FlyRideAledLastDiagTick = -1;
	InvalidateSnapshot();
	ResetKinetixLaserUnfreeze();
	for(auto &Observation : m_aAutoUnfreezeShotObservation)
		Observation = {};
	for(auto &DummyHistory : m_aaAutoUnfreezeTeeHistory)
		for(auto &History : DummyHistory)
			History = {};
	std::fill(std::begin(m_aAutoUnfreezeTeeHistoryWrite),
		std::end(m_aAutoUnfreezeTeeHistoryWrite), 0);
	std::fill(std::begin(m_aAutoUnfreezeObservedPredictionLead),
		std::end(m_aAutoUnfreezeObservedPredictionLead), 0.0f);
	std::fill(std::begin(m_aAutoUnfreezeObservedOriginOffset),
		std::end(m_aAutoUnfreezeObservedOriginOffset), vec2(0.0f, 0.0f));
	mem_zero(m_aaAutoUnfreezeObservedPredictionLeadCount,
		sizeof(m_aaAutoUnfreezeObservedPredictionLeadCount));
	std::fill(std::begin(m_aAutoUnfreezeObservedSamples),
		std::end(m_aAutoUnfreezeObservedSamples), 0);
	for(auto &DummyLearning : m_aaAutoUnfreezeLearning)
		for(auto &Entry : DummyLearning)
			Entry = {};

	m_EditorMovementDelay = 5;

	m_PredictedTick = -1;
	std::fill(std::begin(m_aLastNewPredictedTick),
		std::end(m_aLastNewPredictedTick), -1);

	m_LastRoundStartTick = -1;
	m_LastRaceTick = -1;
	m_LastFlagCarrierRed = -4;
	m_LastFlagCarrierBlue = -4;

	std::fill(std::begin(m_aCheckInfo), std::end(m_aCheckInfo), -1);

	// m_aDDNetVersionStr is initialized once in OnInit

	std::fill(std::begin(m_aLastPos), std::end(m_aLastPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aLastActive), std::end(m_aLastActive), false);

	m_GameOver = false;
	m_GamePaused = false;

	m_SuppressEvents = false;
	m_NewTick = false;
	m_NewPredictedTick = false;

	m_aFlagDropTick[TEAM_RED] = 0;
	m_aFlagDropTick[TEAM_BLUE] = 0;

	m_ServerMode = SERVERMODE_PURE;
	mem_zero(&m_GameInfo, sizeof(m_GameInfo));

	m_DemoSpecId = SPEC_FOLLOW;
	m_LocalCharacterPos = vec2(0.0f, 0.0f);

	m_PredictedPrevChar.Reset();
	m_PredictedChar.Reset();

	// m_Snap was cleared in InvalidateSnapshot

	std::fill(std::begin(m_aLocalTuneZone), std::end(m_aLocalTuneZone), -1);
	std::fill(std::begin(m_aReceivedTuning), std::end(m_aReceivedTuning), false);
	std::fill(std::begin(m_aExpectingTuningForZone),
		std::end(m_aExpectingTuningForZone), -1);
	std::fill(std::begin(m_aExpectingTuningSince),
		std::end(m_aExpectingTuningSince), 0);
	std::fill(std::begin(m_aTuning), std::end(m_aTuning), CTuningParams());

	m_ActiveRecordings.reset();

	for(auto &Client : m_aClients)
		Client.Reset();

	for(auto &Stats : m_aStats)
		Stats.Reset();

	std::fill(std::begin(m_aNextChangeInfo), std::end(m_aNextChangeInfo), -1);
	std::fill(std::begin(m_aLocalIds), std::end(m_aLocalIds), -1);
	m_DummyInput = {};
	m_DummyInputConn = IClient::CONN_MAIN;
	m_HammerInput = {};
	m_DummyFire = 0;
	m_ReceivedDDNetPlayer = false;
	m_ReceivedDDNetPlayerFinishTimes = false;
	m_ReceivedDDNetPlayerFinishTimesMillis = false;

	m_Teams.Reset();
	m_GameWorld.Clear();
	m_GameWorld.m_WorldConfig.m_InfiniteAmmo = true;
	m_PredictedWorld.CopyWorld(&m_GameWorld);
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
	m_RegularPredictedWorld.CopyWorldClean(&m_PredictedWorld);
	m_PrevRegularPredictedWorld.CopyWorldClean(&m_PredictedWorld);

	m_vSnapEntities.clear();

	std::fill(std::begin(m_aDDRaceMsgSent), std::end(m_aDDRaceMsgSent), false);
	std::fill(std::begin(m_aShowOthers), std::end(m_aShowOthers),
		SHOW_OTHERS_NOT_SET);
	std::fill(std::begin(m_aEnableSpectatorCount),
		std::end(m_aEnableSpectatorCount), -1);
	std::fill(std::begin(m_aLastUpdateTick), std::end(m_aLastUpdateTick), 0);

	m_IsDummySwapping = false;
	m_DummySwapFrom = IClient::CONN_MAIN;
	m_CharOrder.Reset();
	std::fill(std::begin(m_aSwitchStateTeam), std::end(m_aSwitchStateTeam), -1);

	m_MapBestTimeSeconds = FinishTime::UNSET;
	m_MapBestTimeMillis = 0;
	m_aMapDescription[0] = '\0';

	// m_MapBugs and m_aTuningList are reset in LoadMapSettings

	m_LastShowDistanceZoom = 0.0f;
	m_LastZoom = 0.0f;
	m_LastScreenAspect = 0.0f;
	m_LastDeadzone = 0.0f;
	m_LastFollowFactor = 0.0f;
	std::fill(std::begin(m_aLastDummyConnected), std::end(m_aLastDummyConnected),
		false);

	m_MultiViewPersonalZoom = 0.0f;
	m_MultiViewActivated = false;
	m_MultiView.m_IsInit = false;

	m_CursorInfo.m_CursorOwnerId = -1;
	m_CursorInfo.m_NumSamples = 0;

	for(auto &pComponent : m_vpAll)
		pComponent->OnReset();
	if(m_pKoGAIController)
		m_pKoGAIController->OnReset();

	Editor()->ResetMentions();
	Editor()->ResetIngameMoved();

	Collision()->Unload();
	Layers()->Unload();
}

void CGameClient::UpdatePositions()
{
	// local character position
	if(g_Config.m_ClPredict &&
		Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(!AntiPingPlayers())
		{
			if(!m_Snap.m_pLocalCharacter ||
				(m_Snap.m_pGameInfoObj &&
					m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
			{
				// don't use predicted
			}
			else
				m_LocalCharacterPos =
					mix(m_PredictedPrevChar.m_Pos, m_PredictedChar.m_Pos,
						Client()->PredIntraGameTick(g_Config.m_ClDummy));
		}
		else
		{
			if(!(m_Snap.m_pGameInfoObj &&
				   m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
			{
				if(m_Snap.m_pLocalCharacter)
					m_LocalCharacterPos =
						mix(m_PredictedPrevChar.m_Pos, m_PredictedChar.m_Pos,
							Client()->PredIntraGameTick(g_Config.m_ClDummy));
			}
			//		else
			//			m_LocalCharacterPos =
			//mix(m_PredictedPrevChar.m_Pos, m_PredictedChar.m_Pos,
			//Client()->PredIntraGameTick(g_Config.m_ClDummy));
		}
	}
	else if(m_Snap.m_pLocalCharacter && m_Snap.m_pLocalPrevCharacter)
	{
		m_LocalCharacterPos =
			mix(vec2(m_Snap.m_pLocalPrevCharacter->m_X,
				    m_Snap.m_pLocalPrevCharacter->m_Y),
				vec2(m_Snap.m_pLocalCharacter->m_X, m_Snap.m_pLocalCharacter->m_Y),
				Client()->IntraGameTick(g_Config.m_ClDummy));
	}

	// spectator position
	if(m_Snap.m_SpecInfo.m_Active)
	{
		if(m_MultiViewActivated)
		{
			HandleMultiView();
		}
		else if(Client()->State() == IClient::STATE_DEMOPLAYBACK &&
			m_DemoSpecId != SPEC_FOLLOW &&
			m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		{
			m_Snap.m_SpecInfo.m_Position = mix(
				vec2(
					m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_X,
					m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_Y),
				vec2(m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_X,
					m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_Y),
				Client()->IntraGameTick(g_Config.m_ClDummy));
			m_Snap.m_SpecInfo.m_UsePosition = true;
		}
		else if(m_Snap.m_pSpectatorInfo &&
			((Client()->State() == IClient::STATE_DEMOPLAYBACK &&
				 m_DemoSpecId == SPEC_FOLLOW) ||
				(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
					m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)))
		{
			if(m_Snap.m_pPrevSpectatorInfo &&
				m_Snap.m_pPrevSpectatorInfo->m_SpectatorId ==
					m_Snap.m_pSpectatorInfo->m_SpectatorId)
				m_Snap.m_SpecInfo.m_Position = mix(
					vec2(m_Snap.m_pPrevSpectatorInfo->m_X,
						m_Snap.m_pPrevSpectatorInfo->m_Y),
					vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y),
					Client()->IntraGameTick(g_Config.m_ClDummy));
			else
				m_Snap.m_SpecInfo.m_Position =
					vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y);
			m_Snap.m_SpecInfo.m_UsePosition = true;
		}
	}

	if(!m_MultiViewActivated && m_MultiView.m_IsInit)
		ResetMultiView();

	UpdateRenderedCharacters();
}

void CGameClient::OnRender()
{
	float AspectOverride = 0.0f;
	switch(g_Config.m_ClZzAspectRatio)
	{
	case 1: AspectOverride = 4.0f / 3.0f; break;
	case 2: AspectOverride = 5.0f / 4.0f; break;
	case 3: AspectOverride = 16.0f / 10.0f; break;
	case 4: AspectOverride = 16.0f / 9.0f; break;
	case 5: AspectOverride = 21.0f / 9.0f; break;
	default: break;
	}
	// The viewport remains at its native pixel resolution. Only the logical
	// camera/interface aspect changes, producing a crisp full-screen stretch.
	Graphics()->SetScreenAspectOverride(AspectOverride);

	const ColorRGBA ClearColor = color_cast<ColorRGBA>(ColorHSLA(
		g_Config.m_ClOverlayEntities ? g_Config.m_ClBackgroundEntitiesColor : g_Config.m_ClBackgroundColor));
	Graphics()->Clear(ClearColor.r, ClearColor.g, ClearColor.b);

	// check if multi view got activated
	if(!m_MultiView.m_IsInit && m_MultiViewActivated)
	{
		int TeamId = 0;
		if(m_Snap.m_SpecInfo.m_SpectatorId >= 0)
			TeamId = m_Teams.Team(m_Snap.m_SpecInfo.m_SpectatorId);

		if(TeamId > MAX_CLIENTS || TeamId < 0)
			TeamId = 0;

		if(!InitMultiView(TeamId))
		{
			dbg_msg("MultiView", "No players found to spectate");
			ResetMultiView();
		}
	}

	// update the local character and spectate position
	UpdatePositions();

	// display warnings
	if(m_Menus.CanDisplayWarning())
	{
		std::optional<SWarning> Warning = Graphics()->CurrentWarning();
		if(!Warning.has_value())
		{
			Warning = Client()->CurrentWarning();
		}
		if(Warning.has_value())
		{
			const SWarning &TheWarning = Warning.value();
			m_Menus.PopupWarning(TheWarning.m_aWarningTitle[0] == '\0' ? Localize("Warning") : TheWarning.m_aWarningTitle,
				TheWarning.m_aWarningMsg, Localize("Ok"),
				TheWarning.m_AutoHide ? 10s : 0s);
		}
	}

	// update camera data prior to CControls::OnRender to allow
	// CControls::m_aTargetPos to compensate using camera data
	m_Camera.UpdateCamera();

	UpdateSpectatorCursor();

	// render all systems
	for(auto &pComponent : m_vpAll)
	{
		// Stretch only the world. HUD, race timer, menus and all text keep the
		// native screen mapping, so changing aspect ratio cannot move UI.
		if(pComponent == &m_Hud)
			Graphics()->SetScreenAspectOverride(0.0f);
		pComponent->OnRender();
	}
	Graphics()->SetScreenAspectOverride(0.0f);
	RenderKinetixLaserUnfreezeAttempt();

	// clear all events/input for this frame
	Input()->Clear();

	CLineInput::RenderCandidates();

	const bool WasNewTick = m_NewTick;

	// clear new tick flags
	m_NewTick = false;
	m_NewPredictedTick = false;

	if(g_Config.m_ClDummy && !Client()->DummyConnected(g_Config.m_ClDummy))
		g_Config.m_ClDummy = IClient::CONN_MAIN;

	// resend player and dummy info if it was filtered by server
	if(m_aLocalIds[0] >= 0 && Client()->State() == IClient::STATE_ONLINE &&
		!m_Menus.IsActive() && WasNewTick)
	{
		if(m_aCheckInfo[0] == 0)
		{
			if(m_pClient->IsSixup())
			{
				if(!GotWantedSkin7(IClient::CONN_MAIN))
					SendSkinChange7(IClient::CONN_MAIN);
				else
					m_aCheckInfo[0] = -1;
			}
			else
			{
				if(str_comp(m_aClients[m_aLocalIds[0]].m_aName,
					   Client()->PlayerName()) ||
					str_comp(m_aClients[m_aLocalIds[0]].m_aClan,
						g_Config.m_PlayerClan) ||
					m_aClients[m_aLocalIds[0]].m_Country != g_Config.m_PlayerCountry ||
					str_comp(m_aClients[m_aLocalIds[0]].m_aSkinName,
						g_Config.m_ClPlayerSkin) ||
					m_aClients[m_aLocalIds[0]].m_UseCustomColor !=
						g_Config.m_ClPlayerUseCustomColor ||
					m_aClients[m_aLocalIds[0]].m_ColorBody !=
						(int)g_Config.m_ClPlayerColorBody ||
					m_aClients[m_aLocalIds[0]].m_ColorFeet !=
						(int)g_Config.m_ClPlayerColorFeet)
					SendInfo(false);
				else
					m_aCheckInfo[0] = -1;
			}
		}

		if(m_aCheckInfo[0] > 0)
		{
			m_aCheckInfo[0] -= minimum(
				Client()->GameTick(0) - Client()->PrevGameTick(0), m_aCheckInfo[0]);
		}

		for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
		{
			if(m_aLocalIds[Conn] < 0)
				continue;
			if(m_aCheckInfo[Conn] == 0)
			{
				if(m_pClient->IsSixup())
				{
					if(!GotWantedSkin7(Conn))
						SendSkinChange7(Conn);
					else
						m_aCheckInfo[Conn] = -1;
				}
				else
				{
					if(str_comp(m_aClients[m_aLocalIds[Conn]].m_aName,
						   Client()->DummyName(Conn)) ||
						str_comp(m_aClients[m_aLocalIds[Conn]].m_aClan,
							g_Config.m_ClDummyClan) ||
						m_aClients[m_aLocalIds[Conn]].m_Country !=
							g_Config.m_ClDummyCountry ||
						str_comp(m_aClients[m_aLocalIds[Conn]].m_aSkinName,
							g_Config.m_ClDummySkin) ||
						m_aClients[m_aLocalIds[Conn]].m_UseCustomColor !=
							g_Config.m_ClDummyUseCustomColor ||
						m_aClients[m_aLocalIds[Conn]].m_ColorBody !=
							(int)g_Config.m_ClDummyColorBody ||
						m_aClients[m_aLocalIds[Conn]].m_ColorFeet !=
							(int)g_Config.m_ClDummyColorFeet)
						SendDummyInfo(false, Conn);
					else
						m_aCheckInfo[Conn] = -1;
				}
			}

			if(m_aCheckInfo[Conn] > 0)
			{
				m_aCheckInfo[Conn] -=
					minimum(Client()->GameTick(Conn) - Client()->PrevGameTick(Conn),
						m_aCheckInfo[Conn]);
			}
		}
	}

	UpdateManagedTeeRenderInfos();
}

void CGameClient::OnDummyDisconnect(int Conn)
{
	m_Controls.ResetInput(Conn);
	m_Controls.m_aInputData[Conn].m_Hook = 0;
	if(Conn == m_DummyInputConn || Conn == Client()->DummyPair())
	{
		const int PairConn = std::clamp(Client()->DummyPair(), 0, NUM_DUMMIES - 1);
		m_DummyInput = m_Controls.m_aInputData[PairConn];
		m_DummyInputConn = PairConn;
		m_HammerInput = m_DummyInput;
		m_DummyFire = 0;
	}
	m_aLocalIds[Conn] = -1;
	m_aDDRaceMsgSent[Conn] = false;
	m_aShowOthers[Conn] = SHOW_OTHERS_NOT_SET;
	m_aEnableSpectatorCount[Conn] = -1;
	m_aLastNewPredictedTick[Conn] = -1;
}

int CGameClient::LastRaceTick() const { return m_LastRaceTick; }

int CGameClient::CurrentRaceTime() const
{
	if(m_LastRaceTick < 0)
	{
		return 0;
	}
	return (Client()->GameTick(g_Config.m_ClDummy) - m_LastRaceTick) /
	       Client()->GameTickSpeed();
}

bool CGameClient::Predict() const
{
	if(!g_Config.m_ClPredict)
		return false;

	if(m_Snap.m_pGameInfoObj)
	{
		if(m_Snap.m_pGameInfoObj->m_GameStateFlags &
			(GAMESTATEFLAG_GAMEOVER | GAMESTATEFLAG_PAUSED))
		{
			return false;
		}
	}

	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return false;

	return !m_Snap.m_SpecInfo.m_Active && m_Snap.m_pLocalCharacter;
}

ColorRGBA CGameClient::GetDDTeamColor(int DDTeam, float Lightness) const
{
	// TClient
	if(g_Config.m_TcOldTeamColors)
		return color_cast<ColorRGBA>(ColorHSLA(DDTeam / 64.0f, 1.0f, Lightness));

	// Use golden angle to generate unique colors with distinct adjacent colors.
	// The first DDTeam (team 1) gets angle 0°, i.e. red hue.
	const float Hue = std::fmod((DDTeam - 1) * (137.50776f / 360.0f), 1.0f);
	return color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, Lightness));
}

void CGameClient::FormatClientId(int ClientId, char (&aClientId)[16],
	EClientIdFormat Format) const
{
	if(Format == EClientIdFormat::NO_INDENT)
	{
		str_format(aClientId, sizeof(aClientId), "%d", ClientId);
	}
	else
	{
		const int HighestClientId =
			Format == EClientIdFormat::INDENT_AUTO ? m_Snap.m_HighestClientId : 64;
		const char *pFigureSpace = " ";
		char aNumber[8];
		str_format(aNumber, sizeof(aNumber), "%d", ClientId);
		aClientId[0] = '\0';
		if(ClientId < 100 && HighestClientId >= 100)
		{
			str_append(aClientId, pFigureSpace);
		}
		if(ClientId < 10 && HighestClientId >= 10)
		{
			str_append(aClientId, pFigureSpace);
		}
		str_append(aClientId, aNumber);
	}
	str_append(aClientId, ": ");
}

void CGameClient::OnRelease()
{
	// release all systems
	for(auto &pComponent : m_vpAll)
		pComponent->OnRelease();
}

void CGameClient::OnMessage(int MsgId, CUnpacker *pUnpacker, int Conn,
	bool Dummy)
{
	// special messages
	static_assert((int)NETMSGTYPE_SV_TUNEPARAMS ==
			      (int)protocol7::NETMSGTYPE_SV_TUNEPARAMS,
		"0.6 and 0.7 tune message id do not match");
	if(MsgId == NETMSGTYPE_SV_TUNEPARAMS)
	{
		// unpack the new tuning
		CTuningParams NewTuning;

		// No jetpack on DDNet incompatible servers,
		// jetpack strength will be received by tune params
		NewTuning.m_JetpackStrength = 0;

		int *pParams = NewTuning.NetworkArray();
		for(int i = 0; i < CTuningParams::Num(); i++)
		{
			static_assert(
				offsetof(CTuningParams, m_LaserDamage) / sizeof(CTuneParam) == 30);
			if(i == 30 && Client()->IsSixup()) // laser_damage was removed in 0.7
			{
				continue;
			}

			const int Value = pUnpacker->GetInt();

			// check for unpacking errors
			if(pUnpacker->Error())
				break;

			pParams[i] = Value;
		}

		m_ServerMode = SERVERMODE_PURE;

		m_aReceivedTuning[Conn] = true;
		// apply new tuning
		m_aTuning[Conn] = NewTuning;
		return;
	}

	void *pRawMsg = TranslateGameMsg(&MsgId, pUnpacker, Conn);

	if(!pRawMsg)
	{
		// the 0.7 version of this error message is printed on translation
		// in sixup/translate_game.cpp
		if(!Client()->IsSixup())
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf),
				"dropped weird message '%s' (%d), failed on '%s'",
				m_NetObjHandler.GetMsgName(MsgId), MsgId,
				m_NetObjHandler.FailedMsgOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "client", aBuf);
		}
		return;
	}

	if(MsgId == NETMSGTYPE_SV_CHANGEINFOCOOLDOWN)
	{
		CNetMsg_Sv_ChangeInfoCooldown *pMsg =
			(CNetMsg_Sv_ChangeInfoCooldown *)pRawMsg;
		m_aNextChangeInfo[Conn] = pMsg->m_WaitUntil;
		return;
	}

	if(Dummy)
	{
		if(MsgId == NETMSGTYPE_SV_CHAT && m_aLocalIds[0] >= 0 &&
			m_aLocalIds[Conn] >= 0)
		{
			CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

			if((pMsg->m_Team == 1 &&
				   (m_aClients[m_aLocalIds[0]].m_Team !=
						   m_aClients[m_aLocalIds[Conn]].m_Team ||
					   m_Teams.Team(m_aLocalIds[0]) != m_Teams.Team(m_aLocalIds[Conn]))) ||
				pMsg->m_Team > 1)
			{
				m_Chat.OnMessage(MsgId, pRawMsg);
			}
		}
		return; // no need of all that stuff for the dummy
	}

	// TODO: this should be done smarter
	for(auto &pComponent : m_vpAll)
		pComponent->OnMessage(MsgId, pRawMsg);

	if(MsgId == NETMSGTYPE_SV_READYTOENTER)
	{
		Client()->EnterGame(Conn);
	}
	else if(MsgId == NETMSGTYPE_SV_EMOTICON)
	{
		CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;

		// apply
		m_aClients[pMsg->m_ClientId].m_Emoticon = pMsg->m_Emoticon;
		m_aClients[pMsg->m_ClientId].m_EmoticonStartTick = Client()->GameTick(Conn);
		m_aClients[pMsg->m_ClientId].m_EmoticonStartFraction =
			Client()->IntraGameTickSincePrev(Conn);
	}
	else if(MsgId == NETMSGTYPE_SV_SOUNDGLOBAL)
	{
		if(m_SuppressEvents)
			return;

		// don't enqueue pseudo-global sounds from demos (created by PlayAndRecord)
		CNetMsg_Sv_SoundGlobal *pMsg = (CNetMsg_Sv_SoundGlobal *)pRawMsg;
		if(pMsg->m_SoundId == SOUND_CTF_DROP ||
			pMsg->m_SoundId == SOUND_CTF_RETURN ||
			pMsg->m_SoundId == SOUND_CTF_CAPTURE ||
			pMsg->m_SoundId == SOUND_CTF_GRAB_EN ||
			pMsg->m_SoundId == SOUND_CTF_GRAB_PL)
		{
			if(g_Config.m_SndGame)
				m_Sounds.Enqueue(CSounds::CHN_GLOBAL, pMsg->m_SoundId);
		}
		else
		{
			if(g_Config.m_SndGame)
				m_Sounds.Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId, 1.0f);
		}
	}
	else if(MsgId == NETMSGTYPE_SV_TEAMSSTATE ||
		MsgId == NETMSGTYPE_SV_TEAMSSTATELEGACY)
	{
		unsigned int i;

		for(i = 0; i < MAX_CLIENTS; i++)
		{
			const int Team = pUnpacker->GetInt();
			if(!pUnpacker->Error() && Team >= TEAM_FLOCK && Team <= TEAM_SUPER)
				m_Teams.Team(i, Team);
			else
			{
				m_Teams.Team(i, 0);
				break;
			}
		}

		if(i <= 16)
			m_Teams.m_IsDDRace16 = true;

		m_Ghost.m_AllowRestart = true;
		m_RaceDemo.m_AllowRestart = true;
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSG)
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;
		// reset character prediction
		if(!(m_GameWorld.m_WorldConfig.m_IsFNG &&
			   pMsg->m_Weapon == WEAPON_LASER))
		{
			m_CharOrder.GiveWeak(pMsg->m_Victim);
			if(CCharacter *pChar = m_GameWorld.GetCharacterById(pMsg->m_Victim))
				pChar->ResetPrediction();
			m_GameWorld.ReleaseHooked(pMsg->m_Victim);
		}

		// if we are spectating a static id set (team 0) and somebody killed, and
		// its not a guy in solo, we remove them from the list never remove players
		// from the list if it is a pvp server
		if(IsMultiViewIdSet() && m_MultiViewTeam == 0 &&
			m_aMultiViewId[pMsg->m_Victim] && !m_aClients[pMsg->m_Victim].m_Spec &&
			!m_MultiView.m_Solo && !m_GameInfo.m_Pvp)
		{
			m_aMultiViewId[pMsg->m_Victim] = false;

			// if everyone of a team killed, we have no ids to spectate anymore, so we
			// disable multi view
			if(!IsMultiViewIdSet())
				ResetMultiView();
			else
			{
				// the "main" tee killed, search a new one
				if(m_Snap.m_SpecInfo.m_SpectatorId == pMsg->m_Victim)
				{
					int NewClientId = FindFirstMultiViewId();
					if(NewClientId < MAX_CLIENTS && NewClientId >= 0)
					{
						CleanMultiViewId(NewClientId);
						m_aMultiViewId[NewClientId] = true;
						m_Spectator.Spectate(NewClientId);
					}
				}
			}
		}
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSGTEAM)
	{
		CNetMsg_Sv_KillMsgTeam *pMsg = (CNetMsg_Sv_KillMsgTeam *)pRawMsg;

		// reset prediction
		std::vector<std::pair<int, int>> vStrongWeakSorted;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_Teams.Team(i) == pMsg->m_Team)
			{
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
				{
					pChar->ResetPrediction();
					vStrongWeakSorted.emplace_back(
						i, pMsg->m_First == i ? MAX_CLIENTS : (pChar ? pChar->GetStrongWeakId() : 0));
				}
				m_GameWorld.ReleaseHooked(i);
			}
		}
		std::stable_sort(
			vStrongWeakSorted.begin(), vStrongWeakSorted.end(),
			[](auto &Left, auto &Right) { return Left.second > Right.second; });
		for(auto Id : vStrongWeakSorted)
		{
			m_CharOrder.GiveWeak(Id.first);
		}
	}
	else if(MsgId == NETMSGTYPE_SV_MAPSOUNDGLOBAL)
	{
		if(m_SuppressEvents)
			return;

		if(!g_Config.m_SndGame)
			return;

		CNetMsg_Sv_MapSoundGlobal *pMsg = (CNetMsg_Sv_MapSoundGlobal *)pRawMsg;
		m_MapSounds.Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId);
	}
	else if(MsgId == NETMSGTYPE_SV_PREINPUT)
	{
		CNetMsg_Sv_PreInput *pMsg = (CNetMsg_Sv_PreInput *)pRawMsg;
		m_aClients[pMsg->m_Owner].m_aPreInputs[pMsg->m_IntendedTick % 200] = *pMsg;
	}
	else if(MsgId == NETMSGTYPE_SV_SAVECODE)
	{
		const CNetMsg_Sv_SaveCode *pMsg = (CNetMsg_Sv_SaveCode *)pRawMsg;
		OnSaveCodeNetMessage(pMsg);
	}
	else if(MsgId == NETMSGTYPE_SV_RECORD ||
		MsgId == NETMSGTYPE_SV_RECORDLEGACY)
	{
		CNetMsg_Sv_Record *pMsg = static_cast<CNetMsg_Sv_Record *>(pRawMsg);
		if(pMsg->m_ServerTimeBest > 0)
		{
			m_MapBestTimeSeconds = pMsg->m_ServerTimeBest / 100;
			m_MapBestTimeMillis = (pMsg->m_ServerTimeBest % 100) * 10;
		}
		else if(m_MapBestTimeSeconds == FinishTime::UNSET)
		{
			// some PvP mods based on DDNet accidentally send a best time of 0,
			// despite having no finished races
		}
	}
	else if(MsgId == NETMSGTYPE_SV_MAPINFO)
	{
		CNetMsg_Sv_MapInfo *pMsg = static_cast<CNetMsg_Sv_MapInfo *>(pRawMsg);
		str_copy(m_aMapDescription, pMsg->m_pDescription);
	}
}

void CGameClient::OnStateChange(int NewState, int OldState)
{
	// reset everything when not already connected (to keep gathered stuff)
	if(NewState < IClient::STATE_ONLINE)
		OnReset();

	if(OldState >= IClient::STATE_ONLINE && NewState < IClient::STATE_ONLINE)
	{
		AutoUnfreezeDebugStop();
		if(m_FentBotRecordActive)
			ConZzFentBotRecordStop(nullptr, this);
		if(m_FentBotClipActive)
			ConZzFentBotClipStop(nullptr, this);
		if(m_FentBotSavePlrActive)
			ConZzFentBotSavePlrStop(nullptr, this);
	}

	// then change the state
	for(auto &pComponent : m_vpAll)
		pComponent->OnStateChange(NewState, OldState);
}

void CGameClient::OnShutdown()
{
	AutoUnfreezeDebugStop();
	for(auto &pComponent : m_vpAll)
		pComponent->OnShutdown();
	if(m_FentBotPySock)
	{
		net_tcp_close(m_FentBotPySock);
		m_FentBotPySock = nullptr;
	}
	if(m_CompanionPySock)
	{
		net_tcp_close(m_CompanionPySock);
		m_CompanionPySock = nullptr;
	}
	m_FentBotPyConnected = false;
	m_CompanionPyConnected = false;
	if(m_FentBotClipActive && m_FentBotClipHandle)
	{
		io_close(m_FentBotClipHandle);
		m_FentBotClipHandle = 0;
		m_FentBotClipActive = false;
	}
	if(m_FentBotSavePlrActive && m_FentBotSavePlrHandle)
	{
		io_close(m_FentBotSavePlrHandle);
		m_FentBotSavePlrHandle = 0;
		m_FentBotSavePlrActive = false;
	}
	if(m_FentBotRecordActive && m_FentBotRecordHandle)
	{
		io_close(m_FentBotRecordHandle);
		m_FentBotRecordHandle = 0;
		m_FentBotRecordActive = false;
	}
	m_LocalServer.KillServer();
}

void CGameClient::OnEnterGame()
{
	if(g_Config.m_ClZzFentBotAutoRecord && !m_FentBotRecordActive)
		ConZzFentBotRecordStart(nullptr, this);
}

void CGameClient::OnGameOver()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
		g_Config.m_ClEditor == 0)
		Client()->AutoScreenshot_Start();
}

void CGameClient::OnStartGame()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
		!g_Config.m_ClAutoDemoOnConnect)
		Client()->DemoRecorder_HandleAutoStart();
	m_Statboard.OnReset();
}

void CGameClient::OnStartRound()
{
	// In GamePaused or GameOver state RoundStartTick is updated on each tick
	// hence no need to reset stats until player leaves GameOver
	// and it would be a mistake to reset stats after or during the pause
	m_Statboard.OnReset();

	// Restart automatic race demo recording
	m_RaceDemo.OnReset();
}

void CGameClient::OnFlagGrab(int TeamId)
{
	if(TeamId == TEAM_RED)
		m_aStats[m_Snap.m_pGameDataObj->m_FlagCarrierRed].m_FlagGrabs++;
	else
		m_aStats[m_Snap.m_pGameDataObj->m_FlagCarrierBlue].m_FlagGrabs++;
}

void CGameClient::OnWindowResize()
{
	for(auto &pComponent : m_vpAll)
		pComponent->OnWindowResize();

	Ui()->OnWindowResize();
}

void CGameClient::OnLanguageChange()
{
	// The actual language change is delayed because it
	// might require clearing the text render font atlas,
	// which would invalidate text that is currently drawn.
	m_LanguageChanged = true;
}

void CGameClient::HandleLanguageChanged()
{
	if(!m_LanguageChanged)
		return;
	m_LanguageChanged = false;

	g_Localization.Load(g_Config.m_ClLanguagefile, Storage(), Console());

	// TClient
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "tclient/%s", g_Config.m_ClLanguagefile);
	g_Localization.Load(aBuf, Storage(), Console(), false);

	TextRender()->SetFontLanguageVariant(g_Config.m_ClLanguagefile);

	// Clear all text containers
	Client()->OnWindowResize();
}

void CGameClient::RenderShutdownMessage()
{
	const char *pMessage = nullptr;
	if(Client()->State() == IClient::STATE_QUITTING)
		pMessage = Localize("Quitting. Please wait…");
	else if(Client()->State() == IClient::STATE_RESTARTING)
		pMessage = Localize("Restarting. Please wait…");
	else
		dbg_assert_failed("Invalid client state for quitting message");

	// This function only gets called after the render loop has already
	// terminated, so we have to call Swap manually.
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
	Ui()->MapScreen();
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	Ui()->DoLabel(Ui()->Screen(), pMessage, 16.0f, TEXTALIGN_MC);
	Graphics()->Swap();
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
}

void CGameClient::ProcessDemoSnapshot(CSnapshot *pSnap)
{
	for(int Index = 0; Index < pSnap->NumItems(); Index++)
	{
		const CSnapshotItem *pItem = pSnap->GetItem(Index);
		int ItemType = pSnap->GetItemType(Index);

		if(ItemType == NETOBJTYPE_PROJECTILE)
		{
			// for antiping: if the projectile netobjects from the server contains
			// extra data, this is removed and the original content restored before
			// recording demo
			CNetObj_Projectile *pProj = (CNetObj_Projectile *)((void *)pItem->Data());
			DemoObjectRemoveExtraProjectileInfo(pProj);
		}
		else if(ItemType == NETOBJTYPE_DDNETSPECTATORINFO)
		{
			// always record local camera info as follow mode
			CNetObj_DDNetSpectatorInfo *pDDNetSpectatorInfo =
				(CNetObj_DDNetSpectatorInfo *)((void *)pItem->Data());
			pDDNetSpectatorInfo->m_HasCameraInfo = true;
			pDDNetSpectatorInfo->m_Zoom =
				(m_Camera.m_Zooming ? m_Camera.m_ZoomSmoothingTarget : m_Camera.m_Zoom) *
				1000.0f;
			pDDNetSpectatorInfo->m_Deadzone = m_Camera.Deadzone();
			pDDNetSpectatorInfo->m_FollowFactor = m_Camera.FollowFactor();
		}
	}
}

void CGameClient::OnRconType(bool UsernameReq)
{
	m_GameConsole.RequireUsername(UsernameReq);
}

void CGameClient::OnRconLine(const char *pLine)
{
	m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, pLine);
}

void CGameClient::ProcessEvents()
{
	if(m_SuppressEvents)
		return;

	int SnapType = IClient::SNAP_CURRENT;
	int Num = Client()->SnapNumItems(SnapType);
	for(int Index = 0; Index < Num; Index++)
	{
		const IClient::CSnapItem Item = Client()->SnapGetItem(SnapType, Index);

		// TODO: We don't have enough info about us, others, to know a correct alpha
		// or volume value.
		const float Alpha = 1.0f;
		const float Volume = 1.0f;

		if(Item.m_Type == NETEVENTTYPE_DAMAGEIND)
		{
			const CNetEvent_DamageInd *pEvent =
				(const CNetEvent_DamageInd *)Item.m_pData;

			vec2 DamageIndPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(
				   CGameWorld::CPredictedEvent(
					   Item.m_Type, DamageIndPos, -1,
					   Client()->GameTick(g_Config.m_ClDummy), pEvent->m_Angle)))
			{
				m_Effects.DamageIndicator(vec2(pEvent->m_X, pEvent->m_Y),
					direction(pEvent->m_Angle / 256.0f), Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_EXPLOSION)
		{
			const CNetEvent_Explosion *pEvent =
				(const CNetEvent_Explosion *)Item.m_pData;

			vec2 ExplosionPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(
				   CGameWorld::CPredictedEvent(
					   Item.m_Type, ExplosionPos, -1,
					   Client()->GameTick(g_Config.m_ClDummy))))
			{
				m_Effects.Explosion(ExplosionPos, Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_HAMMERHIT)
		{
			const CNetEvent_HammerHit *pEvent =
				(const CNetEvent_HammerHit *)Item.m_pData;

			vec2 HammerHitPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(
				   CGameWorld::CPredictedEvent(
					   Item.m_Type, HammerHitPos, -1,
					   Client()->GameTick(g_Config.m_ClDummy))))
			{
				m_Effects.HammerHit(HammerHitPos, Alpha, Volume);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_BIRTHDAY)
		{
			const CNetEvent_Birthday *pEvent =
				(const CNetEvent_Birthday *)Item.m_pData;
			m_Effects.Confetti(vec2(pEvent->m_X, pEvent->m_Y), Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_FINISH)
		{
			const CNetEvent_Finish *pEvent = (const CNetEvent_Finish *)Item.m_pData;
			m_Effects.Confetti(vec2(pEvent->m_X, pEvent->m_Y), Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_SPAWN)
		{
			const CNetEvent_Spawn *pEvent = (const CNetEvent_Spawn *)Item.m_pData;
			m_Effects.PlayerSpawn(vec2(pEvent->m_X, pEvent->m_Y), Alpha, Volume);
		}
		else if(Item.m_Type == NETEVENTTYPE_DEATH)
		{
			const CNetEvent_Death *pEvent = (const CNetEvent_Death *)Item.m_pData;
			m_Effects.PlayerDeath(vec2(pEvent->m_X, pEvent->m_Y), pEvent->m_ClientId,
				Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_SOUNDWORLD)
		{
			const CNetEvent_SoundWorld *pEvent =
				(const CNetEvent_SoundWorld *)Item.m_pData;
			if(!Config()->m_SndGame)
				continue;

			if(m_GameInfo.m_RaceSounds &&
				((pEvent->m_SoundId == SOUND_GUN_FIRE && !g_Config.m_SndGun) ||
					(pEvent->m_SoundId == SOUND_PLAYER_PAIN_LONG &&
						!g_Config.m_SndLongPain)))
				continue;

			vec2 SoundPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(
				   CGameWorld::CPredictedEvent(
					   Item.m_Type, SoundPos, -1,
					   Client()->GameTick(g_Config.m_ClDummy), pEvent->m_SoundId)))
			{
				m_Sounds.PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId, 1.0f, SoundPos);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_MAPSOUNDWORLD)
		{
			CNetEvent_MapSoundWorld *pEvent = (CNetEvent_MapSoundWorld *)Item.m_pData;
			if(!Config()->m_SndGame)
				continue;

			m_MapSounds.PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId,
				vec2(pEvent->m_X, pEvent->m_Y));
		}
	}
}

static CGameInfo GetGameInfo(const CNetObj_GameInfoEx *pInfoEx, int InfoExSize,
	const CServerInfo *pFallbackServerInfo)
{
	int Version = -1;
	if(InfoExSize >= 12)
	{
		Version = pInfoEx->m_Version;
	}
	else if(InfoExSize >= 8)
	{
		Version = minimum(pInfoEx->m_Version, 4);
	}
	else if(InfoExSize >= 4)
	{
		Version = 0;
	}
	int Flags = 0;
	if(Version >= 0)
	{
		Flags = pInfoEx->m_Flags;
	}
	int Flags2 = 0;
	if(Version >= 5)
	{
		Flags2 = pInfoEx->m_Flags2;
	}
	bool Race;
	bool FastCap;
	bool FNG;
	bool DDRace;
	bool DDNet;
	bool BlockWorlds;
	bool City;
	bool Vanilla;
	bool Plus;
	bool FDDrace;
	if(Version < 1)
	{
		// The game type is intentionally only available inside this
		// `if`. Game type sniffing should be avoided and ideally not
		// extended. Mods should set the relevant game flags instead.
		const char *pGameType = pFallbackServerInfo->m_aGameType;
		Race = str_find_nocase(pGameType, "race") ||
		       str_find_nocase(pGameType, "fastcap");
		FastCap = str_find_nocase(pGameType, "fastcap");
		FNG = str_find_nocase(pGameType, "fng");
		DDRace = str_find_nocase(pGameType, "ddrace") ||
			 str_find_nocase(pGameType, "mkrace");
		DDNet = str_find_nocase(pGameType, "ddracenet") ||
			str_find_nocase(pGameType, "ddnet");
		BlockWorlds = str_startswith(pGameType, "bw  ") ||
			      str_comp_nocase(pGameType, "bw") == 0;
		City = str_find_nocase(pGameType, "city");
		Vanilla = str_comp(pGameType, "DM") == 0 ||
			  str_comp(pGameType, "TDM") == 0 ||
			  str_comp(pGameType, "CTF") == 0;
		Plus = str_find(pGameType, "+");
		FDDrace = false;
	}
	else
	{
		Race = Flags & GAMEINFOFLAG_GAMETYPE_RACE;
		FastCap = Flags & GAMEINFOFLAG_GAMETYPE_FASTCAP;
		FNG = Flags & GAMEINFOFLAG_GAMETYPE_FNG;
		DDRace = Flags & GAMEINFOFLAG_GAMETYPE_DDRACE;
		DDNet = Flags & GAMEINFOFLAG_GAMETYPE_DDNET;
		BlockWorlds = Flags & GAMEINFOFLAG_GAMETYPE_BLOCK_WORLDS;
		Vanilla = Flags & GAMEINFOFLAG_GAMETYPE_VANILLA;
		Plus = Flags & GAMEINFOFLAG_GAMETYPE_PLUS;
		City = Version >= 5 && Flags2 & GAMEINFOFLAG2_GAMETYPE_CITY;
		FDDrace = Version >= 6 && Flags2 & GAMEINFOFLAG2_GAMETYPE_FDDRACE;

		// Ensure invariants upheld by the server info parsing business.
		DDRace = DDRace || DDNet || FDDrace;
		Race = Race || FastCap || DDRace;
	}

	CGameInfo Info;
	Info.m_FlagStartsRace = FastCap;
	Info.m_TimeScore = Race;
	Info.m_UnlimitedAmmo = Race;
	Info.m_DDRaceRecordMessage = DDRace && !DDNet;
	Info.m_RaceRecordMessage = DDNet || (Race && !DDRace);
	Info.m_RaceSounds = DDRace || FNG || BlockWorlds;
	Info.m_AllowEyeWheel = DDRace || BlockWorlds || City || Plus;
	Info.m_AllowHookColl = DDRace;
	Info.m_AllowZoom = Race || BlockWorlds || City;
	Info.m_BugDDRaceGhost = DDRace;
	Info.m_BugDDRaceInput = DDRace;
	Info.m_BugFNGLaserRange = FNG;
	Info.m_BugVanillaBounce = Vanilla;
	Info.m_PredictFNG = FNG;
	Info.m_PredictDDRace = DDRace;
	Info.m_PredictDDRaceTiles = DDRace && !BlockWorlds;
	Info.m_PredictVanilla = Vanilla || FastCap;
	Info.m_EntitiesDDNet = DDNet;
	Info.m_EntitiesDDRace = DDRace;
	Info.m_EntitiesRace = Race;
	Info.m_EntitiesFNG = FNG;
	Info.m_EntitiesVanilla = Vanilla;
	Info.m_EntitiesBW = BlockWorlds;
	Info.m_Race = Race;
	Info.m_Pvp = !Race;
	Info.m_DontMaskEntities = !DDNet;
	Info.m_AllowXSkins = false;
	Info.m_EntitiesFDDrace = FDDrace;
	Info.m_HudHealthArmor = true;
	Info.m_HudAmmo = true;
	Info.m_HudDDRace = false;
	Info.m_NoWeakHookAndBounce = false;
	Info.m_NoSkinChangeForFrozen = false;
	Info.m_DDRaceTeam = false;
	Info.m_PredictEvents = Vanilla;

	if(Version >= 0)
	{
		Info.m_TimeScore = Flags & GAMEINFOFLAG_TIMESCORE;
	}
	if(Version >= 2)
	{
		Info.m_FlagStartsRace = Flags & GAMEINFOFLAG_FLAG_STARTS_RACE;
		Info.m_UnlimitedAmmo = Flags & GAMEINFOFLAG_UNLIMITED_AMMO;
		Info.m_DDRaceRecordMessage = Flags & GAMEINFOFLAG_DDRACE_RECORD_MESSAGE;
		Info.m_RaceRecordMessage = Flags & GAMEINFOFLAG_RACE_RECORD_MESSAGE;
		Info.m_AllowEyeWheel = Flags & GAMEINFOFLAG_ALLOW_EYE_WHEEL;
		Info.m_AllowHookColl = Flags & GAMEINFOFLAG_ALLOW_HOOK_COLL;
		Info.m_AllowZoom = Flags & GAMEINFOFLAG_ALLOW_ZOOM;
		Info.m_BugDDRaceGhost = Flags & GAMEINFOFLAG_BUG_DDRACE_GHOST;
		Info.m_BugDDRaceInput = Flags & GAMEINFOFLAG_BUG_DDRACE_INPUT;
		Info.m_BugFNGLaserRange = Flags & GAMEINFOFLAG_BUG_FNG_LASER_RANGE;
		Info.m_BugVanillaBounce = Flags & GAMEINFOFLAG_BUG_VANILLA_BOUNCE;
		Info.m_PredictFNG = Flags & GAMEINFOFLAG_PREDICT_FNG;
		Info.m_PredictDDRace = Flags & GAMEINFOFLAG_PREDICT_DDRACE;
		Info.m_PredictDDRaceTiles = Flags & GAMEINFOFLAG_PREDICT_DDRACE_TILES;
		Info.m_PredictVanilla = Flags & GAMEINFOFLAG_PREDICT_VANILLA;
		Info.m_EntitiesDDNet = Flags & GAMEINFOFLAG_ENTITIES_DDNET;
		Info.m_EntitiesDDRace = Flags & GAMEINFOFLAG_ENTITIES_DDRACE;
		Info.m_EntitiesRace = Flags & GAMEINFOFLAG_ENTITIES_RACE;
		Info.m_EntitiesFNG = Flags & GAMEINFOFLAG_ENTITIES_FNG;
		Info.m_EntitiesVanilla = Flags & GAMEINFOFLAG_ENTITIES_VANILLA;
	}
	if(Version >= 3)
	{
		Info.m_Race = Flags & GAMEINFOFLAG_RACE;
		Info.m_DontMaskEntities = Flags & GAMEINFOFLAG_DONT_MASK_ENTITIES;
	}
	if(Version >= 4)
	{
		Info.m_EntitiesBW = Flags & GAMEINFOFLAG_ENTITIES_BW;
	}
	if(Version >= 5)
	{
		Info.m_AllowXSkins = Flags2 & GAMEINFOFLAG2_ALLOW_X_SKINS;
	}
	if(Version >= 6)
	{
		Info.m_EntitiesFDDrace = Flags2 & GAMEINFOFLAG2_ENTITIES_FDDRACE;
	}
	if(Version >= 7)
	{
		Info.m_HudHealthArmor = Flags2 & GAMEINFOFLAG2_HUD_HEALTH_ARMOR;
		Info.m_HudAmmo = Flags2 & GAMEINFOFLAG2_HUD_AMMO;
		Info.m_HudDDRace = Flags2 & GAMEINFOFLAG2_HUD_DDRACE;
	}
	if(Version >= 8)
	{
		Info.m_NoWeakHookAndBounce = Flags2 & GAMEINFOFLAG2_NO_WEAK_HOOK;
	}
	if(Version >= 9)
	{
		Info.m_NoSkinChangeForFrozen =
			Flags2 & GAMEINFOFLAG2_NO_SKIN_CHANGE_FOR_FROZEN;
	}
	if(Version >= 10)
	{
		Info.m_DDRaceTeam = Flags2 & GAMEINFOFLAG2_DDRACE_TEAM;
	}
	if(Version >= 11)
	{
		Info.m_PredictEvents = Flags2 & GAMEINFOFLAG2_PREDICT_EVENTS;
	}

	// TClient
	str_copy(Info.m_aGameType, pFallbackServerInfo->m_aGameType);

	return Info;
}

void CGameClient::InvalidateSnapshot()
{
	// clear all pointers
	mem_zero(&m_Snap, sizeof(m_Snap));
	m_Snap.m_SpecInfo.m_Zoom = 1.0f;
	m_Snap.m_LocalClientId = -1;
	SnapCollectEntities();
}

void CGameClient::OnNewSnapshot(bool DummySwapped)
{
	auto &&Evolve = [this](CNetObj_Character *pCharacter, int Tick) {
		CWorldCore TempWorld;
		CCharacterCore TempCore = CCharacterCore();
		CTeamsCore TempTeams = CTeamsCore();
		TempCore.Init(&TempWorld, Collision(), &TempTeams);
		TempCore.Read(pCharacter);
		TempCore.m_ActiveWeapon = pCharacter->m_Weapon;

		while(pCharacter->m_Tick < Tick)
		{
			pCharacter->m_Tick++;
			TempCore.Tick(false);
			TempCore.Move();
			TempCore.Quantize();
		}

		TempCore.Write(pCharacter);
	};

	InvalidateSnapshot();

	m_NewTick = true;

	ProcessEvents();

	if(g_Config.m_DbgStress)
	{
		if((Client()->GameTick(g_Config.m_ClDummy) % 100) == 0)
		{
			char aMessage[64];
			int MsgLen = rand() % (sizeof(aMessage) - 1);
			for(int i = 0; i < MsgLen; i++)
				aMessage[i] = (char)('a' + (rand() % ('z' - 'a')));
			aMessage[MsgLen] = 0;

			m_Chat.SendChat(rand() & 1, aMessage);
		}
	}

	CServerInfo ServerInfo;
	Client()->GetServerInfo(&ServerInfo);

	bool FoundGameInfoEx = false;
	bool GotSwitchStateTeam = false;
	bool HasUnsetDDNetFinishTimes = false;
	bool HasTrueMillisecondFinishTimes = false;
	m_aSwitchStateTeam[g_Config.m_ClDummy] = -1;

	for(auto &Client : m_aClients)
	{
		Client.m_SpecCharPresent = false;
	}

	// go through all the items in the snapshot and gather the info we want
	{
		m_Snap.m_aTeamSize[TEAM_RED] = m_Snap.m_aTeamSize[TEAM_BLUE] = 0;

		int Num = Client()->SnapNumItems(IClient::SNAP_CURRENT);
		for(int i = 0; i < Num; i++)
		{
			const IClient::CSnapItem Item =
				Client()->SnapGetItem(IClient::SNAP_CURRENT, i);

			if(Item.m_Type == NETOBJTYPE_CLIENTINFO)
			{
				const CNetObj_ClientInfo *pInfo =
					(const CNetObj_ClientInfo *)Item.m_pData;
				int ClientId = Item.m_Id;
				if(ClientId < MAX_CLIENTS)
				{
					CClientData *pClient = &m_aClients[ClientId];

					if(!IntsToStr(pInfo->m_aName, std::size(pInfo->m_aName),
						   pClient->m_aName, std::size(pClient->m_aName)))
					{
						str_copy(pClient->m_aName, "nameless tee");
					}
					IntsToStr(pInfo->m_aClan, std::size(pInfo->m_aClan), pClient->m_aClan,
						std::size(pClient->m_aClan));
					pClient->m_Country = pInfo->m_Country;

					IntsToStr(pInfo->m_aSkin, std::size(pInfo->m_aSkin),
						pClient->m_aSkinName, std::size(pClient->m_aSkinName));
					if(!CSkin::IsValidName(pClient->m_aSkinName) ||
						(!m_GameInfo.m_AllowXSkins &&
							CSkins::IsSpecialSkin(pClient->m_aSkinName)))
					{
						str_copy(pClient->m_aSkinName, "default");
					}

					pClient->m_UseCustomColor = pInfo->m_UseCustomColor;
					pClient->m_ColorBody = pInfo->m_ColorBody;
					pClient->m_ColorFeet = pInfo->m_ColorFeet;
				}
			}
			else if(Item.m_Type == NETOBJTYPE_PLAYERINFO)
			{
				const CNetObj_PlayerInfo *pInfo =
					(const CNetObj_PlayerInfo *)Item.m_pData;

				if(pInfo->m_ClientId < MAX_CLIENTS && pInfo->m_ClientId == Item.m_Id)
				{
					m_aClients[pInfo->m_ClientId].m_Team = pInfo->m_Team;
					m_aClients[pInfo->m_ClientId].m_Active = true;
					m_Snap.m_apPlayerInfos[pInfo->m_ClientId] = pInfo;
					m_Snap.m_apPrevPlayerInfos[pInfo->m_ClientId] =
						static_cast<const CNetObj_PlayerInfo *>(Client()->SnapFindItem(
							IClient::SNAP_PREV, Item.m_Type, pInfo->m_ClientId));
					m_Snap.m_NumPlayers++;

					if(pInfo->m_Local)
					{
						m_Snap.m_LocalClientId = pInfo->m_ClientId;
						m_Snap.m_pLocalInfo = pInfo;

						if(pInfo->m_Team == TEAM_SPECTATORS)
						{
							m_Snap.m_SpecInfo.m_Active = true;
						}
					}

					m_Snap.m_HighestClientId =
						maximum(m_Snap.m_HighestClientId, pInfo->m_ClientId);

					// calculate team-balance
					if(pInfo->m_Team != TEAM_SPECTATORS)
					{
						m_Snap.m_aTeamSize[pInfo->m_Team]++;
						if(!m_aStats[pInfo->m_ClientId].IsActive())
							m_aStats[pInfo->m_ClientId].JoinGame(
								Client()->GameTick(g_Config.m_ClDummy));
					}
					else if(m_aStats[pInfo->m_ClientId].IsActive())
						m_aStats[pInfo->m_ClientId].JoinSpec(
							Client()->GameTick(g_Config.m_ClDummy));
				}
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETPLAYER)
			{
				m_ReceivedDDNetPlayer = true;
				const CNetObj_DDNetPlayer *pInfo =
					(const CNetObj_DDNetPlayer *)Item.m_pData;
				if(Item.m_Id < MAX_CLIENTS)
				{
					m_aClients[Item.m_Id].m_AuthLevel = pInfo->m_AuthLevel;
					m_aClients[Item.m_Id].m_Afk = pInfo->m_Flags & EXPLAYERFLAG_AFK;
					m_aClients[Item.m_Id].m_Paused = pInfo->m_Flags & EXPLAYERFLAG_PAUSED;
					m_aClients[Item.m_Id].m_Spec = pInfo->m_Flags & EXPLAYERFLAG_SPEC;
					m_aClients[Item.m_Id].m_FinishTimeSeconds =
						pInfo->m_FinishTimeSeconds;
					m_aClients[Item.m_Id].m_FinishTimeMillis = pInfo->m_FinishTimeMillis;

					if(m_aClients[Item.m_Id].m_FinishTimeSeconds == FinishTime::UNSET)
						HasUnsetDDNetFinishTimes = true;
					else if(m_aClients[Item.m_Id].m_FinishTimeMillis % 10 != 0)
						HasTrueMillisecondFinishTimes = true;

					if(Item.m_Id == m_Snap.m_LocalClientId &&
						(m_aClients[Item.m_Id].m_Paused ||
							m_aClients[Item.m_Id].m_Spec))
					{
						m_Snap.m_SpecInfo.m_Active = true;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_CHARACTER)
			{
				if(Item.m_Id < MAX_CLIENTS)
				{
					const void *pOld = Client()->SnapFindItem(
						IClient::SNAP_PREV, NETOBJTYPE_CHARACTER, Item.m_Id);
					m_Snap.m_aCharacters[Item.m_Id].m_Cur =
						*((const CNetObj_Character *)Item.m_pData);
					if(pOld)
					{
						m_Snap.m_aCharacters[Item.m_Id].m_Active = true;
						m_Snap.m_aCharacters[Item.m_Id].m_Prev =
							*((const CNetObj_Character *)pOld);

						// limit evolving to 3 seconds
						bool EvolvePrev =
							Client()->PrevGameTick(g_Config.m_ClDummy) -
								m_Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick <=
							3 * Client()->GameTickSpeed();
						bool EvolveCur = Client()->GameTick(g_Config.m_ClDummy) -
									 m_Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick <=
								 3 * Client()->GameTickSpeed();

						// reuse the result from the previous evolve if the snapped
						// character didn't change since the previous snapshot
						if(EvolveCur && m_aClients[Item.m_Id].m_Evolved.m_Tick ==
									Client()->PrevGameTick(g_Config.m_ClDummy))
						{
							if(mem_comp(&m_Snap.m_aCharacters[Item.m_Id].m_Prev,
								   &m_aClients[Item.m_Id].m_Snapped,
								   sizeof(CNetObj_Character)) == 0)
								m_Snap.m_aCharacters[Item.m_Id].m_Prev =
									m_aClients[Item.m_Id].m_Evolved;
							if(mem_comp(&m_Snap.m_aCharacters[Item.m_Id].m_Cur,
								   &m_aClients[Item.m_Id].m_Snapped,
								   sizeof(CNetObj_Character)) == 0)
								m_Snap.m_aCharacters[Item.m_Id].m_Cur =
									m_aClients[Item.m_Id].m_Evolved;
						}

						if(EvolvePrev && m_Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick)
							Evolve(&m_Snap.m_aCharacters[Item.m_Id].m_Prev,
								Client()->PrevGameTick(g_Config.m_ClDummy));
						if(EvolveCur && m_Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick)
							Evolve(&m_Snap.m_aCharacters[Item.m_Id].m_Cur,
								Client()->GameTick(g_Config.m_ClDummy));

						m_aClients[Item.m_Id].m_Snapped =
							*((const CNetObj_Character *)Item.m_pData);
						m_aClients[Item.m_Id].m_Evolved =
							m_Snap.m_aCharacters[Item.m_Id].m_Cur;
					}
					else
					{
						m_aClients[Item.m_Id].m_Evolved.m_Tick = -1;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETCHARACTER)
			{
				const CNetObj_DDNetCharacter *pCharacterData =
					(const CNetObj_DDNetCharacter *)Item.m_pData;

				if(Item.m_Id < MAX_CLIENTS)
				{
					m_Snap.m_aCharacters[Item.m_Id].m_ExtendedData = *pCharacterData;
					m_Snap.m_aCharacters[Item.m_Id].m_pPrevExtendedData =
						(const CNetObj_DDNetCharacter *)Client()->SnapFindItem(
							IClient::SNAP_PREV, NETOBJTYPE_DDNETCHARACTER, Item.m_Id);
					m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedData = true;
					m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = false;
					if(pCharacterData->m_JumpedTotal != -1)
					{
						m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = true;
					}
					CClientData *pClient = &m_aClients[Item.m_Id];
					// Collision
					pClient->m_Solo = pCharacterData->m_Flags & CHARACTERFLAG_SOLO;
					pClient->m_Jetpack = pCharacterData->m_Flags & CHARACTERFLAG_JETPACK;
					pClient->m_CollisionDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_COLLISION_DISABLED;
					pClient->m_HammerHitDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_HAMMER_HIT_DISABLED;
					pClient->m_GrenadeHitDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_GRENADE_HIT_DISABLED;
					pClient->m_LaserHitDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_LASER_HIT_DISABLED;
					pClient->m_ShotgunHitDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_SHOTGUN_HIT_DISABLED;
					pClient->m_HookHitDisabled =
						pCharacterData->m_Flags & CHARACTERFLAG_HOOK_HIT_DISABLED;
					pClient->m_Super = pCharacterData->m_Flags & CHARACTERFLAG_SUPER;
					pClient->m_Invincible =
						pCharacterData->m_Flags & CHARACTERFLAG_INVINCIBLE;

					// Endless
					pClient->m_EndlessHook =
						pCharacterData->m_Flags & CHARACTERFLAG_ENDLESS_HOOK;
					pClient->m_EndlessJump =
						pCharacterData->m_Flags & CHARACTERFLAG_ENDLESS_JUMP;

					// Freeze
					pClient->m_FreezeEnd = pCharacterData->m_FreezeEnd;
					pClient->m_DeepFrozen = pCharacterData->m_FreezeEnd == -1;
					pClient->m_LiveFrozen =
						(pCharacterData->m_Flags & CHARACTERFLAG_MOVEMENTS_DISABLED) != 0;

					// Telegun
					pClient->m_HasTelegunGrenade =
						pCharacterData->m_Flags & CHARACTERFLAG_TELEGUN_GRENADE;
					pClient->m_HasTelegunGun =
						pCharacterData->m_Flags & CHARACTERFLAG_TELEGUN_GUN;
					pClient->m_HasTelegunLaser =
						pCharacterData->m_Flags & CHARACTERFLAG_TELEGUN_LASER;

					pClient->m_Predicted.ReadDDNet(pCharacterData);

					// TClient
					pClient->m_RegularPredicted.ReadDDNet(pCharacterData);

					m_Teams.SetSolo(Item.m_Id, pClient->m_Solo);
				}
			}
			else if(Item.m_Type == NETOBJTYPE_SPECCHAR)
			{
				const CNetObj_SpecChar *pSpecCharData =
					(const CNetObj_SpecChar *)Item.m_pData;

				if(Item.m_Id < MAX_CLIENTS)
				{
					CClientData *pClient = &m_aClients[Item.m_Id];
					pClient->m_SpecCharPresent = true;
					pClient->m_SpecChar.x = pSpecCharData->m_X;
					pClient->m_SpecChar.y = pSpecCharData->m_Y;
				}
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORINFO)
			{
				m_Snap.m_pSpectatorInfo = (const CNetObj_SpectatorInfo *)Item.m_pData;
				m_Snap.m_pPrevSpectatorInfo =
					(const CNetObj_SpectatorInfo *)Client()->SnapFindItem(
						IClient::SNAP_PREV, NETOBJTYPE_SPECTATORINFO, Item.m_Id);

				// needed for 0.7 survival
				// to auto spec players when dead
				if(Client()->IsSixup())
					m_Snap.m_SpecInfo.m_Active = true;
				m_Snap.m_SpecInfo.m_SpectatorId =
					m_Snap.m_pSpectatorInfo->m_SpectatorId;
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETSPECTATORINFO)
			{
				const CNetObj_DDNetSpectatorInfo *pDDNetSpecInfo =
					(const CNetObj_DDNetSpectatorInfo *)Item.m_pData;
				m_Snap.m_SpecInfo.m_HasCameraInfo = pDDNetSpecInfo->m_HasCameraInfo;
				m_Snap.m_SpecInfo.m_Zoom = pDDNetSpecInfo->m_Zoom / 1000.0f;
				m_Snap.m_SpecInfo.m_Deadzone = pDDNetSpecInfo->m_Deadzone;
				m_Snap.m_SpecInfo.m_FollowFactor = pDDNetSpecInfo->m_FollowFactor;
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORCOUNT)
			{
				m_Snap.m_pSpectatorCount = (const CNetObj_SpectatorCount *)Item.m_pData;
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEINFO)
			{
				m_Snap.m_pGameInfoObj = (const CNetObj_GameInfo *)Item.m_pData;
				bool CurrentTickGameOver =
					(bool)(m_Snap.m_pGameInfoObj->m_GameStateFlags &
						GAMESTATEFLAG_GAMEOVER);
				if(!m_GameOver && CurrentTickGameOver)
					OnGameOver();
				else if(m_GameOver && !CurrentTickGameOver)
					OnStartGame();
				// Handle case that a new round is started (RoundStartTick changed)
				// New round is usually started after `restart` on server
				if(m_Snap.m_pGameInfoObj->m_RoundStartTick != m_LastRoundStartTick &&
					!(CurrentTickGameOver ||
						m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED ||
						m_GamePaused))
					OnStartRound();
				m_LastRoundStartTick = m_Snap.m_pGameInfoObj->m_RoundStartTick;
				m_GameOver = CurrentTickGameOver;
				m_GamePaused = (bool)(m_Snap.m_pGameInfoObj->m_GameStateFlags &
						      GAMESTATEFLAG_PAUSED);
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEINFOEX)
			{
				if(FoundGameInfoEx)
				{
					continue;
				}
				FoundGameInfoEx = true;
				m_GameInfo = GetGameInfo((const CNetObj_GameInfoEx *)Item.m_pData,
					Item.m_DataSize, &ServerInfo);
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEDATA)
			{
				m_Snap.m_pGameDataObj =
					static_cast<const CNetObj_GameData *>(Item.m_pData);
				m_Snap.m_pPrevGameDataObj = static_cast<const CNetObj_GameData *>(
					Client()->SnapFindItem(IClient::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(m_Snap.m_pGameDataObj->m_FlagCarrierRed == FLAG_TAKEN)
				{
					if(m_aFlagDropTick[TEAM_RED] == 0)
						m_aFlagDropTick[TEAM_RED] = Client()->GameTick(g_Config.m_ClDummy);
				}
				else
					m_aFlagDropTick[TEAM_RED] = 0;
				if(m_Snap.m_pGameDataObj->m_FlagCarrierBlue == FLAG_TAKEN)
				{
					if(m_aFlagDropTick[TEAM_BLUE] == 0)
						m_aFlagDropTick[TEAM_BLUE] = Client()->GameTick(g_Config.m_ClDummy);
				}
				else
					m_aFlagDropTick[TEAM_BLUE] = 0;
				if(m_LastFlagCarrierRed == FLAG_ATSTAND &&
					m_Snap.m_pGameDataObj->m_FlagCarrierRed >= 0)
					OnFlagGrab(TEAM_RED);
				else if(m_LastFlagCarrierBlue == FLAG_ATSTAND &&
					m_Snap.m_pGameDataObj->m_FlagCarrierBlue >= 0)
					OnFlagGrab(TEAM_BLUE);

				m_LastFlagCarrierRed = m_Snap.m_pGameDataObj->m_FlagCarrierRed;
				m_LastFlagCarrierBlue = m_Snap.m_pGameDataObj->m_FlagCarrierBlue;
			}
			else if(Item.m_Type == NETOBJTYPE_FLAG)
			{
				const CNetObj_Flag *pPrevFlag = static_cast<const CNetObj_Flag *>(
					Client()->SnapFindItem(IClient::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(pPrevFlag == nullptr)
				{
					continue;
				}
				m_Snap.m_apFlags[m_Snap.m_NumFlags] =
					static_cast<const CNetObj_Flag *>(Item.m_pData);
				m_Snap.m_apPrevFlags[m_Snap.m_NumFlags] = pPrevFlag;
				++m_Snap.m_NumFlags;
			}
			else if(Item.m_Type == NETOBJTYPE_SWITCHSTATE)
			{
				if(Item.m_DataSize < 36)
				{
					continue;
				}
				const CNetObj_SwitchState *pSwitchStateData =
					(const CNetObj_SwitchState *)Item.m_pData;
				int Team = std::clamp(Item.m_Id, (int)TEAM_FLOCK, (int)TEAM_SUPER - 1);

				int HighestSwitchNumber =
					std::clamp(pSwitchStateData->m_HighestSwitchNumber, 0, 255);
				if(HighestSwitchNumber != maximum(0, (int)Switchers().size() - 1))
				{
					m_GameWorld.m_Core.InitSwitchers(HighestSwitchNumber);
					Collision()->m_HighestSwitchNumber = HighestSwitchNumber;
				}

				for(int j = 0; j < (int)Switchers().size(); j++)
				{
					Switchers()[j].m_aStatus[Team] =
						(pSwitchStateData->m_aStatus[j / 32] >> (j % 32)) & 1;
				}

				if(Item.m_DataSize >= 68)
				{
					// update the endtick of up to four timed switchers
					for(int j = 0; j < (int)std::size(pSwitchStateData->m_aEndTicks);
						j++)
					{
						int SwitchNumber = pSwitchStateData->m_aSwitchNumbers[j];
						int EndTick = pSwitchStateData->m_aEndTicks[j];
						if(EndTick > 0 &&
							in_range(SwitchNumber, 0, (int)Switchers().size()))
						{
							Switchers()[SwitchNumber].m_aEndTick[Team] = EndTick;
						}
					}
				}

				// update switch types
				for(auto &Switcher : Switchers())
				{
					if(Switcher.m_aStatus[Team])
						Switcher.m_aType[Team] = Switcher.m_aEndTick[Team] ? TILE_SWITCHTIMEDOPEN : TILE_SWITCHOPEN;
					else
						Switcher.m_aType[Team] = Switcher.m_aEndTick[Team] ? TILE_SWITCHTIMEDCLOSE : TILE_SWITCHCLOSE;
				}

				if(!GotSwitchStateTeam)
					m_aSwitchStateTeam[g_Config.m_ClDummy] = Team;
				else
					m_aSwitchStateTeam[g_Config.m_ClDummy] = -1;
				GotSwitchStateTeam = true;
			}
			else if(Item.m_Type == NETOBJTYPE_MAPBESTTIME)
			{
				const CNetObj_MapBestTime *pMapBestTimeData =
					static_cast<const CNetObj_MapBestTime *>(Item.m_pData);
				m_MapBestTimeSeconds = pMapBestTimeData->m_MapBestTimeSeconds;
				m_MapBestTimeMillis = pMapBestTimeData->m_MapBestTimeMillis;
			}
		}
	}

	if(!FoundGameInfoEx)
	{
		m_GameInfo = GetGameInfo(nullptr, 0, &ServerInfo);
	}

	for(CClientData &Client : m_aClients)
	{
		Client.UpdateSkinInfo();
	}

	// setup local pointers
	if(m_Snap.m_LocalClientId >= 0)
	{
		m_aLocalIds[g_Config.m_ClDummy] = m_Snap.m_LocalClientId;

		CSnapState::CCharacterInfo *pChr =
			&m_Snap.m_aCharacters[m_Snap.m_LocalClientId];
		if(pChr->m_Active)
		{
			if(!m_Snap.m_SpecInfo.m_Active)
			{
				m_Snap.m_pLocalCharacter = &pChr->m_Cur;
				m_Snap.m_pLocalPrevCharacter = &pChr->m_Prev;
				m_LocalCharacterPos =
					vec2(m_Snap.m_pLocalCharacter->m_X, m_Snap.m_pLocalCharacter->m_Y);
			}
		}
		else if(Client()->SnapFindItem(IClient::SNAP_PREV, NETOBJTYPE_CHARACTER,
				m_Snap.m_LocalClientId))
		{
			// player died
			m_Controls.OnPlayerDeath();
		}
	}
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		if(m_Snap.m_LocalClientId == -1 && m_DemoSpecId == SPEC_FOLLOW)
		{
			// TODO: can this be done in the translation layer?
			if(!Client()->IsSixup())
				m_DemoSpecId = SPEC_FREEVIEW;
		}
		if(m_DemoSpecId != SPEC_FOLLOW)
		{
			m_Snap.m_SpecInfo.m_Active = true;
			if(m_DemoSpecId > SPEC_FREEVIEW &&
				m_Snap.m_aCharacters[m_DemoSpecId].m_Active)
				m_Snap.m_SpecInfo.m_SpectatorId = m_DemoSpecId;
			else
				m_Snap.m_SpecInfo.m_SpectatorId = SPEC_FREEVIEW;
		}
	}

	// clear out unneeded client data
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(!m_Snap.m_apPlayerInfos[i] && m_aClients[i].m_Active)
		{
			m_aClients[i].Reset();
			m_aStats[i].Reset();
		}
	}

	if(Client()->State() == IClient::STATE_ONLINE)
	{
		m_pDiscord->UpdatePlayerCount(m_Snap.m_NumPlayers);
	}

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		// update friend state
		m_aClients[i].m_Friend =
			!(i == m_Snap.m_LocalClientId || !m_Snap.m_apPlayerInfos[i] ||
				!Friends()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan,
					true));

		// update foe state
		m_aClients[i].m_Foe = !(
			i == m_Snap.m_LocalClientId || !m_Snap.m_apPlayerInfos[i] ||
			!Foes()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan, true));
	}

	// check if we received all finish times
	m_ReceivedDDNetPlayerFinishTimes =
		m_ReceivedDDNetPlayer && !HasUnsetDDNetFinishTimes;
	m_ReceivedDDNetPlayerFinishTimesMillis =
		m_ReceivedDDNetPlayer && HasTrueMillisecondFinishTimes;

	// sort player infos by name
	mem_copy(m_Snap.m_apInfoByName, m_Snap.m_apPlayerInfos,
		sizeof(m_Snap.m_apInfoByName));
	std::stable_sort(m_Snap.m_apInfoByName, m_Snap.m_apInfoByName + MAX_CLIENTS,
		[this](const CNetObj_PlayerInfo *pPlayer1,
			const CNetObj_PlayerInfo *pPlayer2) -> bool {
			if(!pPlayer2)
				return static_cast<bool>(pPlayer1);
			if(!pPlayer1)
				return false;
			return str_comp_nocase(
				       m_aClients[pPlayer1->m_ClientId].m_aName,
				       m_aClients[pPlayer2->m_ClientId].m_aName) < 0;
		});

	bool TimeScore = m_GameInfo.m_TimeScore;
	bool Race7 = Client()->IsSixup() && m_Snap.m_pGameInfoObj &&
		     m_Snap.m_pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE;

	// sort player infos by score
	mem_copy(m_Snap.m_apInfoByScore, m_Snap.m_apInfoByName,
		sizeof(m_Snap.m_apInfoByScore));
	auto TimeComparator = CGameClient::GetScoreComparator(
		TimeScore, m_ReceivedDDNetPlayerFinishTimes, Race7);
	auto SortByTimeScore = [TimeComparator,
				       this](const CNetObj_PlayerInfo *pPlayer1,
				       const CNetObj_PlayerInfo *pPlayer2) -> bool {
		if(!pPlayer2)
			return static_cast<bool>(pPlayer1);
		if(!pPlayer1)
			return false;
		if(m_ReceivedDDNetPlayerFinishTimes)
			return TimeComparator(
				m_aClients[pPlayer1->m_ClientId].m_FinishTimeSeconds,
				m_aClients[pPlayer2->m_ClientId].m_FinishTimeSeconds,
				m_aClients[pPlayer1->m_ClientId].m_FinishTimeMillis,
				m_aClients[pPlayer2->m_ClientId].m_FinishTimeMillis);
		return TimeComparator(pPlayer1->m_Score, pPlayer2->m_Score, 0, 0);
	};
	std::stable_sort(m_Snap.m_apInfoByScore, m_Snap.m_apInfoByScore + MAX_CLIENTS,
		SortByTimeScore);

	// sort player infos by DDRace Team (and score between)
	int Index = 0;
	for(int Team = TEAM_FLOCK; Team <= TEAM_SUPER; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(m_Snap.m_apInfoByScore[i] &&
				m_Teams.Team(m_Snap.m_apInfoByScore[i]->m_ClientId) == Team)
				m_Snap.m_apInfoByDDTeamScore[Index++] = m_Snap.m_apInfoByScore[i];
		}
	}

	// sort player infos by DDRace Team (and name between)
	Index = 0;
	for(int Team = TEAM_FLOCK; Team <= TEAM_SUPER; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(m_Snap.m_apInfoByName[i] &&
				m_Teams.Team(m_Snap.m_apInfoByName[i]->m_ClientId) == Team)
				m_Snap.m_apInfoByDDTeamName[Index++] = m_Snap.m_apInfoByName[i];
		}
	}

	if(ServerInfo.m_aGameType[0] != '0')
	{
		if(str_comp(ServerInfo.m_aGameType, "DM") != 0 &&
			str_comp(ServerInfo.m_aGameType, "TDM") != 0 &&
			str_comp(ServerInfo.m_aGameType, "CTF") != 0)
			m_ServerMode = SERVERMODE_MOD;
		else if(mem_comp(&CTuningParams::DEFAULT, &m_aTuning[g_Config.m_ClDummy],
				33) == 0)
			m_ServerMode = SERVERMODE_PURE;
		else
			m_ServerMode = SERVERMODE_PUREMOD;
	}

	// add tuning to demo when new recording was started, because server tune
	// message was already received before
	std::bitset<RECORDER_MAX> CurrentRecordings;
	for(int i = 0; i < RECORDER_MAX; i++)
	{
		if(DemoRecorder(i)->IsRecording())
		{
			CurrentRecordings.set(i);
		}
	}
	const bool HasNewRecordings = (CurrentRecordings & ~m_ActiveRecordings).any();
	m_ActiveRecordings = CurrentRecordings;
	if(HasNewRecordings)
	{
		CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
		int *pParams = (int *)&m_aTuning[g_Config.m_ClDummy];
		for(unsigned i = 0; i < sizeof(m_aTuning[0]) / sizeof(int); i++)
			Msg.AddInt(pParams[i]);
		Client()->SendMsgActive(&Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
	}

	for(int i = 0; i < NUM_DUMMIES; i++)
	{
		if(m_aDDRaceMsgSent[i] || !m_Snap.m_pLocalInfo)
		{
			continue;
		}
		if(i != IClient::CONN_MAIN && !Client()->DummyConnected(i))
		{
			continue;
		}
		CMsgPacker Msg(NETMSGTYPE_CL_ISDDNETLEGACY, false);
		Msg.AddInt(DDNetVersion());
		Client()->SendMsg(i, &Msg, MSGFLAG_VITAL);
		m_aDDRaceMsgSent[i] = true;
	}

	if(m_Snap.m_SpecInfo.m_Active && m_MultiViewActivated)
	{
		// dont show other teams while spectating in multi view
		CNetMsg_Cl_ShowOthers Msg;
		Msg.m_Show = SHOW_OTHERS_ONLY_TEAM;
		Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);

		// update state
		m_aShowOthers[g_Config.m_ClDummy] = SHOW_OTHERS_ONLY_TEAM;
	}
	else if(m_aShowOthers[g_Config.m_ClDummy] == SHOW_OTHERS_NOT_SET ||
		m_aShowOthers[g_Config.m_ClDummy] != g_Config.m_ClShowOthers)
	{
		{
			CNetMsg_Cl_ShowOthers Msg;
			Msg.m_Show = g_Config.m_ClShowOthers;
			Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
		}

		// update state
		m_aShowOthers[g_Config.m_ClDummy] = g_Config.m_ClShowOthers;
	}

	for(int Conn = IClient::CONN_MAIN; Conn < NUM_DUMMIES; Conn++)
	{
		if(Conn != IClient::CONN_MAIN && !Client()->DummyConnected(Conn))
			continue;
		const int EnableSpectatorCount =
			g_Config.m_ClShowhudSpectatorCount || g_Config.m_ClKnWatcherList;
		if(m_aEnableSpectatorCount[Conn] != -1 &&
			m_aEnableSpectatorCount[Conn] == EnableSpectatorCount)
			continue;
		CNetMsg_Cl_EnableSpectatorCount Msg;
		Msg.m_Enable = EnableSpectatorCount;
		Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);
		m_aEnableSpectatorCount[Conn] = EnableSpectatorCount;
	}

	if(DummySwapped)
		m_Camera.UpdateCamera();

	float ShowDistanceZoom = m_Camera.m_Zoom;
	float Zoom = m_Camera.m_Zoom;
	if(m_Camera.m_Zooming)
	{
		if(m_Camera.m_ZoomSmoothingTarget > m_Camera.m_Zoom) // Zooming out
			ShowDistanceZoom = m_Camera.m_ZoomSmoothingTarget;
		else if(m_Camera.m_ZoomSmoothingTarget < m_Camera.m_Zoom &&
			m_LastShowDistanceZoom > 0) // Zooming in
			ShowDistanceZoom = m_LastShowDistanceZoom;

		Zoom = m_Camera.m_ZoomSmoothingTarget;
	}

	float Deadzone = m_Camera.Deadzone();
	float FollowFactor = m_Camera.FollowFactor();

	if(!m_Camera.ZoomAllowed())
	{
		ShowDistanceZoom = 1.0f;
		Zoom = 1.0f;
	}

	if(m_Snap.m_SpecInfo.m_Active)
	{
		// don't send camera information when spectating
		Zoom = m_LastZoom;
		Deadzone = m_LastDeadzone;
		FollowFactor = m_LastFollowFactor;
	}

	// initialize dummy vital when first connected
	for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
	{
		if(!Client()->DummyConnected(Conn) || m_aLastDummyConnected[Conn])
			continue;
		{
			CNetMsg_Cl_ShowDistance Msg;
			float x, y;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), ShowDistanceZoom,
				&x, &y);
			Msg.m_X = x;
			Msg.m_Y = y;
			CMsgPacker Packer(&Msg);
			Msg.Pack(&Packer);
			Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		}
		{
			CNetMsg_Cl_CameraInfo Msg;
			Msg.m_Zoom = round_truncate(Zoom * 1000.f);
			Msg.m_Deadzone = Deadzone;
			Msg.m_FollowFactor = FollowFactor;
			CMsgPacker Packer(&Msg);
			Msg.Pack(&Packer);
			Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		}
	}

	// send show distance
	if(ShowDistanceZoom != m_LastShowDistanceZoom ||
		Graphics()->ScreenAspect() != m_LastScreenAspect)
	{
		CNetMsg_Cl_ShowDistance Msg;
		float x, y;
		Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), ShowDistanceZoom,
			&x, &y);
		Msg.m_X = x;
		Msg.m_Y = y;
		Client()->ChecksumData()->m_Zoom = ShowDistanceZoom;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);

		Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
		for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
		{
			if(Client()->DummyConnected(Conn) && m_aLastDummyConnected[Conn])
				Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		}
	}

	// send camera info
	if(Zoom != m_LastZoom || Deadzone != m_LastDeadzone ||
		FollowFactor != m_LastFollowFactor)
	{
		CNetMsg_Cl_CameraInfo Msg;
		Msg.m_Zoom = round_truncate(Zoom * 1000.f);
		Msg.m_Deadzone = Deadzone;
		Msg.m_FollowFactor = FollowFactor;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);

		Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
		for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
		{
			if(Client()->DummyConnected(Conn) && m_aLastDummyConnected[Conn])
				Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		}
	}

	m_LastShowDistanceZoom = ShowDistanceZoom;
	m_LastZoom = Zoom;
	m_LastScreenAspect = Graphics()->ScreenAspect();
	m_LastDeadzone = Deadzone;
	m_LastFollowFactor = FollowFactor;
	for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
		m_aLastDummyConnected[Conn] = Client()->DummyConnected(Conn);

	for(auto &pComponent : m_vpAll)
		pComponent->OnNewSnapshot();

	// notify editor when local character moved
	UpdateEditorIngameMoved();

	// detect air jump for other players
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_Snap.m_aCharacters[i].m_Active &&
			(m_Snap.m_aCharacters[i].m_Cur.m_Jumped & 2) &&
			!(m_Snap.m_aCharacters[i].m_Prev.m_Jumped & 2))
		{
			const int PairConn = Client()->DummyPair();
			bool IsDummy =
				PairConn != g_Config.m_ClDummy && i == m_aLocalIds[PairConn];
			bool IsLocalPlayer = i == m_Snap.m_LocalClientId;

			if(!Predict() || (!IsLocalPlayer && !AntiPingPlayers()) ||
				(!IsLocalPlayer && !IsDummy))
			{
				vec2 Pos = mix(vec2(m_Snap.m_aCharacters[i].m_Prev.m_X,
						       m_Snap.m_aCharacters[i].m_Prev.m_Y),
					vec2(m_Snap.m_aCharacters[i].m_Cur.m_X,
						m_Snap.m_aCharacters[i].m_Cur.m_Y),
					Client()->IntraGameTick(g_Config.m_ClDummy));
				float Alpha = 1.0f;
				if(IsOtherTeam(i))
					Alpha = g_Config.m_ClShowOthersAlpha / 100.0f;
				const float Volume = 1.0f; // TODO snd_game_volume_others
				m_Effects.AirJump(Pos, Alpha, Volume);
			}
		}
	}

	if(g_Config.m_ClFreezeStars && !m_SuppressEvents)
	{
		for(auto &Character : m_Snap.m_aCharacters)
		{
			if(Character.m_Active && Character.m_HasExtendedData &&
				Character.m_pPrevExtendedData)
			{
				int FreezeTimeNow = Character.m_ExtendedData.m_FreezeEnd -
						    Client()->GameTick(g_Config.m_ClDummy);
				int FreezeTimePrev = Character.m_pPrevExtendedData->m_FreezeEnd -
						     Client()->PrevGameTick(g_Config.m_ClDummy);
				vec2 Pos = vec2(Character.m_Cur.m_X, Character.m_Cur.m_Y);
				int StarsNow = (FreezeTimeNow + 1) / Client()->GameTickSpeed();
				int StarsPrev = (FreezeTimePrev + 1) / Client()->GameTickSpeed();
				if(StarsNow < StarsPrev || (StarsPrev == 0 && StarsNow > 0))
				{
					int Amount = StarsNow + 1;
					float Mid = 3 * pi / 2;
					float Min = Mid - pi / 3;
					float Max = Mid + pi / 3;
					for(int j = 0; j < Amount; j++)
					{
						float Angle = mix(Min, Max, (j + 1) / (float)(Amount + 2));
						m_Effects.DamageIndicator(Pos, direction(Angle), 1.0f);
					}
				}
			}
		}
	}

	// Record m_LastRaceTick for g_Config.m_ClConfirmDisconnect/QuitTime
	if(m_GameInfo.m_Race && Client()->State() == IClient::STATE_ONLINE &&
		m_Snap.m_pGameInfoObj && !m_Snap.m_SpecInfo.m_Active &&
		m_Snap.m_pLocalCharacter && m_Snap.m_pLocalPrevCharacter)
	{
		const bool RaceFlag =
			m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME;
		m_LastRaceTick = RaceFlag ? -m_Snap.m_pGameInfoObj->m_WarmupTimer : -1;
	}

	SnapCollectEntities(); // creates a collection that associates EntityEx snap
			       // items with the entities they belong to
	AutoUnfreezeObserveRawLasers();

	UpdateLocalTuning();
	m_IsDummySwapping = 0;
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
		UpdatePrediction();
}

std::function<bool(int, int, int, int)> CGameClient::GetScoreComparator(
	bool TimeScore, bool ReceivedMillisecondFinishTimes, bool Race7)
{
	// 0.7 race score
	if(Race7)
	{
		auto CompareTimeMillis07 = [](int TimeMillis1, int TimeMillis2, int, int) {
			TimeMillis1 = TimeMillis1 == protocol7::FinishTime::NOT_FINISHED ? std::numeric_limits<int>::max() : TimeMillis1;
			TimeMillis2 = TimeMillis2 == protocol7::FinishTime::NOT_FINISHED ? std::numeric_limits<int>::max() : TimeMillis2;
			return TimeMillis1 < TimeMillis2;
		};
		return CompareTimeMillis07;
	}

	// normal scores (like points), biggest score is highest in scoreboard
	if(!TimeScore)
	{
		auto CompareScore = [](int Score1, int Score2, int, int) {
			return Score1 > Score2;
		};
		return CompareScore;
	}

	// 'classical' times, times are send negative, so biggest value has shortest
	// time
	if(!ReceivedMillisecondFinishTimes)
	{
		auto CompareTimeScore = [](int TimeScore1, int TimeScore2, int, int) {
			TimeScore1 = TimeScore1 == FinishTime::NOT_FINISHED_TIMESCORE ? std::numeric_limits<int>::min() : TimeScore1;
			TimeScore2 = TimeScore2 == FinishTime::NOT_FINISHED_TIMESCORE ? std::numeric_limits<int>::min() : TimeScore2;
			return TimeScore1 > TimeScore2;
		};
		return CompareTimeScore;
	}

	// long precise times, smallest value first, subsorting by milliseconds
	auto CompareTimeMillis = [](int TimeSeconds1, int TimeSeconds2,
					 int TimeMillis1, int TimeMillis2) {
		TimeSeconds1 = TimeSeconds1 == FinishTime::NOT_FINISHED_MILLIS ? std::numeric_limits<int>::max() : TimeSeconds1;
		TimeSeconds2 = TimeSeconds2 == FinishTime::NOT_FINISHED_MILLIS ? std::numeric_limits<int>::max() : TimeSeconds2;
		if(TimeSeconds1 == TimeSeconds2)
			return TimeMillis1 < TimeMillis2;
		return TimeSeconds1 < TimeSeconds2;
	};
	return CompareTimeMillis;
}

void CGameClient::UpdateEditorIngameMoved()
{
	const bool LocalCharacterMoved =
		m_Snap.m_pLocalCharacter && m_Snap.m_pLocalPrevCharacter &&
		(m_Snap.m_pLocalCharacter->m_X != m_Snap.m_pLocalPrevCharacter->m_X ||
			m_Snap.m_pLocalCharacter->m_Y != m_Snap.m_pLocalPrevCharacter->m_Y);
	if(!g_Config.m_ClEditor)
	{
		m_EditorMovementDelay = 5;
	}
	else if(m_EditorMovementDelay > 0 && !LocalCharacterMoved)
	{
		--m_EditorMovementDelay;
	}
	if(m_EditorMovementDelay == 0 && LocalCharacterMoved)
	{
		Editor()->OnIngameMoved();
	}
}

// TClient
bool CGameClient::GetDummyFastInput(CNetObj_PlayerInput &DummyFastInput,
	const CNetObj_PlayerInput *pDummyInputData,
	const CCharacter *pDummyChar, int LocalTee,
	int DummyTee) const
{
	if(!PredictDummy() || !pDummyChar || m_IsDummySwapping)
		return false;

	if(g_Config.m_ClDummyHammer)
	{
		DummyFastInput = m_HammerInput;
		return true;
	}

	if(g_Config.m_ClDummyCopyMoves)
	{
		DummyFastInput = m_Controls.m_aFastInput[LocalTee];
		DummyFastInput.m_Fire = m_Controls.m_aFastInput[DummyTee].m_Fire;
		DummyFastInput.m_WantedWeapon =
			m_Controls.m_aFastInput[DummyTee].m_WantedWeapon;
		DummyFastInput.m_NextWeapon =
			m_Controls.m_aFastInput[DummyTee].m_NextWeapon;
		DummyFastInput.m_PrevWeapon =
			m_Controls.m_aFastInput[DummyTee].m_PrevWeapon;
		if(g_Config.m_ClDummyControl)
		{
			const CNetObj_PlayerInput BaseDummyInput =
				pDummyInputData ? *pDummyInputData : CNetObj_PlayerInput{};
			DummyFastInput.m_Jump = BaseDummyInput.m_Jump;
			DummyFastInput.m_Fire = BaseDummyInput.m_Fire;
			DummyFastInput.m_Hook = BaseDummyInput.m_Hook;
		}
		return true;
	}

	if(g_Config.m_ClDummyControl)
	{
		const CNetObj_PlayerInput BaseDummyInput =
			pDummyInputData ? *pDummyInputData : CNetObj_PlayerInput{};
		DummyFastInput = BaseDummyInput;
		DummyFastInput.m_Direction = m_Controls.m_aFastInput[DummyTee].m_Direction;
		DummyFastInput.m_PlayerFlags =
			m_Controls.m_aFastInput[DummyTee].m_PlayerFlags;
		DummyFastInput.m_TargetX = m_Controls.m_aFastInput[DummyTee].m_TargetX;
		DummyFastInput.m_TargetY = m_Controls.m_aFastInput[DummyTee].m_TargetY;
		DummyFastInput.m_WantedWeapon =
			m_Controls.m_aFastInput[DummyTee].m_WantedWeapon;
		DummyFastInput.m_NextWeapon =
			m_Controls.m_aFastInput[DummyTee].m_NextWeapon;
		DummyFastInput.m_PrevWeapon =
			m_Controls.m_aFastInput[DummyTee].m_PrevWeapon;
		return true;
	}

	return false;
}

void CGameClient::ApplyPreInputs(int Tick, bool Direct, CGameWorld &GameWorld)
{
	if(!g_Config.m_ClAntiPingPreInput)
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(CCharacter *pChar = GameWorld.GetCharacterById(ClientId))
		{
			bool IsLocalPlayer = false;
			for(int Conn = 0; Conn < NUM_DUMMIES; Conn++)
				IsLocalPlayer |= ClientId == m_aLocalIds[Conn];
			if(IsLocalPlayer)
				continue;

			const CNetMsg_Sv_PreInput PreInput =
				m_aClients[ClientId].m_aPreInputs[Tick % 200];
			if(PreInput.m_IntendedTick != Tick)
				continue;

			// convert preinput to input
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = PreInput.m_Direction;
			Input.m_TargetX = PreInput.m_TargetX;
			Input.m_TargetY = PreInput.m_TargetY;
			Input.m_Jump = PreInput.m_Jump;
			Input.m_Fire = PreInput.m_Fire;
			Input.m_Hook = PreInput.m_Hook;
			Input.m_WantedWeapon = PreInput.m_WantedWeapon;
			Input.m_NextWeapon = PreInput.m_NextWeapon;
			Input.m_PrevWeapon = PreInput.m_PrevWeapon;

			if(Direct)
			{
				pChar->OnDirectInput(&Input);
			}
			else
			{
				pChar->OnPredictedInput(&Input);
			}
		}
	}
}

void CGameClient::OnPredict()
{
	// store the previous values so we can detect prediction errors
	CCharacterCore BeforePrevChar = m_PredictedPrevChar;
	CCharacterCore BeforeChar = m_PredictedChar;

	// we can't predict without our own id or own character
	if(m_Snap.m_LocalClientId == -1 ||
		!m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_Active)
		return;

	// don't predict anything if we are paused
	if(m_Snap.m_pGameInfoObj &&
		m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED)
	{
		if(m_Snap.m_pLocalCharacter)
		{
			m_PredictedChar.Read(m_Snap.m_pLocalCharacter);
			m_PredictedChar.m_ActiveWeapon = m_Snap.m_pLocalCharacter->m_Weapon;
		}
		if(m_Snap.m_pLocalPrevCharacter)
		{
			m_PredictedPrevChar.Read(m_Snap.m_pLocalPrevCharacter);
			m_PredictedPrevChar.m_ActiveWeapon =
				m_Snap.m_pLocalPrevCharacter->m_Weapon;
		}
		return;
	}

	vec2 aBeforeRender[MAX_CLIENTS];
	for(int i = 0; i < MAX_CLIENTS; i++)
		aBeforeRender[i] = GetSmoothPos(i);

	// init
	int Dummy = m_IsDummySwapping ? m_DummySwapFrom : g_Config.m_ClDummy;

	// PredictedEvents are only handled in predicted world, so update them here
	m_GameWorld.m_PredictedEvents = m_PredictedWorld.m_PredictedEvents;
	m_PredictedWorld.CopyWorld(&m_GameWorld);

	// don't predict inactive players, or entities from other teams
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
			if((!m_Snap.m_aCharacters[i].m_Active && pChar->m_SnapTicks > 10) ||
				IsOtherTeam(i))
				pChar->Destroy();

	CProjectile *pProjNext = nullptr;
	for(CProjectile *pProj = (CProjectile *)m_PredictedWorld.FindFirst(
		    CGameWorld::ENTTYPE_PROJECTILE);
		pProj; pProj = pProjNext)
	{
		pProjNext = (CProjectile *)pProj->TypeNext();
		if(IsOtherTeam(pProj->GetOwner()))
		{
			pProj->Destroy();
		}
	}

	CCharacter *pLocalChar =
		m_PredictedWorld.GetCharacterById(m_Snap.m_LocalClientId);
	if(!pLocalChar)
		return;
	CCharacter *pDummyChar = nullptr;
	if(PredictDummy())
		pDummyChar =
			m_PredictedWorld.GetCharacterById(m_aLocalIds[Client()->DummyPair()]);

	bool RealPredTick = false;
	// predict

	int FastInputTicks = 0;
	if(g_Config.m_TcFastInput)
		FastInputTicks = (g_Config.m_TcFastInputAmount + 19) / 20;

	int FinalTickRegular = Client()->PredGameTick(
		g_Config.m_ClDummy); // The vanilla final tick disregarding fast input

	int FinalTickSelf = FinalTickRegular +
			    FastInputTicks; // the final tick for just our local tee
	int FinalTickOthers = FinalTickSelf; // the final tick for all other tees
	if(g_Config.m_TcFastInput && !g_Config.m_TcFastInputOthers)
		FinalTickOthers = FinalTickSelf - FastInputTicks;

	int LocalTee = m_IsDummySwapping ? m_DummySwapFrom : g_Config.m_ClDummy;
	int DummyTee = Client()->DummyPair();

	for(int Tick = Client()->GameTick(g_Config.m_ClDummy) + 1;
		Tick <= FinalTickSelf; Tick++)
	{
		// fetch the previous characters
		if(Tick == FinalTickSelf)
		{
			m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
			m_PredictedPrevChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_PrevPredicted =
				pLocalChar->GetCore();
		}
		if(Tick == FinalTickOthers)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
					m_aClients[i].m_PrevPredicted = pChar->GetCore();
		}

		if(Tick == Client()->PredGameTick(g_Config.m_ClDummy))
		{
			m_PredictedPrevChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_PrevPredicted =
				pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[m_aLocalIds[Client()->DummyPair()]].m_PrevPredicted =
					pDummyChar->GetCore();
		}

		if(Tick == FinalTickRegular)
			m_PrevRegularPredictedWorld.CopyWorldClean(&m_PredictedWorld);

		// optionally allow some movement in freeze by not predicting freeze the
		// last one to two ticks
		if(g_Config.m_ClPredictFreeze == 2 &&
			Client()->PredGameTick(g_Config.m_ClDummy) - 1 -
					Client()->PredGameTick(g_Config.m_ClDummy) % 2 <=
				Tick)
			pLocalChar->m_CanMoveInFreeze = true;

		// apply inputs and tick
		CNetObj_PlayerInput *pInputData =
			(CNetObj_PlayerInput *)Client()->GetInput(Tick, 0);
		CNetObj_PlayerInput *pDummyInputData =
			!pDummyChar ? nullptr : (CNetObj_PlayerInput *)Client()->GetInput(Tick, 1);
		CNetObj_PlayerInput DummyFastInput{};
		bool DummyFirst = pInputData && pDummyInputData &&
				  pDummyChar->GetCid() < pLocalChar->GetCid();

		if(g_Config.m_TcFastInput && Tick > FinalTickRegular)
		{
			pInputData = &m_Controls.m_aFastInput[LocalTee];
			if(GetDummyFastInput(DummyFastInput, pDummyInputData, pDummyChar,
				   LocalTee, DummyTee))
				pDummyInputData = &DummyFastInput;
		}

		// TClient
		// Disable predicted events during fastinput over-run prediction ticks
		// because they are not real This has to be before direct input because
		// physics happens in there
		bool TempPredEventState = m_PredictedWorld.m_WorldConfig.m_PredictEvents;
		if(Tick > FinalTickRegular)
			m_PredictedWorld.m_WorldConfig.m_PredictEvents = false;

		if(DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);
		if(pInputData)
			pLocalChar->OnDirectInput(pInputData);
		if(pDummyInputData && !DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);

		ApplyPreInputs(Tick, true, m_PredictedWorld);

		m_PredictedWorld.m_GameTick = Tick;
		if(pInputData)
			pLocalChar->OnPredictedInput(pInputData);
		if(pDummyInputData)
			pDummyChar->OnPredictedInput(pDummyInputData);

		ApplyPreInputs(Tick, false, m_PredictedWorld);

		m_PredictedWorld.Tick();

		// TClient
		m_PredictedWorld.m_WorldConfig.m_PredictEvents = TempPredEventState;

		// fetch the current characters
		if(Tick == FinalTickSelf)
		{
			m_PredictedChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_Predicted = pLocalChar->GetCore();
		}
		if(Tick == FinalTickOthers)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
					m_aClients[i].m_Predicted = pChar->GetCore();
		}
		if(Tick == FinalTickRegular)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
					m_aClients[i].m_RegularPredicted = pChar->GetCore();
		}

		if(Tick == Client()->PredGameTick(g_Config.m_ClDummy))
		{
			m_PredictedChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_Predicted = pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[m_aLocalIds[Client()->DummyPair()]].m_Predicted =
					pDummyChar->GetCore();
		}

		for(int i = 0; i < MAX_CLIENTS; i++)
			if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
			{
				m_aClients[i].m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
				m_aClients[i].m_aPredTick[Tick % 200] = Tick;
			}

		// check if we want to trigger effects
		if(Tick > m_aLastNewPredictedTick[Dummy] && (Tick <= FinalTickRegular))
		{
			m_aLastNewPredictedTick[Dummy] = Tick;
			m_NewPredictedTick = true;
			RealPredTick = true;
			vec2 Pos = pLocalChar->Core()->m_Pos;
			int Events = pLocalChar->Core()->m_TriggeredEvents;

			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(Pos, 1.0f, 1.0f);
			if(g_Config.m_SndGame && !m_SuppressEvents)
			{
				if(Events & COREEVENT_GROUND_JUMP)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_PLAYER_JUMP, 1.0f,
						Pos);
				if(Events & COREEVENT_HOOK_ATTACH_GROUND)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_ATTACH_GROUND,
						1.0f, Pos);
				if(Events & COREEVENT_HOOK_HIT_NOHOOK)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_NOATTACH, 1.0f,
						Pos);
				if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
				{
					m_PredictedWorld.CreatePredictedSound(Pos, SOUND_HOOK_ATTACH_PLAYER,
						pLocalChar->GetCid());
				}
			}
		}

		// check if we want to trigger predicted airjump for dummy
		if(AntiPingPlayers() && pDummyChar &&
			Tick > m_aLastNewPredictedTick[Client()->DummyPair()])
		{
			m_aLastNewPredictedTick[Client()->DummyPair()] = Tick;
			vec2 Pos = pDummyChar->Core()->m_Pos;
			int Events = pDummyChar->Core()->m_TriggeredEvents;
			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(Pos, 1.0f, 1.0f);
		}

		if(Tick <= FinalTickRegular)
			HandlePredictedEvents(Tick);

		if(Tick == FinalTickRegular)
			m_RegularPredictedWorld.CopyWorldClean(&m_PredictedWorld);
	}

	if(FastInputTicks > 0)
	{
		m_PredictedWorld.CopyWorld(&m_RegularPredictedWorld);
		// m_PrevPredictedWorld.CopyWorld(&m_PrevRegularPredictedWorld); // not sure
		// if this is worth performance cost, it seems to not matter
	}

	if(g_Config.m_TcRemoveAnti)
	{
		m_ExtraPredictedWorld.CopyWorldClean(&m_PredictedWorld);

		// Remove other tees to reduce lag and because they aren't really important
		// in this case
		for(int i = 0; i < MAX_CLIENTS; i++)
			if(i != m_Snap.m_LocalClientId)
				if(CCharacter *pDelChar = m_ExtraPredictedWorld.GetCharacterById(i))
					pDelChar->Destroy();

		CCharacter *pExtraChar =
			m_ExtraPredictedWorld.GetCharacterById(m_Snap.m_LocalClientId);
		if(pExtraChar)
		{
			bool Unfrozen = false;
			bool Frozen = false;
			for(int i = 0; i < g_Config.m_TcUnfreezeLagDelayTicks; i++)
			{
				if(!pExtraChar)
					continue;

				if(Frozen && pExtraChar->m_AliveAccumulation > 0)
					Unfrozen = true;

				if(pExtraChar->m_AliveAccumulation < 0)
					Frozen = true;

				if(!Unfrozen)
				{
					m_ExtraPredictedWorld.m_GameTick++;
					m_ExtraPredictedWorld.Tick();
				}
				else
				{
					pExtraChar->m_AliveAccumulation =
						std::max(pExtraChar->m_AliveAccumulation, 1);
					pExtraChar->m_AliveAccumulation =
						std::min(pExtraChar->m_AliveAccumulation + 1,
							g_Config.m_TcUnfreezeLagDelayTicks);
				}
			}
		}
	}

	// detect mispredictions of other players and make corrections smoother when
	// possible
	if(g_Config.m_ClAntiPingSmooth && Predict() && AntiPingPlayers() &&
		m_NewTick && m_PredictedTick >= MIN_TICK &&
		absolute(m_PredictedTick - Client()->PredGameTick(g_Config.m_ClDummy)) <=
			1 &&
		absolute(Client()->GameTick(g_Config.m_ClDummy) -
			 Client()->PrevGameTick(g_Config.m_ClDummy)) <= 2)
	{
		int PredTime = std::clamp(Client()->GetPredictionTime(), 0, 800);
		float SmoothPace =
			4 - 1.5f * PredTime / 800.f; // smoothing pace (a lower value will make
						     // the smoothing quicker)
		int64_t Len = 1000 * PredTime * SmoothPace;

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!m_Snap.m_aCharacters[i].m_Active || i == m_Snap.m_LocalClientId ||
				!m_aLastActive[i])
				continue;
			vec2 NewPos = m_aClients[i].m_Predicted.m_Pos;
			vec2 PredErr = (m_aLastPos[i] - NewPos) /
				       (float)minimum(Client()->GetPredictionTime(), 200);
			if(in_range(length(PredErr), 0.05f, 5.f))
			{
				vec2 PredPos = mix(m_aClients[i].m_PrevPredicted.m_Pos,
					m_aClients[i].m_Predicted.m_Pos,
					Client()->PredIntraGameTick(g_Config.m_ClDummy));
				vec2 CurPos = mix(vec2(m_Snap.m_aCharacters[i].m_Prev.m_X,
							  m_Snap.m_aCharacters[i].m_Prev.m_Y),
					vec2(m_Snap.m_aCharacters[i].m_Cur.m_X,
						m_Snap.m_aCharacters[i].m_Cur.m_Y),
					Client()->IntraGameTick(g_Config.m_ClDummy));
				vec2 RenderDiff = PredPos - aBeforeRender[i];
				vec2 PredDiff = PredPos - CurPos;

				float aMixAmount[2];
				for(int j = 0; j < 2; j++)
				{
					aMixAmount[j] = 1.0f;
					if(absolute(PredErr[j]) > 0.05f)
					{
						aMixAmount[j] = 0.0f;
						if(absolute(RenderDiff[j]) > 0.01f)
						{
							aMixAmount[j] =
								1.f - std::clamp(RenderDiff[j] / PredDiff[j], 0.f, 1.f);
							aMixAmount[j] = 1.f - std::pow(1.f - aMixAmount[j], 1 / 1.2f);
						}
					}
					int64_t TimePassed = time_get() - m_aClients[i].m_aSmoothStart[j];
					if(in_range(TimePassed, (int64_t)0, Len - 1))
						aMixAmount[j] =
							minimum(aMixAmount[j], (float)(TimePassed / (double)Len));
				}
				for(int j = 0; j < 2; j++)
					if(absolute(RenderDiff[j]) < 0.01f &&
						absolute(PredDiff[j]) < 0.01f &&
						absolute(m_aClients[i].m_PrevPredicted.m_Pos[j] -
							 m_aClients[i].m_Predicted.m_Pos[j]) < 0.01f &&
						aMixAmount[j] > aMixAmount[j ^ 1])
						aMixAmount[j] = aMixAmount[j ^ 1];
				for(int j = 0; j < 2; j++)
				{
					int64_t Remaining = minimum(
						(1.f - aMixAmount[j]) * Len,
						minimum(time_freq() * 0.700f,
							(1.f - aMixAmount[j ^ 1]) * Len +
								time_freq() *
									0.300f)); // don't smooth for longer than 700ms,
										  // or more than 300ms longer along one
										  // axis than the other axis
					int64_t Start = time_get() - (Len - Remaining);
					if(!in_range(Start + Len, m_aClients[i].m_aSmoothStart[j],
						   m_aClients[i].m_aSmoothStart[j] + Len))
					{
						m_aClients[i].m_aSmoothStart[j] = Start;
						m_aClients[i].m_aSmoothLen[j] = Len;
					}
				}
			}
		}
	}

	// TClient
	// New antiping smoothing
	CCharacter *pSmoothLocalChar =
		m_PredSmoothingWorld.GetCharacterById(m_Snap.m_LocalClientId);
	if(g_Config.m_TcAntiPingImproved && Predict() && AntiPingPlayers() &&
		pSmoothLocalChar && RealPredTick && m_PredictedTick >= MIN_TICK)
	{
		constexpr int PREDICTION_HISTORY_SIZE = 200;
		int PredTime = std::clamp(
			Client()->GetPredictionTime(), 0,
			8000); // Milliseconds for some reason?? TODO: Use more precision
		const int PredEndTick = FinalTickRegular;
		const int SmoothTick = PredEndTick;
		// Select the history tick once per prediction update. The previous version
		// updated this state inside the loop over clients, so every client after
		// the first one could use a different tick in the same frame.
		const int BaseSmoothGameTick = Client()->GameTick(g_Config.m_ClDummy) +
			(int)Client()->IntraGameTick(g_Config.m_ClDummy);
		const int SmoothDummy =
			std::clamp(g_Config.m_ClDummy, 0, NUM_DUMMIES - 1);
		static int s_aPrevSmoothGameTick[NUM_DUMMIES] = {-1, -1, -1};
		if(BaseSmoothGameTick < s_aPrevSmoothGameTick[SmoothDummy])
			s_aPrevSmoothGameTick[SmoothDummy] = -1;
		const int ImprovedSmoothGameTick =
			s_aPrevSmoothGameTick[SmoothDummy] == BaseSmoothGameTick ?
				BaseSmoothGameTick + 1 :
				BaseSmoothGameTick;
		s_aPrevSmoothGameTick[SmoothDummy] = BaseSmoothGameTick;
		const int OldestHistoryTick =
			std::max(0, PredEndTick - (PREDICTION_HISTORY_SIZE - 1));

		// Nightmare: in order to get 100% accurate comparison to detect
		// mispredictions we must tick the PREVIOUS predicted world with our CURRENT
		// predicted inputs
		CCharacter *pSmoothDummyChar = 0;
		CCharacter *pPredDummyChar = 0;
		if(PredictDummy())
		{
			pSmoothDummyChar = m_PredSmoothingWorld.GetCharacterById(
				m_aLocalIds[Client()->DummyPair()]);
			pPredDummyChar =
				m_PredictedWorld.GetCharacterById(m_aLocalIds[Client()->DummyPair()]);
		}
		CNetObj_PlayerInput *pInputData =
			m_PredictedWorld.GetCharacterById(m_Snap.m_LocalClientId)
				->LatestInput();
		CNetObj_PlayerInput *pDummyInputData =
			!pPredDummyChar ? 0 : m_PredictedWorld.GetCharacterById(m_aLocalIds[Client()->DummyPair()])->LatestInput();
		bool DummyFirst = pSmoothLocalChar && pSmoothDummyChar &&
				  pSmoothDummyChar->GetCid() < pSmoothLocalChar->GetCid();

		if(DummyFirst && pSmoothDummyChar && pDummyInputData)
			pSmoothDummyChar->OnDirectInput(pDummyInputData);

		if(pInputData && pSmoothLocalChar)
			pSmoothLocalChar->OnDirectInput(pInputData);

		if(!DummyFirst && pSmoothDummyChar && pDummyInputData)
			pSmoothDummyChar->OnDirectInput(pDummyInputData);

		ApplyPreInputs(SmoothTick, true, m_PredSmoothingWorld);
		m_PredSmoothingWorld.m_GameTick = SmoothTick;

		if(pInputData && pSmoothLocalChar)
			pSmoothLocalChar->OnPredictedInput(pInputData);

		if(pDummyInputData && pSmoothDummyChar)
			pSmoothDummyChar->OnPredictedInput(pDummyInputData);
		ApplyPreInputs(SmoothTick, false, m_PredSmoothingWorld);
		m_PredSmoothingWorld.Tick();

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!m_Snap.m_aCharacters[i].m_Active || !m_aLastActive[i])
			{
				m_aClients[i].m_ValidAntipingSmooth = false;
				continue;
			}

			if(i == m_aLocalIds[Client()->DummyPair()] ||
				i == m_Snap.m_LocalClientId)
			{
				m_aClients[i].m_PrevImprovedPredPos =
					m_aClients[i].m_PrevPredicted.m_Pos;
				m_aClients[i].m_ImprovedPredPos = m_aClients[i].m_Predicted.m_Pos;
				m_aClients[i].m_ValidAntipingSmooth = false;
				continue;
			}

			CCharacter *pChar = m_PredSmoothingWorld.GetCharacterById(i);
			if(!pChar)
			{
				m_aClients[i].m_ValidAntipingSmooth = false;
				continue;
			}

			vec2 PredPos = m_aClients[i].m_RegularPredicted.m_Pos;

			vec2 PrevPredPos = pChar->GetCore().m_Pos;

			// Do not access (GameTick - 1) in the ring buffer before the
			// first server tick. A negative modulo would index before the array.
			if(ImprovedSmoothGameTick < 1)
			{
				m_aClients[i].m_PrevImprovedPredPos = PrevPredPos;
				m_aClients[i].m_ImprovedPredPos = PredPos;
				m_aClients[i].m_ValidAntipingSmooth = false;
				continue;
			}
			const int GameTick = ImprovedSmoothGameTick;
			const int GameTickIndex = GameTick % PREDICTION_HISTORY_SIZE;
			const int PrevGameTickIndex =
				(GameTick - 1) % PREDICTION_HISTORY_SIZE;
			vec2 ServerPos = m_aClients[i].m_aPredPos[GameTickIndex];
			vec2 PrevServerPos = m_aClients[i].m_aPredPos[PrevGameTickIndex];

			vec2 PredDir = normalize(PredPos - ServerPos);
			vec2 LastDir = normalize(PrevPredPos - ServerPos);

			vec2 MaxPos = vec2(0, 0);
			vec2 MinPos = vec2(0, 0);
			bool FoundBoundingBox = false;
			// Get a bounding box for our final prediction position to minimize going
			// through walls
			for(int Tick = std::max(GameTick - 1, OldestHistoryTick);
				Tick <= PredEndTick; Tick++)
			{
				const int TickIndex = Tick % PREDICTION_HISTORY_SIZE;
				if(m_aClients[i].m_aPredTick[TickIndex] != Tick)
					continue;
				vec2 Pos = m_aClients[i].m_aPredPos[TickIndex];
				if(!FoundBoundingBox)
				{
					MaxPos = Pos;
					MinPos = Pos;
					FoundBoundingBox = true;
				}
				else
				{
					MaxPos.x = std::max(Pos.x, MaxPos.x);
					MaxPos.y = std::max(Pos.y, MaxPos.y);
					MinPos.x = std::min(Pos.x, MinPos.x);
					MinPos.y = std::min(Pos.y, MinPos.y);
				}
			}
			int PredStartTick = GameTick;
			int HistoryStartTick = PredStartTick - (PredEndTick - PredStartTick);
			HistoryStartTick =
				std::max({1, HistoryStartTick, OldestHistoryTick});
			vec2 HistoryVector = vec2(0, 0);
			float HistoryDistance = 0.0f;
			int HistoryCount = 0;
			// Find the average history vector
			for(int Tick = HistoryStartTick; Tick <= PredStartTick; Tick++)
			{
				const int TickIndex = Tick % PREDICTION_HISTORY_SIZE;
				const int PrevTickIndex =
					(Tick - 1) % PREDICTION_HISTORY_SIZE;
				if(m_aClients[i].m_aPredTick[TickIndex] != Tick ||
					m_aClients[i].m_aPredTick[PrevTickIndex] != Tick - 1)
					continue;
				vec2 DirVector = m_aClients[i].m_aPredPos[TickIndex] -
						 m_aClients[i].m_aPredPos[PrevTickIndex];
				HistoryVector += DirVector;
				HistoryDistance += length(DirVector);
				HistoryCount++;
			}

			bool ValidRecentPositions =
				m_aClients[i].m_aPredTick[GameTickIndex] == GameTick &&
				m_aClients[i].m_aPredTick[PrevGameTickIndex] == GameTick - 1;
			// Not enough history data
			if(!ValidRecentPositions || !FoundBoundingBox || HistoryCount == 0 ||
				HistoryDistance <= 0.0001f)
			{
				m_aClients[i].m_PrevImprovedPredPos = PrevPredPos;
				m_aClients[i].m_ImprovedPredPos = PredPos;
				m_aClients[i].m_ValidAntipingSmooth = false;
				continue;
			}

			HistoryVector = HistoryVector / HistoryCount;
			HistoryVector = normalize(HistoryVector);
			float Variance = 0.0f;
			// Find the variance over the history window
			if(length(HistoryVector) > 0.0f)
			{
				for(int Tick = HistoryStartTick; Tick <= PredStartTick; Tick++)
				{
					const int TickIndex = Tick % PREDICTION_HISTORY_SIZE;
					const int PrevTickIndex =
						(Tick - 1) % PREDICTION_HISTORY_SIZE;
					if(m_aClients[i].m_aPredTick[TickIndex] != Tick ||
						m_aClients[i].m_aPredTick[PrevTickIndex] != Tick - 1)
						continue;
					vec2 DirVector = m_aClients[i].m_aPredPos[TickIndex] -
							 m_aClients[i].m_aPredPos[PrevTickIndex];
					vec2 Diff = normalize(DirVector) - HistoryVector;
					Variance += dot(Diff, Diff);
				}
				Variance /= HistoryCount;
			}
			else
			{
				Variance = 0.0f;
			}
			float Sigma = 1.5f; // Can be adjusted
			float SigmaScale = length(PredPos - ServerPos) / HistoryDistance;
			if(SigmaScale > 0)
				Sigma /= SigmaScale;
			float TrustFactor = std::max<float>(0.0f, 1.0f - (std::sqrt(Variance) / Sigma));
			vec2 TrustedVector = HistoryVector;

			// Detect mispredictions
			float Confidence = 1.0f;
			if(PredDir == vec2(0, 0))
				Confidence = 1.0f;
			else
			{
				Confidence = std::max(0.0f, dot(LastDir, PredDir));
				Confidence = std::pow(Confidence, 4.0f); // Can be adjusted
			}
			float Uncertainty = 1.0f - Confidence;
			float TickDuration = (float)1000 / (float)Client()->GameTickSpeed();

			// Manage uncertainty value
			float PredTimeScale =
				(float)g_Config.m_TcAntiPingUncertaintyScale / 100.0f;
			const float TickSize = PredTime > 0 && PredTimeScale > 0.0f ?
				TickDuration / ((float)PredTime * PredTimeScale) :
				1.0f;
			float PrevConfidence = 1.0f - m_aClients[i].m_Uncertainty;
			float NewConfidence = PrevConfidence - Uncertainty + TickSize;
			float MinConfidence = g_Config.m_TcAntiPingNegativeBuffer ? -1.0f : 0.0f;
			NewConfidence =
				std::clamp(NewConfidence, MinConfidence,
					1.0f); // A certain about of "negative buffer" is allowed
			m_aClients[i].m_Uncertainty = 1.0f - NewConfidence;
			NewConfidence = std::max(0.0f, NewConfidence);

			// Decompose prediction vector into 2 components based on the trusted
			// vector
			vec2 PredVector = PredPos - ServerPos;
			vec2 Forward = normalize(TrustedVector);
			float DotPf = std::max(0.0f, dot(normalize(PredVector), Forward));
			vec2 ConfidenceParallel = Forward * DotPf * length(PredVector);
			if(DotPf == 0.0f)
				ConfidenceParallel = vec2(0, 0);
			vec2 ConfidencePerp = PredVector - ConfidenceParallel;

			if(!g_Config.m_TcAntiPingStableDirection)
				TrustFactor = 0.0f;

			vec2 ConfidenceVector =
				ConfidenceParallel * std::max(TrustFactor, NewConfidence) +
				ConfidencePerp * NewConfidence;

			// Minor safe guard against insane predictions
			if(length(ConfidenceVector) > HistoryDistance)
				ConfidenceVector = mix(normalize(ConfidenceVector) * HistoryDistance,
					ConfidenceVector, NewConfidence);

			vec2 ConfidencePos = ServerPos + ConfidenceVector;

			// Clamp final position to bounding box
			ConfidencePos.x = std::clamp(ConfidencePos.x, MinPos.x, MaxPos.x);
			ConfidencePos.y = std::clamp(ConfidencePos.y, MinPos.y, MaxPos.y);

			m_aClients[i].m_PrevImprovedPredPos = m_aClients[i].m_ImprovedPredPos;
			m_aClients[i].m_ImprovedPredPos = ConfidencePos;
			if(distance(ServerPos, PrevServerPos) > 600.0f ||
				distance(m_aClients[i].m_PrevImprovedPredPos,
					m_aClients[i].m_ImprovedPredPos) > 600.0f)
			{
				m_aClients[i].m_PrevImprovedPredPos = m_aClients[i].m_ImprovedPredPos;
			}
			m_aClients[i].m_ValidAntipingSmooth = true;
		}
	}
	// Copy the current pred world so on the next tick we have the "previous" pred
	// world to advance and test against
	if(m_NewPredictedTick && g_Config.m_TcAntiPingImproved)
		m_PredSmoothingWorld.CopyWorldClean(&m_RegularPredictedWorld);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_Snap.m_aCharacters[i].m_Active)
		{
			if(m_NewPredictedTick)
			{
				m_aLastPos[i] = m_aClients[i].m_Predicted.m_Pos;
				m_aLastActive[i] = true;
			}
		}
		else
			m_aLastActive[i] = false;
	}

	if(g_Config.m_Debug && g_Config.m_ClPredict && FastInputTicks == 0 &&
		m_PredictedTick == Client()->PredGameTick(g_Config.m_ClDummy))
	{
		CNetObj_CharacterCore Before = {0}, Now = {0}, BeforePrev = {0},
				      NowPrev = {0};
		BeforeChar.Write(&Before);
		BeforePrevChar.Write(&BeforePrev);
		m_PredictedChar.Write(&Now);
		m_PredictedPrevChar.Write(&NowPrev);

		if(mem_comp(&Before, &Now, sizeof(CNetObj_CharacterCore)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client",
				"prediction error");
			for(unsigned i = 0; i < sizeof(CNetObj_CharacterCore) / sizeof(int); i++)
				if(((int *)&Before)[i] != ((int *)&Now)[i])
				{
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "	%d %d %d (%d %d)", i,
						((int *)&Before)[i], ((int *)&Now)[i],
						((int *)&BeforePrev)[i], ((int *)&NowPrev)[i]);
					Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client", aBuf);
				}
		}
	}

	m_PredictedTick = FinalTickRegular;

	if(m_NewPredictedTick)
		m_Ghost.OnNewPredictedSnapshot();
}

void CGameClient::OnActivateEditor() { OnRelease(); }

CGameClient::CClientStats::CClientStats() { Reset(); }

void CGameClient::CClientStats::Reset()
{
	m_JoinTick = 0;
	m_IngameTicks = 0;
	m_Active = false;

	std::fill(std::begin(m_aFragsWith), std::end(m_aFragsWith), 0);
	std::fill(std::begin(m_aDeathsFrom), std::end(m_aDeathsFrom), 0);
	m_Frags = 0;
	m_Deaths = 0;
	m_Suicides = 0;
	m_BestSpree = 0;
	m_CurrentSpree = 0;

	m_FlagGrabs = 0;
	m_FlagCaptures = 0;
}

void CGameClient::CClientData::UpdateSkinInfo()
{
	const CSkinDescriptor SkinDescriptor = ToSkinDescriptor();
	if(SkinDescriptor.m_Flags == 0)
	{
		return;
	}

	const auto &&ApplySkinProperties = [&]() {
		if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SIX)
		{
			m_pSkinInfo->TeeRenderInfo().ApplyColors(m_UseCustomColor, m_ColorBody,
				m_ColorFeet);
		}
		if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SEVEN)
		{
			for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
			{
				const CClientData::CSixup &SixupData = m_aSixup[Dummy];
				CTeeRenderInfo::CSixup &SixupSkinInfo =
					m_pSkinInfo->TeeRenderInfo().m_aSixup[Dummy];
				for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
				{
					m_pGameClient->m_Skins7.ApplyColorTo(
						SixupSkinInfo, SixupData.m_aUseCustomColors[Part],
						SixupData.m_aSkinPartColors[Part], Part);
				}
				UpdateSkin7HatSprite(Dummy);
				UpdateSkin7BotDecoration(Dummy);
			}
		}
		m_pSkinInfo->TeeRenderInfo().m_Size = 64.0f;
	};

	if(m_pSkinInfo == nullptr)
	{
		CTeeRenderInfo TeeRenderInfo;
		m_pSkinInfo = m_pGameClient->CreateManagedTeeRenderInfo(TeeRenderInfo,
			SkinDescriptor);
		m_pSkinInfo->SetRefreshCallback([&]() { UpdateRenderInfo(); });
		ApplySkinProperties();
		m_pSkinInfo->m_RefreshCallback();
	}
	else if(m_pSkinInfo->SkinDescriptor() != SkinDescriptor)
	{
		m_pSkinInfo->m_SkinDescriptor = SkinDescriptor;
		m_pGameClient->RefreshSkin(m_pSkinInfo);
		ApplySkinProperties();
	}
	else
	{
		ApplySkinProperties();
		m_pSkinInfo->m_RefreshCallback();
	}
}

void CGameClient::CClientData::UpdateRenderInfo()
{
	m_RenderInfo = m_pSkinInfo->TeeRenderInfo();

	// force team colors
	if(m_pGameClient->IsTeamPlay())
	{
		m_RenderInfo.m_CustomColoredSkin = true;
		for(auto &Sixup : m_RenderInfo.m_aSixup)
		{
			std::fill(std::begin(Sixup.m_aUseCustomColors),
				std::end(Sixup.m_aUseCustomColors), true);
		}

		if(m_Team >= TEAM_RED && m_Team <= TEAM_BLUE)
		{
			const int aTeamColors[2] = {65461, 10223541};
			m_RenderInfo.m_ColorBody =
				color_cast<ColorRGBA>(ColorHSLA(aTeamColors[m_Team]));
			m_RenderInfo.m_ColorFeet =
				color_cast<ColorRGBA>(ColorHSLA(aTeamColors[m_Team]));

			// 0.7
			for(auto &Sixup : m_RenderInfo.m_aSixup)
			{
				const ColorRGBA aTeamColorsSixup[2] = {
					ColorRGBA(0.753f, 0.318f, 0.318f, 1.0f),
					ColorRGBA(0.318f, 0.471f, 0.753f, 1.0f)};
				const ColorRGBA aMarkingColorsSixup[2] = {
					ColorRGBA(0.824f, 0.345f, 0.345f, 1.0f),
					ColorRGBA(0.345f, 0.514f, 0.824f, 1.0f)};
				float MarkingAlpha = Sixup.m_aColors[protocol7::SKINPART_MARKING].a;
				for(auto &Color : Sixup.m_aColors)
				{
					Color = aTeamColorsSixup[m_Team];
				}
				if(MarkingAlpha > 0.1f)
				{
					Sixup.m_aColors[protocol7::SKINPART_MARKING] =
						aMarkingColorsSixup[m_Team];
				}
			}
		}
		else
		{
			m_RenderInfo.m_ColorBody = color_cast<ColorRGBA>(ColorHSLA(12829350));
			m_RenderInfo.m_ColorFeet = color_cast<ColorRGBA>(ColorHSLA(12829350));
			for(auto &Sixup : m_RenderInfo.m_aSixup)
			{
				for(auto &Color : Sixup.m_aColors)
				{
					Color = color_cast<ColorRGBA>(ColorHSLA(12829350));
				}
			}
		}
	}
}

void CGameClient::CClientData::Reset()
{
	m_UseCustomColor = 0;
	m_ColorBody = 0;
	m_ColorFeet = 0;

	m_aName[0] = '\0';
	m_aClan[0] = '\0';
	m_Country = -1;
	str_copy(m_aSkinName, "default");

	m_Team = 0;
	m_Emoticon = 0;
	m_EmoticonStartFraction = 0;
	m_EmoticonStartTick = -1;

	m_Solo = false;
	m_Jetpack = false;
	m_CollisionDisabled = false;
	m_EndlessHook = false;
	m_EndlessJump = false;
	m_HammerHitDisabled = false;
	m_GrenadeHitDisabled = false;
	m_LaserHitDisabled = false;
	m_ShotgunHitDisabled = false;
	m_HookHitDisabled = false;
	m_Super = false;
	m_Invincible = false;
	m_HasTelegunGun = false;
	m_HasTelegunGrenade = false;
	m_HasTelegunLaser = false;
	m_FreezeEnd = 0;
	m_DeepFrozen = false;
	m_LiveFrozen = false;

	m_Predicted.Reset();
	m_PrevPredicted.Reset();

	// TClient
	m_RegularPredicted.Reset();

	if(m_pSkinInfo != nullptr)
	{
		// Make sure other `shared_ptr`s to this skin info will not use the refresh
		// callback that refers to this reset client data
		m_pSkinInfo->SetRefreshCallback(nullptr);
		m_pSkinInfo = nullptr;
	}
	m_RenderInfo.Reset();

	m_Angle = 0.0f;
	m_Active = false;
	m_ChatIgnore = false;
	m_EmoticonIgnore = false;
	m_Friend = false;
	m_Foe = false;

	m_AuthLevel = AUTHED_NO;
	m_Afk = false;
	m_Paused = false;
	m_Spec = false;

	std::fill(std::begin(m_aSwitchStates), std::end(m_aSwitchStates), 0);

	m_Snapped.m_Tick = -1;
	m_Evolved.m_Tick = -1;

	for(auto &PreInput : m_aPreInputs)
	{
		PreInput.m_IntendedTick = -1;
	}

	m_RenderCur.m_Tick = -1;
	m_RenderPrev.m_Tick = -1;
	m_RenderPos = vec2(0.0f, 0.0f);
	m_IsPredicted = false;
	m_IsPredictedLocal = false;
	std::fill(std::begin(m_aSmoothStart), std::end(m_aSmoothStart), 0);
	std::fill(std::begin(m_aSmoothLen), std::end(m_aSmoothLen), 0);
	std::fill(std::begin(m_aPredPos), std::end(m_aPredPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPredTick), std::end(m_aPredTick), 0);
	m_SpecCharPresent = false;
	m_SpecChar = vec2(0.0f, 0.0f);

	for(auto &Info : m_aSixup)
		Info.Reset();
}

CSkinDescriptor CGameClient::CClientData::ToSkinDescriptor() const
{
	CSkinDescriptor SkinDescriptor;

	CTranslationContext::CClientData &TranslatedClient =
		m_pGameClient->m_pClient->m_TranslationContext.m_aClients[ClientId()];
	if(m_Active && !TranslatedClient.m_Active)
	{
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SIX;
		str_copy(SkinDescriptor.m_aSkinName, m_aSkinName);
	}
	else if(TranslatedClient.m_Active)
	{
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SEVEN;
		for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
		{
			for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
			{
				str_copy(SkinDescriptor.m_aSixup[Dummy].m_aaSkinPartNames[Part],
					m_aSixup[Dummy].m_aaSkinPartNames[Part]);
			}
			SkinDescriptor.m_aSixup[Dummy].m_XmasHat =
				time_season() == ETimeSeason::XMAS;
			SkinDescriptor.m_aSixup[Dummy].m_BotDecoration =
				(TranslatedClient.m_PlayerFlags7 & protocol7::PLAYERFLAG_BOT) != 0;
		}
	}

	return SkinDescriptor;
}

void CGameClient::CClientData::CSixup::Reset()
{
	for(int i = 0; i < protocol7::NUM_SKINPARTS; ++i)
	{
		m_aaSkinPartNames[i][0] = '\0';
		m_aUseCustomColors[i] = 0;
		m_aSkinPartColors[i] = 0;
	}
}

void CGameClient::SendSwitchTeam(int Team) const
{
	CNetMsg_Cl_SetTeam Msg;
	Msg.m_Team = Team;
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
}

void CGameClient::SendStartInfo7(int Conn)
{
	const bool Dummy = Conn != IClient::CONN_MAIN;
	const int SkinConfig = Dummy ? 1 : 0;
	protocol7::CNetMsg_Cl_StartInfo Msg;
	Msg.m_pName = Dummy ? Client()->DummyName(Conn) : Client()->PlayerName();
	Msg.m_pClan = Dummy ? Config()->m_ClDummyClan : Config()->m_PlayerClan;
	Msg.m_Country =
		Dummy ? Config()->m_ClDummyCountry : Config()->m_PlayerCountry;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[SkinConfig][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[SkinConfig][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[SkinConfig][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	m_aCheckInfo[Conn] = -1;
}

void CGameClient::SendSkinChange7(int Conn)
{
	const int SkinConfig = Conn == IClient::CONN_MAIN ? 0 : 1;
	protocol7::CNetMsg_Cl_SkinChange Msg;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[SkinConfig][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[SkinConfig][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[SkinConfig][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	m_aCheckInfo[Conn] = Client()->GameTickSpeed();
}

bool CGameClient::GotWantedSkin7(int Conn)
{
	const int SkinConfig = Conn == IClient::CONN_MAIN ? 0 : 1;
	// validate the wanted skinparts before comparison
	// because the skin parts we compare against are also validated
	// otherwise it tries to resend the skin info when the eyes are set to
	// "negative" in team based modes
	char aSkinParts[protocol7::NUM_SKINPARTS][protocol7::MAX_SKIN_ARRAY_SIZE];
	char *apSkinPartsPtr[protocol7::NUM_SKINPARTS];
	int aUCCVars[protocol7::NUM_SKINPARTS];
	int aColorVars[protocol7::NUM_SKINPARTS];
	for(int SkinPart = 0; SkinPart < protocol7::NUM_SKINPARTS; SkinPart++)
	{
		str_copy(aSkinParts[SkinPart],
			CSkins7::ms_apSkinVariables[SkinConfig][SkinPart],
			protocol7::MAX_SKIN_ARRAY_SIZE);
		apSkinPartsPtr[SkinPart] = aSkinParts[SkinPart];
		aUCCVars[SkinPart] = *CSkins7::ms_apUCCVariables[SkinConfig][SkinPart];
		aColorVars[SkinPart] = *CSkins7::ms_apColorVariables[SkinConfig][SkinPart];
	}
	m_Skins7.ValidateSkinParts(apSkinPartsPtr, aUCCVars, aColorVars,
		m_pClient->m_TranslationContext.m_GameFlags);

	for(int SkinPart = 0; SkinPart < protocol7::NUM_SKINPARTS; SkinPart++)
	{
		if(str_comp(m_aClients[m_aLocalIds[Conn]]
				    .m_aSixup[Conn]
				    .m_aaSkinPartNames[SkinPart],
			   apSkinPartsPtr[SkinPart]))
			return false;
		if(m_aClients[m_aLocalIds[Conn]]
				.m_aSixup[Conn]
				.m_aUseCustomColors[SkinPart] != aUCCVars[SkinPart])
			return false;
		if(m_aClients[m_aLocalIds[Conn]]
				.m_aSixup[Conn]
				.m_aSkinPartColors[SkinPart] != aColorVars[SkinPart])
			return false;
	}

	// TODO: add name change ddnet extension to 0.7 protocol
	// if(str_comp(m_aClients[m_aLocalIds[(int)Dummy]].m_aName, Dummy ?
	// Client()->DummyName() : Client()->PlayerName())) 	return false;
	// if(str_comp(m_aClients[m_aLocalIds[(int)Dummy]].m_aClan, Dummy ?
	// g_Config.m_ClDummyClan : g_Config.m_PlayerClan)) 	return false;
	// if(m_aClients[m_aLocalIds[(int)Dummy]].m_Country != (Dummy ?
	// g_Config.m_ClDummyCountry : g_Config.m_PlayerCountry)) 	return false;

	return true;
}

void CGameClient::SendInfo(bool Start)
{
	if(m_pClient->IsSixup())
	{
		if(Start)
			SendStartInfo7(IClient::CONN_MAIN);
		else
			SendSkinChange7(IClient::CONN_MAIN);
		return;
	}
	if(Start)
	{
		CNetMsg_Cl_StartInfo Msg;
		Msg.m_pName = Client()->PlayerName();
		Msg.m_pClan = g_Config.m_PlayerClan;
		Msg.m_Country = g_Config.m_PlayerCountry;
		Msg.m_pSkin = g_Config.m_ClPlayerSkin;
		Msg.m_UseCustomColor = g_Config.m_ClPlayerUseCustomColor;
		Msg.m_ColorBody = g_Config.m_ClPlayerColorBody;
		Msg.m_ColorFeet = g_Config.m_ClPlayerColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		Client()->SendMsg(IClient::CONN_MAIN, &Packer,
			MSGFLAG_VITAL | MSGFLAG_FLUSH);
		m_aCheckInfo[0] = -1;
	}
	else
	{
		CNetMsg_Cl_ChangeInfo Msg;
		Msg.m_pName = Client()->PlayerName();
		Msg.m_pClan = g_Config.m_PlayerClan;
		Msg.m_Country = g_Config.m_PlayerCountry;
		Msg.m_pSkin = g_Config.m_ClPlayerSkin;
		Msg.m_UseCustomColor = g_Config.m_ClPlayerUseCustomColor;
		Msg.m_ColorBody = g_Config.m_ClPlayerColorBody;
		Msg.m_ColorFeet = g_Config.m_ClPlayerColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
		m_aCheckInfo[0] = Client()->GameTickSpeed();
	}
}

void CGameClient::SendDummyInfo(bool Start, int Conn)
{
	if(Conn <= IClient::CONN_MAIN || Conn >= NUM_DUMMIES)
		return;
	if(m_pClient->IsSixup())
	{
		if(Start)
			SendStartInfo7(Conn);
		else
			SendSkinChange7(Conn);
		return;
	}
	if(Start)
	{
		CNetMsg_Cl_StartInfo Msg;
		Msg.m_pName = Client()->DummyName(Conn);
		Msg.m_pClan = g_Config.m_ClDummyClan;
		Msg.m_Country = g_Config.m_ClDummyCountry;
		Msg.m_pSkin = g_Config.m_ClDummySkin;
		Msg.m_UseCustomColor = g_Config.m_ClDummyUseCustomColor;
		Msg.m_ColorBody = g_Config.m_ClDummyColorBody;
		Msg.m_ColorFeet = g_Config.m_ClDummyColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		m_aCheckInfo[Conn] = -1;
	}
	else
	{
		CNetMsg_Cl_ChangeInfo Msg;
		Msg.m_pName = Client()->DummyName(Conn);
		Msg.m_pClan = g_Config.m_ClDummyClan;
		Msg.m_Country = g_Config.m_ClDummyCountry;
		Msg.m_pSkin = g_Config.m_ClDummySkin;
		Msg.m_UseCustomColor = g_Config.m_ClDummyUseCustomColor;
		Msg.m_ColorBody = g_Config.m_ClDummyColorBody;
		Msg.m_ColorFeet = g_Config.m_ClDummyColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		m_aCheckInfo[Conn] = Client()->GameTickSpeed();
	}
}

void CGameClient::SendKill() const
{
	CNetMsg_Cl_Kill Msg;
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgP(NETMSGTYPE_CL_KILL, false);
		Client()->SendMsg(Client()->DummyPair(), &MsgP, MSGFLAG_VITAL);
	}
}

void CGameClient::SendReadyChange7()
{
	if(!Client()->IsSixup())
	{
		Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD, "client",
			"Error you have to be connected to a 0.7 server to use ready_change");
		return;
	}
	protocol7::CNetMsg_Cl_ReadyChange Msg;
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL, true);
}

void CGameClient::ConTeam(IConsole::IResult *pResult, void *pUserData)
{
	((CGameClient *)pUserData)->SendSwitchTeam(pResult->GetInteger(0));
}

void CGameClient::ConKill(IConsole::IResult *pResult, void *pUserData)
{
	((CGameClient *)pUserData)->SendKill();
}

void CGameClient::ConReadyChange7(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	if(pClient->Client()->State() == IClient::STATE_ONLINE)
		pClient->SendReadyChange7();
}

void CGameClient::ConZzFentBotPlan(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	if(!pClient->m_Snap.m_pLocalCharacter)
		return;
	if(!pClient->m_FentBotHasWaypoint)
		return;
	const int LocalId = pClient->m_Snap.m_LocalClientId;
	pClient->FentBotInvalidatePlan();
	pClient->FentBotUpdatePlan(LocalId, g_Config.m_ClDummy);
}

void CGameClient::ConZzFentBotExportDataset(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	if(!pClient->m_Snap.m_pLocalCharacter)
		return;
	if(!pClient->m_FentBotHasWaypoint)
		return;

	const int Episodes = pResult->NumArguments() > 0 ? std::clamp(pResult->GetInteger(0), 1, 10000) : 64;
	const int Steps = pResult->NumArguments() > 1 ? std::clamp(pResult->GetInteger(1), 1, 20000) : 800;
	const char *pFilenameArg =
		pResult->NumArguments() > 2 ? pResult->GetString(2) : "";

	if(!pClient->Storage()->CreateFolder("fentbot", IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/general",
			IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/saveplr",
			IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/waypoints",
			IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/blocktricks",
			IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/savedin10sec",
			IStorage::TYPE_SAVE))
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to create fentbot folder");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	if(pFilenameArg && pFilenameArg[0] != '\0')
	{
		if(str_endswith_nocase(pFilenameArg, ".jsonl"))
			str_format(aFilename, sizeof(aFilename), "fentbot/%s", pFilenameArg);
		else
			str_format(aFilename, sizeof(aFilename), "fentbot/%s.jsonl",
				pFilenameArg);
	}
	else
	{
		SHA256_DIGEST Sha256 = pClient->Map()->Sha256();
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(Sha256, aSha256, sizeof(aSha256));
		char aTimestamp[32];
		str_timestamp_format(aTimestamp, sizeof(aTimestamp),
			TimestampFormat::NOSPACE);
		str_format(aFilename, sizeof(aFilename), "fentbot/%s_%s_%s.jsonl",
			pClient->Map()->BaseName(), aTimestamp, aSha256);
	}

	IOHANDLE Handle = pClient->Storage()->OpenFile(aFilename, IOFLAG_WRITE,
		IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open dataset file");
		return;
	}

	const int LocalId = pClient->m_Snap.m_LocalClientId;
	const int DummyIndex = g_Config.m_ClDummy;
	const int NowTick = pClient->Client()->GameTick(DummyIndex);
	const int Horizon = std::clamp(g_Config.m_ClZzFentBotHorizonTicks, 8, 80);
	const float HookLen = (float)pClient->m_aTuning[DummyIndex].m_HookLength;

	auto IsFreezeTile = [&](int Tile) {
		return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
	};
	auto IsFreezeAt = [&](const vec2 &Pos) {
		const int PureIndex = pClient->Collision()->GetPureMapIndex(Pos);
		if(PureIndex < 0)
			return false;
		const int Tile = pClient->Collision()->GetTileIndex(PureIndex);
		const int FTile = pClient->Collision()->GetFrontTileIndex(PureIndex);
		return IsFreezeTile(Tile) || IsFreezeTile(FTile);
	};

	struct SCandidate
	{
		int m_Direction;
		bool m_Jump;
		bool m_Hook;
		vec2 m_Aim;
	};
	static const vec2 s_aAimCands[] = {
		vec2(0.0f, -1.0f),
		normalize(vec2(-0.4f, -1.0f)),
		normalize(vec2(0.4f, -1.0f)),
		vec2(-1.0f, 0.0f),
		vec2(1.0f, 0.0f),
	};
	auto EncodeAction = [&](const SCandidate &Cand) -> int {
		int AimIndex = 0;
		for(int i = 0; i < (int)std::size(s_aAimCands); i++)
		{
			if(distance(Cand.m_Aim, s_aAimCands[i]) < 0.0001f)
			{
				AimIndex = i;
				break;
			}
		}
		const int DirIndex = Cand.m_Direction + 1;
		return (((DirIndex * 2 + (Cand.m_Jump ? 1 : 0)) * 2 +
				(Cand.m_Hook ? 1 : 0)) *
			       (int)std::size(s_aAimCands)) +
		       AimIndex;
	};

	int Written = 0;
	for(int Ep = 0; Ep < Episodes; Ep++)
	{
		CGameWorld SimWorld;
		SimWorld.CopyWorldClean(&pClient->m_RegularPredictedWorld);
		SimWorld.m_GameTick = NowTick;
		CCharacter *pChar = SimWorld.GetCharacterById(LocalId);
		if(!pChar)
			continue;

		pClient->FentBotInvalidatePlan();
		pClient->FentBotUpdatePlan(LocalId, DummyIndex);
		const vec2 Goal = pClient->m_FentBotHasSubgoal ? pClient->m_FentBotSubgoal : pClient->m_FentBotWaypoint;

		for(int Step = 0; Step < Steps; Step++)
		{
			const vec2 Pos0 = pChar->Core()->m_Pos;
			const vec2 Vel0 = pChar->Core()->m_Vel;
			if(IsFreezeAt(Pos0))
				break;
			if(distance(Pos0, Goal) < 32.0f)
				break;

			SCandidate Best{};
			float BestScore = -1e18f;
			bool Found = false;

			for(int DirCand = -1; DirCand <= 1; DirCand++)
			{
				for(int JumpCand = 0; JumpCand <= 1; JumpCand++)
				{
					for(int HookCand = 0; HookCand <= 1; HookCand++)
					{
						for(const vec2 &AimCand : s_aAimCands)
						{
							SCandidate Cand;
							Cand.m_Direction = DirCand;
							Cand.m_Jump = JumpCand != 0;
							Cand.m_Hook = HookCand != 0;
							Cand.m_Aim = AimCand;

							CGameWorld Roll;
							Roll.CopyWorldClean(&SimWorld);
							Roll.m_GameTick = SimWorld.m_GameTick;
							CCharacter *pRollChar = Roll.GetCharacterById(LocalId);
							if(!pRollChar)
								continue;

							CNetObj_PlayerInput In{};
							In.m_Direction = Cand.m_Direction;
							In.m_TargetX = (int)round_to_int(Cand.m_Aim.x * 1000.0f);
							In.m_TargetY = (int)round_to_int(Cand.m_Aim.y * 1000.0f);
							if(!In.m_TargetX && !In.m_TargetY)
								In.m_TargetY = -1;
							In.m_Hook = Cand.m_Hook ? 1 : 0;
							if(Cand.m_Jump)
								In.m_Jump = (In.m_Jump + 2) | 1;

							float Score = 0.0f;
							if(Cand.m_Hook)
							{
								vec2 HitPos;
								int TeleNr = 0;
								const int Hit = pClient->Collision()->IntersectLineTeleHook(
									Pos0, Pos0 + Cand.m_Aim * HookLen, &HitPos, nullptr,
									&TeleNr);
								if(Hit == 0 || Hit == TILE_NOHOOK || Hit == TILE_TELEINHOOK)
									Score -= 1000.0f;
								else
									Score += 0.25f;
							}
							if(Cand.m_Direction == 0)
								Score -= 0.01f;
							if(Cand.m_Jump)
								Score -= 0.02f;

							vec2 PrevPos = pRollChar->Core()->m_Pos;
							for(int t = 1; t <= Horizon; t++)
							{
								pRollChar->OnPredictedInput(&In);
								Roll.Tick();
								const vec2 P = pRollChar->Core()->m_Pos;
								if(IsFreezeAt(P))
								{
									Score -= 10000.0f;
									break;
								}
								const float D0 = distance(PrevPos, Goal);
								const float D1 = distance(P, Goal);
								Score += (D0 - D1);
								PrevPos = P;
							}

							if(!Found || Score > BestScore)
							{
								Found = true;
								BestScore = Score;
								Best = Cand;
							}
						}
					}
				}
			}

			if(!Found)
				break;

			CNetObj_PlayerInput Act{};
			Act.m_Direction = Best.m_Direction;
			Act.m_TargetX = (int)round_to_int(Best.m_Aim.x * 1000.0f);
			Act.m_TargetY = (int)round_to_int(Best.m_Aim.y * 1000.0f);
			if(!Act.m_TargetX && !Act.m_TargetY)
				Act.m_TargetY = -1;
			Act.m_Hook = Best.m_Hook ? 1 : 0;
			if(Best.m_Jump)
				Act.m_Jump = (Act.m_Jump + 2) | 1;

			pChar->OnPredictedInput(&Act);
			SimWorld.Tick();
			const vec2 Pos1 = pChar->Core()->m_Pos;
			const vec2 Vel1 = pChar->Core()->m_Vel;
			const float D0 = distance(Pos0, Goal);
			const float D1 = distance(Pos1, Goal);
			float Reward = (D0 - D1);
			bool Done = false;
			if(IsFreezeAt(Pos1))
			{
				Reward -= 100.0f;
				Done = true;
			}
			if(distance(Pos1, Goal) < 32.0f)
			{
				Reward += 25.0f;
				Done = true;
			}

			char aLine[1024];
			const int ActionId = EncodeAction(Best);
			str_format(aLine, sizeof(aLine),
				"{\"x\":%.3f,\"y\":%.3f,\"vx\":%.3f,\"vy\":%.3f,\"gx\":%.3f,"
				"\"gy\":%.3f,\"a\":%d,\"r\":%.6f,\"nx\":%.3f,\"ny\":%.3f,"
				"\"nvx\":%.3f,\"nvy\":%.3f,\"d\":%d}\n",
				Pos0.x, Pos0.y, Vel0.x, Vel0.y, Goal.x, Goal.y, ActionId,
				Reward, Pos1.x, Pos1.y, Vel1.x, Vel1.y, Done ? 1 : 0);
			io_write(Handle, aLine, str_length(aLine));
			Written++;
			if(Done)
				break;
		}
	}

	io_close(Handle);
	char aMsg[256];
	str_format(aMsg, sizeof(aMsg), "Written %d transitions to %s", Written,
		aFilename);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

static void FentBotMakeUniqueDatasetFilename(CGameClient *pClient,
	const char *pFolder,
	const char *pMap, const char *pTag,
	char *pOut, int OutSize)
{
	char aTimestamp[32];
	str_timestamp_format(aTimestamp, sizeof(aTimestamp),
		TimestampFormat::NOSPACE);
	char aTag[64];
	aTag[0] = 0;
	if(pTag && pTag[0])
	{
		int k = 0;
		for(int i = 0; pTag[i] && k + 1 < (int)sizeof(aTag); i++)
		{
			char c = pTag[i];
			if(c == '/' || c == '\\' || c == ':' || c == '*')
				c = '_';
			aTag[k++] = c;
		}
		aTag[k] = 0;
	}

	char aBase[IO_MAX_PATH_LENGTH];
	if(aTag[0])
		str_format(aBase, sizeof(aBase), "%s/%s_%s_%s.bin", pFolder, pMap, aTag,
			aTimestamp);
	else
		str_format(aBase, sizeof(aBase), "%s/%s_%s.bin", pFolder, pMap, aTimestamp);

	str_copy(pOut, aBase, OutSize);
	if(!pClient->Storage()->FileExists(pOut, IStorage::TYPE_SAVE))
		return;

	for(int n = 1; n < 10000; n++)
	{
		if(aTag[0])
			str_format(pOut, OutSize, "%s/%s_%s_%s_%d.bin", pFolder, pMap, aTag,
				aTimestamp, n);
		else
			str_format(pOut, OutSize, "%s/%s_%s_%d.bin", pFolder, pMap, aTimestamp,
				n);
		if(!pClient->Storage()->FileExists(pOut, IStorage::TYPE_SAVE))
			return;
	}
}

void CGameClient::ConZzFentBotRecordStart(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pFilenameArg =
		pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(pClient->m_FentBotRecordActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Recording already active");
		return;
	}
	if(!pClient->m_Snap.m_pLocalCharacter)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Not in game");
		return;
	}
	if(!pClient->Storage()->CreateFolder("fentbot", IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/general",
			IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/waypoints",
			IStorage::TYPE_SAVE))
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to create fentbot dataset folders");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	if(pFilenameArg && pFilenameArg[0])
	{
		if(str_endswith_nocase(pFilenameArg, ".bin"))
			str_format(aFilename, sizeof(aFilename), "fentbot/general/%s",
				pFilenameArg);
		else
			str_format(aFilename, sizeof(aFilename), "fentbot/general/%s.bin",
				pFilenameArg);
		if(pClient->Storage()->FileExists(aFilename, IStorage::TYPE_SAVE))
		{
			char aMsg[512];
			str_format(aMsg, sizeof(aMsg), "Record file already exists: %s",
				aFilename);
			pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
				aMsg);
			return;
		}
	}
	else
	{
		const char *pFolder =
			pClient->m_FentBotHasWaypoint ? "fentbot/waypoints" : "fentbot/general";
		FentBotMakeUniqueDatasetFilename(pClient, pFolder,
			pClient->Map()->BaseName(), nullptr,
			aFilename, sizeof(aFilename));
	}

	IOHANDLE Handle = pClient->Storage()->OpenFile(aFilename, IOFLAG_WRITE,
		IStorage::TYPE_SAVE);
	if(!Handle)
	{
		char aMsg[512];
		str_format(aMsg, sizeof(aMsg), "Failed to open record file: %s", aFilename);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
		return;
	}

	// Binary BC dataset:
	// char magic[8] = "FENTBC01"
	// int32 version = 1
	// int32 obs_dim
	// int32 actions
	io_write(Handle, "FENTBC01", 8);
	int Version = 1;
	io_write(Handle, &Version, (int)sizeof(Version));
	int ObsDim = FENTBOT_OBS_DIM;
	io_write(Handle, &ObsDim, (int)sizeof(ObsDim));
	int Actions = FENTBOT_ACTIONS;
	io_write(Handle, &Actions, (int)sizeof(Actions));

	pClient->m_FentBotRecordHandle = Handle;
	pClient->m_FentBotRecordActive = true;
	pClient->m_FentBotRecordWritten = 0;
	{
		char aMsg[512];
		str_format(aMsg, sizeof(aMsg), "Recording started: %s", aFilename);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
	}
}

void CGameClient::ConZzFentBotRecordStop(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	(void)pResult;
	if(!pClient->m_FentBotRecordActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Recording is not active");
		return;
	}
	if(pClient->m_FentBotRecordHandle)
		io_close(pClient->m_FentBotRecordHandle);
	pClient->m_FentBotRecordHandle = 0;
	pClient->m_FentBotRecordActive = false;
	char aMsg[256];
	str_format(aMsg, sizeof(aMsg), "Recording stopped (samples=%llu)",
		(unsigned long long)pClient->m_FentBotRecordWritten);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

void CGameClient::ConZzFentBotClipStart(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pTag = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	if(pClient->m_FentBotClipActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Clip recording already active");
		return;
	}
	if(!pClient->m_Snap.m_pLocalCharacter)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Not in game");
		return;
	}
	if(!pClient->Storage()->CreateFolder("fentbot", IStorage::TYPE_SAVE) ||
		!pClient->Storage()->CreateFolder("fentbot/blocktricks",
			IStorage::TYPE_SAVE))
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to create fentbot/blocktricks folder");
		return;
	}

	char aFilename[IO_MAX_PATH_LENGTH];
	FentBotMakeUniqueDatasetFilename(pClient, "fentbot/blocktricks",
		pClient->Map()->BaseName(), pTag, aFilename,
		sizeof(aFilename));

	IOHANDLE Handle = pClient->Storage()->OpenFile(aFilename, IOFLAG_WRITE,
		IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open clip record file");
		return;
	}

	io_write(Handle, "FENTBC01", 8);
	int Version = 1;
	io_write(Handle, &Version, (int)sizeof(Version));
	int ObsDim = FENTBOT_OBS_DIM;
	io_write(Handle, &ObsDim, (int)sizeof(ObsDim));
	int Actions = FENTBOT_ACTIONS;
	io_write(Handle, &Actions, (int)sizeof(Actions));

	pClient->m_FentBotClipHandle = Handle;
	pClient->m_FentBotClipActive = true;
	pClient->m_FentBotClipWritten = 0;
	{
		char aMsg[512];
		str_format(aMsg, sizeof(aMsg), "Clip recording started: %s", aFilename);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
	}
}

void CGameClient::ConZzFentBotClipStop(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	(void)pResult;
	if(!pClient->m_FentBotClipActive)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Clip recording is not active");
		return;
	}
	if(pClient->m_FentBotClipHandle)
		io_close(pClient->m_FentBotClipHandle);
	pClient->m_FentBotClipHandle = 0;
	pClient->m_FentBotClipActive = false;
	char aMsg[256];
	str_format(aMsg, sizeof(aMsg), "Clip recording stopped (samples=%llu)",
		(unsigned long long)pClient->m_FentBotClipWritten);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

void CGameClient::ConZzFentBotBcInfer(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const int Enable = pResult->GetInteger(0);
	pClient->m_FentBotBcInfer = Enable != 0;
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
		pClient->m_FentBotBcInfer ? "BC infer enabled" : "BC infer disabled");
}

void CGameClient::ConZzFentBotBcTrain(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pFile = pResult->GetString(0);
	if(!pFile || pFile[0] == '\0')
		return;
	const int Epochs = pResult->NumArguments() > 1 ? std::clamp(pResult->GetInteger(1), 1, 200) : 10;

	auto TryOpen = [&](const char *pPath, int Type) -> IOHANDLE {
		return pClient->Storage()->OpenFile(pPath, IOFLAG_READ, Type);
	};

	char aTry1[IO_MAX_PATH_LENGTH];
	str_copy(aTry1, pFile);
	char aTry2[IO_MAX_PATH_LENGTH];
	char aTry3[IO_MAX_PATH_LENGTH];
	aTry2[0] = 0;
	aTry3[0] = 0;
	if(str_find(aTry1, "/") == nullptr && str_find(aTry1, "\\") == nullptr)
		str_format(aTry2, sizeof(aTry2), "fentbot/%s", aTry1);
	if(!str_endswith_nocase(aTry1, ".bin"))
		str_format(aTry3, sizeof(aTry3), "%s.bin", aTry1);

	const char *apPaths[4] = {aTry1, aTry2[0] ? aTry2 : nullptr,
		aTry3[0] ? aTry3 : nullptr, nullptr};
	IOHANDLE Handle = nullptr;
	for(const char *pPath : apPaths)
	{
		if(!pPath)
			continue;
		Handle = TryOpen(pPath, IStorage::TYPE_SAVE);
		if(!Handle)
			Handle = TryOpen(pPath, IStorage::TYPE_ALL);
		if(Handle)
			break;
	}
	if(!Handle)
	{
		char aBase[IO_MAX_PATH_LENGTH];
		pClient->Storage()->GetCompletePath(IStorage::TYPE_SAVE, "", aBase,
			sizeof(aBase));
		char aMsg[512];
		str_format(aMsg, sizeof(aMsg), "Failed to open BC dataset (save base: %s)",
			aBase);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Tried paths:");
		for(const char *pPath : apPaths)
			if(pPath)
				pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
					pPath);
		return;
	}
	char aMagic[8];
	if(io_read(Handle, aMagic, (int)sizeof(aMagic)) != (int)sizeof(aMagic) ||
		mem_comp(aMagic, "FENTBC01", 8) != 0)
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Invalid BC dataset magic");
		return;
	}
	int Version = 0;
	int ObsDim = 0;
	int Actions = 0;
	if(io_read(Handle, &Version, (int)sizeof(Version)) != (int)sizeof(Version) ||
		Version != 1 ||
		io_read(Handle, &ObsDim, (int)sizeof(ObsDim)) != (int)sizeof(ObsDim) ||
		io_read(Handle, &Actions, (int)sizeof(Actions)) != (int)sizeof(Actions))
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Invalid BC dataset header");
		return;
	}
	if(ObsDim != FENTBOT_OBS_DIM || Actions != FENTBOT_ACTIONS)
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"BC dataset incompatible with current model");
		return;
	}

	struct SSample
	{
		int m_A;
		std::array<float, FENTBOT_OBS_DIM> m_Obs;
	};
	std::vector<SSample> vSamples;
	vSamples.reserve(200000);
	while(true)
	{
		SSample S;
		if(io_read(Handle, &S.m_A, (int)sizeof(S.m_A)) != (int)sizeof(S.m_A))
			break;
		if(io_read(Handle, S.m_Obs.data(),
			   (int)(S.m_Obs.size() * sizeof(float))) !=
			(int)(S.m_Obs.size() * sizeof(float)))
			break;
		if(S.m_A < 0 || S.m_A >= FENTBOT_ACTIONS)
			continue;
		vSamples.push_back(S);
		if(vSamples.size() >= 200000)
			break;
	}
	io_close(Handle);
	if(vSamples.empty())
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"BC dataset has no samples");
		return;
	}

	// Init model if needed
	if(!pClient->m_FentBotAiModelLoaded)
	{
		std::mt19937 Rng((uint32_t)time_get());
		std::uniform_real_distribution<float> U(-0.02f, 0.02f);
		for(float &v : pClient->m_aFentBotPpoW1)
			v = U(Rng);
		for(float &v : pClient->m_aFentBotPpoB1)
			v = 0.0f;
		for(float &v : pClient->m_aFentBotPpoWpi)
			v = U(Rng);
		for(float &v : pClient->m_aFentBotPpoBpi)
			v = 0.0f;
		for(float &v : pClient->m_aFentBotPpoWv)
			v = U(Rng);
		pClient->m_FentBotPpoBv = 0.0f;
		pClient->m_FentBotAiModelLoaded = true;
	}

	std::mt19937 Rng((uint32_t)time_get());
	std::vector<int> vIdx((int)vSamples.size());
	for(int i = 0; i < (int)vIdx.size(); i++)
		vIdx[i] = i;
	const int Batch = 256;
	const float Lr = 1e-3f;

	for(int Ep = 0; Ep < Epochs; Ep++)
	{
		std::shuffle(vIdx.begin(), vIdx.end(), Rng);
		for(int mb0 = 0; mb0 < (int)vIdx.size(); mb0 += Batch)
		{
			const int mb1 = std::min((int)vIdx.size(), mb0 + Batch);
			const int Mb = mb1 - mb0;
			std::array<float, FENTBOT_HIDDEN * FENTBOT_OBS_DIM> gW1{};
			std::array<float, FENTBOT_HIDDEN> gB1{};
			std::array<float, FENTBOT_ACTIONS * FENTBOT_HIDDEN> gWpi{};
			std::array<float, FENTBOT_ACTIONS> gBpi{};

			for(int it = mb0; it < mb1; it++)
			{
				const SSample &S = vSamples[vIdx[it]];
				float aZ[FENTBOT_HIDDEN];
				float aH[FENTBOT_HIDDEN];
				for(int h = 0; h < FENTBOT_HIDDEN; h++)
				{
					float v = pClient->m_aFentBotPpoB1[h];
					const int Base = h * FENTBOT_OBS_DIM;
					for(int j = 0; j < FENTBOT_OBS_DIM; j++)
						v += pClient->m_aFentBotPpoW1[Base + j] * S.m_Obs[j];
					aZ[h] = v;
					aH[h] = v > 0.0f ? v : 0.0f;
				}
				float aLogits[FENTBOT_ACTIONS];
				for(int a = 0; a < FENTBOT_ACTIONS; a++)
				{
					float q = pClient->m_aFentBotPpoBpi[a];
					const int Base = a * FENTBOT_HIDDEN;
					for(int h = 0; h < FENTBOT_HIDDEN; h++)
						q += pClient->m_aFentBotPpoWpi[Base + h] * aH[h];
					aLogits[a] = q;
				}
				float M = aLogits[0];
				for(int a = 1; a < FENTBOT_ACTIONS; a++)
					M = std::max(M, aLogits[a]);
				float Sum = 0.0f;
				for(int a = 0; a < FENTBOT_ACTIONS; a++)
					Sum += expf(aLogits[a] - M);
				const float LogZ = M + logf(std::max(1e-12f, Sum));
				float aP[FENTBOT_ACTIONS];
				for(int a = 0; a < FENTBOT_ACTIONS; a++)
					aP[a] = expf(aLogits[a] - LogZ);

				float aGradLogits[FENTBOT_ACTIONS];
				for(int a = 0; a < FENTBOT_ACTIONS; a++)
				{
					const float one = (a == S.m_A) ? 1.0f : 0.0f;
					aGradLogits[a] = (aP[a] - one);
				}

				float aGradH[FENTBOT_HIDDEN] = {0};
				for(int a = 0; a < FENTBOT_ACTIONS; a++)
				{
					gBpi[a] += aGradLogits[a];
					const int Base = a * FENTBOT_HIDDEN;
					for(int h = 0; h < FENTBOT_HIDDEN; h++)
					{
						gWpi[Base + h] += aGradLogits[a] * aH[h];
						aGradH[h] += pClient->m_aFentBotPpoWpi[Base + h] * aGradLogits[a];
					}
				}
				for(int h = 0; h < FENTBOT_HIDDEN; h++)
				{
					const float gz = aZ[h] > 0.0f ? aGradH[h] : 0.0f;
					gB1[h] += gz;
					const int Base = h * FENTBOT_OBS_DIM;
					for(int j = 0; j < FENTBOT_OBS_DIM; j++)
						gW1[Base + j] += gz * S.m_Obs[j];
				}
			}

			const float invMb = 1.0f / (float)std::max(1, Mb);
			for(float &v : gW1)
				v *= invMb;
			for(float &v : gB1)
				v *= invMb;
			for(float &v : gWpi)
				v *= invMb;
			for(float &v : gBpi)
				v *= invMb;
			for(size_t k = 0; k < pClient->m_aFentBotPpoW1.size(); k++)
				pClient->m_aFentBotPpoW1[k] -= Lr * gW1[k];
			for(size_t k = 0; k < pClient->m_aFentBotPpoB1.size(); k++)
				pClient->m_aFentBotPpoB1[k] -= Lr * gB1[k];
			for(size_t k = 0; k < pClient->m_aFentBotPpoWpi.size(); k++)
				pClient->m_aFentBotPpoWpi[k] -= Lr * gWpi[k];
			for(size_t k = 0; k < pClient->m_aFentBotPpoBpi.size(); k++)
				pClient->m_aFentBotPpoBpi[k] -= Lr * gBpi[k];
		}
	}

	char aMsg[256];
	str_format(aMsg, sizeof(aMsg), "BC train done (samples=%d epochs=%d)",
		(int)vSamples.size(), Epochs);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot", aMsg);
}

void CGameClient::ConZzFentBotLoadModel(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pFile = pResult->GetString(0);
	if(!pFile || pFile[0] == '\0')
		return;

	IOHANDLE Handle =
		pClient->Storage()->OpenFile(pFile, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open model file");
		pClient->m_FentBotAiModelLoaded = false;
		return;
	}

	// Binary format:
	// char magic[8] = "FENTPPO1" (no null)
	// int32 version = 1
	char aMagic[8];
	if(io_read(Handle, aMagic, sizeof(aMagic)) != (int)sizeof(aMagic) ||
		mem_comp(aMagic, "FENTPPO1", 8) != 0)
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Invalid model magic");
		pClient->m_FentBotAiModelLoaded = false;
		return;
	}
	int Version = 0;
	if(io_read(Handle, &Version, sizeof(Version)) != (int)sizeof(Version) ||
		Version != 1)
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Unsupported model version");
		pClient->m_FentBotAiModelLoaded = false;
		return;
	}

	auto ReadExact = [&](void *pDst, int Size) -> bool {
		return io_read(Handle, pDst, Size) == Size;
	};

	if(!ReadExact(pClient->m_aFentBotPpoW1.data(),
		   (int)(pClient->m_aFentBotPpoW1.size() * sizeof(float))) ||
		!ReadExact(pClient->m_aFentBotPpoB1.data(),
			(int)(pClient->m_aFentBotPpoB1.size() * sizeof(float))) ||
		!ReadExact(pClient->m_aFentBotPpoWpi.data(),
			(int)(pClient->m_aFentBotPpoWpi.size() * sizeof(float))) ||
		!ReadExact(pClient->m_aFentBotPpoBpi.data(),
			(int)(pClient->m_aFentBotPpoBpi.size() * sizeof(float))) ||
		!ReadExact(pClient->m_aFentBotPpoWv.data(),
			(int)(pClient->m_aFentBotPpoWv.size() * sizeof(float))) ||
		!ReadExact(&pClient->m_FentBotPpoBv, (int)sizeof(float)))
	{
		io_close(Handle);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to read model weights");
		pClient->m_FentBotAiModelLoaded = false;
		return;
	}

	io_close(Handle);
	pClient->m_FentBotAiModelLoaded = true;
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
		"Model loaded");
}

void CGameClient::ConZzFentBotSaveModel(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	const char *pFile = pResult->GetString(0);
	if(!pFile || pFile[0] == '\0')
		return;
	if(!pClient->m_FentBotAiModelLoaded)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"No model loaded to save");
		return;
	}

	IOHANDLE Handle =
		pClient->Storage()->OpenFile(pFile, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!Handle)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Failed to open model file for writing");
		return;
	}

	io_write(Handle, "FENTPPO1", 8);
	int Version = 1;
	io_write(Handle, &Version, sizeof(Version));
	io_write(Handle, pClient->m_aFentBotPpoW1.data(),
		(int)(pClient->m_aFentBotPpoW1.size() * sizeof(float)));
	io_write(Handle, pClient->m_aFentBotPpoB1.data(),
		(int)(pClient->m_aFentBotPpoB1.size() * sizeof(float)));
	io_write(Handle, pClient->m_aFentBotPpoWpi.data(),
		(int)(pClient->m_aFentBotPpoWpi.size() * sizeof(float)));
	io_write(Handle, pClient->m_aFentBotPpoBpi.data(),
		(int)(pClient->m_aFentBotPpoBpi.size() * sizeof(float)));
	io_write(Handle, pClient->m_aFentBotPpoWv.data(),
		(int)(pClient->m_aFentBotPpoWv.size() * sizeof(float)));
	io_write(Handle, &pClient->m_FentBotPpoBv, (int)sizeof(float));
	io_close(Handle);
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
		"Model saved");
}

void CGameClient::ConZzFentBotTrainStart(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	if(!pClient->m_Snap.m_pLocalCharacter)
		return;
	if(!pClient->m_FentBotHasWaypoint)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
			"Set waypoint first");
		return;
	}
	g_Config.m_ClZzFentBotTrain = 1;
	// force re-init on next update
	pClient->m_FentBotTrainWasEnabled = false;
}

void CGameClient::ConZzFentBotTrainStop(IConsole::IResult *pResult,
	void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	g_Config.m_ClZzFentBotTrain = 0;
	pClient->m_FentBotTrainWasEnabled = false;
	pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "fentbot",
		"Training stopped");
}

void CGameClient::ConZzAiMode(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	auto PrintStatus = [&](const char *pPrefix) {
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg),
			"%s: fentbot=%d ai_cfg=%d py=%d bc=%d companion=%d "
			"companion_py=%d kog=%d train=%d",
			pPrefix, g_Config.m_ClZzFentBotEnabled,
			g_Config.m_ClZzFentBotAiInfer, g_Config.m_ClZzFentBotPyInfer,
			(int)pClient->m_FentBotBcInfer, g_Config.m_ClZzCompanionEnabled,
			g_Config.m_ClZzCompanionPyInfer, g_Config.m_ClZzKogAiEnabled,
			g_Config.m_ClZzFentBotTrain);
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/ai", aMsg);
	};

	if(pResult->NumArguments() <= 0)
	{
		PrintStatus("status");
		pClient->Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD, "zz/ai",
			"modes: 0=off 1=fentbot 2=python 3=bc 4=companion 5=kog");
		return;
	}

	const int Mode = std::clamp(pResult->GetInteger(0), 0, 5);

	// Reset AI modes first so we always end in a deterministic state.
	g_Config.m_ClZzFentBotEnabled = 0;
	g_Config.m_ClZzFentBotAiInfer = 0;
	g_Config.m_ClZzFentBotPyInfer = 0;
	pClient->m_FentBotBcInfer = false;
	g_Config.m_ClZzCompanionEnabled = 0;
	g_Config.m_ClZzCompanionPyInfer = 0;
	g_Config.m_ClZzKogAiEnabled = 0;
	g_Config.m_ClZzFentBotTrain = 0;
	pClient->m_FentBotTrainWasEnabled = false;

	const char *pModeName = "off";
	switch(Mode)
	{
	case 0:
		pModeName = "off";
		break;
	case 1:
		pModeName = "fentbot";
		g_Config.m_ClZzFentBotEnabled = 1;
		break;
	case 2:
		pModeName = "python";
		g_Config.m_ClZzFentBotEnabled = 1;
		g_Config.m_ClZzFentBotPyInfer = 1;
		break;
	case 3:
		pModeName = "bc";
		g_Config.m_ClZzFentBotEnabled = 1;
		pClient->m_FentBotBcInfer = true;
		break;
	case 4:
		pModeName = "companion";
		g_Config.m_ClZzCompanionEnabled = 1;
		g_Config.m_ClZzCompanionPyInfer = 1;
		break;
	case 5:
		pModeName = "kog";
		g_Config.m_ClZzKogAiEnabled = 1;
		break;
	default:
		break;
	}

	if((Mode == 1 || Mode == 2 || Mode == 3) && !pClient->m_FentBotHasWaypoint)
	{
		pClient->Console()->Print(
			IConsole::OUTPUT_LEVEL_STANDARD, "zz/ai",
			"hint: set waypoint first (+zz_fentbot_waypoint), then bot has a clear "
			"goal");
	}
	if(Mode == 4 && !pClient->Client()->DummyConnected())
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/ai",
			"hint: companion mode needs connected dummy");
	}
	if(Mode == 5)
	{
		pClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/ai",
			"hint: KoG AI uses normal client input path with "
			"reaction-time and cooldown limits");
	}

	PrintStatus(pModeName);
}

void CGameClient::ConchainLanguageUpdate(IConsole::IResult *pResult,
	void *pUserData,
	IConsole::FCommandCallback pfnCallback,
	void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	const bool Changed =
		pThis->Client()->GlobalTime() && pResult->NumArguments() &&
		str_comp(pResult->GetString(0), g_Config.m_ClLanguagefile) != 0;
	pfnCallback(pResult, pCallbackUserData);
	if(Changed)
	{
		pThis->OnLanguageChange();
	}
}

void CGameClient::ConchainSpecialInfoupdate(
	IConsole::IResult *pResult, void *pUserData,
	IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CGameClient *)pUserData)->SendInfo(false);
}

void CGameClient::ConchainSpecialDummyInfoupdate(
	IConsole::IResult *pResult, void *pUserData,
	IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameClient *pClient = (CGameClient *)pUserData;
		for(int Conn = IClient::CONN_DUMMY; Conn < NUM_DUMMIES; Conn++)
		{
			if(pClient->Client()->DummyConnected(Conn))
				pClient->SendDummyInfo(false, Conn);
		}
	}
}

void CGameClient::ConchainSpecialDummy(IConsole::IResult *pResult,
	void *pUserData,
	IConsole::FCommandCallback pfnCallback,
	void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		if(g_Config.m_ClDummy && !((CGameClient *)pUserData)
						 ->Client()
						 ->DummyConnected(g_Config.m_ClDummy))
			g_Config.m_ClDummy = 0;
	}
}

IGameClient *CreateGameClient() { return new CGameClient(); }

int CGameClient::IntersectCharacter(vec2 HookPos, vec2 NewPos, vec2 &NewPos2,
	int OwnId, vec2 *pPlayerPosition)
{
	float Distance = 0.0f;
	int ClosestId = -1;

	const CClientData &OwnClientData = m_aClients[OwnId];

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == OwnId)
			continue;

		const CClientData &Data = m_aClients[i];

		if(!Data.m_Active || !m_Snap.m_aCharacters[i].m_Active)
			continue;

		CNetObj_Character Prev = m_Snap.m_aCharacters[i].m_Prev;
		CNetObj_Character Player = m_Snap.m_aCharacters[i].m_Cur;

		vec2 Position = mix(vec2(Prev.m_X, Prev.m_Y), vec2(Player.m_X, Player.m_Y),
			Client()->IntraGameTick(g_Config.m_ClDummy));

		bool IsOneSuper = Data.m_Super || OwnClientData.m_Super;
		bool IsOneSolo = Data.m_Solo || OwnClientData.m_Solo;

		if(!IsOneSuper && (!m_Teams.SameTeam(i, OwnId) || IsOneSolo ||
					  OwnClientData.m_HookHitDisabled))
			continue;

		vec2 ClosestPoint;
		if(closest_point_on_line(HookPos, NewPos, Position, ClosestPoint))
		{
			if(distance(Position, ClosestPoint) <
				CCharacterCore::PhysicalSize() + 2.0f)
			{
				if(ClosestId == -1 || distance(HookPos, Position) < Distance)
				{
					NewPos2 = ClosestPoint;
					ClosestId = i;
					Distance = distance(HookPos, Position);
					if(pPlayerPosition)
						*pPlayerPosition = Position;
				}
			}
		}
	}

	return ClosestId;
}

ColorRGBA CalculateNameColor(ColorHSLA TextColorHSL)
{
	return color_cast<ColorRGBA>(ColorHSLA(TextColorHSL.h, TextColorHSL.s * 0.68f,
		TextColorHSL.l * 0.81f));
}

void CGameClient::UpdateLocalTuning()
{
	m_GameWorld.m_WorldConfig.m_UseTuneZones = m_GameInfo.m_PredictDDRaceTiles;

	// always update default tune zone, even without character
	if(!m_GameWorld.m_WorldConfig.m_UseTuneZones)
		m_GameWorld.TuningList()[0] = m_aTuning[g_Config.m_ClDummy];

	if(!m_Snap.m_pLocalCharacter && !m_Snap.m_pSpectatorInfo)
		return;

	vec2 LocalPos =
		m_Snap.m_pLocalCharacter ? vec2(m_Snap.m_pLocalCharacter->m_X, m_Snap.m_pLocalCharacter->m_Y) : vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y);

	// update the tuning at the local position with the latest tunings received
	// before the new snapshot
	if(m_GameWorld.m_WorldConfig.m_UseTuneZones)
	{
		int TuneZone =
			m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData &&
					m_Snap.m_aCharacters[m_Snap.m_LocalClientId]
							.m_ExtendedData.m_TuneZoneOverride !=
						TuneZone::OVERRIDE_NONE ?
				m_Snap.m_aCharacters[m_Snap.m_LocalClientId]
					.m_ExtendedData.m_TuneZoneOverride :
				Collision()->IsTune(Collision()->GetMapIndex(LocalPos));

		if(TuneZone != m_aLocalTuneZone[g_Config.m_ClDummy])
		{
			// our tunezone changed, expecting tuning message
			m_aLocalTuneZone[g_Config.m_ClDummy] =
				m_aExpectingTuningForZone[g_Config.m_ClDummy] = TuneZone;
			m_aExpectingTuningSince[g_Config.m_ClDummy] = 0;
		}

		// tunezone could have changed, send dummy tuning to demo
		if(m_ActiveRecordings.any() && m_IsDummySwapping &&
			m_aLocalTuneZone[0] != m_aLocalTuneZone[1])
		{
			CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
			int *pParams = (int *)&m_aTuning[g_Config.m_ClDummy];
			for(unsigned i = 0; i < sizeof(m_aTuning[0]) / sizeof(int); i++)
				Msg.AddInt(pParams[i]);
			Client()->SendMsgActive(&Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
		}

		if(m_aExpectingTuningForZone[g_Config.m_ClDummy] >= 0)
		{
			if(m_aReceivedTuning[g_Config.m_ClDummy])
			{
				TuningList()[m_aExpectingTuningForZone[g_Config.m_ClDummy]] =
					m_aTuning[g_Config.m_ClDummy];
				m_GameWorld
					.TuningList()[m_aExpectingTuningForZone[g_Config.m_ClDummy]] =
					m_aTuning[g_Config.m_ClDummy];
				m_aReceivedTuning[g_Config.m_ClDummy] = false;
				m_aExpectingTuningForZone[g_Config.m_ClDummy] = -1;
			}
			else if(m_aExpectingTuningSince[g_Config.m_ClDummy] >= 5)
			{
				// if we are expecting tuning for more than 10 snaps (less than a
				// quarter of a second) it is probably dropped or it was received out of
				// order or applied to another tunezone. we need to fallback to current
				// tuning to fix ourselves.
				m_aExpectingTuningForZone[g_Config.m_ClDummy] = -1;
				m_aExpectingTuningSince[g_Config.m_ClDummy] = 0;
				m_aReceivedTuning[g_Config.m_ClDummy] = false;
				dbg_msg("tunezone", "the tuning was missed");
			}
			else
			{
				// if we are expecting tuning and have not received one yet.
				// do not update any tuning, so we don't apply it to the wrong tunezone.
				dbg_msg("tunezone", "waiting for tuning for zone %d",
					m_aExpectingTuningForZone[g_Config.m_ClDummy]);
				m_aExpectingTuningSince[g_Config.m_ClDummy]++;
			}
		}
		else
		{
			// if we have processed what we need, and the tuning is still wrong due to
			// out of order message fix our tuning by using the current one
			m_GameWorld.TuningList()[TuneZone] = m_aTuning[g_Config.m_ClDummy];
			m_aExpectingTuningSince[g_Config.m_ClDummy] = 0;
			m_aReceivedTuning[g_Config.m_ClDummy] = false;
		}
	}
}

void CGameClient::UpdatePrediction()
{
	m_GameWorld.m_WorldConfig.m_IsVanilla = m_GameInfo.m_PredictVanilla;
	m_GameWorld.m_WorldConfig.m_IsDDRace = m_GameInfo.m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_IsFNG = m_GameInfo.m_PredictFNG;
	m_GameWorld.m_WorldConfig.m_PredictDDRace = m_GameInfo.m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_PredictTiles =
		m_GameInfo.m_PredictDDRace && m_GameInfo.m_PredictDDRaceTiles;
	m_GameWorld.m_WorldConfig.m_PredictFreeze = g_Config.m_ClPredictFreeze;
	m_GameWorld.m_WorldConfig.m_PredictWeapons = AntiPingWeapons();
	m_GameWorld.m_WorldConfig.m_BugDDRaceInput = m_GameInfo.m_BugDDRaceInput;
	m_GameWorld.m_WorldConfig.m_NoWeakHookAndBounce =
		m_GameInfo.m_NoWeakHookAndBounce;
	m_GameWorld.m_WorldConfig.m_PredictEvents = m_GameInfo.m_PredictEvents;

	if(!m_Snap.m_pLocalCharacter)
	{
		if(CCharacter *pLocalChar =
				m_GameWorld.GetCharacterById(m_Snap.m_LocalClientId))
			pLocalChar->Destroy();
		return;
	}

	if(m_Snap.m_pLocalCharacter->m_AmmoCount > 0 &&
		m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		m_GameWorld.m_WorldConfig.m_InfiniteAmmo = false;
	m_GameWorld.m_WorldConfig.m_IsSolo =
		!m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData &&
		!m_aTuning[g_Config.m_ClDummy].m_PlayerCollision &&
		!m_aTuning[g_Config.m_ClDummy].m_PlayerHooking;

	CCharacter *pLocalChar = m_GameWorld.GetCharacterById(m_Snap.m_LocalClientId);
	CCharacter *pDummyChar = nullptr;
	if(PredictDummy())
		pDummyChar =
			m_GameWorld.GetCharacterById(m_aLocalIds[Client()->DummyPair()]);

	// update strong and weak hook
	if(pLocalChar && !m_Snap.m_SpecInfo.m_Active &&
		Client()->State() != IClient::STATE_DEMOPLAYBACK &&
		(m_aTuning[g_Config.m_ClDummy].m_PlayerCollision ||
			m_aTuning[g_Config.m_ClDummy].m_PlayerHooking))
	{
		if(m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData)
		{
			int aIds[MAX_CLIENTS];
			for(int &Id : aIds)
				Id = -1;
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
					aIds[pChar->GetStrongWeakId()] = i;
			for(int Id : aIds)
				if(Id >= 0)
					m_CharOrder.GiveStrong(Id);
		}
		else
		{
			// manual detection
			DetectStrongHook();
		}
		for(int i : m_CharOrder.m_Ids)
		{
			if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
			{
				m_GameWorld.RemoveEntity(pChar);
				m_GameWorld.InsertEntity(pChar);
			}
		}
	}

	// advance the gameworld to the current gametick
	if(pLocalChar && absolute(m_GameWorld.GameTick() -
				  Client()->GameTick(g_Config.m_ClDummy)) <
				 Client()->GameTickSpeed())
	{
		for(int Tick = m_GameWorld.GameTick() + 1;
			Tick <= Client()->GameTick(g_Config.m_ClDummy); Tick++)
		{
			CNetObj_PlayerInput *pInput =
				(CNetObj_PlayerInput *)Client()->GetInput(Tick);
			CNetObj_PlayerInput *pDummyInput = nullptr;
			if(pDummyChar)
				pDummyInput = (CNetObj_PlayerInput *)Client()->GetInput(Tick, 1);
			if(pInput)
				pLocalChar->OnDirectInput(pInput);
			if(pDummyInput)
				pDummyChar->OnDirectInput(pDummyInput);

			ApplyPreInputs(Tick, true, m_GameWorld);

			m_GameWorld.m_GameTick = Tick;
			if(pInput)
				pLocalChar->OnPredictedInput(pInput);
			if(pDummyInput)
				pDummyChar->OnPredictedInput(pDummyInput);

			ApplyPreInputs(Tick, false, m_GameWorld);

			m_GameWorld.Tick();

			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
				{
					m_aClients[i].m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
					m_aClients[i].m_aPredTick[Tick % 200] = Tick;
				}
		}
	}
	else
	{
		// skip to current gametick
		m_GameWorld.m_GameTick = Client()->GameTick(g_Config.m_ClDummy);
		if(pLocalChar)
			if(CNetObj_PlayerInput *pInput =
					(CNetObj_PlayerInput *)Client()->GetInput(
						Client()->GameTick(g_Config.m_ClDummy)))
				pLocalChar->SetInput(pInput);
		if(pDummyChar)
			if(CNetObj_PlayerInput *pInput =
					(CNetObj_PlayerInput *)Client()->GetInput(
						Client()->GameTick(g_Config.m_ClDummy), 1))
				pDummyChar->SetInput(pInput);
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
		{
			m_aClients[i].m_aPredPos[Client()->GameTick(g_Config.m_ClDummy) % 200] =
				pChar->Core()->m_Pos;
			m_aClients[i].m_aPredTick[Client()->GameTick(g_Config.m_ClDummy) % 200] =
				Client()->GameTick(g_Config.m_ClDummy);
		}

	// update the local gameworld with the new snapshot
	m_GameWorld.NetObjBegin(m_Teams, m_Snap.m_LocalClientId);

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(m_Snap.m_aCharacters[i].m_Active)
		{
			bool IsLocal =
				(i == m_Snap.m_LocalClientId ||
					(PredictDummy() && i == m_aLocalIds[Client()->DummyPair()]));
			int GameTeam = IsTeamPlay() ? m_aClients[i].m_Team : i;
			m_GameWorld.NetCharAdd(i, &m_Snap.m_aCharacters[i].m_Cur,
				m_Snap.m_aCharacters[i].m_HasExtendedData ? &m_Snap.m_aCharacters[i].m_ExtendedData : nullptr,
				GameTeam, IsLocal);
		}

	for(const CSnapEntities &EntData : SnapEntities())
		m_GameWorld.NetObjAdd(EntData.m_Item.m_Id, EntData.m_Item.m_Type,
			EntData.m_Item.m_pData, EntData.m_pDataEx);

	m_GameWorld.NetObjEnd();
}

void CGameClient::UpdateSpectatorCursor()
{
	int CursorOwnerId = m_Snap.m_LocalClientId;
	if(m_Snap.m_SpecInfo.m_Active)
	{
		CursorOwnerId = m_Snap.m_SpecInfo.m_SpectatorId;
	}

	if(CursorOwnerId != m_CursorInfo.m_CursorOwnerId)
	{
		// reset cursor sample count upon changing spectating character
		m_CursorInfo.m_NumSamples = 0;
		m_CursorInfo.m_CursorOwnerId = CursorOwnerId;
	}

	if(m_MultiViewActivated || CursorOwnerId < 0 ||
		CursorOwnerId >= MAX_CLIENTS)
	{
		// do not show spec cursor in multi-view
		m_CursorInfo.m_Available = false;
		m_CursorInfo.m_NumSamples = 0;
		return;
	}

	const CSnapState::CCharacterInfo &CharInfo =
		m_Snap.m_aCharacters[CursorOwnerId];
	const CClientData &CursorOwnerClient = m_aClients[CursorOwnerId];
	if(!CharInfo.m_HasExtendedDisplayInfo || !CursorOwnerClient.m_Active ||
		(!g_Config.m_Debug && CursorOwnerClient.m_Paused))
	{
		// hide cursor when the spectating player is paused
		m_CursorInfo.m_Available = false;
		m_CursorInfo.m_NumSamples = 0;
		return;
	}

	m_CursorInfo.m_Available = true;
	m_CursorInfo.m_Position = CursorOwnerClient.m_RenderPos;
	m_CursorInfo.m_Weapon = CharInfo.m_Cur.m_Weapon;

	const vec2 Target = vec2(CharInfo.m_ExtendedData.m_TargetX,
		CharInfo.m_ExtendedData.m_TargetY);

	if(Client()->State() == IClient::STATE_DEMOPLAYBACK &&
		DemoPlayer()->BaseInfo()->m_Paused)
	{
		m_CursorInfo.m_CursorOwnerId = -1;
		m_CursorInfo.m_NumSamples = 0;
		const vec2 TargetNew = vec2(CharInfo.m_ExtendedData.m_TargetX,
			CharInfo.m_ExtendedData.m_TargetY);
		if(CharInfo.m_pPrevExtendedData)
		{
			const vec2 TargetOld = vec2(CharInfo.m_pPrevExtendedData->m_TargetX,
				CharInfo.m_pPrevExtendedData->m_TargetY);
			m_CursorInfo.m_Target = mix(TargetOld, TargetNew,
				Client()->IntraGameTick(g_Config.m_ClDummy));
		}
		else
		{
			m_CursorInfo.m_Target = TargetNew;
		}
	}
	else
	{
		// interpolate cursor positions
		const double Tick = Client()->GameTick(g_Config.m_ClDummy);

		const bool HasSample = m_CursorInfo.m_NumSamples > 0;
		const vec2 LastInput =
			HasSample ? m_CursorInfo.m_aTargetSamplesData[m_CursorInfo.m_NumSamples - 1] : vec2(0.0f, 0.0f);
		const double LastTime =
			HasSample ? m_CursorInfo.m_aTargetSamplesTime[m_CursorInfo.m_NumSamples - 1] : 0.0;
		bool NewSample =
			LastInput != Target || LastTime + CCursorInfo::REST_THRESHOLD < Tick;

		if(LastTime > Tick)
		{
			// clear samples when time flows backwards
			m_CursorInfo.m_NumSamples = 0;
			NewSample = true;
		}

		if(m_CursorInfo.m_NumSamples == 0)
		{
			m_CursorInfo.m_aTargetSamplesTime[0] = Tick - CCursorInfo::INTERP_DELAY;
			m_CursorInfo.m_aTargetSamplesData[0] = Target;
		}

		if(NewSample)
		{
			if(m_CursorInfo.m_NumSamples == CCursorInfo::CURSOR_SAMPLES)
			{
				m_CursorInfo.m_NumSamples--;
				mem_move(m_CursorInfo.m_aTargetSamplesTime,
					m_CursorInfo.m_aTargetSamplesTime + 1,
					m_CursorInfo.m_NumSamples * sizeof(double));
				mem_move(m_CursorInfo.m_aTargetSamplesData,
					m_CursorInfo.m_aTargetSamplesData + 1,
					m_CursorInfo.m_NumSamples * sizeof(vec2));
			}
			m_CursorInfo.m_aTargetSamplesTime[m_CursorInfo.m_NumSamples] = Tick;
			m_CursorInfo.m_aTargetSamplesData[m_CursorInfo.m_NumSamples] = Target;
			m_CursorInfo.m_NumSamples++;
		}

		// using double to avoid precision loss when converting int tick to decimal
		// type
		const double DisplayTime =
			Tick - CCursorInfo::INTERP_DELAY +
			double(Client()->IntraGameTickSincePrev(g_Config.m_ClDummy));
		double aTime[CCursorInfo::SAMPLE_FRAME_WINDOW];
		vec2 aData[CCursorInfo::SAMPLE_FRAME_WINDOW];

		// find the available sample timing
		int Index = m_CursorInfo.m_NumSamples;
		for(int i = 0; i < m_CursorInfo.m_NumSamples; i++)
		{
			if(m_CursorInfo.m_aTargetSamplesTime[i] > DisplayTime)
			{
				Index = i;
				break;
			}
		}

		for(int i = 0; i < CCursorInfo::SAMPLE_FRAME_WINDOW; i++)
		{
			const int Offset = i - CCursorInfo::SAMPLE_FRAME_OFFSET;
			const int SampleIndex = Index + Offset;
			if(SampleIndex < 0)
			{
				aTime[i] = m_CursorInfo.m_aTargetSamplesTime[0] +
					   CCursorInfo::REST_THRESHOLD * Offset;
				aData[i] = m_CursorInfo.m_aTargetSamplesData[0];
			}
			else if(SampleIndex >= m_CursorInfo.m_NumSamples)
			{
				aTime[i] =
					m_CursorInfo.m_aTargetSamplesTime[m_CursorInfo.m_NumSamples - 1] +
					CCursorInfo::REST_THRESHOLD * (Offset + 1);
				aData[i] =
					m_CursorInfo.m_aTargetSamplesData[m_CursorInfo.m_NumSamples - 1];
			}
			else
			{
				aTime[i] = m_CursorInfo.m_aTargetSamplesTime[SampleIndex];
				aData[i] = m_CursorInfo.m_aTargetSamplesData[SampleIndex];
			}
		}

		m_CursorInfo.m_Target =
			mix_polynomial(aTime, aData, CCursorInfo::SAMPLE_FRAME_WINDOW,
				DisplayTime, vec2(0.0f, 0.0f));
	}

	vec2 TargetCameraOffset(0, 0);
	float l = length(m_CursorInfo.m_Target);

	if(l > 0.0001f) // make sure that this isn't 0
	{
		float OffsetAmount = maximum(l - m_Snap.m_SpecInfo.m_Deadzone, 0.0f) *
				     (m_Snap.m_SpecInfo.m_FollowFactor / 100.0f);
		TargetCameraOffset = normalize(m_CursorInfo.m_Target) * OffsetAmount;
	}

	// if we are in auto spec mode, use camera zoom to smooth out cursor
	// transitions
	const float Zoom = (m_Camera.m_Zooming && m_Camera.m_AutoSpecCameraZooming) ? m_Camera.m_Zoom : m_Snap.m_SpecInfo.m_Zoom;
	m_CursorInfo.m_WorldTarget =
		m_CursorInfo.m_Position +
		(m_CursorInfo.m_Target - TargetCameraOffset) * Zoom + TargetCameraOffset;
}

void CGameClient::UpdateRenderedCharacters()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!m_Snap.m_aCharacters[i].m_Active)
			continue;
		m_aClients[i].m_RenderCur = m_Snap.m_aCharacters[i].m_Cur;
		m_aClients[i].m_RenderPrev = m_Snap.m_aCharacters[i].m_Prev;
		m_aClients[i].m_IsPredicted = false;
		m_aClients[i].m_IsPredictedLocal = false;
		vec2 UnpredPos = mix(vec2(m_Snap.m_aCharacters[i].m_Prev.m_X,
					     m_Snap.m_aCharacters[i].m_Prev.m_Y),
			vec2(m_Snap.m_aCharacters[i].m_Cur.m_X,
				m_Snap.m_aCharacters[i].m_Cur.m_Y),
			Client()->IntraGameTick(g_Config.m_ClDummy));
		vec2 Pos = UnpredPos;
		CCharacter *pChar = m_PredictedWorld.GetCharacterById(i);

		// TClient
		if(i == m_Snap.m_LocalClientId)
			Client()->m_IsLocalFrozen = pChar && pChar->m_FreezeTime > 0;

		if(Predict() &&
			(i == m_Snap.m_LocalClientId ||
				(AntiPingPlayers() && !IsOtherTeam(i))) &&
			pChar)
		{
			m_aClients[i].m_Predicted.Write(&m_aClients[i].m_RenderCur);
			m_aClients[i].m_PrevPredicted.Write(&m_aClients[i].m_RenderPrev);

			m_aClients[i].m_IsPredicted = true;

			Pos = mix(
				vec2(m_aClients[i].m_RenderPrev.m_X, m_aClients[i].m_RenderPrev.m_Y),
				vec2(m_aClients[i].m_RenderCur.m_X, m_aClients[i].m_RenderCur.m_Y),
				m_aClients[i].m_IsPredicted ? Client()->PredIntraGameTick(g_Config.m_ClDummy) : Client()->IntraGameTick(g_Config.m_ClDummy));

			if(g_Config.m_TcRemoveAnti)
				Pos = GetFreezePos(i);
			else if(g_Config.m_TcFastInput &&
				(i == m_Snap.m_LocalClientId ||
					(PredictDummy() && i == m_aLocalIds[Client()->DummyPair()])))
				Pos = GetFastInputPos(i);

			if(i == m_Snap.m_LocalClientId ||
				(PredictDummy() && i == m_aLocalIds[Client()->DummyPair()]))
			{
				m_aClients[i].m_IsPredictedLocal = true;
				if(AntiPingGunfire() &&
					((pChar->m_NinjaJetpack && pChar->m_FreezeTime == 0) ||
						m_Snap.m_aCharacters[i].m_Cur.m_Weapon != WEAPON_NINJA ||
						m_Snap.m_aCharacters[i].m_Cur.m_Weapon ==
							m_aClients[i].m_Predicted.m_ActiveWeapon))
				{
					m_aClients[i].m_RenderCur.m_AttackTick = pChar->GetAttackTick();
					if(m_Snap.m_aCharacters[i].m_Cur.m_Weapon != WEAPON_NINJA &&
						!(pChar->m_NinjaJetpack &&
							pChar->Core()->m_ActiveWeapon == WEAPON_GUN))
						m_aClients[i].m_RenderCur.m_Weapon =
							m_aClients[i].m_Predicted.m_ActiveWeapon;
				}
			}
			else
			{
				// use unpredicted values for other players
				m_aClients[i].m_RenderPrev.m_Angle =
					m_Snap.m_aCharacters[i].m_Prev.m_Angle;
				m_aClients[i].m_RenderCur.m_Angle =
					m_Snap.m_aCharacters[i].m_Cur.m_Angle;

				if(g_Config.m_ClAntiPingSmooth)
					Pos = GetSmoothPos(i);

				if(g_Config.m_TcAntiPingImproved &&
					m_aClients[i].m_ValidAntipingSmooth)
					Pos = mix(m_aClients[i].m_PrevImprovedPredPos,
						m_aClients[i].m_ImprovedPredPos,
						Client()->PredIntraGameTick(g_Config.m_ClDummy));

				if(g_Config.m_TcRemoveAnti && m_pClient->m_IsLocalFrozen)
					Pos = GetFreezePos(i);
				else if(g_Config.m_TcFastInput && g_Config.m_TcFastInputOthers &&
					!g_Config.m_TcAntiPingImproved)
					Pos = GetFastInputPos(i);

				if(g_Config.m_TcShowOthersGhosts && g_Config.m_TcSwapGhosts &&
					!(m_aClients[i].m_FreezeEnd > 0 && g_Config.m_TcHideFrozenGhosts))
					Pos = UnpredPos;

				if(g_Config.m_TcUnpredOthersInFreeze && Client()->m_IsLocalFrozen)
					Pos = UnpredPos;
			}
		}
		m_aClients[i].m_RenderPos = Pos;
		if(Predict() && i == m_Snap.m_LocalClientId)
			m_LocalCharacterPos = Pos;
	}
}

void CGameClient::HandlePredictedEvents(const int Tick)
{
	const float Alpha = 1.0f;
	const float Volume = 1.0f;

	auto EventsIterator = m_PredictedWorld.m_PredictedEvents.begin();
	while(EventsIterator != m_PredictedWorld.m_PredictedEvents.end())
	{
		if(!EventsIterator->m_Handled && EventsIterator->m_Tick <= Tick)
		{
			if(EventsIterator->m_EventId == NETEVENTTYPE_SOUNDWORLD)
			{
				if(m_GameInfo.m_RaceSounds &&
					((EventsIterator->m_ExtraInfo == SOUND_GUN_FIRE &&
						 !g_Config.m_SndGun) ||
						(EventsIterator->m_ExtraInfo == SOUND_PLAYER_PAIN_LONG &&
							!g_Config.m_SndLongPain)))
				{
					EventsIterator =
						m_PredictedWorld.m_PredictedEvents.erase(EventsIterator);
					continue;
				}
				m_Sounds.PlayAt(CSounds::CHN_WORLD, EventsIterator->m_ExtraInfo, 1.0f,
					EventsIterator->m_Pos);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_EXPLOSION)
			{
				m_Effects.Explosion(EventsIterator->m_Pos, Alpha);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_HAMMERHIT)
			{
				m_Effects.HammerHit(EventsIterator->m_Pos, Alpha, Volume);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_DAMAGEIND)
			{
				m_Effects.DamageIndicator(
					EventsIterator->m_Pos,
					direction(EventsIterator->m_ExtraInfo / 256.0f), Alpha);
			}

			EventsIterator->m_Handled = true;
			++EventsIterator;
			continue;
		}
		else if(Tick - EventsIterator->m_Tick >
			3 * Client()->GameTickSpeed()) // 3 seconds
		{
			// remove too old events
			EventsIterator = m_PredictedWorld.m_PredictedEvents.erase(EventsIterator);
		}
		else
		{
			++EventsIterator;
		}
	}
}

void CGameClient::DetectStrongHook()
{
	// attempt to detect strong/weak between players
	for(int FromPlayer = 0; FromPlayer < MAX_CLIENTS; FromPlayer++)
	{
		if(!m_Snap.m_aCharacters[FromPlayer].m_Active)
			continue;
		int ToPlayer = m_Snap.m_aCharacters[FromPlayer].m_Prev.m_HookedPlayer;
		if(ToPlayer < 0 || ToPlayer >= MAX_CLIENTS ||
			!m_Snap.m_aCharacters[ToPlayer].m_Active ||
			ToPlayer != m_Snap.m_aCharacters[FromPlayer].m_Cur.m_HookedPlayer)
			continue;
		if(absolute(minimum(m_aLastUpdateTick[ToPlayer],
				    m_aLastUpdateTick[FromPlayer]) -
			    Client()->GameTick(g_Config.m_ClDummy)) <
			Client()->GameTickSpeed() / 4)
			continue;
		if(m_Snap.m_aCharacters[FromPlayer].m_Prev.m_Direction !=
				m_Snap.m_aCharacters[FromPlayer].m_Cur.m_Direction ||
			m_Snap.m_aCharacters[ToPlayer].m_Prev.m_Direction !=
				m_Snap.m_aCharacters[ToPlayer].m_Cur.m_Direction)
			continue;

		CCharacter *pFromCharWorld = m_GameWorld.GetCharacterById(FromPlayer);
		CCharacter *pToCharWorld = m_GameWorld.GetCharacterById(ToPlayer);
		if(!pFromCharWorld || !pToCharWorld)
			continue;

		m_aLastUpdateTick[ToPlayer] = m_aLastUpdateTick[FromPlayer] =
			Client()->GameTick(g_Config.m_ClDummy);

		float aPredictErr[2];
		CCharacterCore ToCharCur;
		ToCharCur.Read(&m_Snap.m_aCharacters[ToPlayer].m_Cur);

		CWorldCore World;

		for(int Direction = 0; Direction < 2; Direction++)
		{
			CCharacterCore ToChar = pFromCharWorld->GetCore();
			ToChar.Init(&World, Collision(), &m_Teams);
			World.m_apCharacters[ToPlayer] = &ToChar;
			ToChar.Read(&m_Snap.m_aCharacters[ToPlayer].m_Prev);

			CCharacterCore FromChar = pFromCharWorld->GetCore();
			FromChar.Init(&World, Collision(), &m_Teams);
			World.m_apCharacters[FromPlayer] = &FromChar;
			FromChar.Read(&m_Snap.m_aCharacters[FromPlayer].m_Prev);

			for(int Tick = Client()->PrevGameTick(g_Config.m_ClDummy);
				Tick < Client()->GameTick(g_Config.m_ClDummy); Tick++)
			{
				if(Direction == 0)
				{
					FromChar.Tick(false);
					ToChar.Tick(false);
				}
				else
				{
					ToChar.Tick(false);
					FromChar.Tick(false);
				}
				FromChar.Move();
				FromChar.Quantize();
				ToChar.Move();
				ToChar.Quantize();
			}
			aPredictErr[Direction] = distance(ToChar.m_Vel, ToCharCur.m_Vel);
		}
		const float LOW = 0.0001f;
		const float HIGH = 0.07f;
		if(aPredictErr[1] < LOW && aPredictErr[0] > HIGH)
		{
			if(m_CharOrder.HasStrongAgainst(ToPlayer, FromPlayer))
			{
				if(ToPlayer != m_Snap.m_LocalClientId)
					m_CharOrder.GiveWeak(ToPlayer);
				else
					m_CharOrder.GiveStrong(FromPlayer);
			}
		}
		else if(aPredictErr[0] < LOW && aPredictErr[1] > HIGH)
		{
			if(m_CharOrder.HasStrongAgainst(FromPlayer, ToPlayer))
			{
				if(ToPlayer != m_Snap.m_LocalClientId)
					m_CharOrder.GiveStrong(ToPlayer);
				else
					m_CharOrder.GiveWeak(FromPlayer);
			}
		}
	}
}

vec2 CGameClient::GetSmoothPos(int ClientId)
{
	const int FastInputTicks =
		g_Config.m_TcFastInput ? (g_Config.m_TcFastInputAmount + 19) / 20 : 0;
	vec2 Pos = mix(m_aClients[ClientId].m_PrevPredicted.m_Pos,
		m_aClients[ClientId].m_Predicted.m_Pos,
		Client()->PredIntraGameTick(g_Config.m_ClDummy));
	int64_t Now = time_get();
	for(int i = 0; i < 2; i++)
	{
		int64_t Len = std::clamp(m_aClients[ClientId].m_aSmoothLen[i], (int64_t)1,
			time_freq());
		int64_t TimePassed = Now - m_aClients[ClientId].m_aSmoothStart[i];
		if(in_range(TimePassed, (int64_t)0, Len - 1))
		{
			float MixAmount = 1.f - std::pow(1.f - TimePassed / (float)Len, 1.2f);
			int SmoothTick;
			float SmoothIntra;
			Client()->GetSmoothTick(&SmoothTick, &SmoothIntra, MixAmount);

			if(ClientId != m_Snap.m_LocalClientId && g_Config.m_TcFastInputOthers &&
				FastInputTicks > 0)
				SmoothTick += FastInputTicks;

			if(SmoothTick > 0 &&
				m_aClients[ClientId].m_aPredTick[(SmoothTick - 1) % 200] >=
					Client()->PrevGameTick(g_Config.m_ClDummy) &&
				m_aClients[ClientId].m_aPredTick[SmoothTick % 200] <=
					Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks)
				Pos[i] = mix(m_aClients[ClientId].m_aPredPos[(SmoothTick - 1) % 200][i],
					m_aClients[ClientId].m_aPredPos[SmoothTick % 200][i],
					SmoothIntra);
		}
	}
	return Pos;
}
vec2 CGameClient::GetFastInputPos(int ClientId)
{
	float PredIntraTick = Client()->PredIntraGameTick(g_Config.m_ClDummy);
	int PredTick = Client()->PredGameTick(g_Config.m_ClDummy);

	vec2 Pos = mix(m_aClients[ClientId].m_PrevPredicted.m_Pos,
		m_aClients[ClientId].m_Predicted.m_Pos, PredIntraTick);

	float FastInputIntra = (g_Config.m_TcFastInputAmount % 20) / 20.0f;
	int FastInputTicks = g_Config.m_TcFastInputAmount / 20;

	float CombinedIntra = PredIntraTick + FastInputIntra;

	double IntraRemainder = 0.0;
	float FinalIntra = static_cast<float>(std::modf(static_cast<double>(CombinedIntra), &IntraRemainder));
	int CarryOverTicks = static_cast<int>(IntraRemainder);

	FastInputTicks += CarryOverTicks;

	int FinalTick = PredTick + FastInputTicks;

	if(FinalTick > 0 &&
		m_aClients[ClientId].m_aPredTick[(FinalTick - 1) % 200] >=
			Client()->PrevGameTick(g_Config.m_ClDummy) &&
		m_aClients[ClientId].m_aPredTick[FinalTick % 200] <=
			Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks)
	{
		Pos = mix(m_aClients[ClientId].m_aPredPos[(FinalTick - 1) % 200],
			m_aClients[ClientId].m_aPredPos[FinalTick % 200], FinalIntra);
	}

	return Pos;
}
vec2 CGameClient::GetFreezePos(int ClientId)
{
	vec2 Pos = mix(m_aClients[ClientId].m_PrevPredicted.m_Pos,
		m_aClients[ClientId].m_Predicted.m_Pos,
		Client()->PredIntraGameTick(g_Config.m_ClDummy));
	// int64_t Now = time_get();
	CCharacter *pChar = m_PredictedWorld.GetCharacterById(m_Snap.m_LocalClientId);
	CCharacter *pExtraChar =
		m_ExtraPredictedWorld.GetCharacterById(m_Snap.m_LocalClientId);

	// int64_t Len = clamp(m_aClients[ClientId].m_aSmoothLen[i], (int64_t)1,
	// time_freq()); int64_t TimePassed = Now -
	// m_aClients[ClientId].m_aSmoothStart[i];
	float MixAmount = 0.0f;
	int SmoothTick;
	float SmoothIntra;

	int AdjustTicks = 0;
	int DelayTicks = g_Config.m_TcUnfreezeLagDelayTicks;
	int FreezeTime = 0;
	if(pExtraChar && pChar)
	{
		AdjustTicks = pChar->m_FreezeAccumulation;
		if(pExtraChar->m_AliveAccumulation > 0)
			AdjustTicks -= pExtraChar->m_AliveAccumulation;

		AdjustTicks = std::max(AdjustTicks, 0);
		FreezeTime = pChar->m_FreezeTime;

		AdjustTicks = std::min(FreezeTime, AdjustTicks);
	}
	if(g_Config.m_TcRemoveAnti && pChar && AdjustTicks > 0 && FreezeTime > 0)
		MixAmount = mix(0.0f, 1.0f, 1.0f - AdjustTicks / (float)DelayTicks);
	// else if(AdjustTicks == 0 && ClientId != m_Snap.m_LocalClientId)
	//	MixAmount = 1.f - std::pow(1.f - TimePassed / (float)Len, 1.2f);
	else // our tee when not frozen
		MixAmount = 1.f;

	Client()->GetSmoothFreezeTick(&SmoothTick, &SmoothIntra, MixAmount);

	m_SmoothTick = SmoothTick;
	m_SmoothIntraTick = SmoothIntra;

	float FastInputIntra = (g_Config.m_TcFastInputAmount % 20) / 20.0f;
	int FastInputTicks = g_Config.m_TcFastInputAmount / 20;

	float CombinedIntra = SmoothIntra + FastInputIntra;

	double IntraRemainder = 0.0;
	float FinalIntra = static_cast<float>(std::modf(static_cast<double>(CombinedIntra), &IntraRemainder));
	int CarryOverTicks = static_cast<int>(IntraRemainder);

	FastInputTicks += CarryOverTicks;

	const bool IsLocal =
		ClientId == m_Snap.m_LocalClientId ||
		(PredictDummy() && ClientId == m_aLocalIds[Client()->DummyPair()]);
	if(IsLocal && g_Config.m_TcFastInput)
	{
		SmoothTick += FastInputTicks;
		SmoothIntra = FinalIntra;
	}
	else if(!IsLocal && g_Config.m_TcFastInputOthers &&
		g_Config.m_TcFastInput)
	{
		SmoothTick += FastInputTicks;
		SmoothIntra = FinalIntra;
	}

	if(SmoothTick > 0 &&
		m_aClients[ClientId].m_aPredTick[(SmoothTick - 1) % 200] >=
			Client()->PrevGameTick(g_Config.m_ClDummy) &&
		m_aClients[ClientId].m_aPredTick[SmoothTick % 200] <=
			Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks)
	{
		Pos = mix(m_aClients[ClientId].m_aPredPos[(SmoothTick - 1) % 200],
			m_aClients[ClientId].m_aPredPos[SmoothTick % 200], SmoothIntra);
	}

	return Pos;
}

void CGameClient::Echo(const char *pString) { m_Chat.Echo(pString); }

bool CGameClient::IsOtherTeam(int ClientId) const
{
	bool Local = m_Snap.m_LocalClientId == ClientId;

	if(m_Snap.m_LocalClientId < 0)
		return false;
	else if((m_Snap.m_SpecInfo.m_Active &&
			m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW) ||
		ClientId < 0)
		return false;
	else if(m_Snap.m_SpecInfo.m_Active &&
		m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
	{
		if(m_Teams.Team(ClientId) == TEAM_SUPER ||
			m_Teams.Team(m_Snap.m_SpecInfo.m_SpectatorId) == TEAM_SUPER)
			return false;
		return m_Teams.Team(ClientId) !=
		       m_Teams.Team(m_Snap.m_SpecInfo.m_SpectatorId);
	}
	else if((m_aClients[m_Snap.m_LocalClientId].m_Solo ||
			m_aClients[ClientId].m_Solo) &&
		!Local)
		return true;

	if(m_Teams.Team(ClientId) == TEAM_SUPER ||
		m_Teams.Team(m_Snap.m_LocalClientId) == TEAM_SUPER)
		return false;

	return m_Teams.Team(ClientId) != m_Teams.Team(m_Snap.m_LocalClientId);
}

int CGameClient::SwitchStateTeam() const
{
	if(m_aSwitchStateTeam[g_Config.m_ClDummy] >= 0)
		return m_aSwitchStateTeam[g_Config.m_ClDummy];
	else if(m_Snap.m_LocalClientId < 0)
		return 0;
	else if(m_Snap.m_SpecInfo.m_Active &&
		m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		return m_Teams.Team(m_Snap.m_SpecInfo.m_SpectatorId);
	return m_Teams.Team(m_Snap.m_LocalClientId);
}

bool CGameClient::IsLocalCharSuper() const
{
	if(m_Snap.m_LocalClientId < 0)
		return false;
	return m_aClients[m_Snap.m_LocalClientId].m_Super;
}

void CGameClient::LoadGameSkin(const char *pPath, bool AsDir)
{
	if(m_GameSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthEmpty);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorEmpty);

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserCursor);

		for(auto &SpriteWeaponCursor : m_GameSkin.m_aSpriteWeaponCursors)
		{
			SpriteWeaponCursor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookChain);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookHead);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammer);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaser);

		for(auto &SpriteWeapon : m_GameSkin.m_aSpriteWeapons)
		{
			SpriteWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpriteParticle : m_GameSkin.m_aSpriteParticles)
		{
			Graphics()->UnloadTexture(&SpriteParticle);
		}

		for(auto &SpriteStar : m_GameSkin.m_aSpriteStars)
		{
			Graphics()->UnloadTexture(&SpriteStar);
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserProjectile);

		for(auto &SpriteWeaponProjectile : m_GameSkin.m_aSpriteWeaponProjectiles)
		{
			SpriteWeaponProjectile = IGraphics::CTextureHandle();
		}

		for(int i = 0; i < 3; ++i)
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponGunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i]);

			for(auto &SpriteWeaponsMuzzle : m_GameSkin.m_aaSpriteWeaponsMuzzles)
			{
				SpriteWeaponsMuzzle[i] = IGraphics::CTextureHandle();
			}
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHealth);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHammer);

		for(auto &SpritePickupWeapon : m_GameSkin.m_aSpritePickupWeapons)
		{
			SpritePickupWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpritePickupWeaponArmor :
			m_GameSkin.m_aSpritePickupWeaponArmor)
		{
			SpritePickupWeaponArmor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagBlue);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagRed);

		if(m_GameSkin.IsSixup())
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFullLeft);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFull);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmpty);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmptyRight);
		}

		m_GameSkinLoaded = false;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	bool IsDefault = false;
	if(str_comp(pPath, "default") == 0)
	{
		str_copy(aPath, g_pData->m_aImages[IMAGE_GAME].m_pFilename);
		IsDefault = true;
	}
	else
	{
		if(AsDir)
			str_format(aPath, sizeof(aPath), "assets/game/%s/%s", pPath,
				g_pData->m_aImages[IMAGE_GAME].m_pFilename);
		else
			str_format(aPath, sizeof(aPath), "assets/game/%s.png", pPath);
	}

	CImageInfo ImgInfo;
	bool PngLoaded = Graphics()->LoadPng(ImgInfo, aPath, IStorage::TYPE_ALL);
	if(!PngLoaded && !IsDefault)
	{
		if(AsDir)
			LoadGameSkin("default");
		else
			LoadGameSkin(pPath, true);
	}
	else if(PngLoaded &&
		Graphics()->CheckImageDivisibility(
			aPath, ImgInfo,
			g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridx,
			g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridy,
			true) &&
		Graphics()->IsImageFormatRgba(aPath, ImgInfo))
	{
		const SZZThemePalette Palette = GetZZThemePalette(
			g_Config.m_ClZzTheme,
			color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
		const ColorRGBA CursorAccent =
			ZZThemeLerp(Palette.m_Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.18f);
		ZzRecolorGameSkinCursors(ImgInfo, CursorAccent);

		m_GameSkin.m_SpriteHealthFull = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_FULL]);
		m_GameSkin.m_SpriteHealthEmpty = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_EMPTY]);
		m_GameSkin.m_SpriteArmorFull = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_FULL]);
		m_GameSkin.m_SpriteArmorEmpty = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_EMPTY]);

		m_GameSkin.m_SpriteWeaponHammerCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_CURSOR]);
		m_GameSkin.m_SpriteWeaponGunCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponShotgunCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponGrenadeCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_CURSOR]);
		m_GameSkin.m_SpriteWeaponNinjaCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_CURSOR]);
		m_GameSkin.m_SpriteWeaponLaserCursor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_CURSOR]);

		m_GameSkin.m_aSpriteWeaponCursors[0] =
			m_GameSkin.m_SpriteWeaponHammerCursor;
		m_GameSkin.m_aSpriteWeaponCursors[1] = m_GameSkin.m_SpriteWeaponGunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[2] =
			m_GameSkin.m_SpriteWeaponShotgunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[3] =
			m_GameSkin.m_SpriteWeaponGrenadeCursor;
		m_GameSkin.m_aSpriteWeaponCursors[4] = m_GameSkin.m_SpriteWeaponLaserCursor;
		m_GameSkin.m_aSpriteWeaponCursors[5] = m_GameSkin.m_SpriteWeaponNinjaCursor;

		// weapons and hook
		m_GameSkin.m_SpriteHookChain = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_CHAIN]);
		m_GameSkin.m_SpriteHookHead = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_HEAD]);
		m_GameSkin.m_SpriteWeaponHammer = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_BODY]);
		m_GameSkin.m_SpriteWeaponGun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_BODY]);
		m_GameSkin.m_SpriteWeaponShotgun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_BODY]);
		m_GameSkin.m_SpriteWeaponGrenade = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_BODY]);
		m_GameSkin.m_SpriteWeaponNinja = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_BODY]);
		m_GameSkin.m_SpriteWeaponLaser = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_BODY]);

		m_GameSkin.m_aSpriteWeapons[0] = m_GameSkin.m_SpriteWeaponHammer;
		m_GameSkin.m_aSpriteWeapons[1] = m_GameSkin.m_SpriteWeaponGun;
		m_GameSkin.m_aSpriteWeapons[2] = m_GameSkin.m_SpriteWeaponShotgun;
		m_GameSkin.m_aSpriteWeapons[3] = m_GameSkin.m_SpriteWeaponGrenade;
		m_GameSkin.m_aSpriteWeapons[4] = m_GameSkin.m_SpriteWeaponLaser;
		m_GameSkin.m_aSpriteWeapons[5] = m_GameSkin.m_SpriteWeaponNinja;

		// particles
		for(int i = 0; i < 9; ++i)
		{
			m_GameSkin.m_aSpriteParticles[i] = Graphics()->LoadSpriteTexture(
				ImgInfo, &g_pData->m_aSprites[SPRITE_PART1 + i]);
		}

		// stars
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteStars[i] = Graphics()->LoadSpriteTexture(
				ImgInfo, &g_pData->m_aSprites[SPRITE_STAR1 + i]);
		}

		// projectiles
		m_GameSkin.m_SpriteWeaponGunProjectile = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_PROJ]);
		m_GameSkin.m_SpriteWeaponShotgunProjectile = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_PROJ]);
		m_GameSkin.m_SpriteWeaponGrenadeProjectile = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_PROJ]);

		// these weapons have no projectiles
		m_GameSkin.m_SpriteWeaponHammerProjectile = IGraphics::CTextureHandle();
		m_GameSkin.m_SpriteWeaponNinjaProjectile = IGraphics::CTextureHandle();

		m_GameSkin.m_SpriteWeaponLaserProjectile = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_PROJ]);

		m_GameSkin.m_aSpriteWeaponProjectiles[0] =
			m_GameSkin.m_SpriteWeaponHammerProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[1] =
			m_GameSkin.m_SpriteWeaponGunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[2] =
			m_GameSkin.m_SpriteWeaponShotgunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[3] =
			m_GameSkin.m_SpriteWeaponGrenadeProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[4] =
			m_GameSkin.m_SpriteWeaponLaserProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[5] =
			m_GameSkin.m_SpriteWeaponNinjaProjectile;

		// muzzles
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteWeaponGunMuzzles[i] = Graphics()->LoadSpriteTexture(
				ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_MUZZLE1 + i]);
			m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i] =
				Graphics()->LoadSpriteTexture(
					ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_MUZZLE1 + i]);
			m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i] =
				Graphics()->LoadSpriteTexture(
					ImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_MUZZLE1 + i]);

			m_GameSkin.m_aaSpriteWeaponsMuzzles[1][i] =
				m_GameSkin.m_aSpriteWeaponGunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[2][i] =
				m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[5][i] =
				m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i];
		}

		// pickups
		m_GameSkin.m_SpritePickupHealth = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HEALTH]);
		m_GameSkin.m_SpritePickupArmor = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR]);
		m_GameSkin.m_SpritePickupHammer = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HAMMER]);
		m_GameSkin.m_SpritePickupGun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GUN]);
		m_GameSkin.m_SpritePickupShotgun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_SHOTGUN]);
		m_GameSkin.m_SpritePickupGrenade = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GRENADE]);
		m_GameSkin.m_SpritePickupLaser = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_LASER]);
		m_GameSkin.m_SpritePickupNinja = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_NINJA]);
		m_GameSkin.m_SpritePickupArmorShotgun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_SHOTGUN]);
		m_GameSkin.m_SpritePickupArmorGrenade = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_GRENADE]);
		m_GameSkin.m_SpritePickupArmorNinja = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_NINJA]);
		m_GameSkin.m_SpritePickupArmorLaser = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_LASER]);

		m_GameSkin.m_aSpritePickupWeapons[0] = m_GameSkin.m_SpritePickupHammer;
		m_GameSkin.m_aSpritePickupWeapons[1] = m_GameSkin.m_SpritePickupGun;
		m_GameSkin.m_aSpritePickupWeapons[2] = m_GameSkin.m_SpritePickupShotgun;
		m_GameSkin.m_aSpritePickupWeapons[3] = m_GameSkin.m_SpritePickupGrenade;
		m_GameSkin.m_aSpritePickupWeapons[4] = m_GameSkin.m_SpritePickupLaser;
		m_GameSkin.m_aSpritePickupWeapons[5] = m_GameSkin.m_SpritePickupNinja;

		m_GameSkin.m_aSpritePickupWeaponArmor[0] =
			m_GameSkin.m_SpritePickupArmorShotgun;
		m_GameSkin.m_aSpritePickupWeaponArmor[1] =
			m_GameSkin.m_SpritePickupArmorGrenade;
		m_GameSkin.m_aSpritePickupWeaponArmor[2] =
			m_GameSkin.m_SpritePickupArmorNinja;
		m_GameSkin.m_aSpritePickupWeaponArmor[3] =
			m_GameSkin.m_SpritePickupArmorLaser;

		// flags
		m_GameSkin.m_SpriteFlagBlue = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_BLUE]);
		m_GameSkin.m_SpriteFlagRed = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_RED]);

		// ninja bar (0.7)
		if(!Graphics()->IsSpriteTextureFullyTransparent(
			   ImgInfo,
			   &client_data7::g_pData
				   ->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(
				ImgInfo, &client_data7::g_pData
						 ->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(
				ImgInfo, &client_data7::g_pData
						 ->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(
				ImgInfo,
				&client_data7::g_pData
					->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]))
		{
			m_GameSkin.m_SpriteNinjaBarFullLeft = Graphics()->LoadSpriteTexture(
				ImgInfo, &client_data7::g_pData
						 ->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]);
			m_GameSkin.m_SpriteNinjaBarFull = Graphics()->LoadSpriteTexture(
				ImgInfo, &client_data7::g_pData
						 ->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]);
			m_GameSkin.m_SpriteNinjaBarEmpty = Graphics()->LoadSpriteTexture(
				ImgInfo, &client_data7::g_pData
						 ->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]);
			m_GameSkin.m_SpriteNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(
				ImgInfo,
				&client_data7::g_pData
					->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]);
		}

		m_GameSkinLoaded = true;
	}
	ImgInfo.Free();
}

void CGameClient::LoadEmoticonsSkin(const char *pPath, bool AsDir)
{
	if(m_EmoticonsSkinLoaded)
	{
		for(auto &SpriteEmoticon : m_EmoticonsSkin.m_aSpriteEmoticons)
			Graphics()->UnloadTexture(&SpriteEmoticon);

		m_EmoticonsSkinLoaded = false;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	bool IsDefault = false;
	if(str_comp(pPath, "default") == 0)
	{
		str_copy(aPath, g_pData->m_aImages[IMAGE_EMOTICONS].m_pFilename);
		IsDefault = true;
	}
	else
	{
		if(AsDir)
			str_format(aPath, sizeof(aPath), "assets/emoticons/%s/%s", pPath,
				g_pData->m_aImages[IMAGE_EMOTICONS].m_pFilename);
		else
			str_format(aPath, sizeof(aPath), "assets/emoticons/%s.png", pPath);
	}

	CImageInfo ImgInfo;
	bool PngLoaded = Graphics()->LoadPng(ImgInfo, aPath, IStorage::TYPE_ALL);
	if(!PngLoaded && !IsDefault)
	{
		if(AsDir)
			LoadEmoticonsSkin("default");
		else
			LoadEmoticonsSkin(pPath, true);
	}
	else if(PngLoaded &&
		Graphics()->CheckImageDivisibility(
			aPath, ImgInfo,
			g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridx,
			g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridy, true) &&
		Graphics()->IsImageFormatRgba(aPath, ImgInfo))
	{
		for(int i = 0; i < 16; ++i)
			m_EmoticonsSkin.m_aSpriteEmoticons[i] = Graphics()->LoadSpriteTexture(
				ImgInfo, &g_pData->m_aSprites[SPRITE_OOP + i]);

		m_EmoticonsSkinLoaded = true;
	}
	ImgInfo.Free();
}

void CGameClient::LoadParticlesSkin(const char *pPath, bool AsDir)
{
	if(m_ParticlesSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSlice);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleBall);
		for(auto &SpriteParticleSplat : m_ParticlesSkin.m_aSpriteParticleSplat)
			Graphics()->UnloadTexture(&SpriteParticleSplat);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSmoke);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleShell);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleExpl);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleAirJump);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleHit);

		for(auto &SpriteParticle : m_ParticlesSkin.m_aSpriteParticles)
			SpriteParticle = IGraphics::CTextureHandle();

		m_ParticlesSkinLoaded = false;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	bool IsDefault = false;
	if(str_comp(pPath, "default") == 0)
	{
		str_copy(aPath, g_pData->m_aImages[IMAGE_PARTICLES].m_pFilename);
		IsDefault = true;
	}
	else
	{
		if(AsDir)
			str_format(aPath, sizeof(aPath), "assets/particles/%s/%s", pPath,
				g_pData->m_aImages[IMAGE_PARTICLES].m_pFilename);
		else
			str_format(aPath, sizeof(aPath), "assets/particles/%s.png", pPath);
	}

	CImageInfo ImgInfo;
	bool PngLoaded = Graphics()->LoadPng(ImgInfo, aPath, IStorage::TYPE_ALL);
	if(!PngLoaded && !IsDefault)
	{
		if(AsDir)
			LoadParticlesSkin("default");
		else
			LoadParticlesSkin(pPath, true);
	}
	else if(PngLoaded &&
		Graphics()->CheckImageDivisibility(
			aPath, ImgInfo,
			g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridx,
			g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridy,
			true) &&
		Graphics()->IsImageFormatRgba(aPath, ImgInfo))
	{
		m_ParticlesSkin.m_SpriteParticleSlice = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SLICE]);
		m_ParticlesSkin.m_SpriteParticleBall = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_BALL]);
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticleSplat[i] = Graphics()->LoadSpriteTexture(
				ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPLAT01 + i]);
		m_ParticlesSkin.m_SpriteParticleSmoke = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SMOKE]);
		m_ParticlesSkin.m_SpriteParticleShell = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SHELL]);
		m_ParticlesSkin.m_SpriteParticleExpl = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_EXPL01]);
		m_ParticlesSkin.m_SpriteParticleAirJump = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_AIRJUMP]);
		m_ParticlesSkin.m_SpriteParticleHit = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_HIT01]);

		m_ParticlesSkin.m_aSpriteParticles[0] =
			m_ParticlesSkin.m_SpriteParticleSlice;
		m_ParticlesSkin.m_aSpriteParticles[1] =
			m_ParticlesSkin.m_SpriteParticleBall;
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticles[2 + i] =
				m_ParticlesSkin.m_aSpriteParticleSplat[i];
		m_ParticlesSkin.m_aSpriteParticles[5] =
			m_ParticlesSkin.m_SpriteParticleSmoke;
		m_ParticlesSkin.m_aSpriteParticles[6] =
			m_ParticlesSkin.m_SpriteParticleShell;
		m_ParticlesSkin.m_aSpriteParticles[7] =
			m_ParticlesSkin.m_SpriteParticleExpl;
		m_ParticlesSkin.m_aSpriteParticles[8] =
			m_ParticlesSkin.m_SpriteParticleAirJump;
		m_ParticlesSkin.m_aSpriteParticles[9] = m_ParticlesSkin.m_SpriteParticleHit;

		m_ParticlesSkinLoaded = true;
	}
	ImgInfo.Free();
}

void CGameClient::LoadHudSkin(const char *pPath, bool AsDir)
{
	if(m_HudSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjumpEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudSolo);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudCollisionDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessJump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessHook);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudJetpack);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHookHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHammerHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudShotgunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGrenadeHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLaserHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDeepFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLiveFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGrenade);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGun);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportLaser);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudPracticeMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLockMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeam0Mode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyHammer);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyCopy);
		m_HudSkinLoaded = false;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	bool IsDefault = false;
	if(str_comp(pPath, "default") == 0)
	{
		str_copy(aPath, g_pData->m_aImages[IMAGE_HUD].m_pFilename);
		IsDefault = true;
	}
	else
	{
		if(AsDir)
			str_format(aPath, sizeof(aPath), "assets/hud/%s/%s", pPath,
				g_pData->m_aImages[IMAGE_HUD].m_pFilename);
		else
			str_format(aPath, sizeof(aPath), "assets/hud/%s.png", pPath);
	}

	CImageInfo ImgInfo;
	bool PngLoaded = Graphics()->LoadPng(ImgInfo, aPath, IStorage::TYPE_ALL);
	if(!PngLoaded && !IsDefault)
	{
		if(AsDir)
			LoadHudSkin("default");
		else
			LoadHudSkin(pPath, true);
	}
	else if(PngLoaded &&
		Graphics()->CheckImageDivisibility(
			aPath, ImgInfo,
			g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridx,
			g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridy,
			true) &&
		Graphics()->IsImageFormatRgba(aPath, ImgInfo))
	{
		m_HudSkin.m_SpriteHudAirjump = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP]);
		m_HudSkin.m_SpriteHudAirjumpEmpty = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP_EMPTY]);
		m_HudSkin.m_SpriteHudSolo = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SOLO]);
		m_HudSkin.m_SpriteHudCollisionDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_COLLISION_DISABLED]);
		m_HudSkin.m_SpriteHudEndlessJump = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_JUMP]);
		m_HudSkin.m_SpriteHudEndlessHook = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_HOOK]);
		m_HudSkin.m_SpriteHudJetpack = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_JETPACK]);
		m_HudSkin.m_SpriteHudFreezeBarFullLeft = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudFreezeBarFull = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL]);
		m_HudSkin.m_SpriteHudFreezeBarEmpty = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudFreezeBarEmptyRight = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudNinjaBarFullLeft = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudNinjaBarFull = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL]);
		m_HudSkin.m_SpriteHudNinjaBarEmpty = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudHookHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HOOK_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudHammerHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HAMMER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudShotgunHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SHOTGUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGrenadeHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GRENADE_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudLaserHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LASER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGunHitDisabled = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudDeepFrozen = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DEEP_FROZEN]);
		m_HudSkin.m_SpriteHudLiveFrozen = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LIVE_FROZEN]);
		m_HudSkin.m_SpriteHudTeleportGrenade = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GRENADE]);
		m_HudSkin.m_SpriteHudTeleportGun = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GUN]);
		m_HudSkin.m_SpriteHudTeleportLaser = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_LASER]);
		m_HudSkin.m_SpriteHudPracticeMode = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_PRACTICE_MODE]);
		m_HudSkin.m_SpriteHudLockMode = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LOCK_MODE]);
		m_HudSkin.m_SpriteHudTeam0Mode = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TEAM0_MODE]);
		m_HudSkin.m_SpriteHudDummyHammer = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_HAMMER]);
		m_HudSkin.m_SpriteHudDummyCopy = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_COPY]);

		m_HudSkinLoaded = true;
	}
	ImgInfo.Free();
}

void CGameClient::LoadExtrasSkin(const char *pPath, bool AsDir)
{
	if(m_ExtrasSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSnowflake);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSparkle);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpritePulley);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteHectagon);

		for(auto &SpriteParticle : m_ExtrasSkin.m_aSpriteParticles)
			SpriteParticle = IGraphics::CTextureHandle();

		m_ExtrasSkinLoaded = false;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	bool IsDefault = false;
	if(str_comp(pPath, "default") == 0)
	{
		str_copy(aPath, g_pData->m_aImages[IMAGE_EXTRAS].m_pFilename);
		IsDefault = true;
	}
	else
	{
		if(AsDir)
			str_format(aPath, sizeof(aPath), "assets/extras/%s/%s", pPath,
				g_pData->m_aImages[IMAGE_EXTRAS].m_pFilename);
		else
			str_format(aPath, sizeof(aPath), "assets/extras/%s.png", pPath);
	}

	CImageInfo ImgInfo;
	bool PngLoaded = Graphics()->LoadPng(ImgInfo, aPath, IStorage::TYPE_ALL);
	if(!PngLoaded && !IsDefault)
	{
		if(AsDir)
			LoadExtrasSkin("default");
		else
			LoadExtrasSkin(pPath, true);
	}
	else if(PngLoaded &&
		Graphics()->CheckImageDivisibility(
			aPath, ImgInfo,
			g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridx,
			g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridy,
			true) &&
		Graphics()->IsImageFormatRgba(aPath, ImgInfo))
	{
		m_ExtrasSkin.m_SpriteParticleSnowflake = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE]);
		m_ExtrasSkin.m_SpriteParticleSparkle = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPARKLE]);
		m_ExtrasSkin.m_SpritePulley = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_PULLEY]);
		m_ExtrasSkin.m_SpriteHectagon = Graphics()->LoadSpriteTexture(
			ImgInfo, &g_pData->m_aSprites[SPRITE_PART_HECTAGON]);

		m_ExtrasSkin.m_aSpriteParticles[0] = m_ExtrasSkin.m_SpriteParticleSnowflake;
		m_ExtrasSkin.m_aSpriteParticles[1] = m_ExtrasSkin.m_SpriteParticleSparkle;
		m_ExtrasSkin.m_aSpriteParticles[2] = m_ExtrasSkin.m_SpritePulley;
		m_ExtrasSkin.m_aSpriteParticles[3] = m_ExtrasSkin.m_SpriteHectagon;

		m_ExtrasSkinLoaded = true;
	}
	ImgInfo.Free();
}

void CGameClient::RefreshSkin(
	const std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo)
{
	CTeeRenderInfo &TeeInfo = pManagedTeeRenderInfo->TeeRenderInfo();
	const CSkinDescriptor &SkinDescriptor =
		pManagedTeeRenderInfo->SkinDescriptor();

	if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SIX)
	{
		TeeInfo.Apply(m_Skins.Find(SkinDescriptor.m_aSkinName));
	}

	if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SEVEN)
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
		{
			for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
			{
				m_Skins7
					.FindSkinPart(
						Part, SkinDescriptor.m_aSixup[Dummy].m_aaSkinPartNames[Part],
						true)
					->ApplyTo(TeeInfo.m_aSixup[Dummy]);

				if(SkinDescriptor.m_aSixup[Dummy].m_XmasHat)
				{
					TeeInfo.m_aSixup[Dummy].m_HatTexture = m_Skins7.XmasHatTexture();
				}
				else
				{
					TeeInfo.m_aSixup[Dummy].m_HatTexture.Invalidate();
				}

				if(SkinDescriptor.m_aSixup[Dummy].m_BotDecoration)
				{
					TeeInfo.m_aSixup[Dummy].m_BotTexture =
						m_Skins7.BotDecorationTexture();
				}
				else
				{
					TeeInfo.m_aSixup[Dummy].m_BotTexture.Invalidate();
				}
			}
		}
	}

	if(SkinDescriptor.m_Flags != 0 && pManagedTeeRenderInfo->m_RefreshCallback)
	{
		pManagedTeeRenderInfo->m_RefreshCallback();
	}
}

void CGameClient::RefreshSkins(int SkinDescriptorFlags)
{
	dbg_assert(SkinDescriptorFlags != 0, "SkinDescriptorFlags invalid");

	const auto SkinStartLoadTime = time_get_nanoseconds();
	const auto &&ProgressCallback = [&]() {
		// if skin refreshing takes to long, swap to a loading screen
		if(time_get_nanoseconds() - SkinStartLoadTime > 500ms)
		{
			m_Menus.RenderLoading(Localize("Loading skin files"), "", 0);
		}
	};
	if(SkinDescriptorFlags & CSkinDescriptor::FLAG_SIX)
	{
		m_Skins.Refresh(ProgressCallback);
	}
	if(SkinDescriptorFlags & CSkinDescriptor::FLAG_SEVEN)
	{
		m_Skins7.Refresh(ProgressCallback);
	}

	for(std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo :
		m_vpManagedTeeRenderInfos)
	{
		if(!(pManagedTeeRenderInfo->SkinDescriptor().m_Flags &
			   SkinDescriptorFlags))
		{
			continue;
		}
		RefreshSkin(pManagedTeeRenderInfo);
	}
}

void CGameClient::OnSkinUpdate(const char *pSkinName)
{
	// If the refreshed skin's name starts with the current skin prefix, we also
	// have to refresh skins matching the unprefixed skin name, e.g. if
	// "santa_cammo" is refreshed with prefix "santa" we need to refresh both
	// "santa_cammo" and "cammo".
	const char *pSkinPrefix = m_Skins.SkinPrefix();
	const int SkinPrefixLength = str_length(pSkinPrefix);
	char aSkinNameWithoutPrefix[MAX_SKIN_LENGTH];
	if(SkinPrefixLength > 0 &&
		str_comp_num(pSkinName, pSkinPrefix, SkinPrefixLength) == 0 &&
		pSkinName[SkinPrefixLength] == '_' &&
		pSkinName[SkinPrefixLength + 1] != '\0')
	{
		str_copy(aSkinNameWithoutPrefix, &pSkinName[SkinPrefixLength + 1]);
	}
	else
	{
		aSkinNameWithoutPrefix[0] = '\0';
	}
	const auto &&NameMatches = [&](const char *pCheckName) {
		if(str_comp(pCheckName, pSkinName) == 0)
		{
			return true;
		}
		if(aSkinNameWithoutPrefix[0] != '\0' &&
			str_comp(pCheckName, aSkinNameWithoutPrefix) == 0)
		{
			return true;
		}
		return false;
	};

	for(std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo :
		m_vpManagedTeeRenderInfos)
	{
		if(!(pManagedTeeRenderInfo->SkinDescriptor().m_Flags &
			   CSkinDescriptor::FLAG_SIX) ||
			!NameMatches(pManagedTeeRenderInfo->SkinDescriptor().m_aSkinName))
		{
			continue;
		}
		RefreshSkin(pManagedTeeRenderInfo);
	}
}

std::shared_ptr<CManagedTeeRenderInfo>
CGameClient::CreateManagedTeeRenderInfo(const CTeeRenderInfo &TeeRenderInfo,
	const CSkinDescriptor &SkinDescriptor)
{
	std::shared_ptr<CManagedTeeRenderInfo> pManagedTeeRenderInfo =
		std::make_shared<CManagedTeeRenderInfo>(TeeRenderInfo, SkinDescriptor);
	RefreshSkin(pManagedTeeRenderInfo);
	m_vpManagedTeeRenderInfos.emplace_back(pManagedTeeRenderInfo);
	return pManagedTeeRenderInfo;
}

std::shared_ptr<CManagedTeeRenderInfo>
CGameClient::CreateManagedTeeRenderInfo(const CClientData &Client)
{
	return CreateManagedTeeRenderInfo(Client.m_RenderInfo,
		Client.ToSkinDescriptor());
}

void CGameClient::UpdateManagedTeeRenderInfos()
{
	while(!m_vpManagedTeeRenderInfos.empty())
	{
		auto UnusedInfo = std::find_if(
			m_vpManagedTeeRenderInfos.begin(), m_vpManagedTeeRenderInfos.end(),
			[&](const auto &pItem) { return pItem.use_count() <= 1; });
		if(UnusedInfo == m_vpManagedTeeRenderInfos.end())
		{
			break;
		}
		m_vpManagedTeeRenderInfos.erase(UnusedInfo);
	}
}

void CGameClient::CollectManagedTeeRenderInfos(
	const std::function<void(const char *pSkinName)> &ActiveSkinAcceptor)
{
	for(const std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo :
		m_vpManagedTeeRenderInfos)
	{
		if(pManagedTeeRenderInfo->m_SkinDescriptor.m_Flags &
			CSkinDescriptor::FLAG_SIX)
		{
			ActiveSkinAcceptor(pManagedTeeRenderInfo->m_SkinDescriptor.m_aSkinName);
		}
	}
}

void CGameClient::ConchainRefreshSkins(IConsole::IResult *pResult,
	void *pUserData,
	IConsole::FCommandCallback pfnCallback,
	void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pThis->m_Menus.IsInit())
	{
		pThis->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

void CGameClient::ConchainRefreshEventSkins(
	IConsole::IResult *pResult, void *pUserData,
	IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pThis->m_Menus.IsInit())
	{
		pThis->m_Skins.RefreshEventSkins();
		pThis->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

static bool UnknownMapSettingCallback(const char *pCommand, void *pUser)
{
	return true;
}

void CGameClient::LoadMapSettings()
{
	m_MapBugs =
		CMapBugs::Create(Map()->BaseName(), Map()->Size(), Map()->Sha256());

	// Reset Tunezones
	for(int TuneZone = 0; TuneZone < TuneZone::NUM; TuneZone++)
	{
		TuningList()[TuneZone] = CTuningParams::DEFAULT;
		TuningList()[TuneZone].Set("gun_curvature", 0);
		TuningList()[TuneZone].Set("gun_speed", 1400);
		TuningList()[TuneZone].Set("shotgun_curvature", 0);
		TuningList()[TuneZone].Set("shotgun_speed", 500);
		TuningList()[TuneZone].Set("shotgun_speeddiff", 0);
	}
	TuningList()[250] = TuningList()[0];
	TuningList()[250].Set("gun_curvature", 6);

	// Load map tunings
	int Start, Num;
	Map()->GetType(MAPITEMTYPE_INFO, &Start, &Num);
	for(int i = Start; i < Start + Num; i++)
	{
		int ItemId;
		CMapItemInfoSettings *pItem =
			(CMapItemInfoSettings *)Map()->GetItem(i, nullptr, &ItemId);
		int ItemSize = Map()->GetItemSize(i);
		if(!pItem || ItemId != 0)
			continue;

		if(ItemSize < (int)sizeof(CMapItemInfoSettings))
			break;
		if(!(pItem->m_Settings > -1))
			break;

		int Size = Map()->GetDataSize(pItem->m_Settings);
		char *pSettings = (char *)Map()->GetData(pItem->m_Settings);
		char *pNext = pSettings;
		Console()->SetUnknownCommandCallback(UnknownMapSettingCallback, nullptr);
		while(pNext < pSettings + Size)
		{
			int StrSize = str_length(pNext) + 1;
			Console()->ExecuteLine(pNext, IConsole::CLIENT_ID_GAME);
			pNext += StrSize;
		}
		Console()->SetUnknownCommandCallback(IConsole::EmptyUnknownCommandCallback,
			nullptr);
		Map()->UnloadData(pItem->m_Settings);
		break;
	}
}

void CGameClient::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pSelf = (CGameClient *)pUserData;
	const char *pParamName = pResult->GetString(0);
	if(pResult->NumArguments() == 2)
	{
		float NewValue = pResult->GetFloat(1);
		pSelf->TuningList()[0].Set(pParamName, NewValue);
	}
}

void CGameClient::ConTuneZone(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pSelf = (CGameClient *)pUserData;
	int List = pResult->GetInteger(0);
	const char *pParamName = pResult->GetString(1);
	float NewValue = pResult->GetFloat(2);

	if(List >= 0 && List < TuneZone::NUM)
		pSelf->TuningList()[List].Set(pParamName, NewValue);
}

void CGameClient::ConMapbug(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pSelf = (CGameClient *)pUserData;
	const char *pMapBugName = pResult->GetString(0);

	switch(pSelf->m_MapBugs.Update(pMapBugName))
	{
	case EMapBugUpdate::OK:
		break;
	case EMapBugUpdate::OVERRIDDEN:
		log_debug("mapbugs", "map-internal setting overridden by database");
		break;
	case EMapBugUpdate::NOTFOUND:
		log_debug("mapbugs", "unknown map bug '%s', ignoring", pMapBugName);
		break;
	default:
		dbg_assert_failed("unreachable");
	}
}

void CGameClient::ConchainMenuMap(IConsole::IResult *pResult, void *pUserData,
	IConsole::FCommandCallback pfnCallback,
	void *pCallbackUserData)
{
	CGameClient *pSelf = (CGameClient *)pUserData;
	if(pResult->NumArguments())
	{
		if(str_comp(g_Config.m_ClMenuMap, pResult->GetString(0)) != 0)
		{
			str_copy(g_Config.m_ClMenuMap, pResult->GetString(0));
			pSelf->m_MenuBackground.LoadMenuBackground();
		}
	}
	else
		pfnCallback(pResult, pCallbackUserData);
}

void CGameClient::DummyResetInput(int Conn)
{
	if(Conn <= IClient::CONN_MAIN || Conn >= NUM_DUMMIES ||
		!Client()->DummyConnected(Conn))
		return;

	int ReleasedFire = m_Controls.m_aInputData[Conn].m_Fire & INPUT_STATE_MASK;
	if((ReleasedFire & 1) != 0)
		ReleasedFire = (ReleasedFire + 1) & INPUT_STATE_MASK;

	m_Controls.ResetInput(Conn);
	m_Controls.m_aInputData[Conn].m_Hook = 0;
	m_Controls.m_aInputData[Conn].m_Fire = ReleasedFire;

	if(Conn == Client()->DummyPair())
	{
		m_DummyInput = m_Controls.m_aInputData[Conn];
		m_DummyInputConn = Conn;
		m_HammerInput = m_DummyInput;
		m_DummyFire = 0;
	}
}

bool CGameClient::CanDisplayWarning() const
{
	return m_Menus.CanDisplayWarning();
}

CNetObjHandler *CGameClient::GetNetObjHandler() { return &m_NetObjHandler; }

protocol7::CNetObjHandler *CGameClient::GetNetObjHandler7()
{
	return &m_NetObjHandler7;
}

void CGameClient::SnapCollectEntities()
{
	int NumSnapItems = Client()->SnapNumItems(IClient::SNAP_CURRENT);

	std::vector<CSnapEntities> vItemData;
	std::vector<CSnapEntities> vItemEx;

	for(int Index = 0; Index < NumSnapItems; Index++)
	{
		const IClient::CSnapItem Item =
			Client()->SnapGetItem(IClient::SNAP_CURRENT, Index);
		if(Item.m_Type == NETOBJTYPE_ENTITYEX)
			vItemEx.push_back({Item, nullptr});
		else if(Item.m_Type == NETOBJTYPE_PICKUP ||
			Item.m_Type == NETOBJTYPE_DDNETPICKUP ||
			Item.m_Type == NETOBJTYPE_LASER ||
			Item.m_Type == NETOBJTYPE_DDNETLASER ||
			Item.m_Type == NETOBJTYPE_PROJECTILE ||
			Item.m_Type == NETOBJTYPE_DDRACEPROJECTILE ||
			Item.m_Type == NETOBJTYPE_DDNETPROJECTILE)
			vItemData.push_back({Item, nullptr});
	}

	// sort by id
	class CEntComparer
	{
	public:
		bool operator()(const CSnapEntities &Lhs, const CSnapEntities &Rhs) const
		{
			return Lhs.m_Item.m_Id < Rhs.m_Item.m_Id;
		}
	};

	std::sort(vItemData.begin(), vItemData.end(), CEntComparer());
	std::sort(vItemEx.begin(), vItemEx.end(), CEntComparer());

	// merge extended items with items they belong to
	m_vSnapEntities.clear();

	size_t IndexEx = 0;
	for(const CSnapEntities &Ent : vItemData)
	{
		while(IndexEx < vItemEx.size() &&
			vItemEx[IndexEx].m_Item.m_Id < Ent.m_Item.m_Id)
			IndexEx++;

		const CNetObj_EntityEx *pDataEx = nullptr;
		if(IndexEx < vItemEx.size() &&
			vItemEx[IndexEx].m_Item.m_Id == Ent.m_Item.m_Id)
			pDataEx = (const CNetObj_EntityEx *)vItemEx[IndexEx].m_Item.m_pData;

		m_vSnapEntities.push_back({Ent.m_Item, pDataEx});
	}
}

void CGameClient::HandleMultiView()
{
	bool IsTeamZero = IsMultiViewIdSet();
	bool Init = false;
	vec2 MinPos, MaxPos;
	float SumVel = 0.0f;
	int AmountPlayers = 0;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		// look at players who are vanished
		if(m_MultiView.m_aVanish[ClientId])
		{
			// not in freeze anymore and the delay is over
			if(m_MultiView.m_aLastFreeze[ClientId] + 6.0f <= Client()->LocalTime() &&
				m_aClients[ClientId].m_FreezeEnd == 0)
			{
				m_MultiView.m_aVanish[ClientId] = false;
				m_MultiView.m_aLastFreeze[ClientId] = 0.0f;
			}
		}

		// we look at team 0 and the player is not in the spec list
		if(IsTeamZero && !m_aMultiViewId[ClientId])
			continue;

		// player is vanished
		if(m_MultiView.m_aVanish[ClientId])
			continue;

		// the player is not in the team we are spectating
		if(m_Teams.Team(ClientId) != m_MultiViewTeam)
			continue;

		vec2 PlayerPos;
		if(m_Snap.m_aCharacters[ClientId].m_Active)
			PlayerPos = m_aClients[ClientId].m_RenderPos;
		else if(m_aClients[ClientId].m_Spec) // tee is in spec
			PlayerPos = m_aClients[ClientId].m_SpecChar;
		else
			continue;

		// player is far away and frozen
		if(distance(m_MultiView.m_OldPos, PlayerPos) > 1100 &&
			m_aClients[ClientId].m_FreezeEnd != 0)
		{
			// check if the player is frozen for more than 3 seconds, if so vanish
			// them
			if(m_MultiView.m_aLastFreeze[ClientId] == 0.0f)
			{
				m_MultiView.m_aLastFreeze[ClientId] = Client()->LocalTime();
			}
			else if(m_MultiView.m_aLastFreeze[ClientId] + 3.0f <=
				Client()->LocalTime())
			{
				m_MultiView.m_aVanish[ClientId] = true;
				// player we want to be vanished is our "main" tee, so lets switch the
				// tee
				if(ClientId == m_Snap.m_SpecInfo.m_SpectatorId)
					m_Spectator.Spectate(FindFirstMultiViewId());
			}
		}
		else if(m_MultiView.m_aLastFreeze[ClientId] != 0)
		{
			m_MultiView.m_aLastFreeze[ClientId] = 0;
		}

		// set the minimum and maximum position
		if(!Init)
		{
			MinPos = PlayerPos;
			MaxPos = PlayerPos;
			Init = true;
		}
		else
		{
			MinPos.x = std::min(MinPos.x, PlayerPos.x);
			MaxPos.x = std::max(MaxPos.x, PlayerPos.x);
			MinPos.y = std::min(MinPos.y, PlayerPos.y);
			MaxPos.y = std::max(MaxPos.y, PlayerPos.y);
		}

		// sum up the velocity of all players we are spectating
		const CNetObj_Character &CurrentCharacter =
			m_Snap.m_aCharacters[ClientId].m_Cur;
		SumVel += length(vec2(CurrentCharacter.m_VelX / 256.0f,
				  CurrentCharacter.m_VelY / 256.0f)) *
			  50.0f / 32.0f;
		AmountPlayers++;
	}

	// if we have found no players, we disable multi view
	if(AmountPlayers == 0)
	{
		if(m_MultiView.m_SecondChance == 0.0f)
		{
			m_MultiView.m_SecondChance = Client()->LocalTime() + 0.3f;
		}
		else if(m_MultiView.m_SecondChance < Client()->LocalTime())
		{
			ResetMultiView();
			return;
		}
		return;
	}
	else if(m_MultiView.m_SecondChance != 0.0f)
	{
		m_MultiView.m_SecondChance = 0.0f;
	}

	// if we only have one tee that's in the list, we activate solo-mode
	m_MultiView.m_Solo = std::count(std::begin(m_aMultiViewId),
				     std::end(m_aMultiViewId), true) == 1;

	vec2 TargetPos =
		vec2((MinPos.x + MaxPos.x) / 2.0f, (MinPos.y + MaxPos.y) / 2.0f);
	// dont hide the position hud if its only one player
	m_MultiViewShowHud = AmountPlayers == 1;
	// get the average velocity
	float AvgVel =
		std::clamp(SumVel / AmountPlayers ? SumVel / (float)AmountPlayers : 0.0f,
			0.0f, 1000.0f);

	if(m_MultiView.m_OldPersonalZoom == m_MultiViewPersonalZoom)
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel),
			g_Config.m_ClMultiViewZoomSmoothness, false);
	else
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel), 50, false);

	m_Snap.m_SpecInfo.m_Position =
		m_MultiView.m_OldPos + ((TargetPos - m_MultiView.m_OldPos) *
					       CalculateMultiViewMultiplier(TargetPos));
	m_MultiView.m_OldPos = m_Snap.m_SpecInfo.m_Position;
	m_Snap.m_SpecInfo.m_UsePosition = true;
}

bool CGameClient::InitMultiView(int Team)
{
	float Width, Height;
	CleanMultiViewIds();
	m_MultiView.m_IsInit = true;

	// get the current view coordinates
	Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), m_Camera.m_Zoom,
		&Width, &Height);
	vec2 AxisX = vec2(m_Camera.m_Center.x - (Width / 2.0f),
		m_Camera.m_Center.x + (Width / 2.0f));
	vec2 AxisY = vec2(m_Camera.m_Center.y - (Height / 2.0f),
		m_Camera.m_Center.y + (Height / 2.0f));

	if(Team > 0)
	{
		m_MultiViewTeam = Team;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			m_aMultiViewId[ClientId] = m_Teams.Team(ClientId) == Team;
	}
	else
	{
		// we want to allow spectating players in teams directly if there is no
		// other team on screen to do that, -1 is used temporarily for "we don't
		// know which team to spectate yet"
		m_MultiViewTeam = -1;

		int Count = 0;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			vec2 PlayerPos;

			// get the position of the player
			if(m_Snap.m_aCharacters[ClientId].m_Active)
				PlayerPos = vec2(m_Snap.m_aCharacters[ClientId].m_Cur.m_X,
					m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
			else if(m_aClients[ClientId].m_Spec)
				PlayerPos = m_aClients[ClientId].m_SpecChar;
			else
				continue;

			if(PlayerPos.x == 0 || PlayerPos.y == 0)
				continue;

			// skip players that aren't in view
			if(PlayerPos.x <= AxisX.x || PlayerPos.x >= AxisX.y ||
				PlayerPos.y <= AxisY.x || PlayerPos.y >= AxisY.y)
				continue;

			if(m_MultiViewTeam == -1)
			{
				// use the current player's team for now, but it might switch to team 0
				// if any other team is found
				m_MultiViewTeam = m_Teams.Team(ClientId);
			}
			else if(m_MultiViewTeam != 0 &&
				m_Teams.Team(ClientId) != m_MultiViewTeam)
			{
				// mismatched teams; remove all previously added players again and
				// switch to team 0 instead
				std::fill_n(m_aMultiViewId, ClientId, false);
				m_MultiViewTeam = 0;
			}

			m_aMultiViewId[ClientId] = true;
			Count++;
		}

		// might still be -1 if not a single player was in view; fallback to team 0
		// in that case
		if(m_MultiViewTeam == -1)
			m_MultiViewTeam = 0;

		// we are spectating only one player
		m_MultiView.m_Solo = Count == 1;
	}

	if(IsMultiViewIdSet())
	{
		int SpectatorId = m_Snap.m_SpecInfo.m_SpectatorId;
		int NewSpectatorId = -1;

		vec2 CurPosition(m_Camera.m_Center);
		if(SpectatorId != SPEC_FREEVIEW)
		{
			const CNetObj_Character &CurCharacter =
				m_Snap.m_aCharacters[SpectatorId].m_Cur;
			CurPosition.x = CurCharacter.m_X;
			CurPosition.y = CurCharacter.m_Y;
		}

		int ClosestDistance = std::numeric_limits<int>::max();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(!m_Snap.m_apPlayerInfos[ClientId] ||
				m_Snap.m_apPlayerInfos[ClientId]->m_Team == TEAM_SPECTATORS ||
				m_Teams.Team(ClientId) != m_MultiViewTeam)
				continue;

			vec2 PlayerPos;
			if(m_Snap.m_aCharacters[ClientId].m_Active)
				PlayerPos = vec2(m_aClients[ClientId].m_RenderPos.x,
					m_aClients[ClientId].m_RenderPos.y);
			else if(m_aClients[ClientId].m_Spec) // tee is in spec
				PlayerPos = m_aClients[ClientId].m_SpecChar;
			else
				continue;

			int Distance = distance(CurPosition, PlayerPos);
			if(NewSpectatorId == -1 || Distance < ClosestDistance)
			{
				NewSpectatorId = ClientId;
				ClosestDistance = Distance;
			}
		}

		if(NewSpectatorId > -1)
			m_Spectator.Spectate(NewSpectatorId);
	}

	return IsMultiViewIdSet();
}

float CGameClient::CalculateMultiViewMultiplier(vec2 TargetPos)
{
	float MaxCameraDist = 200.0f;
	float MinCameraDist = 20.0f;
	float MaxVel = g_Config.m_ClMultiViewSensitivity / 150.0f;
	float MinVel = 0.007f;
	float CurrentCameraDistance = distance(m_MultiView.m_OldPos, TargetPos);
	float UpperLimit = 1.0f;

	if(m_MultiView.m_Teleported && CurrentCameraDistance <= 100.0f)
		m_MultiView.m_Teleported = false;

	// somebody got teleported very likely
	if((m_MultiView.m_Teleported ||
		   CurrentCameraDistance - m_MultiView.m_OldCameraDistance > 100.0f) &&
		m_MultiView.m_OldCameraDistance != 0.0f)
	{
		UpperLimit = 0.1f; // dont try to compensate it by flickering
		m_MultiView.m_Teleported = true;
	}
	m_MultiView.m_OldCameraDistance = CurrentCameraDistance;

	return std::clamp(MapValue(MaxCameraDist, MinCameraDist, MaxVel, MinVel,
				  CurrentCameraDistance),
		MinVel, UpperLimit);
}

float CGameClient::CalculateMultiViewZoom(vec2 MinPos, vec2 MaxPos, float Vel)
{
	float Ratio = Graphics()->ScreenAspect();
	float ZoomX = 0.0f, ZoomY;

	// only calc two axis if the aspect ratio is not 1:1
	if(Ratio != 1.0f)
		ZoomX = (0.001309f - 0.000328f * Ratio) * (MaxPos.x - MinPos.x) +
			(0.741413f - 0.032959f * Ratio);

	// calculate the according zoom with linear function
	ZoomY = 0.001309f * (MaxPos.y - MinPos.y) + 0.741413f;
	// choose the highest zoom
	float Zoom = std::max(ZoomX, ZoomY);
	// zoom out to maximum 10 percent of the current zoom for 70 velocity
	float Diff = std::clamp(MapValue(70.0f, 15.0f, Zoom * 0.10f, 0.0f, Vel), 0.0f,
		Zoom * 0.10f);
	// zoom should stay between 1.1 and 20.0
	Zoom = std::clamp(Zoom + Diff, 1.1f, 20.0f);
	// dont go below default zoom
	Zoom =
		std::max(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), Zoom);
	// add the user preference
	Zoom -= Zoom * 0.1f * m_MultiViewPersonalZoom;
	m_MultiView.m_OldPersonalZoom = m_MultiViewPersonalZoom;

	return Zoom;
}

float CGameClient::MapValue(float MaxValue, float MinValue, float MaxRange,
	float MinRange, float Value)
{
	return (MaxRange - MinRange) / (MaxValue - MinValue) * (Value - MinValue) +
	       MinRange;
}

void CGameClient::ResetMultiView()
{
	m_Camera.SetZoom(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10),
		g_Config.m_ClSmoothZoomTime, true);
	m_MultiViewPersonalZoom = 0.0f;
	m_MultiViewActivated = false;
	m_MultiView.m_Solo = false;
	m_MultiView.m_IsInit = false;
	m_MultiView.m_Teleported = false;
	m_MultiView.m_OldCameraDistance = 0.0f;
}

void CGameClient::CleanMultiViewIds()
{
	std::fill(std::begin(m_aMultiViewId), std::end(m_aMultiViewId), false);
	std::fill(std::begin(m_MultiView.m_aLastFreeze),
		std::end(m_MultiView.m_aLastFreeze), 0.0f);
	std::fill(std::begin(m_MultiView.m_aVanish), std::end(m_MultiView.m_aVanish),
		false);
}

void CGameClient::CleanMultiViewId(int ClientId)
{
	if(ClientId >= MAX_CLIENTS || ClientId < 0)
		return;

	m_aMultiViewId[ClientId] = false;
	m_MultiView.m_aLastFreeze[ClientId] = 0.0f;
	m_MultiView.m_aVanish[ClientId] = false;
}

bool CGameClient::IsMultiViewIdSet()
{
	return std::any_of(std::begin(m_aMultiViewId), std::end(m_aMultiViewId),
		[](bool IsSet) { return IsSet; });
}

int CGameClient::FindFirstMultiViewId()
{
	int ClientId = -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aMultiViewId[i] && !m_MultiView.m_aVanish[i])
			return i;
	}
	return ClientId;
}

void CGameClient::OnSaveCodeNetMessage(const CNetMsg_Sv_SaveCode *pMsg)
{
	char aBuf[512];
	if(pMsg->m_pError[0] != '\0')
		m_Chat.AddLine(-1, TEAM_ALL, pMsg->m_pError);

	int State = pMsg->m_State;
	if(State == SAVESTATE_PENDING)
	{
		if(pMsg->m_pCode[0] == '\0')
		{
			str_format(
				aBuf, sizeof(aBuf),
				Localize(
					"Team save in progress. You'll be able to load with '/load %s'"),
				pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(
				aBuf, sizeof(aBuf),
				Localize("Team save in progress. You'll be able to load with '/load "
					 "%s' if save is successful or with '/load %s' if it fails"),
				pMsg->m_pCode, pMsg->m_pGeneratedCode);
		}
		m_Chat.AddLine(-1, TEAM_ALL, aBuf);
	}
	else if(State == SAVESTATE_DONE)
	{
		if(pMsg->m_pServerName[0] == '\0')
		{
			str_format(aBuf, sizeof(aBuf),
				"Team successfully saved by %s. Use '/load %s' to continue",
				pMsg->m_pSaveRequester,
				pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(
				aBuf, sizeof(aBuf),
				"Team successfully saved by %s. Use '/load %s' on %s to continue",
				pMsg->m_pSaveRequester,
				pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode,
				pMsg->m_pServerName);
		}
		m_Chat.AddLine(-1, TEAM_ALL, aBuf);
	}
	else if(State == SAVESTATE_FALLBACKFILE)
	{
		if(pMsg->m_pServerName[0] == '\0')
		{
			str_format(
				aBuf, sizeof(aBuf),
				Localize("Team successfully saved by %s. The database connection "
					 "failed, using generated save code instead to avoid "
					 "collisions. Use '/load %s' to continue"),
				pMsg->m_pSaveRequester, pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(
				aBuf, sizeof(aBuf),
				Localize("Team successfully saved by %s. The database connection "
					 "failed, using generated save code instead to avoid "
					 "collisions. Use '/load %s' on %s to continue"),
				pMsg->m_pSaveRequester, pMsg->m_pGeneratedCode, pMsg->m_pServerName);
		}
		m_Chat.AddLine(-1, TEAM_ALL, aBuf);
	}
	else if(State == SAVESTATE_ERROR)
	{
		m_Chat.AddLine(-1, TEAM_ALL, Localize("Save failed!"));
	}

	if(State != SAVESTATE_PENDING && State != SAVESTATE_ERROR &&
		Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		StoreSave(pMsg->m_pTeamMembers,
			pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode);
	}
}

void CGameClient::StoreSave(const char *pTeamMembers,
	const char *pGeneratedCode) const
{
	static constexpr const char *SAVES_HEADER[] = {
		"Time",
		"Players",
		"Map",
		"Code",
	};

	char aTimestamp[20];
	str_timestamp_format(aTimestamp, sizeof(aTimestamp), TimestampFormat::SPACE);

	const bool SavesFileExists =
		Storage()->FileExists(SAVES_FILE, IStorage::TYPE_SAVE);
	IOHANDLE File =
		Storage()->OpenFile(SAVES_FILE, IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("saves", "Failed to open the saves file '%s'", SAVES_FILE);
		return;
	}

	const char *apColumns[std::size(SAVES_HEADER)] = {
		aTimestamp,
		pTeamMembers,
		Map()->BaseName(),
		pGeneratedCode,
	};

	if(!SavesFileExists)
	{
		CsvWrite(File, std::size(SAVES_HEADER), SAVES_HEADER);
	}
	CsvWrite(File, std::size(SAVES_HEADER), apColumns);
	io_close(File);
}

// TClient

bool CGameClient::CheckNewInput() { return m_Controls.CheckNewInput(); }

void CGameClient::SetConnectInfo(const NETADDR *pAddress)
{
	m_ConnectServerInfo = std::nullopt;
	if(!pAddress)
		return;
	const auto *pEntry = ServerBrowser()->Find(*pAddress);
	if(!pEntry)
		return;
	m_ConnectServerInfo = pEntry->m_Info;
	const CNetObj_GameInfoEx GameInfoEx = {.m_Version = 0};
	m_GameInfo = GetGameInfo(&GameInfoEx, 0, &*m_ConnectServerInfo);
}
