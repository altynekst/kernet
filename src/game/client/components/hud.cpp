/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "hud.h"

#include "binds.h"
#include "camera.h"
#include "controls.h"
#include "voting.h"

#include <base/color.h>
#include <base/time.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/tclient/zz_theme.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/layers.h>
#include <game/localization.h>

#include <algorithm>
#include <cmath>
#include <vector>

CHud::CHud()
{
	m_FPSTextContainerIndex.Reset();
	m_DDRaceEffectsTextContainerIndex.Reset();
	m_PlayerAngleTextContainerIndex.Reset();
	m_PlayerPrevAngle = -INFINITY;

	for(int i = 0; i < 2; i++)
	{
		m_aPlayerSpeedTextContainers[i].Reset();
		m_aPlayerPrevSpeed[i] = -INFINITY;
		m_aPlayerPositionContainers[i].Reset();
		m_aPlayerPrevPosition[i] = -INFINITY;
	}
}

void CHud::ResetHudContainers()
{
	for(auto &ScoreInfo : m_aScoreInfo)
	{
		TextRender()->DeleteTextContainer(ScoreInfo.m_OptionalNameTextContainerIndex);
		TextRender()->DeleteTextContainer(ScoreInfo.m_TextRankContainerIndex);
		TextRender()->DeleteTextContainer(ScoreInfo.m_TextScoreContainerIndex);
		Graphics()->DeleteQuadContainer(ScoreInfo.m_RoundRectQuadContainerIndex);

		ScoreInfo.Reset();
	}

	TextRender()->DeleteTextContainer(m_FPSTextContainerIndex);
	TextRender()->DeleteTextContainer(m_DDRaceEffectsTextContainerIndex);
	TextRender()->DeleteTextContainer(m_PlayerAngleTextContainerIndex);
	m_PlayerPrevAngle = -INFINITY;
	for(int i = 0; i < 2; i++)
	{
		TextRender()->DeleteTextContainer(m_aPlayerSpeedTextContainers[i]);
		m_aPlayerPrevSpeed[i] = -INFINITY;
		TextRender()->DeleteTextContainer(m_aPlayerPositionContainers[i]);
		m_aPlayerPrevPosition[i] = -INFINITY;
	}
	Graphics()->DeleteQuadContainer(m_ZzTimerQuadContainerIndex);
	m_ZzTimerQuadContainerIndex = -1;
	m_ZzTimerQuadW = 0.0f;
	Graphics()->DeleteQuadContainer(m_ZzFeatureQuadContainerIndex);
	m_ZzFeatureQuadContainerIndex = -1;
	m_ZzFeatureQuadW = 0.0f;
	m_ZzFeatureQuadH = 0.0f;
}

void CHud::OnWindowResize()
{
	ResetHudContainers();
}

void CHud::OnReset()
{
	m_TimeCpDiff = 0.0f;
	m_DDRaceTime = 0;
	m_FinishTimeLastReceivedTick = 0;
	m_TimeCpLastReceivedTick = 0;
	m_ShowFinishTime = false;
	m_aPlayerRecord[0] = -1.0f;
	m_aPlayerRecord[1] = -1.0f;
	m_aPlayerSpeed[0] = 0;
	m_aPlayerSpeed[1] = 0;
	m_aLastPlayerSpeedChange[0] = ESpeedChange::NONE;
	m_aLastPlayerSpeedChange[1] = ESpeedChange::NONE;
	m_LastSpectatorCountTick = 0;
	m_ZzFovVisibility = 0.0f;
	m_ZzFovPosition = vec2(0.0f, 0.0f);
	m_ZzFovAimAngle = 0.0f;
	m_ZzFovHalfAngle = 0.0f;
	m_ZzFovReach = 0.0f;
	m_ZzFovMotionTime = 0.0f;
	m_ZzFovSmoothingInitialized = false;

	ResetHudContainers();
}

void CHud::OnInit()
{
	OnReset();

	Graphics()->SetColor(1.0, 1.0, 1.0, 1.0);

	m_HudQuadContainerIndex = Graphics()->CreateQuadContainer(false);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	PrepareAmmoHealthAndArmorQuads();

	// all cursors for the different weapons
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteCursor, ScaleX, ScaleY);
		m_aCursorOffset[i] = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 64.f * ScaleX, 64.f * ScaleY);
	}

	// the flags
	m_FlagOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 8.f, 16.f);

	PreparePlayerStateQuads();

	Graphics()->QuadContainerUpload(m_HudQuadContainerIndex);
}

void CHud::RenderGameTimer()
{
	float Half = m_Width / 2.0f;

	if(!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH))
	{
		char aBuf[32];
		int Time = 0;
		if(GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit && (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0))
		{
			Time = GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit * 60 - ((Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed());

			if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
				Time = 0;
		}
		else if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
		{
			// The Warmup timer is negative in this case to make sure that incompatible clients will not see a warmup timer
			Time = (Client()->GameTick(g_Config.m_ClDummy) + GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer) / Client()->GameTickSpeed();
		}
		else
			Time = (Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / Client()->GameTickSpeed();

		str_time((int64_t)Time * 100, ETimeFormat::DAYS, aBuf, sizeof(aBuf));
		float FontSize = 10.0f;
		static float s_TextWidthM = TextRender()->TextWidth(FontSize, "00:00", -1, -1.0f);
		static float s_TextWidthH = TextRender()->TextWidth(FontSize, "00:00:00", -1, -1.0f);
		static float s_TextWidth0D = TextRender()->TextWidth(FontSize, "0d 00:00:00", -1, -1.0f);
		static float s_TextWidth00D = TextRender()->TextWidth(FontSize, "00d 00:00:00", -1, -1.0f);
		static float s_TextWidth000D = TextRender()->TextWidth(FontSize, "000d 00:00:00", -1, -1.0f);
		float w = Time >= 3600 * 24 * 100 ? s_TextWidth000D : (Time >= 3600 * 24 * 10 ? s_TextWidth00D : (Time >= 3600 * 24 ? s_TextWidth0D : (Time >= 3600 ? s_TextWidthH : s_TextWidthM)));
		const float PadX = 10.0f;
		const float PadY = 3.0f;
		const float BoxH = FontSize + PadY * 2.0f;
		const float Corner = minimum(4.0f, BoxH * 0.5f);
		const float Y = 2.0f;

		// Timer + waves indicator (controls moved to ESC -> Game).
		const auto Track = GameClient()->m_MusicSMTC.GetTrackInfo();
		const bool Playing = Track.m_Available && Track.m_Playing;
		const float Gap = 6.0f;
		const float BarW = 3.0f;
		const float BarGap = 2.0f;
		const int Bars = 6;
		const float BarsW = Playing ? (Bars * BarW + (Bars - 1) * BarGap) : 0.0f;
		const float BoxW = w + PadX * 2.0f + (Playing ? (BarsW + Gap) : 0.0f);
		const float X = Half - BoxW * 0.5f;
		Graphics()->TextureClear();
		Graphics()->DrawRect(X, Y, BoxW, BoxH, ColorRGBA(0.08f, 0.10f, 0.12f, 0.80f), IGraphics::CORNER_ALL, Corner);
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		// last 60 sec red, last 10 sec blink
		if(GameClient()->m_Snap.m_pGameInfoObj->m_TimeLimit && Time <= 60 && (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer <= 0))
		{
			float Alpha = Time <= 10 && (2 * time() / time_freq()) % 2 ? 0.5f : 1.0f;
			TextRender()->TextColor(1.0f, 0.25f, 0.25f, Alpha);
		}
		float CursorX = X + PadX;
		if(Playing)
		{
			const float T = (float)time() / (float)time_freq();
			const float BaseY = Y + BoxH * 0.5f;
			for(int i = 0; i < Bars; i++)
			{
				const float PhA = T * 11.0f + i * 0.95f;
				const float PhB = T * 6.0f + i * 1.65f;
				const float S = 0.2f + 0.8f * (0.5f + 0.5f * (0.65f * sinf(PhA) + 0.35f * sinf(PhB)));
				const float H = 2.0f + 9.0f * S;
				Graphics()->DrawRect(CursorX + i * (BarW + BarGap), BaseY - H * 0.5f, BarW, H, ColorRGBA(0.02f, 0.02f, 0.02f, 0.95f), IGraphics::CORNER_ALL, 1.0f);
			}
			CursorX += BarsW + Gap;
		}
		TextRender()->Text(CursorX, Y + PadY, FontSize, aBuf, -1.0f);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
}

void CHud::RenderPauseNotification()
{
	if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED &&
		!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
	{
		const char *pText = Localize("Game paused");
		float FontSize = 20.0f;
		float w = TextRender()->TextWidth(FontSize, pText, -1, -1.0f);
		TextRender()->Text(150.0f * Graphics()->ScreenAspect() + -w / 2.0f, 50.0f, FontSize, pText, -1.0f);
	}
}

void CHud::RenderSuddenDeath()
{
	if(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_SUDDENDEATH)
	{
		float Half = m_Width / 2.0f;
		const char *pText = Localize("Sudden Death");
		float FontSize = 12.0f;
		float w = TextRender()->TextWidth(FontSize, pText, -1, -1.0f);
		TextRender()->Text(Half - w / 2, 2, FontSize, pText, -1.0f);
	}
}

void CHud::RenderScoreHud()
{
	// render small score hud
	if(!(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
	{
		float StartY = 229.0f; // the height of this display is 56, so EndY is 285

		const float ScoreSingleBoxHeight = 18.0f;

		bool ForceScoreInfoInit = !m_aScoreInfo[0].m_Initialized || !m_aScoreInfo[1].m_Initialized;
		m_aScoreInfo[0].m_Initialized = m_aScoreInfo[1].m_Initialized = true;

		if(GameClient()->IsTeamPlay() && GameClient()->m_Snap.m_pGameDataObj)
		{
			char aScoreTeam[2][16];
			str_format(aScoreTeam[TEAM_RED], sizeof(aScoreTeam[TEAM_RED]), "%d", GameClient()->m_Snap.m_pGameDataObj->m_TeamscoreRed);
			str_format(aScoreTeam[TEAM_BLUE], sizeof(aScoreTeam[TEAM_BLUE]), "%d", GameClient()->m_Snap.m_pGameDataObj->m_TeamscoreBlue);

			bool aRecreateTeamScore[2] = {str_comp(aScoreTeam[0], m_aScoreInfo[0].m_aScoreText) != 0, str_comp(aScoreTeam[1], m_aScoreInfo[1].m_aScoreText) != 0};

			const int aFlagCarrier[2] = {
				GameClient()->m_Snap.m_pGameDataObj->m_FlagCarrierRed,
				GameClient()->m_Snap.m_pGameDataObj->m_FlagCarrierBlue};

			bool RecreateRect = ForceScoreInfoInit;
			for(int t = 0; t < 2; t++)
			{
				if(aRecreateTeamScore[t])
				{
					m_aScoreInfo[t].m_ScoreTextWidth = TextRender()->TextWidth(14.0f, aScoreTeam[t == 0 ? TEAM_RED : TEAM_BLUE], -1, -1.0f);
					str_copy(m_aScoreInfo[t].m_aScoreText, aScoreTeam[t == 0 ? TEAM_RED : TEAM_BLUE]);
					RecreateRect = true;
				}
			}

			static float s_TextWidth100 = TextRender()->TextWidth(14.0f, "100", -1, -1.0f);
			float ScoreWidthMax = maximum(maximum(m_aScoreInfo[0].m_ScoreTextWidth, m_aScoreInfo[1].m_ScoreTextWidth), s_TextWidth100);
			float Split = 3.0f;
			float ImageSize = (GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS) ? 16.0f : Split;
			for(int t = 0; t < 2; t++)
			{
				// draw box
				if(RecreateRect)
				{
					Graphics()->DeleteQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex);

					if(t == 0)
						Graphics()->SetColor(0.975f, 0.17f, 0.17f, 0.3f);
					else
						Graphics()->SetColor(0.17f, 0.46f, 0.975f, 0.3f);
					m_aScoreInfo[t].m_RoundRectQuadContainerIndex = Graphics()->CreateRectQuadContainer(m_Width - ScoreWidthMax - ImageSize - 2 * Split, StartY + t * 20, ScoreWidthMax + ImageSize + 2 * Split, ScoreSingleBoxHeight, 5.0f, IGraphics::CORNER_L);
				}
				Graphics()->TextureClear();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
				if(m_aScoreInfo[t].m_RoundRectQuadContainerIndex != -1)
					Graphics()->RenderQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex, -1);

				// draw score
				if(aRecreateTeamScore[t])
				{
					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax + (ScoreWidthMax - m_aScoreInfo[t].m_ScoreTextWidth) / 2 - Split, StartY + t * 20 + (18.f - 14.f) / 2.f));
					Cursor.m_FontSize = 14.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, &Cursor, aScoreTeam[t]);
				}
				if(m_aScoreInfo[t].m_TextScoreContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, TColor, TOutlineColor);
				}

				if(GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS)
				{
					int BlinkTimer = (GameClient()->m_aFlagDropTick[t] != 0 &&
								 (Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_aFlagDropTick[t]) / Client()->GameTickSpeed() >= 25) ?
								 10 :
								 20;
					if(aFlagCarrier[t] == FLAG_ATSTAND || (aFlagCarrier[t] == FLAG_TAKEN && ((Client()->GameTick(g_Config.m_ClDummy) / BlinkTimer) & 1)))
					{
						// draw flag
						Graphics()->TextureSet(t == 0 ? GameClient()->m_GameSkin.m_SpriteFlagRed : GameClient()->m_GameSkin.m_SpriteFlagBlue);
						Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
						Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_FlagOffset, m_Width - ScoreWidthMax - ImageSize, StartY + 1.0f + t * 20);
					}
					else if(aFlagCarrier[t] >= 0)
					{
						// draw name of the flag holder
						int Id = aFlagCarrier[t] % MAX_CLIENTS;
						const char *pName = GameClient()->m_aClients[Id].m_aName;
						if(str_comp(pName, m_aScoreInfo[t].m_aPlayerNameText) != 0 || RecreateRect)
						{
							str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);

							float w = TextRender()->TextWidth(8.0f, pName, -1, -1.0f);

							CTextCursor Cursor;
							Cursor.SetPosition(vec2(minimum(m_Width - w - 1.0f, m_Width - ScoreWidthMax - ImageSize - 2 * Split), StartY + (t + 1) * 20.0f - 2.0f));
							Cursor.m_FontSize = 8.0f;
							TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, &Cursor, pName);
						}

						if(m_aScoreInfo[t].m_OptionalNameTextContainerIndex.Valid())
						{
							ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
							ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
							TextRender()->RenderTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, TColor, TOutlineColor);
						}

						// draw tee of the flag holder
						CTeeRenderInfo TeeInfo = GameClient()->m_aClients[Id].m_RenderInfo;
						TeeInfo.m_Size = ScoreSingleBoxHeight;

						const CAnimState *pIdleState = CAnimState::GetIdle();
						vec2 OffsetToMid;
						CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
						vec2 TeeRenderPos(m_Width - ScoreWidthMax - TeeInfo.m_Size / 2 - Split, StartY + (t * 20) + ScoreSingleBoxHeight / 2.0f + OffsetToMid.y);

						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos);
					}
				}
				StartY += 8.0f;
			}
		}
		else
		{
			int Local = -1;
			int aPos[2] = {1, 2};
			const CNetObj_PlayerInfo *apPlayerInfo[2] = {nullptr, nullptr};
			int i = 0;
			for(int t = 0; t < 2 && i < MAX_CLIENTS && GameClient()->m_Snap.m_apInfoByScore[i]; ++i)
			{
				if(GameClient()->m_Snap.m_apInfoByScore[i]->m_Team != TEAM_SPECTATORS)
				{
					apPlayerInfo[t] = GameClient()->m_Snap.m_apInfoByScore[i];
					if(apPlayerInfo[t]->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
						Local = t;
					++t;
				}
			}
			// search local player info if not a spectator, nor within top2 scores
			if(Local == -1 && GameClient()->m_Snap.m_pLocalInfo && GameClient()->m_Snap.m_pLocalInfo->m_Team != TEAM_SPECTATORS)
			{
				for(; i < MAX_CLIENTS && GameClient()->m_Snap.m_apInfoByScore[i]; ++i)
				{
					if(GameClient()->m_Snap.m_apInfoByScore[i]->m_Team != TEAM_SPECTATORS)
						++aPos[1];
					if(GameClient()->m_Snap.m_apInfoByScore[i]->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
					{
						apPlayerInfo[1] = GameClient()->m_Snap.m_apInfoByScore[i];
						Local = 1;
						break;
					}
				}
			}
			char aScore[2][16];
			for(int t = 0; t < 2; ++t)
			{
				if(apPlayerInfo[t])
				{
					if(Client()->IsSixup() && GameClient()->m_Snap.m_pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE)
						str_time((int64_t)absolute(apPlayerInfo[t]->m_Score) / 10, ETimeFormat::MINS_CENTISECS, aScore[t], sizeof(aScore[t]));
					else if(GameClient()->m_GameInfo.m_TimeScore)
					{
						CGameClient::CClientData &ClientData = GameClient()->m_aClients[apPlayerInfo[t]->m_ClientId];
						if(GameClient()->m_ReceivedDDNetPlayerFinishTimes && ClientData.m_FinishTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
						{
							int64_t TimeSeconds = static_cast<int64_t>(absolute(ClientData.m_FinishTimeSeconds));
							int64_t TimeMillis = TimeSeconds * 1000 + (absolute(ClientData.m_FinishTimeMillis) % 1000);

							str_time(TimeMillis / 10, ETimeFormat::HOURS, aScore[t], sizeof(aScore[t]));
						}
						else if(apPlayerInfo[t]->m_Score != FinishTime::NOT_FINISHED_TIMESCORE)
						{
							str_time((int64_t)absolute(apPlayerInfo[t]->m_Score) * 100, ETimeFormat::HOURS, aScore[t], sizeof(aScore[t]));
						}
						else
							aScore[t][0] = 0;
					}
					else
						str_format(aScore[t], sizeof(aScore[t]), "%d", apPlayerInfo[t]->m_Score);
				}
				else
					aScore[t][0] = 0;
			}

			bool RecreateScores = str_comp(aScore[0], m_aScoreInfo[0].m_aScoreText) != 0 || str_comp(aScore[1], m_aScoreInfo[1].m_aScoreText) != 0 || m_LastLocalClientId != GameClient()->m_Snap.m_LocalClientId;
			m_LastLocalClientId = GameClient()->m_Snap.m_LocalClientId;

			bool RecreateRect = ForceScoreInfoInit;
			for(int t = 0; t < 2; t++)
			{
				if(RecreateScores)
				{
					m_aScoreInfo[t].m_ScoreTextWidth = TextRender()->TextWidth(14.0f, aScore[t], -1, -1.0f);
					str_copy(m_aScoreInfo[t].m_aScoreText, aScore[t]);
					RecreateRect = true;
				}

				if(apPlayerInfo[t])
				{
					int Id = apPlayerInfo[t]->m_ClientId;
					if(Id >= 0 && Id < MAX_CLIENTS)
					{
						const char *pName = GameClient()->m_aClients[Id].m_aName;
						if(str_comp(pName, m_aScoreInfo[t].m_aPlayerNameText) != 0)
							RecreateRect = true;
					}
				}
				else
				{
					if(m_aScoreInfo[t].m_aPlayerNameText[0] != 0)
						RecreateRect = true;
				}

				char aBuf[16];
				str_format(aBuf, sizeof(aBuf), "%d.", aPos[t]);
				if(str_comp(aBuf, m_aScoreInfo[t].m_aRankText) != 0)
					RecreateRect = true;
			}

			static float s_TextWidth10 = TextRender()->TextWidth(14.0f, "10", -1, -1.0f);
			float ScoreWidthMax = maximum(maximum(m_aScoreInfo[0].m_ScoreTextWidth, m_aScoreInfo[1].m_ScoreTextWidth), s_TextWidth10);
			float Split = 3.0f, ImageSize = 16.0f, PosSize = 16.0f;

			for(int t = 0; t < 2; t++)
			{
				// draw box
				if(RecreateRect)
				{
					Graphics()->DeleteQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex);
					Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.40f);
					m_aScoreInfo[t].m_RoundRectQuadContainerIndex = Graphics()->CreateRectQuadContainer(m_Width - ScoreWidthMax - ImageSize - 2 * Split - PosSize, StartY + t * 20, ScoreWidthMax + ImageSize + 2 * Split + PosSize, ScoreSingleBoxHeight, 5.0f, IGraphics::CORNER_ALL);
				}
				Graphics()->TextureClear();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
				if(m_aScoreInfo[t].m_RoundRectQuadContainerIndex != -1)
					Graphics()->RenderQuadContainer(m_aScoreInfo[t].m_RoundRectQuadContainerIndex, -1);

				if(RecreateScores)
				{
					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax + (ScoreWidthMax - m_aScoreInfo[t].m_ScoreTextWidth) - Split, StartY + t * 20 + (18.f - 14.f) / 2.f));
					Cursor.m_FontSize = 14.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, &Cursor, aScore[t]);
				}
				// draw score
				if(m_aScoreInfo[t].m_TextScoreContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextScoreContainerIndex, TColor, TOutlineColor);
				}

				if(apPlayerInfo[t])
				{
					// draw name
					int Id = apPlayerInfo[t]->m_ClientId;
					if(Id >= 0 && Id < MAX_CLIENTS)
					{
						const char *pName = GameClient()->m_aClients[Id].m_aName;
						if(RecreateRect)
						{
							str_copy(m_aScoreInfo[t].m_aPlayerNameText, pName);

							CTextCursor Cursor;
							Cursor.SetPosition(vec2(minimum(m_Width - TextRender()->TextWidth(8.0f, pName) - 1.0f, m_Width - ScoreWidthMax - ImageSize - 2 * Split - PosSize), StartY + (t + 1) * 20.0f - 2.0f));
							Cursor.m_FontSize = 8.0f;
							TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, &Cursor, pName);
						}

						if(m_aScoreInfo[t].m_OptionalNameTextContainerIndex.Valid())
						{
							ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
							ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
							TextRender()->RenderTextContainer(m_aScoreInfo[t].m_OptionalNameTextContainerIndex, TColor, TOutlineColor);
						}

						// draw tee
						CTeeRenderInfo TeeInfo = GameClient()->m_aClients[Id].m_RenderInfo;
						TeeInfo.m_Size = ScoreSingleBoxHeight;

						const CAnimState *pIdleState = CAnimState::GetIdle();
						vec2 OffsetToMid;
						CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
						vec2 TeeRenderPos(m_Width - ScoreWidthMax - TeeInfo.m_Size / 2 - Split, StartY + (t * 20) + ScoreSingleBoxHeight / 2.0f + OffsetToMid.y);

						RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos);
					}
				}
				else
				{
					m_aScoreInfo[t].m_aPlayerNameText[0] = 0;
				}

				// draw position
				char aBuf[16];
				str_format(aBuf, sizeof(aBuf), "%d.", aPos[t]);
				if(RecreateRect)
				{
					str_copy(m_aScoreInfo[t].m_aRankText, aBuf);

					CTextCursor Cursor;
					Cursor.SetPosition(vec2(m_Width - ScoreWidthMax - ImageSize - Split - PosSize, StartY + t * 20 + (18.f - 10.f) / 2.f));
					Cursor.m_FontSize = 10.0f;
					TextRender()->RecreateTextContainer(m_aScoreInfo[t].m_TextRankContainerIndex, &Cursor, aBuf);
				}
				if(m_aScoreInfo[t].m_TextRankContainerIndex.Valid())
				{
					ColorRGBA TColor(1.f, 1.f, 1.f, 1.f);
					ColorRGBA TOutlineColor(0.f, 0.f, 0.f, 0.3f);
					TextRender()->RenderTextContainer(m_aScoreInfo[t].m_TextRankContainerIndex, TColor, TOutlineColor);
				}

				StartY += 8.0f;
			}
		}
	}
}

void CHud::RenderWarmupTimer()
{
	// render warmup timer
	if(GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer > 0 && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME))
	{
		char aBuf[256];
		float FontSize = 20.0f;
		float w = TextRender()->TextWidth(FontSize, Localize("Warmup"), -1, -1.0f);
		TextRender()->Text(150 * Graphics()->ScreenAspect() + -w / 2, 50, FontSize, Localize("Warmup"), -1.0f);

		int Seconds = GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer / Client()->GameTickSpeed();
		if(Seconds < 5)
			str_format(aBuf, sizeof(aBuf), "%d.%d", Seconds, (GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer * 10 / Client()->GameTickSpeed()) % 10);
		else
			str_format(aBuf, sizeof(aBuf), "%d", Seconds);
		w = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
		TextRender()->Text(150 * Graphics()->ScreenAspect() + -w / 2, 75, FontSize, aBuf, -1.0f);
	}
}

void CHud::RenderTextInfo(bool ShowSystemInfo)
{
	int Showfps = ShowSystemInfo ? g_Config.m_ClShowfps : 0;
#if defined(CONF_VIDEORECORDER)
	if(IVideo::Current())
		Showfps = 0;
#endif
	if(Showfps)
	{
		char aBuf[16];
		const int FramesPerSecond = round_to_int(1.0f / Client()->FrameTimeAverage());
		str_format(aBuf, sizeof(aBuf), "%d", FramesPerSecond);

		static float s_TextWidth0 = TextRender()->TextWidth(12.f, "0", -1, -1.0f);
		static float s_TextWidth00 = TextRender()->TextWidth(12.f, "00", -1, -1.0f);
		static float s_TextWidth000 = TextRender()->TextWidth(12.f, "000", -1, -1.0f);
		static float s_TextWidth0000 = TextRender()->TextWidth(12.f, "0000", -1, -1.0f);
		static float s_TextWidth00000 = TextRender()->TextWidth(12.f, "00000", -1, -1.0f);
		static const float s_aTextWidth[5] = {s_TextWidth0, s_TextWidth00, s_TextWidth000, s_TextWidth0000, s_TextWidth00000};

		int DigitIndex = GetDigitsIndex(FramesPerSecond, 4);

		CTextCursor Cursor;
		Cursor.SetPosition(vec2(m_Width - 10 - s_aTextWidth[DigitIndex], 5));
		Cursor.m_FontSize = 12.0f;
		auto OldFlags = TextRender()->GetRenderFlags();
		TextRender()->SetRenderFlags(OldFlags | TEXT_RENDER_FLAG_ONE_TIME_USE);
		if(m_FPSTextContainerIndex.Valid())
			TextRender()->RecreateTextContainerSoft(m_FPSTextContainerIndex, &Cursor, aBuf);
		else
			TextRender()->CreateTextContainer(m_FPSTextContainerIndex, &Cursor, "0");
		TextRender()->SetRenderFlags(OldFlags);
		if(m_FPSTextContainerIndex.Valid())
		{
			TextRender()->RenderTextContainer(m_FPSTextContainerIndex, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor());
		}
	}
	if(ShowSystemInfo && g_Config.m_ClShowpred && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "%d", Client()->GetPredictionTime());
		TextRender()->Text(m_Width - 10 - TextRender()->TextWidth(12, aBuf, -1, -1.0f), Showfps ? 20 : 5, 12, aBuf, -1.0f);
	}

	if(GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		std::vector<const char *> vLines;
		vLines.reserve(24);
		if(g_Config.m_ClZzAimbotEnabled)
			vLines.push_back(g_Config.m_ClZzAimbotSilent ? "Aimbot [silent]" : "Aimbot");
		if(g_Config.m_ClZzHammerAssistEnabled)
			vLines.push_back("Auto-hit");
		if(g_Config.m_ClZzAutoAled)
			vLines.push_back("Auto ALED");
		if(g_Config.m_ClZzFlyHelperEnabled)
			vLines.push_back("Fly");
		if(g_Config.m_ClZzAvoidEnabled)
			vLines.push_back("Avoid");
		if(g_Config.m_ClZzBalanceBot)
			vLines.push_back("Balance");
		if(g_Config.m_ClZzAutoUnfreeze)
			vLines.push_back("Auto unfreeze");
		if(g_Config.m_ClZzFngAutoLaser)
			vLines.push_back("FNG laser");
		if(g_Config.m_ClZzFngTriggerLaser)
			vLines.push_back("FNG trigger");
		if(g_Config.m_ClZzAutoShotgun)
			vLines.push_back("Auto shotgun");
		if(g_Config.m_ClZzHammerGun)
			vLines.push_back("Hammer gun");
		if(g_Config.m_ClZzFentBotEnabled)
			vLines.push_back("Fent bot");
		if(g_Config.m_ClZzCompanionEnabled)
			vLines.push_back("Duo companion");
		if(g_Config.m_ClZzKogAiEnabled)
			vLines.push_back("KoG AI");
		if(g_Config.m_ClKnHookBlocks)
			vLines.push_back("Hook aim");
		if(g_Config.m_ClZzSaveInBlockSilent)
			vLines.push_back("Hook tile [silent]");
		if(g_Config.m_ClZzAvoidEnabled && g_Config.m_ClZzAvoidHookAssist)
			vLines.push_back("Avoid hook assist");
		if(g_Config.m_ClZzFlyRide)
			vLines.push_back("Hook drive");
		if(g_Config.m_ClKnGrenadeSave)
			vLines.push_back("Auto grenade save");
		if(g_Config.m_ClKnWeaponPrediction)
			vLines.push_back("Weapon prediction");
		if(g_Config.m_ClKnWatcherList)
			vLines.push_back("Observers");
		if(g_Config.m_ClZzForceZoom)
			vLines.push_back("Force zoom");
		if(!vLines.empty())
		{
			const float FontSize = 5.2f;
			const float LineH = 6.6f;
			const float PadX = 5.5f;
			const float PadTop = 9.0f;
			float MaxTextW = 0.0f;
			for(const char *pText : vLines)
				MaxTextW = maximum(MaxTextW,
					TextRender()->TextWidth(FontSize, pText, -1, -1.0f));
			const float PanelW = maximum(54.0f, MaxTextW + PadX * 2.0f + 4.0f);
			const float PanelH = PadTop + (float)vLines.size() * LineH + 4.0f;
			const float X = m_Width - PanelW - 8.0f;
			// Keep the enabled-feature list in the upper HUD area. The watcher panel
			// lives near the standard eye counter at the bottom, so the two glass
			// surfaces can no longer cover each other.
			const float PanelY = g_Config.m_ClShowhudScore ? 62.0f : 24.0f;
			float Y = PanelY + PadTop;
			const SZZThemePalette Palette = GetZZThemePalette(
				g_Config.m_ClZzTheme,
				color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
			const ColorRGBA Glass = ZZThemeLerp(
				ColorRGBA(0.34f, 0.36f, 0.39f, 1.0f), Palette.m_Accent, 0.04f);
			Graphics()->TextureClear();
			Graphics()->DrawRect(X, PanelY, PanelW, PanelH,
				Glass.WithAlpha(0.24f), IGraphics::CORNER_ALL, 6.6f);
			Graphics()->DrawRect(X + 5.0f, PanelY + 5.0f, 10.0f, 1.2f,
				Palette.m_Accent.WithAlpha(0.72f), IGraphics::CORNER_ALL, 0.6f);
			TextRender()->TextColor(Palette.m_TextMuted.WithAlpha(0.88f));
			TextRender()->Text(X + 18.0f, PanelY + 2.9f, 4.2f, "ACTIVE", -1.0f);

			for(const char *pText : vLines)
			{
				Graphics()->DrawRect(X + PadX, Y + 2.1f, 2.2f, 2.2f,
					Palette.m_Accent.WithAlpha(0.82f), IGraphics::CORNER_ALL, 1.1f);
				TextRender()->TextColor(Palette.m_Text.WithAlpha(0.96f));
				TextRender()->Text(X + PadX + 4.0f, Y + 0.5f, FontSize, pText, -1.0f);
				Y += LineH;
			}
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}

	if(g_Config.m_TcMiniDebug)
	{
		float FontSize = 8.0f;
		float TextHeight = 11.0f;
		char aBuf[64];
		float OffsetY = 3.0f;

		int PlayerId = GameClient()->m_Snap.m_LocalClientId;
		if(GameClient()->m_Snap.m_SpecInfo.m_Active)
			PlayerId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

		if(g_Config.m_ClShowhudDDRace && GameClient()->m_Snap.m_aCharacters[PlayerId].m_HasExtendedData && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
			OffsetY += 50.0f;
		else if(g_Config.m_ClShowhudHealthAmmo && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
			OffsetY += 27.0f;

		vec2 Pos;
		if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW)
			Pos = vec2(GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy].x, GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy].y);
		else
			Pos = GameClient()->m_aClients[PlayerId].m_RenderPos;

		str_format(aBuf, sizeof(aBuf), "X: %.2f", Pos.x / 32.0f);
		TextRender()->Text(4, OffsetY, FontSize, aBuf, -1.0f);

		OffsetY += TextHeight;
		str_format(aBuf, sizeof(aBuf), "Y: %.2f", Pos.y / 32.0f);
		TextRender()->Text(4, OffsetY, FontSize, aBuf, -1.0f);
		if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		{
			OffsetY += TextHeight;
			str_format(aBuf, sizeof(aBuf), "Angle: %d", GameClient()->m_aClients[PlayerId].m_RenderCur.m_Angle);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);

			OffsetY += TextHeight;
			str_format(aBuf, sizeof(aBuf), "VelY: %.2f", GameClient()->m_Snap.m_aCharacters[PlayerId].m_Cur.m_VelY / 256.0f * 50.0f / 32.0f);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);

			OffsetY += TextHeight;

			str_format(aBuf, sizeof(aBuf), "VelX: %.2f", GameClient()->m_Snap.m_aCharacters[PlayerId].m_Cur.m_VelX / 256.0f * 50.0f / 32.0f);
			TextRender()->Text(4.0f, OffsetY, FontSize, aBuf, -1.0f);
		}
	}
	if(g_Config.m_TcRenderCursorSpec && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW)
	{
		int CurWeapon = 1;
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, g_Config.m_TcRenderCursorSpecAlpha / 100.0f);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponCursors[CurWeapon]);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aCursorOffset[CurWeapon], m_Width / 2.0f, m_Height / 2.0f, 0.36f, 0.36f);
	}
	// render team in freeze text and last notify
	if((g_Config.m_TcShowFrozenText > 0 || g_Config.m_TcShowFrozenHud > 0 || g_Config.m_TcNotifyWhenLast) && GameClient()->m_GameInfo.m_EntitiesDDRace)
	{
		int NumInTeam = 0;
		int NumFrozen = 0;
		int LocalTeamID = 0;
		if(GameClient()->m_Snap.m_LocalClientId >= 0 && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId >= 0)
		{
			if(GameClient()->m_Snap.m_SpecInfo.m_Active == 1 && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != -1)
				LocalTeamID = GameClient()->m_Teams.Team(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId);
			else
				LocalTeamID = GameClient()->m_Teams.Team(GameClient()->m_Snap.m_LocalClientId);
		}
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!GameClient()->m_Snap.m_apPlayerInfos[i])
				continue;

			if(GameClient()->m_Teams.Team(i) == LocalTeamID)
			{
				NumInTeam++;
				if(GameClient()->m_aClients[i].m_FreezeEnd > 0 || GameClient()->m_aClients[i].m_DeepFrozen)
					NumFrozen++;
			}
		}

		// Notify when last
		if(g_Config.m_TcNotifyWhenLast)
		{
			if(NumInTeam > 1 && NumInTeam - NumFrozen == 1)
			{
				TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcNotifyWhenLastColor)));
				float FontSize = g_Config.m_TcNotifyWhenLastSize;
				float XPos = std::clamp((g_Config.m_TcNotifyWhenLastX / 100.0f) * m_Width, 1.0f, m_Width - FontSize);
				float YPos = std::clamp((g_Config.m_TcNotifyWhenLastY / 100.0f) * m_Height, 1.0f, m_Height - FontSize);

				TextRender()->Text(XPos, YPos, FontSize, g_Config.m_TcNotifyWhenLastText, -1.0f);
				TextRender()->TextColor(TextRender()->DefaultTextColor());
			}
		}
		// Show freeze text
		char aBuf[64];
		if(g_Config.m_TcShowFrozenText == 1)
			str_format(aBuf, sizeof(aBuf), "%d / %d", NumInTeam - NumFrozen, NumInTeam);
		else if(g_Config.m_TcShowFrozenText == 2)
			str_format(aBuf, sizeof(aBuf), "%d / %d", NumFrozen, NumInTeam);
		if(g_Config.m_TcShowFrozenText > 0)
			TextRender()->Text(m_Width / 2.0f - TextRender()->TextWidth(10.0f, aBuf) / 2.0f, 12.0f, 10.0f, aBuf);

		// str_format(aBuf, sizeof(aBuf), "%d", GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_PrevPredicted.m_FreezeEnd);
		// str_format(aBuf, sizeof(aBuf), "%d", g_Config.m_ClWhatsMyPing);
		// TextRender()->Text(0, m_Width / 2 - TextRender()->TextWidth(0, 10, aBuf, -1, -1.0f) / 2, 20, 10, aBuf, -1.0f);

		if(g_Config.m_TcShowFrozenHud > 0 && !GameClient()->m_Scoreboard.IsActive() && !(LocalTeamID == 0 && g_Config.m_TcFrozenHudTeamOnly))
		{
			CTeeRenderInfo FreezeInfo;
			const CSkin *pSkin = GameClient()->m_Skins.Find("x_ninja");
			FreezeInfo.m_OriginalRenderSkin = pSkin->m_OriginalSkin;
			FreezeInfo.m_ColorableRenderSkin = pSkin->m_ColorableSkin;
			FreezeInfo.m_BloodColor = pSkin->m_BloodColor;
			FreezeInfo.m_SkinMetrics = pSkin->m_Metrics;
			FreezeInfo.m_ColorBody = ColorRGBA(1.0f, 1.0f, 1.0f);
			FreezeInfo.m_ColorFeet = ColorRGBA(1.0f, 1.0f, 1.0f);
			FreezeInfo.m_CustomColoredSkin = false;

			float ProgressiveOffset = 0.0f;
			float TeeSize = g_Config.m_TcFrozenHudTeeSize;
			int MaxTees = (int)(8.3f * (m_Width / m_Height) * 13.0f / TeeSize);
			if(!g_Config.m_ClShowfps && !g_Config.m_ClShowpred)
				MaxTees = (int)(9.5f * (m_Width / m_Height) * 13.0f / TeeSize);
			int MaxRows = g_Config.m_TcFrozenMaxRows;
			float StartPos = m_Width / 2.0f + 38.0f * (m_Width / m_Height) / 1.78f;

			int TotalRows = std::min(MaxRows, (NumInTeam + MaxTees - 1) / MaxTees);
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.4f);
			Graphics()->DrawRectExt(StartPos - TeeSize / 2.0f, 0.0f, TeeSize * std::min(NumInTeam, MaxTees), TeeSize + 3.0f + (TotalRows - 1) * TeeSize, 5.0f, IGraphics::CORNER_B);
			Graphics()->QuadsEnd();

			bool Overflow = NumInTeam > MaxTees * MaxRows;

			int NumDisplayed = 0;
			int NumInRow = 0;
			int CurrentRow = 0;

			for(int OverflowIndex = 0; OverflowIndex < 1 + Overflow; OverflowIndex++)
			{
				for(int i = 0; i < MAX_CLIENTS && NumDisplayed < MaxTees * MaxRows; i++)
				{
					if(!GameClient()->m_Snap.m_apPlayerInfos[i])
						continue;
					if(GameClient()->m_Teams.Team(i) == LocalTeamID)
					{
						bool Frozen = false;
						CTeeRenderInfo TeeInfo = GameClient()->m_aClients[i].m_RenderInfo;
						if(GameClient()->m_aClients[i].m_FreezeEnd > 0 || GameClient()->m_aClients[i].m_DeepFrozen)
						{
							if(!g_Config.m_TcShowFrozenHudSkins)
								TeeInfo = FreezeInfo;
							Frozen = true;
						}

						if(Overflow && Frozen && OverflowIndex == 0)
							continue;
						if(Overflow && !Frozen && OverflowIndex == 1)
							continue;

						NumDisplayed++;
						NumInRow++;
						if(NumInRow > MaxTees)
						{
							NumInRow = 1;
							ProgressiveOffset = 0.0f;
							CurrentRow++;
						}

						TeeInfo.m_Size = TeeSize;
						const CAnimState *pIdleState = CAnimState::GetIdle();
						vec2 OffsetToMid;
						CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
						vec2 TeeRenderPos(StartPos + ProgressiveOffset, TeeSize * (0.7f) + CurrentRow * TeeSize);
						float Alpha = 1.0f;
						CNetObj_Character CurChar = GameClient()->m_aClients[i].m_RenderCur;
						if(g_Config.m_TcShowFrozenHudSkins && Frozen)
						{
							Alpha = 0.6f;
							TeeInfo.m_ColorBody.r *= 0.4f;
							TeeInfo.m_ColorBody.g *= 0.4f;
							TeeInfo.m_ColorBody.b *= 0.4f;
							TeeInfo.m_ColorFeet.r *= 0.4f;
							TeeInfo.m_ColorFeet.g *= 0.4f;
							TeeInfo.m_ColorFeet.b *= 0.4f;
						}
						if(Frozen)
							RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_PAIN, vec2(1.0f, 0.0f), TeeRenderPos, Alpha);
						else
							RenderTools()->RenderTee(pIdleState, &TeeInfo, CurChar.m_Emote, vec2(1.0f, 0.0f), TeeRenderPos);
						ProgressiveOffset += TeeSize;
					}
				}
			}
		}
	}
}

void CHud::RenderConnectionWarning()
{
	if(Client()->ConnectionProblems())
	{
		const char *pText = Localize("Connection Problems…");
		float w = TextRender()->TextWidth(24, pText, -1, -1.0f);
		TextRender()->Text(150 * Graphics()->ScreenAspect() - w / 2, 50, 24, pText, -1.0f);
	}
}

void CHud::RenderTeambalanceWarning()
{
	// render prompt about team-balance
	bool Flash = time() / (time_freq() / 2) % 2 == 0;
	if(GameClient()->IsTeamPlay())
	{
		int TeamDiff = GameClient()->m_Snap.m_aTeamSize[TEAM_RED] - GameClient()->m_Snap.m_aTeamSize[TEAM_BLUE];
		if(g_Config.m_ClWarningTeambalance && (TeamDiff >= 2 || TeamDiff <= -2))
		{
			const char *pText = Localize("Please balance teams!");
			if(Flash)
				TextRender()->TextColor(1, 1, 0.5f, 1);
			else
				TextRender()->TextColor(0.7f, 0.7f, 0.2f, 1.0f);
			TextRender()->Text(5, 50, 6, pText, -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}
}

void CHud::RenderCursor()
{
	const float Scale = (float)g_Config.m_TcCursorScale / 100.0f;
	if(Scale <= 0.0f)
		return;

	int CurWeapon = 0;
	vec2 TargetPos;
	float Alpha = 1.0f;

	const vec2 Center = GameClient()->m_Camera.m_Center;
	float aPoints[4];
	Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), 1.0f, aPoints);
	Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);

	if(Client()->State() != IClient::STATE_DEMOPLAYBACK && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Render local cursor
		CurWeapon = maximum(0, GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_Predicted.m_ActiveWeapon);
		TargetPos = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	}
	else
	{
		// Render spec cursor
		if(!g_Config.m_ClSpecCursor || !GameClient()->m_CursorInfo.IsAvailable())
			return;

		bool RenderSpecCursor = (GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW) || Client()->State() == IClient::STATE_DEMOPLAYBACK;

		if(!RenderSpecCursor)
			return;

		// Calculate factor to keep cursor on screen
		const vec2 HalfSize = vec2(Center.x - aPoints[0], Center.y - aPoints[1]);
		const vec2 ScreenPos = (GameClient()->m_CursorInfo.WorldTarget() - Center) / GameClient()->m_Camera.m_Zoom;
		const float ClampFactor = maximum(
			1.0f,
			absolute(ScreenPos.x / HalfSize.x),
			absolute(ScreenPos.y / HalfSize.y));

		CurWeapon = maximum(0, GameClient()->m_CursorInfo.Weapon() % NUM_WEAPONS);
		TargetPos = ScreenPos / ClampFactor + Center;
		if(ClampFactor != 1.0f)
			Alpha /= 2.0f;
	}

	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponCursors[CurWeapon]);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aCursorOffset[CurWeapon], TargetPos.x, TargetPos.y, Scale, Scale);
}

void CHud::PrepareAmmoHealthAndArmorQuads()
{
	float x = 5;
	float y = 5;
	IGraphics::CQuadItem Array[10];

	// ammo of the different weapons
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		// 0.6
		for(int n = 0; n < 10; n++)
			Array[n] = IGraphics::CQuadItem(x + n * 12, y, 10, 10);

		m_aAmmoOffset[i] = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

		// 0.7
		if(i == WEAPON_GRENADE)
		{
			// special case for 0.7 grenade
			for(int n = 0; n < 10; n++)
				Array[n] = IGraphics::CQuadItem(1 + x + n * 12, y, 10, 10);
		}
		else
		{
			for(int n = 0; n < 10; n++)
				Array[n] = IGraphics::CQuadItem(x + n * 12, y, 12, 12);
		}

		Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);
	}

	// health
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 10, 10);
	m_HealthOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// empty health
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 10, 10);
	m_EmptyHealthOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// armor meter
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 10, 10);
	m_ArmorOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// empty armor meter
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 10, 10);
	m_EmptyArmorOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// 0.7
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y + 12, 12, 12);
	Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);
}

void CHud::RenderAmmoHealthAndArmor(const CNetObj_Character *pCharacter)
{
	if(!pCharacter)
		return;

	bool IsSixupGameSkin = GameClient()->m_GameSkin.IsSixup();
	int QuadOffsetSixup = (IsSixupGameSkin ? 10 : 0);

	if(GameClient()->m_GameInfo.m_HudAmmo)
	{
		// ammo display
		float AmmoOffsetY = GameClient()->m_GameInfo.m_HudHealthArmor ? 24 : 0;
		int CurWeapon = pCharacter->m_Weapon % NUM_WEAPONS;
		// 0.7 only
		if(CurWeapon == WEAPON_NINJA)
		{
			if(!GameClient()->m_GameInfo.m_HudDDRace && Client()->IsSixup())
			{
				const int Max = g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000;
				float NinjaProgress = std::clamp(pCharacter->m_AmmoCount - Client()->GameTick(g_Config.m_ClDummy), 0, Max) / (float)Max;
				RenderNinjaBarPos(5 + 10 * 12, 5, 6.f, 24.f, NinjaProgress);
			}
		}
		else if(CurWeapon >= 0 && GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon].IsValid())
		{
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[CurWeapon]);
			if(AmmoOffsetY > 0)
			{
				Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_aAmmoOffset[CurWeapon] + QuadOffsetSixup, std::clamp(pCharacter->m_AmmoCount, 0, 10), 0, AmmoOffsetY);
			}
			else
			{
				Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_aAmmoOffset[CurWeapon] + QuadOffsetSixup, std::clamp(pCharacter->m_AmmoCount, 0, 10));
			}
		}
	}

	if(GameClient()->m_GameInfo.m_HudHealthArmor)
	{
		// health display
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHealthFull);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_HealthOffset + QuadOffsetSixup, minimum(pCharacter->m_Health, 10));
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHealthEmpty);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_EmptyHealthOffset + QuadOffsetSixup + minimum(pCharacter->m_Health, 10), 10 - minimum(pCharacter->m_Health, 10));

		// armor display
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteArmorFull);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_ArmorOffset + QuadOffsetSixup, minimum(pCharacter->m_Armor, 10));
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteArmorEmpty);
		Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_ArmorOffset + QuadOffsetSixup + minimum(pCharacter->m_Armor, 10), 10 - minimum(pCharacter->m_Armor, 10));
	}
}

void CHud::PreparePlayerStateQuads()
{
	float x = 5;
	float y = 5 + 24;
	IGraphics::CQuadItem Array[10];

	// Quads for displaying the available and used jumps
	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	m_AirjumpOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	for(int i = 0; i < 10; ++i)
		Array[i] = IGraphics::CQuadItem(x + i * 12, y, 12, 12);
	m_AirjumpEmptyOffset = Graphics()->QuadContainerAddQuads(m_HudQuadContainerIndex, Array, 10);

	// Quads for displaying weapons
	for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
	{
		const CDataWeaponspec &WeaponSpec = g_pData->m_Weapons.m_aId[Weapon];
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(WeaponSpec.m_pSpriteBody, ScaleX, ScaleY);
		constexpr float HudWeaponScale = 0.25f;
		float Width = WeaponSpec.m_VisualSize * ScaleX * HudWeaponScale;
		float Height = WeaponSpec.m_VisualSize * ScaleY * HudWeaponScale;
		m_aWeaponOffset[Weapon] = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, Width, Height);
	}

	// Quads for displaying capabilities
	m_EndlessJumpOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_EndlessHookOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_JetpackOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportGrenadeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportGunOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_TeleportLaserOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying prohibited capabilities
	m_SoloOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_CollisionDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_HookHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_HammerHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_GunHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_ShotgunHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_GrenadeHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LaserHitDisabledOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying freeze status
	m_DeepFrozenOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LiveFrozenOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying dummy actions
	m_DummyHammerOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_DummyCopyOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);

	// Quads for displaying team modes
	m_PracticeModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_LockModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
	m_Team0ModeOffset = Graphics()->QuadContainerAddSprite(m_HudQuadContainerIndex, 0.f, 0.f, 12.f, 12.f);
}

void CHud::RenderPlayerState(const int ClientId)
{
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	// pCharacter contains the predicted character for local players or the last snap for players who are spectated
	CCharacterCore *pCharacter = &GameClient()->m_aClients[ClientId].m_Predicted;
	CNetObj_Character *pPlayer = &GameClient()->m_aClients[ClientId].m_RenderCur;
	int TotalJumpsToDisplay = 0;
	if(g_Config.m_ClShowhudJumpsIndicator)
	{
		int AvailableJumpsToDisplay;
		if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo)
		{
			bool Grounded = false;
			if(Collision()->CheckPoint(pPlayer->m_X + CCharacterCore::PhysicalSize() / 2,
				   pPlayer->m_Y + CCharacterCore::PhysicalSize() / 2 + 5))
			{
				Grounded = true;
			}
			if(Collision()->CheckPoint(pPlayer->m_X - CCharacterCore::PhysicalSize() / 2,
				   pPlayer->m_Y + CCharacterCore::PhysicalSize() / 2 + 5))
			{
				Grounded = true;
			}

			int UsedJumps = pCharacter->m_JumpedTotal;
			if(pCharacter->m_Jumps > 1)
			{
				UsedJumps += !Grounded;
			}
			else if(pCharacter->m_Jumps == 1)
			{
				// If the player has only one jump, each jump is the last one
				UsedJumps = pPlayer->m_Jumped & 2;
			}
			else if(pCharacter->m_Jumps == -1)
			{
				// The player has only one ground jump
				UsedJumps = !Grounded;
			}

			if(pCharacter->m_EndlessJump && UsedJumps >= absolute(pCharacter->m_Jumps))
			{
				UsedJumps = absolute(pCharacter->m_Jumps) - 1;
			}

			int UnusedJumps = absolute(pCharacter->m_Jumps) - UsedJumps;
			if(!(pPlayer->m_Jumped & 2) && UnusedJumps <= 0)
			{
				// In some edge cases when the player just got another number of jumps, UnusedJumps is not correct
				UnusedJumps = 1;
			}
			TotalJumpsToDisplay = maximum(minimum(absolute(pCharacter->m_Jumps), 10), 0);
			AvailableJumpsToDisplay = maximum(minimum(UnusedJumps, TotalJumpsToDisplay), 0);
		}
		else
		{
			TotalJumpsToDisplay = AvailableJumpsToDisplay = absolute(GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Jumps);
		}

		// render available and used jumps
		int JumpsOffsetY = ((GameClient()->m_GameInfo.m_HudHealthArmor && g_Config.m_ClShowhudHealthAmmo ? 24 : 0) +
				    (GameClient()->m_GameInfo.m_HudAmmo && g_Config.m_ClShowhudHealthAmmo ? 12 : 0));
		if(JumpsOffsetY > 0)
		{
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjump);
			Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_AirjumpOffset, AvailableJumpsToDisplay, 0, JumpsOffsetY);
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjumpEmpty);
			Graphics()->RenderQuadContainerEx(m_HudQuadContainerIndex, m_AirjumpEmptyOffset + AvailableJumpsToDisplay, TotalJumpsToDisplay - AvailableJumpsToDisplay, 0, JumpsOffsetY);
		}
		else
		{
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjump);
			Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_AirjumpOffset, AvailableJumpsToDisplay);
			Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudAirjumpEmpty);
			Graphics()->RenderQuadContainer(m_HudQuadContainerIndex, m_AirjumpEmptyOffset + AvailableJumpsToDisplay, TotalJumpsToDisplay - AvailableJumpsToDisplay);
		}
	}

	float x = 5 + 12;
	float y = (5 + 12 + (GameClient()->m_GameInfo.m_HudHealthArmor && g_Config.m_ClShowhudHealthAmmo ? 24 : 0) +
		   (GameClient()->m_GameInfo.m_HudAmmo && g_Config.m_ClShowhudHealthAmmo ? 12 : 0));

	// render weapons
	{
		constexpr float aWeaponWidth[NUM_WEAPONS] = {16, 12, 12, 12, 12, 12};
		constexpr float aWeaponInitialOffset[NUM_WEAPONS] = {-3, -4, -1, -1, -2, -4};
		bool InitialOffsetAdded = false;
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
		{
			if(!pCharacter->m_aWeapons[Weapon].m_Got)
				continue;
			if(!InitialOffsetAdded)
			{
				x += aWeaponInitialOffset[Weapon];
				InitialOffsetAdded = true;
			}
			if(pPlayer->m_Weapon != Weapon)
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
			Graphics()->QuadsSetRotation(pi * 7 / 4);
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpritePickupWeapons[Weapon]);
			Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_aWeaponOffset[Weapon], x, y);
			Graphics()->QuadsSetRotation(0);
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			x += aWeaponWidth[Weapon];
		}
		if(pCharacter->m_aWeapons[WEAPON_NINJA].m_Got)
		{
			const int Max = g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000;
			float NinjaProgress = std::clamp(pCharacter->m_Ninja.m_ActivationTick + g_pData->m_Weapons.m_Ninja.m_Duration * Client()->GameTickSpeed() / 1000 - Client()->GameTick(g_Config.m_ClDummy), 0, Max) / (float)Max;
			if(NinjaProgress > 0.0f && GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo)
			{
				RenderNinjaBarPos(x, y - 12, 6.f, 24.f, NinjaProgress);
			}
		}
	}

	// render capabilities
	x = 5;
	y += 12;
	if(TotalJumpsToDisplay > 0)
	{
		y += 12;
	}
	bool HasCapabilities = false;
	if(pCharacter->m_EndlessJump)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudEndlessJump);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_EndlessJumpOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_EndlessHook)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudEndlessHook);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_EndlessHookOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_Jetpack)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudJetpack);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_JetpackOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunGun && pCharacter->m_aWeapons[WEAPON_GUN].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportGun);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportGunOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunGrenade && pCharacter->m_aWeapons[WEAPON_GRENADE].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportGrenade);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportGrenadeOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HasTelegunLaser && pCharacter->m_aWeapons[WEAPON_LASER].m_Got)
	{
		HasCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeleportLaser);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_TeleportLaserOffset, x, y);
	}

	// render prohibited capabilities
	x = 5;
	if(HasCapabilities)
	{
		y += 12;
	}
	bool HasProhibitedCapabilities = false;
	if(pCharacter->m_Solo)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudSolo);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_SoloOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_CollisionDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudCollisionDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_CollisionDisabledOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HookHitDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudHookHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_HookHitDisabledOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_HammerHitDisabled)
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudHammerHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_HammerHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_GrenadeHitDisabled && pCharacter->m_HasTelegunGun && pCharacter->m_aWeapons[WEAPON_GUN].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudGunHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LaserHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_ShotgunHitDisabled && pCharacter->m_aWeapons[WEAPON_SHOTGUN].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudShotgunHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_ShotgunHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_GrenadeHitDisabled && pCharacter->m_aWeapons[WEAPON_GRENADE].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudGrenadeHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_GrenadeHitDisabledOffset, x, y);
		x += 12;
	}
	if((pCharacter->m_LaserHitDisabled && pCharacter->m_aWeapons[WEAPON_LASER].m_Got))
	{
		HasProhibitedCapabilities = true;
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLaserHitDisabled);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LaserHitDisabledOffset, x, y);
	}

	// render dummy actions and freeze state
	x = 5;
	if(HasProhibitedCapabilities)
	{
		y += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_LOCK_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLockMode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LockModeOffset, x, y);
		x += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_PRACTICE_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudPracticeMode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_PracticeModeOffset, x, y);
		x += 12;
	}
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_TEAM0_MODE)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudTeam0Mode);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_Team0ModeOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_DeepFrozen)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDeepFrozen);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DeepFrozenOffset, x, y);
		x += 12;
	}
	if(pCharacter->m_LiveFrozen)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudLiveFrozen);
		Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_LiveFrozenOffset, x, y);
	}
}

void CHud::RenderNinjaBarPos(const float x, float y, const float Width, const float Height, float Progress, const float Alpha)
{
	Progress = std::clamp(Progress, 0.0f, 1.0f);

	// what percentage of the end pieces is used for the progress indicator and how much is the rest
	// half of the ends are used for the progress display
	const float RestPct = 0.5f;
	const float ProgPct = 0.5f;

	const float EndHeight = Width; // to keep the correct scale - the width of the sprite is as long as the height
	const float BarWidth = Width;
	const float WholeBarHeight = Height;
	const float MiddleBarHeight = WholeBarHeight - (EndHeight * 2.0f);
	const float EndProgressHeight = EndHeight * ProgPct;
	const float EndRestHeight = EndHeight * RestPct;
	const float ProgressBarHeight = WholeBarHeight - (EndProgressHeight * 2.0f);
	const float EndProgressProportion = EndProgressHeight / ProgressBarHeight;
	const float MiddleProgressProportion = MiddleBarHeight / ProgressBarHeight;

	// beginning piece
	float BeginningPieceProgress = 1;
	if(Progress <= 1)
	{
		if(Progress <= (EndProgressProportion + MiddleProgressProportion))
		{
			BeginningPieceProgress = 0;
		}
		else
		{
			BeginningPieceProgress = (Progress - EndProgressProportion - MiddleProgressProportion) / EndProgressProportion;
		}
	}
	// empty
	Graphics()->WrapClamp();
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// Subset: btm_r, top_r, top_m, btm_m | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
	Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, ProgPct - ProgPct * (1.0f - BeginningPieceProgress), 0, ProgPct - ProgPct * (1.0f - BeginningPieceProgress), 1);
	IGraphics::CQuadItem QuadEmptyBeginning(x, y, BarWidth, EndRestHeight + EndProgressHeight * (1.0f - BeginningPieceProgress));
	Graphics()->QuadsDrawTL(&QuadEmptyBeginning, 1);
	Graphics()->QuadsEnd();
	// full
	if(BeginningPieceProgress > 0.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFullLeft);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// Subset: btm_m, top_m, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(RestPct + ProgPct * (1.0f - BeginningPieceProgress), 1, RestPct + ProgPct * (1.0f - BeginningPieceProgress), 0, 1, 0, 1, 1);
		IGraphics::CQuadItem QuadFullBeginning(x, y + (EndRestHeight + EndProgressHeight * (1.0f - BeginningPieceProgress)), BarWidth, EndProgressHeight * BeginningPieceProgress);
		Graphics()->QuadsDrawTL(&QuadFullBeginning, 1);
		Graphics()->QuadsEnd();
	}

	// middle piece
	y += EndHeight;

	float MiddlePieceProgress = 1;
	if(Progress <= EndProgressProportion + MiddleProgressProportion)
	{
		if(Progress <= EndProgressProportion)
		{
			MiddlePieceProgress = 0;
		}
		else
		{
			MiddlePieceProgress = (Progress - EndProgressProportion) / MiddleProgressProportion;
		}
	}

	const float FullMiddleBarHeight = MiddleBarHeight * MiddlePieceProgress;
	const float EmptyMiddleBarHeight = MiddleBarHeight - FullMiddleBarHeight;

	// empty ninja bar
	if(EmptyMiddleBarHeight > 0.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmpty);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// select the middle portion of the sprite so we don't get edge bleeding
		if(EmptyMiddleBarHeight <= EndHeight)
		{
			// prevent pixel puree, select only a small slice
			// Subset: btm_r, top_r, top_m, btm_m | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
			Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, 1.0f - (EmptyMiddleBarHeight / EndHeight), 0, 1.0f - (EmptyMiddleBarHeight / EndHeight), 1);
		}
		else
		{
			// Subset: btm_r, top_r, top_l, btm_l | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
			Graphics()->QuadsSetSubsetFree(1, 1, 1, 0, 0, 0, 0, 1);
		}
		IGraphics::CQuadItem QuadEmpty(x, y, BarWidth, EmptyMiddleBarHeight);
		Graphics()->QuadsDrawTL(&QuadEmpty, 1);
		Graphics()->QuadsEnd();
	}

	// full ninja bar
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFull);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// select the middle portion of the sprite so we don't get edge bleeding
	if(FullMiddleBarHeight <= EndHeight)
	{
		// prevent pixel puree, select only a small slice
		// Subset: btm_m, top_m, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(1.0f - (FullMiddleBarHeight / EndHeight), 1, 1.0f - (FullMiddleBarHeight / EndHeight), 0, 1, 0, 1, 1);
	}
	else
	{
		// Subset: btm_l, top_l, top_r, btm_r | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(0, 1, 0, 0, 1, 0, 1, 1);
	}
	IGraphics::CQuadItem QuadFull(x, y + EmptyMiddleBarHeight, BarWidth, FullMiddleBarHeight);
	Graphics()->QuadsDrawTL(&QuadFull, 1);
	Graphics()->QuadsEnd();

	// ending piece
	y += MiddleBarHeight;
	float EndingPieceProgress = 1;
	if(Progress <= EndProgressProportion)
	{
		EndingPieceProgress = Progress / EndProgressProportion;
	}
	// empty
	if(EndingPieceProgress < 1.0f)
	{
		Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
		// Subset: btm_l, top_l, top_m, btm_m | it is rotated 90 degrees clockwise
		Graphics()->QuadsSetSubsetFree(0, 1, 0, 0, ProgPct - ProgPct * EndingPieceProgress, 0, ProgPct - ProgPct * EndingPieceProgress, 1);
		IGraphics::CQuadItem QuadEmptyEnding(x, y, BarWidth, EndProgressHeight * (1.0f - EndingPieceProgress));
		Graphics()->QuadsDrawTL(&QuadEmptyEnding, 1);
		Graphics()->QuadsEnd();
	}
	// full
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudNinjaBarFullLeft);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.f, 1.f, 1.f, Alpha);
	// Subset: btm_m, top_m, top_l, btm_l | it is mirrored on the horizontal axe and rotated 90 degrees counterclockwise
	Graphics()->QuadsSetSubsetFree(RestPct + ProgPct * EndingPieceProgress, 1, RestPct + ProgPct * EndingPieceProgress, 0, 0, 0, 0, 1);
	IGraphics::CQuadItem QuadFullEnding(x, y + (EndProgressHeight * (1.0f - EndingPieceProgress)), BarWidth, EndRestHeight + EndProgressHeight * EndingPieceProgress);
	Graphics()->QuadsDrawTL(&QuadFullEnding, 1);
	Graphics()->QuadsEnd();

	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
	Graphics()->WrapNormal();
}

void CHud::RenderSpectatorCount()
{
	const bool ShowStandardCount = g_Config.m_ClShowhud && g_Config.m_ClShowhudSpectatorCount;
	const bool ShowWatcherList = g_Config.m_ClKnWatcherList;
	if(!ShowStandardCount && !ShowWatcherList)
	{
		return;
	}

	struct SWatcher
	{
		int m_ClientId;
		float m_Distance;
		int m_Confidence;
	};
	std::vector<SWatcher> vConfirmedWatchers;
	std::vector<SWatcher> vWatcherCandidates;
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;

	// DDNet sends the exact amount of direct and free-view spectators in the
	// SpectatorCount extension, including on sixup servers. Vanilla 0.7 instead
	// marks every player who follows us with PLAYERFLAG_WATCHING.
	const CNetObj_SpectatorCount *pSpectatorCount = GameClient()->m_Snap.m_pSpectatorCount;
	int ReportedCount = pSpectatorCount ? pSpectatorCount->m_NumSpectators : -1;

	if(ShowWatcherList && LocalId >= 0 && LocalId < MAX_CLIENTS)
	{
		const vec2 LocalPos = GameClient()->m_aClients[LocalId].m_RenderPos;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(GameClient()->IsLocalClientId(ClientId) ||
				GameClient()->m_aClients[ClientId].m_aName[0] == '\0')
				continue;
			const auto &ClientData = GameClient()->m_aClients[ClientId];
			const bool WatchingFlag = Client()->IsSixup() &&
						  (Client()->m_TranslationContext.m_aClients[ClientId].m_PlayerFlags7 &
							  protocol7::PLAYERFLAG_WATCHING) != 0;
			if(WatchingFlag)
			{
				vConfirmedWatchers.push_back({ClientId, 0.0f, 100});
				continue;
			}

			const CNetObj_PlayerInfo *pPlayerInfo = GameClient()->m_Snap.m_apPlayerInfos[ClientId];
			const bool TeamSpectator = pPlayerInfo && pPlayerInfo->m_Team == TEAM_SPECTATORS;
			const bool SpectatorCandidate = ClientData.m_Spec || ClientData.m_Paused || TeamSpectator;
			if(!SpectatorCandidate || ClientData.m_Afk)
				continue;

			// SpecChar is the paused tee position on DDNet, not the spectator camera.
			// It is only useful as a stable tie breaker when the protocol gives us an
			// exact count but does not expose the spectator target or camera.
			const float DistanceHint = ClientData.m_SpecCharPresent ?
							   distance(ClientData.m_SpecChar, LocalPos) :
							   1e18f;
			int Confidence = 0;
			if(ClientData.m_Spec)
				Confidence += 4;
			if(TeamSpectator)
				Confidence += 2;
			if(ClientData.m_Paused)
				Confidence += 1;
			vWatcherCandidates.push_back({ClientId, DistanceHint, Confidence});
		}
		std::sort(vWatcherCandidates.begin(), vWatcherCandidates.end(), [](const SWatcher &A, const SWatcher &B) {
			if(A.m_Confidence != B.m_Confidence)
				return A.m_Confidence > B.m_Confidence;
			return A.m_Distance < B.m_Distance;
		});
	}

	std::vector<SWatcher> vWatchers = vConfirmedWatchers;
	if(ShowWatcherList)
	{
		const int MissingNames = ReportedCount >= 0 ?
						 maximum(0, ReportedCount - (int)vWatchers.size()) :
						 (int)vWatcherCandidates.size();
		for(int Index = 0; Index < MissingNames && Index < (int)vWatcherCandidates.size(); ++Index)
			vWatchers.push_back(vWatcherCandidates[Index]);
	}

	int Count = ReportedCount;
	if(Count < 0)
	{
		Count = (int)vConfirmedWatchers.size();
		if(Count == 0 && !Client()->IsSixup())
			Count = (int)vWatchers.size();
	}
	Count = maximum(Count, (int)vWatchers.size());

	if(Count == 0 && vWatchers.empty())
	{
		m_LastSpectatorCountTick = Client()->GameTick(g_Config.m_ClDummy);
		return;
	}

	// Keep the delay only for the standard eye counter. The custom list must
	// appear immediately as soon as a watcher identity becomes available.
	if(ShowStandardCount && vWatchers.empty() &&
		Client()->GameTick(g_Config.m_ClDummy) <
			m_LastSpectatorCountTick + Client()->GameTickSpeed())
		return;

	char aBuf[16];
	str_format(aBuf, sizeof(aBuf), "%d", Count);

	const float Fontsize = 6.0f;
	const float BoxHeight = 14.f;
	const float BoxWidth = 13.f + TextRender()->TextWidth(Fontsize, aBuf);

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		StartY -= 4;
	}
	StartY -= GetMovementInformationBoxHeight();

	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56;
	}

	if(g_Config.m_ClShowhudDummyActions && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) && Client()->DummyConnected())
	{
		StartY = StartY - 29.0f - 4; // dummy actions height and padding
	}

	if(ShowStandardCount)
	{
		Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

		const float y = StartY + BoxHeight / 3;
		const float x = StartX + 2;

		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->Text(x, y, Fontsize, FontIcon::EYE, -1.0f);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		TextRender()->Text(x + Fontsize + 3.f, y, Fontsize, aBuf, -1.0f);
	}

	if(!ShowWatcherList || vWatchers.empty())
		return;

	const int Visible = minimum((int)vWatchers.size(), 8);
	const float RowHeight = 8.0f;
	float PanelWidth = 34.0f;
	for(int Index = 0; Index < Visible; ++Index)
		PanelWidth = maximum(PanelWidth,
			TextRender()->TextWidth(5.5f,
				GameClient()->m_aClients[vWatchers[Index].m_ClientId].m_aName) +
				9.0f);
	PanelWidth = minimum(PanelWidth, 105.0f);
	const float PanelHeight = RowHeight * Visible + 4.0f;
	const float PanelX = ShowStandardCount ? StartX - PanelWidth - 2.0f : m_Width - PanelWidth - 2.0f;
	const float PanelY = StartY;
	const SZZThemePalette Theme = GetZZThemePalette(g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	const ColorRGBA WatcherGlass = ZZThemeLerp(
		ColorRGBA(0.30f, 0.32f, 0.36f, 1.0f), Theme.m_Panel, 0.58f);
	// The watcher list uses the same rounded, translucent material as ClickGUI.
	// Symmetric shells provide a colored rim without an offset drop shadow.
	Graphics()->DrawRect(PanelX - 2.0f, PanelY - 2.0f,
		PanelWidth + 4.0f, PanelHeight + 4.0f,
		Theme.m_Accent.WithAlpha(0.055f), IGraphics::CORNER_ALL, 8.5f);
	Graphics()->DrawRect(PanelX - 0.7f, PanelY - 0.7f,
		PanelWidth + 1.4f, PanelHeight + 1.4f,
		ZZThemeLerp(ColorRGBA(0.96f, 0.98f, 1.0f, 1.0f), Theme.m_Accent, 0.58f)
			.WithAlpha(0.34f),
		IGraphics::CORNER_ALL, 7.2f);
	Graphics()->DrawRect(PanelX, PanelY, PanelWidth, PanelHeight,
		WatcherGlass.WithAlpha(0.38f), IGraphics::CORNER_ALL, 6.6f);
	Graphics()->DrawRect(PanelX + 1.0f, PanelY + 1.0f,
		PanelWidth - 2.0f, PanelHeight - 2.0f,
		Theme.m_AccentAlt.WithAlpha(0.035f), IGraphics::CORNER_ALL, 5.8f);
	Graphics()->DrawRect(PanelX + 5.0f, PanelY + 3.0f,
		minimum(PanelWidth * 0.42f, 28.0f), 1.1f,
		ColorRGBA(1.0f, 1.0f, 1.0f, 0.24f), IGraphics::CORNER_ALL, 0.55f);
	TextRender()->TextColor(Theme.m_Text);
	for(int Index = 0; Index < Visible; ++Index)
	{
		const char *pName =
			GameClient()->m_aClients[vWatchers[Index].m_ClientId].m_aName;
		TextRender()->Text(PanelX + 5.0f,
			PanelY + 2.0f + Index * RowHeight, 5.5f, pName,
			PanelWidth - 8.0f);
	}
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CHud::RenderDummyActions()
{
	if(!g_Config.m_ClShowhudDummyActions || (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) || !Client()->DummyConnected())
	{
		return;
	}
	// render small dummy actions hud
	const float BoxHeight = 29.0f;
	const float BoxWidth = 16.0f;

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		StartY -= 4;
	}
	StartY -= GetMovementInformationBoxHeight();

	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56;
	}

	Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

	float y = StartY + 2;
	float x = StartX + 2;
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
	if(g_Config.m_ClDummyHammer)
	{
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDummyHammer);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DummyHammerOffset, x, y);
	y += 13;
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.4f);
	if(g_Config.m_ClDummyCopyMoves)
	{
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
	Graphics()->TextureSet(GameClient()->m_HudSkin.m_SpriteHudDummyCopy);
	Graphics()->RenderQuadContainerAsSprite(m_HudQuadContainerIndex, m_DummyCopyOffset, x, y);
}

inline int CHud::GetDigitsIndex(int Value, int Max)
{
	if(Value < 0)
	{
		Value *= -1;
	}
	int DigitsIndex = std::log10((Value ? Value : 1));
	if(DigitsIndex > Max)
	{
		DigitsIndex = Max;
	}
	if(DigitsIndex < 0)
	{
		DigitsIndex = 0;
	}
	return DigitsIndex;
}

inline float CHud::GetMovementInformationBoxHeight()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && (GameClient()->m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW || GameClient()->m_aClients[GameClient()->m_Snap.m_SpecInfo.m_SpectatorId].m_SpecCharPresent))
		return g_Config.m_ClShowhudPlayerPosition ? 3.0f * MOVEMENT_INFORMATION_LINE_HEIGHT + 2.0f : 0.0f;
	float BoxHeight = 3.0f * MOVEMENT_INFORMATION_LINE_HEIGHT * (g_Config.m_ClShowhudPlayerPosition + g_Config.m_ClShowhudPlayerSpeed) + 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT * g_Config.m_ClShowhudPlayerAngle;
	if(g_Config.m_ClShowhudPlayerPosition || g_Config.m_ClShowhudPlayerSpeed || g_Config.m_ClShowhudPlayerAngle)
	{
		BoxHeight += 2.0f;
	}
	return BoxHeight;
}

void CHud::UpdateMovementInformationTextContainer(STextContainerIndex &TextContainer, float FontSize, float Value, float &PrevValue)
{
	Value = std::round(Value * 100.0f) / 100.0f; // Round to 2dp
	if(TextContainer.Valid() && PrevValue == Value)
		return;
	PrevValue = Value;

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%.2f", Value);

	CTextCursor Cursor;
	Cursor.m_FontSize = FontSize;
	TextRender()->RecreateTextContainer(TextContainer, &Cursor, aBuf);
}

void CHud::RenderMovementInformationTextContainer(STextContainerIndex &TextContainer, const ColorRGBA &Color, float X, float Y)
{
	if(TextContainer.Valid())
	{
		TextRender()->RenderTextContainer(TextContainer, Color, TextRender()->DefaultTextOutlineColor(), X - TextRender()->GetBoundingBoxTextContainer(TextContainer).m_W, Y);
	}
}

CHud::CMovementInformation CHud::GetMovementInformation(int ClientId, int Conn) const
{
	CMovementInformation Out;
	if(ClientId == SPEC_FREEVIEW)
	{
		Out.m_Pos = GameClient()->m_Camera.m_Center / 32.0f;
	}
	else if(GameClient()->m_aClients[ClientId].m_SpecCharPresent)
	{
		Out.m_Pos = GameClient()->m_aClients[ClientId].m_SpecChar / 32.0f;
	}
	else
	{
		const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev;
		const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
		const float IntraTick = Client()->IntraGameTick(Conn);

		// To make the player position relative to blocks we need to divide by the block size
		Out.m_Pos = mix(vec2(pPrevChar->m_X, pPrevChar->m_Y), vec2(pCurChar->m_X, pCurChar->m_Y), IntraTick) / 32.0f;

		const vec2 Vel = mix(vec2(pPrevChar->m_VelX, pPrevChar->m_VelY), vec2(pCurChar->m_VelX, pCurChar->m_VelY), IntraTick);

		float VelspeedX = Vel.x / 256.0f * Client()->GameTickSpeed();
		if(Vel.x >= -1.0f && Vel.x <= 1.0f)
		{
			VelspeedX = 0.0f;
		}
		float VelspeedY = Vel.y / 256.0f * Client()->GameTickSpeed();
		if(Vel.y >= -128.0f && Vel.y <= 128.0f)
		{
			VelspeedY = 0.0f;
		}
		// We show the speed in Blocks per Second (Bps) and therefore have to divide by the block size
		Out.m_Speed.x = VelspeedX / 32.0f;
		float VelspeedLength = length(vec2(Vel.x, Vel.y) / 256.0f) * Client()->GameTickSpeed();
		// Todo: Use Velramp tuning of each individual player
		// Since these tuning parameters are almost never changed, the default values are sufficient in most cases
		float Ramp = VelocityRamp(VelspeedLength, GameClient()->m_aTuning[Conn].m_VelrampStart, GameClient()->m_aTuning[Conn].m_VelrampRange, GameClient()->m_aTuning[Conn].m_VelrampCurvature);
		Out.m_Speed.x *= Ramp;
		Out.m_Speed.y = VelspeedY / 32.0f;

		float Angle = GameClient()->m_Players.GetPlayerTargetAngle(pPrevChar, pCurChar, ClientId, IntraTick);
		if(Angle < 0.0f)
		{
			Angle += 2.0f * pi;
		}
		Out.m_Angle = Angle * 180.0f / pi;
	}
	return Out;
}

void CHud::RenderMovementInformation()
{
	const int ClientId = GameClient()->m_Snap.m_SpecInfo.m_Active ? GameClient()->m_Snap.m_SpecInfo.m_SpectatorId : GameClient()->m_Snap.m_LocalClientId;
	const bool PosOnly = ClientId == SPEC_FREEVIEW || (GameClient()->m_aClients[ClientId].m_SpecCharPresent);
	// Draw the information depending on settings: Position, speed and target angle
	// This display is only to present the available information from the last snapshot, not to interpolate or predict
	if(!g_Config.m_ClShowhudPlayerPosition && (PosOnly || (!g_Config.m_ClShowhudPlayerSpeed && !g_Config.m_ClShowhudPlayerAngle)))
	{
		return;
	}
	const float LineSpacer = 1.0f; // above and below each entry
	const float Fontsize = 6.0f;

	float BoxHeight = GetMovementInformationBoxHeight();
	bool HasDummyInfo = false;
	CMovementInformation DummyInfo{};

	const int PairConn = Client()->DummyPair();
	if(PairConn != g_Config.m_ClDummy)
	{
		const int DummyClientId = GameClient()->m_aLocalIds[PairConn];

		if(DummyClientId >= 0 && DummyClientId < MAX_CLIENTS &&
			GameClient()->m_aClients[DummyClientId].m_Active)
		{
			DummyInfo = GetMovementInformation(DummyClientId, PairConn);
			HasDummyInfo = true;
		}
	}

	const bool ShowDummyPos = HasDummyInfo && g_Config.m_ClShowhudPlayerPosition && g_Config.m_TcShowhudDummyPosition;
	const bool ShowDummySpeed = HasDummyInfo && !PosOnly && g_Config.m_ClShowhudPlayerSpeed && g_Config.m_TcShowhudDummySpeed;
	const bool ShowDummyAngle = HasDummyInfo && !PosOnly && g_Config.m_ClShowhudPlayerAngle && g_Config.m_TcShowhudDummyAngle;

	if(ShowDummyPos)
		BoxHeight += 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;
	if(ShowDummySpeed)
		BoxHeight += 2.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;
	if(ShowDummyAngle)
		BoxHeight += 1.0f * MOVEMENT_INFORMATION_LINE_HEIGHT;

	const float BoxWidth = 62.0f;

	float StartX = m_Width - BoxWidth;
	float StartY = 285.0f - BoxHeight - 4.0f; // 4 units distance to the next display;
	if(g_Config.m_ClShowhudScore)
	{
		StartY -= 56.0f;
	}

	Graphics()->DrawRect(StartX, StartY, BoxWidth, BoxHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_L, 5.0f);

	const CMovementInformation Info = GetMovementInformation(ClientId, g_Config.m_ClDummy);

	float y = StartY + LineSpacer * 2.0f;
	const float LeftX = StartX + 2.0f;
	const float RightX = m_Width - 2.0f;

	if(g_Config.m_ClShowhudPlayerPosition)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Position:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		TextRender()->Text(LeftX, y, Fontsize, "X:", -1.0f);
		UpdateMovementInformationTextContainer(m_aPlayerPositionContainers[0], Fontsize, Info.m_Pos.x, m_aPlayerPrevPosition[0]);

		ColorRGBA TextColor = TextRender()->DefaultTextColor();
		if(ShowDummyPos && fabsf(Info.m_Pos.x - DummyInfo.m_Pos.x) < 0.01f)
			TextColor = ColorRGBA(0.2f, 1.0f, 0.2f, 1.0f);

		RenderMovementInformationTextContainer(m_aPlayerPositionContainers[0], TextColor, RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		TextRender()->Text(LeftX, y, Fontsize, "Y:", -1.0f);
		UpdateMovementInformationTextContainer(m_aPlayerPositionContainers[1], Fontsize, Info.m_Pos.y, m_aPlayerPrevPosition[1]);
		RenderMovementInformationTextContainer(m_aPlayerPositionContainers[1], TextRender()->DefaultTextColor(), RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		if(ShowDummyPos)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DX:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Pos.x);

			ColorRGBA DummyTextColor = TextRender()->DefaultTextColor();
			if(fabsf(Info.m_Pos.x - DummyInfo.m_Pos.x) < 0.01f)
				DummyTextColor = ColorRGBA(0.2f, 1.0f, 0.2f, 1.0f);

			TextRender()->TextColor(DummyTextColor);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;

			TextRender()->Text(LeftX, y, Fontsize, "DY:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Pos.y);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}
	}

	if(PosOnly)
		return;

	if(g_Config.m_ClShowhudPlayerSpeed)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Speed:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		const char aaCoordinates[][4] = {"X:", "Y:"};
		for(int i = 0; i < 2; i++)
		{
			ColorRGBA Color(1.0f, 1.0f, 1.0f, 1.0f);
			if(m_aLastPlayerSpeedChange[i] == ESpeedChange::INCREASE)
				Color = ColorRGBA(0.0f, 1.0f, 0.0f, 1.0f);
			if(m_aLastPlayerSpeedChange[i] == ESpeedChange::DECREASE)
				Color = ColorRGBA(1.0f, 0.5f, 0.5f, 1.0f);
			TextRender()->Text(LeftX, y, Fontsize, aaCoordinates[i], -1.0f);
			UpdateMovementInformationTextContainer(m_aPlayerSpeedTextContainers[i], Fontsize, i == 0 ? Info.m_Speed.x : Info.m_Speed.y, m_aPlayerPrevSpeed[i]);
			RenderMovementInformationTextContainer(m_aPlayerSpeedTextContainers[i], Color, RightX, y);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}

		if(ShowDummySpeed)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DX:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Speed.x);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;

			TextRender()->Text(LeftX, y, Fontsize, "DY:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Speed.y);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
			y += MOVEMENT_INFORMATION_LINE_HEIGHT;
		}

		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	if(g_Config.m_ClShowhudPlayerAngle)
	{
		TextRender()->Text(LeftX, y, Fontsize, Localize("Angle:"), -1.0f);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		UpdateMovementInformationTextContainer(m_PlayerAngleTextContainerIndex, Fontsize, Info.m_Angle, m_PlayerPrevAngle);
		RenderMovementInformationTextContainer(m_PlayerAngleTextContainerIndex, TextRender()->DefaultTextColor(), RightX, y);
		y += MOVEMENT_INFORMATION_LINE_HEIGHT;

		if(ShowDummyAngle)
		{
			char aBuf[32];

			TextRender()->Text(LeftX, y, Fontsize, "DA:", -1.0f);
			str_format(aBuf, sizeof(aBuf), "%.2f", DummyInfo.m_Angle);
			TextRender()->Text(RightX - TextRender()->TextWidth(Fontsize, aBuf), y, Fontsize, aBuf, -1.0f);
		}
	}
}

void CHud::RenderSpectatorHud()
{
	if(!g_Config.m_ClShowhudSpectator)
		return;

	// TClient
	float AdjustedHeight = m_Height - (g_Config.m_TcStatusBar ? g_Config.m_TcStatusBarHeight : 0.0f);
	// draw the box
	Graphics()->DrawRect(m_Width - 180.0f, AdjustedHeight - 15.0f, 180.0f, 15.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_TL, 5.0f);

	// draw the text
	char aBuf[128];
	if(GameClient()->m_MultiViewActivated)
	{
		str_copy(aBuf, Localize("Multi-View"));
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
	{
		const auto &Player = GameClient()->m_aClients[GameClient()->m_Snap.m_SpecInfo.m_SpectatorId];
		if(g_Config.m_ClShowIds)
			str_format(aBuf, sizeof(aBuf), Localize("Following %d: %s", "Spectating"), Player.ClientId(), Player.m_aName);
		else
			str_format(aBuf, sizeof(aBuf), Localize("Following %s", "Spectating"), Player.m_aName);
	}
	else
	{
		str_copy(aBuf, Localize("Free-View"));
	}
	TextRender()->Text(m_Width - 174.0f, AdjustedHeight - 15.0f + (15.f - 8.f) / 2.f, 8.0f, aBuf, -1.0f);

	// draw the camera info
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK && GameClient()->m_Camera.SpectatingPlayer() && GameClient()->m_Camera.CanUseAutoSpecCamera() && g_Config.m_ClSpecAutoSync)
	{
		bool AutoSpecCameraEnabled = GameClient()->m_Camera.m_AutoSpecCamera;
		const char *pLabelText = Localize("AUTO", "Spectating Camera Mode Icon");
		const float TextWidth = TextRender()->TextWidth(6.0f, pLabelText);

		constexpr float RightMargin = 4.0f;
		constexpr float IconWidth = 6.0f;
		constexpr float Padding = 3.0f;
		const float TagWidth = IconWidth + TextWidth + Padding * 3.0f;
		const float TagX = m_Width - RightMargin - TagWidth;
		Graphics()->DrawRect(TagX, m_Height - 12.0f, TagWidth, 10.0f, ColorRGBA(1.0f, 1.0f, 1.0f, AutoSpecCameraEnabled ? 0.50f : 0.10f), IGraphics::CORNER_ALL, 2.5f);
		TextRender()->TextColor(1, 1, 1, AutoSpecCameraEnabled ? 1.0f : 0.65f);
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->Text(TagX + Padding, m_Height - 10.0f, 6.0f, FontIcon::CAMERA, -1.0f);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		TextRender()->Text(TagX + Padding + IconWidth + Padding, m_Height - 10.0f, 6.0f, pLabelText, -1.0f);
		TextRender()->TextColor(1, 1, 1, 1);
	}
}

void CHud::RenderLocalTime(float x)
{
	if(!g_Config.m_ClShowLocalTimeAlways && !GameClient()->m_Scoreboard.IsActive())
		return;

	const bool Seconds = g_Config.m_TcShowLocalTimeSeconds; // TClient

	char aTimeStr[16];
	str_timestamp_format(aTimeStr, sizeof(aTimeStr), Seconds ? "%H:%M.%S" : "%H:%M");
	const float Width = std::round(TextRender()->TextBoundingBox(5.0f, aTimeStr).m_W);

	Graphics()->DrawRect(x - (Width + 15.0f), 0.0f, Width + 10.0f, 12.5f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_B, 3.75f);
	TextRender()->Text(x - (Width + 10.0f), (12.5f - 5.f) / 2.f, 5.0f, aTimeStr, -1.0f);

	// Graphics()->DrawRect(x - 30.0f, 0.0f, 25.0f, 12.5f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_B, 3.75f);
	// TextRender()->Text(x - 25.0f, (12.5f - 5.f) / 2.f, 5.0f, aTimeStr, -1.0f);
}

void CHud::OnNewSnapshot()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	int ClientId = -1;
	if(GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
		ClientId = GameClient()->m_Snap.m_LocalClientId;
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		ClientId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;

	if(ClientId == -1)
		return;

	const CNetObj_Character *pPrevChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev;
	const CNetObj_Character *pCurChar = &GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur;
	const float IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
	ivec2 Vel = mix(ivec2(pPrevChar->m_VelX, pPrevChar->m_VelY), ivec2(pCurChar->m_VelX, pCurChar->m_VelY), IntraTick);

	CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	if(pChar && pChar->IsGrounded())
		Vel.y = 0;

	int aVels[2] = {Vel.x, Vel.y};

	for(int i = 0; i < 2; i++)
	{
		int AbsVel = abs(aVels[i]);
		if(AbsVel > m_aPlayerSpeed[i])
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::INCREASE;
		}
		if(AbsVel < m_aPlayerSpeed[i])
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::DECREASE;
		}
		if(AbsVel < 2)
		{
			m_aLastPlayerSpeedChange[i] = ESpeedChange::NONE;
		}
		m_aPlayerSpeed[i] = AbsVel;
	}
}

void CHud::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	m_Width = 300.0f * Graphics()->ScreenAspect();
	m_Height = 300.0f;
	Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);

	bool ShowHud = g_Config.m_ClShowhud;
#if defined(CONF_VIDEORECORDER)
	ShowHud = (IVideo::Current() && g_Config.m_ClVideoShowhud) || (!IVideo::Current() && g_Config.m_ClShowhud);
#endif
	if(ShowHud)
	{
		if(GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active && !(GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
		{
			// KernelNet: visual FOV
			const bool ShowRegularAimbotFov = g_Config.m_ClZzAimbotEnabled && g_Config.m_ClZzAimbotShowFov &&
							  GameClient()->m_Controls.m_aSaveInBlock[g_Config.m_ClDummy] == 0;
			float FovDeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
#if defined(CONF_VIDEORECORDER)
			if(IVideo::Current())
				FovDeltaTime = 1.0f / (float)maximum(g_Config.m_ClVideoRecorderFPS, 1);
#endif
			const float FovSpeedScale = std::clamp(g_Config.m_ClZzAnimationSpeed / 100.0f, 0.25f, 2.0f);
			const float FovTrackingScale = 0.85f + std::clamp(g_Config.m_ClZzAnimationSpeed / 100.0f, 0.0f, 2.0f) * 0.15f;
			const float FovBlend = 1.0f - std::exp(-FovDeltaTime * (ShowRegularAimbotFov ? 11.0f : 7.0f) * FovSpeedScale);
			m_ZzFovVisibility += ((ShowRegularAimbotFov ? 1.0f : 0.0f) - m_ZzFovVisibility) * FovBlend;
			if(m_ZzFovVisibility < 0.002f)
			{
				m_ZzFovVisibility = 0.0f;
				m_ZzFovSmoothingInitialized = false;
				m_ZzFovMotionTime = 0.0f;
			}
			if(m_ZzFovVisibility > 0.0f)
			{
				// draw in world space
				float WorldW = 0.0f;
				float WorldH = 0.0f;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &WorldW, &WorldH);
				const float X0 = GameClient()->m_Camera.m_Center.x - WorldW / 2.0f;
				const float Y0 = GameClient()->m_Camera.m_Center.y - WorldH / 2.0f;
				const float X1 = X0 + WorldW;
				const float Y1 = Y0 + WorldH;
				Graphics()->MapScreen(X0, Y0, X1, Y1);

				const int LocalId = GameClient()->m_Snap.m_LocalClientId;
				vec2 RawAim((float)GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetX, (float)GameClient()->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetY);
				const auto &LocalCharacter = GameClient()->m_Snap.m_aCharacters[LocalId];
				if(Client()->State() == IClient::STATE_DEMOPLAYBACK &&
					LocalCharacter.m_HasExtendedDisplayInfo)
				{
					const vec2 CurrentAim(
						(float)LocalCharacter.m_ExtendedData.m_TargetX,
						(float)LocalCharacter.m_ExtendedData.m_TargetY);
					if(LocalCharacter.m_pPrevExtendedData)
					{
						const vec2 PreviousAim(
							(float)LocalCharacter.m_pPrevExtendedData->m_TargetX,
							(float)LocalCharacter.m_pPrevExtendedData->m_TargetY);
						RawAim = mix(PreviousAim, CurrentAim,
							Client()->IntraGameTick(g_Config.m_ClDummy));
					}
					else
					{
						RawAim = CurrentAim;
					}
				}
				else if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
				{
					// Controls are not updated while an offline video is rendered. Match the
					// regular player renderer and fall back to the character snapshot angle
					// when the demo does not contain DDNet extended cursor data.
					const float Intra = Client()->IntraGameTick(g_Config.m_ClDummy);
					const float PrevAngle = (float)LocalCharacter.m_Prev.m_Angle;
					float CurAngle = (float)LocalCharacter.m_Cur.m_Angle;
					if(CurAngle > 256.0f * pi && PrevAngle < 0.0f)
						CurAngle -= 256.0f * 2.0f * pi;
					else if(CurAngle < 0.0f && PrevAngle > 256.0f * pi)
						CurAngle += 256.0f * 2.0f * pi;
					RawAim = direction(mix(PrevAngle, CurAngle, Intra) / 256.0f);
				}
				if(length(RawAim) < 0.001f)
					RawAim = vec2(1.0f, 0.0f);
				RawAim = normalize(RawAim);
				// Match the rendered tee and start the cone between its eyes.
				const vec2 EyeOffset(RawAim.x * 8.0f, -3.2f + RawAim.y * 6.4f);
				const vec2 RawLocalPos = GameClient()->m_aClients[LocalId].m_RenderPos + EyeOffset;
				const float RawAimAngle = angle(RawAim);
				const float RawHalfFov = (float)g_Config.m_ClZzAimbotFov * (pi / 180.0f) * 0.5f;
				const float HookRange = (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength;
				// Match CZZHookAimbot's small allowance for a visible tee edge at maximum hook range.
				const float RawReach = HookRange + CCharacterCore::PhysicalSize() * 0.15f;
				if(!m_ZzFovSmoothingInitialized || distance(m_ZzFovPosition, RawLocalPos) > 160.0f)
				{
					m_ZzFovPosition = RawLocalPos;
					m_ZzFovAimAngle = RawAimAngle;
					m_ZzFovHalfAngle = RawHalfFov;
					m_ZzFovReach = RawReach;
					m_ZzFovSmoothingInitialized = true;
				}
				else
				{
					const float ShapeBlend = 1.0f - std::exp(-FovDeltaTime * 32.0f * FovTrackingScale);
					const float AngleDelta = atan2f(sinf(RawAimAngle - m_ZzFovAimAngle), cosf(RawAimAngle - m_ZzFovAimAngle));
					const float BaseDirectionBlend = 1.0f - std::exp(-FovDeltaTime * 38.0f * FovTrackingScale);
					const float FastTurn = std::clamp(std::abs(AngleDelta) / (pi * 0.30f), 0.0f, 1.0f);
					const float DirectionBlend = BaseDirectionBlend + (1.0f - BaseDirectionBlend) * FastTurn * 0.92f;
					// Keep the origin on the eyes while smoothly catching up to fast cursor turns.
					m_ZzFovPosition = RawLocalPos;
					m_ZzFovAimAngle += AngleDelta * DirectionBlend;
					m_ZzFovHalfAngle += (RawHalfFov - m_ZzFovHalfAngle) * ShapeBlend;
					m_ZzFovReach += (RawReach - m_ZzFovReach) * ShapeBlend;
				}
				m_ZzFovMotionTime += FovDeltaTime * FovSpeedScale;
				const vec2 LocalPos = m_ZzFovPosition;
				const float AimAngle = m_ZzFovAimAngle;
				const float HalfFov = m_ZzFovHalfAngle;
				const float Reach = m_ZzFovReach;
				const vec2 Aim = direction(AimAngle);
				const vec2 LeftDir = direction(AimAngle - HalfFov);
				const vec2 RightDir = direction(AimAngle + HalfFov);
				const SZZThemePalette Palette = GetZZThemePalette(g_Config.m_ClZzTheme, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
				const float FovTime = m_ZzFovMotionTime;
				const float Pulse = 0.5f + 0.5f * sinf(FovTime * 2.8f);
				const float Visibility = m_ZzFovVisibility * m_ZzFovVisibility * (3.0f - 2.0f * m_ZzFovVisibility);
				IGraphics::CLineItem aFovLines[128];
				int NumFovLines = 0;
				const int ArcSegments = std::clamp(32 + g_Config.m_ClZzAimbotFov / 5, 36, 64);
				for(int i = 0; i < ArcSegments; ++i)
				{
					const float T0 = (float)i / ArcSegments;
					const float T1 = (float)(i + 1) / ArcSegments;
					const float A0 = AimAngle - HalfFov + HalfFov * 2.0f * T0;
					const float A1 = AimAngle - HalfFov + HalfFov * 2.0f * T1;
					const vec2 P0 = LocalPos + direction(A0) * Reach;
					const vec2 P1 = LocalPos + direction(A1) * Reach;
					aFovLines[NumFovLines++] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
				}
				const vec2 aBoundaryDirs[2] = {LeftDir, RightDir};
				for(const vec2 &BoundaryDir : aBoundaryDirs)
				{
					for(int Dash = 0; Dash < 7; ++Dash)
					{
						const float StartFactor = 0.055f + Dash * 0.14f;
						const float EndFactor = Dash == 6 ? 1.0f : std::min(1.0f, StartFactor + 0.105f + Pulse * 0.004f);
						const float Start = Reach * StartFactor;
						const float End = Reach * EndFactor;
						const vec2 P0 = LocalPos + BoundaryDir * Start;
						const vec2 P1 = LocalPos + BoundaryDir * End;
						aFovLines[NumFovLines++] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
					}
				}
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				const IGraphics::CLineItem aGuideLines[2] = {
					IGraphics::CLineItem(LocalPos.x, LocalPos.y, LocalPos.x + LeftDir.x * Reach, LocalPos.y + LeftDir.y * Reach),
					IGraphics::CLineItem(LocalPos.x, LocalPos.y, LocalPos.x + RightDir.x * Reach, LocalPos.y + RightDir.y * Reach)};
				Graphics()->SetColor(Palette.m_Accent.WithAlpha((0.15f + Pulse * 0.035f) * Visibility));
				Graphics()->LinesDraw(aGuideLines, std::size(aGuideLines));
				Graphics()->SetColor(Palette.m_Accent.WithAlpha((0.62f + Pulse * 0.14f) * Visibility));
				Graphics()->LinesDraw(aFovLines, NumFovLines);

				const float Travel = 0.10f + 0.82f * (0.5f + 0.5f * sinf(FovTime * 1.35f));
				IGraphics::CLineItem aTravelHighlights[2];
				for(int i = 0; i < 2; ++i)
				{
					const float Start = Reach * std::clamp(Travel + i * 0.035f, 0.08f, 0.94f);
					const float End = std::min(Reach, Start + Reach * 0.065f);
					const vec2 &Dir = aBoundaryDirs[i];
					aTravelHighlights[i] = IGraphics::CLineItem(LocalPos + Dir * Start, LocalPos + Dir * End);
				}
				Graphics()->SetColor(Palette.m_AccentAlt.WithAlpha((0.80f + Pulse * 0.18f) * Visibility));
				Graphics()->LinesDraw(aTravelHighlights, std::size(aTravelHighlights));

				const vec2 Focus = LocalPos + Aim * Reach * 0.58f;
				const vec2 Normal(-Aim.y, Aim.x);
				const float FocusSize = (9.0f + Pulse * 1.8f) * Visibility;
				const vec2 Tip = Focus + Aim * FocusSize;
				const vec2 Back = Focus - Aim * FocusSize * 0.72f;
				const vec2 Left = Back + Normal * FocusSize * 0.72f;
				const vec2 Right = Back - Normal * FocusSize * 0.72f;
				IGraphics::CLineItem aFocus[3] = {
					IGraphics::CLineItem(Tip.x, Tip.y, Left.x, Left.y),
					IGraphics::CLineItem(Tip.x, Tip.y, Right.x, Right.y),
					IGraphics::CLineItem(Left.x, Left.y, Right.x, Right.y)};
				Graphics()->SetColor(Palette.m_AccentAlt.WithAlpha((0.78f + Pulse * 0.20f) * Visibility));
				Graphics()->LinesDraw(aFocus, std::size(aFocus));

				const float TickSize = 8.5f + Pulse * 1.2f;
				const vec2 LeftEnd = LocalPos + LeftDir * Reach;
				const vec2 RightEnd = LocalPos + RightDir * Reach;
				const vec2 LeftNormal(-LeftDir.y, LeftDir.x);
				const vec2 RightNormal(-RightDir.y, RightDir.x);
				const IGraphics::CLineItem aReachTicks[2] = {
					IGraphics::CLineItem(LeftEnd - LeftNormal * TickSize, LeftEnd + LeftNormal * TickSize),
					IGraphics::CLineItem(RightEnd - RightNormal * TickSize, RightEnd + RightNormal * TickSize)};
				Graphics()->SetColor(Palette.m_Text.WithAlpha((0.60f + Pulse * 0.22f) * Visibility));
				Graphics()->LinesDraw(aReachTicks, std::size(aReachTicks));
				Graphics()->LinesEnd();

				// restore HUD screen
				m_Width = 300.0f * Graphics()->ScreenAspect();
				m_Height = 300.0f;
				Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);
			}

			if(g_Config.m_ClZzFngShowDecision && GameClient()->m_Controls.m_ZzFngDecisionValid &&
				Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Controls.m_ZzFngDecisionTick <= 2)
			{
				float WorldW = 0.0f;
				float WorldH = 0.0f;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &WorldW, &WorldH);
				const float X0 = GameClient()->m_Camera.m_Center.x - WorldW / 2.0f;
				const float Y0 = GameClient()->m_Camera.m_Center.y - WorldH / 2.0f;
				Graphics()->MapScreen(X0, Y0, X0 + WorldW, Y0 + WorldH);

				vec2 Pos = GameClient()->m_Controls.m_ZzFngDecisionStart;
				vec2 Dir = GameClient()->m_Controls.m_ZzFngDecisionDir;
				const float DirLen = length(Dir);
				if(DirLen > 0.001f)
					Dir *= 1.0f / DirLen;
				float RemainingTravel = std::max(0.0f, GameClient()->m_Controls.m_ZzFngDecisionTravel);
				const int DecisionBounces = std::clamp(GameClient()->m_Controls.m_ZzFngDecisionBounces, 0, 8);
				IGraphics::CLineItem aPathLines[9];
				vec2 aBouncePositions[8];
				int NumPathLines = 0;
				int NumBouncePositions = 0;
				vec2 EndPos = Pos;
				for(int Segment = 0; Segment <= DecisionBounces && RemainingTravel > 0.001f && NumPathLines < 9; Segment++)
				{
					const vec2 From = Pos;
					vec2 CollisionPos;
					vec2 To = Pos + Dir * RemainingTravel;
					const int HitWall = Collision()->IntersectLineTeleWeapon(Pos, To, &CollisionPos, &To);
					if(!HitWall || Segment >= DecisionBounces)
					{
						EndPos = Pos + Dir * RemainingTravel;
						aPathLines[NumPathLines++] = IGraphics::CLineItem(Pos.x, Pos.y, EndPos.x, EndPos.y);
						RemainingTravel = 0.0f;
						break;
					}

					aPathLines[NumPathLines++] = IGraphics::CLineItem(Pos.x, Pos.y, To.x, To.y);
					if(NumBouncePositions < (int)std::size(aBouncePositions))
						aBouncePositions[NumBouncePositions++] = To;
					vec2 ReflectedPos = To;
					vec2 ReflectedDir = Dir * 4.0f;
					Collision()->MovePoint(&ReflectedPos, &ReflectedDir, 1.0f, nullptr);
					RemainingTravel = std::max(0.0f, RemainingTravel - distance(From, ReflectedPos));
					Pos = ReflectedPos;
					EndPos = Pos;
					if(length(ReflectedDir) > 0.001f)
						Dir = normalize(ReflectedDir);
				}

				float MarkerRadius = 10.0f;
				float MarkerPulse = 0.0f;
				if(g_Config.m_ClZzVisualEffects && g_Config.m_ClZzVisualEffectsFngPulse)
				{
					const float EffectStrength = std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
					MarkerPulse = 0.5f + 0.5f * sinf(LocalTime() * 8.0f * (g_Config.m_ClZzAnimationSpeed / 100.0f));
					MarkerRadius += (1.5f + 3.5f * MarkerPulse) * EffectStrength;
				}
				IGraphics::CLineItem aTargetRing[16];
				for(int i = 0; i < (int)std::size(aTargetRing); ++i)
				{
					const float Step = 2.0f * pi / std::size(aTargetRing);
					const float A0 = i * Step + LocalTime() * 0.45f;
					const float A1 = A0 + Step * 0.68f;
					const vec2 P0 = EndPos + direction(A0) * MarkerRadius;
					const vec2 P1 = EndPos + direction(A1) * MarkerRadius;
					aTargetRing[i] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
				}
				IGraphics::CLineItem aTargetTicks[4];
				for(int i = 0; i < (int)std::size(aTargetTicks); ++i)
				{
					const vec2 TickDir = direction(i * pi / 2.0f + LocalTime() * 0.45f);
					const vec2 P0 = EndPos + TickDir * (MarkerRadius + 2.0f);
					const vec2 P1 = EndPos + TickDir * (MarkerRadius + 6.0f + MarkerPulse * 3.0f);
					aTargetTicks[i] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
				}
				IGraphics::CLineItem aBounceDiamonds[32];
				int NumBounceLines = 0;
				for(int i = 0; i < NumBouncePositions; ++i)
				{
					const float R = 4.0f + MarkerPulse * 1.5f;
					const vec2 P0 = aBouncePositions[i] + vec2(0.0f, -R);
					const vec2 P1 = aBouncePositions[i] + vec2(R, 0.0f);
					const vec2 P2 = aBouncePositions[i] + vec2(0.0f, R);
					const vec2 P3 = aBouncePositions[i] + vec2(-R, 0.0f);
					aBounceDiamonds[NumBounceLines++] = IGraphics::CLineItem(P0, P1);
					aBounceDiamonds[NumBounceLines++] = IGraphics::CLineItem(P1, P2);
					aBounceDiamonds[NumBounceLines++] = IGraphics::CLineItem(P2, P3);
					aBounceDiamonds[NumBounceLines++] = IGraphics::CLineItem(P3, P0);
				}
				IGraphics::CLineItem aFlowDashes[9];
				int NumFlowDashes = 0;
				for(int i = 0; i < NumPathLines; ++i)
				{
					const vec2 From(aPathLines[i].m_X0, aPathLines[i].m_Y0);
					const vec2 To(aPathLines[i].m_X1, aPathLines[i].m_Y1);
					const float SegmentLength = distance(From, To);
					if(SegmentLength <= 0.001f)
						continue;
					const vec2 SegmentDir = (To - From) / SegmentLength;
					const float Flow = fmodf(LocalTime() * 125.0f + i * 29.0f, SegmentLength);
					const float FlowEnd = std::min(SegmentLength, Flow + 12.0f + MarkerPulse * 7.0f);
					aFlowDashes[NumFlowDashes++] = IGraphics::CLineItem(From + SegmentDir * Flow, From + SegmentDir * FlowEnd);
				}
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				const SZZThemePalette Palette = GetZZThemePalette(g_Config.m_ClZzTheme, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
				const ColorRGBA PathColor = GameClient()->m_Controls.m_ZzFngDecisionTrigger ? ColorRGBA(0.25f, 1.0f, 0.4f, 1.0f) :
													      (DecisionBounces > 0 ? Palette.m_Accent : Palette.m_AccentAlt);
				Graphics()->SetColor(PathColor.WithAlpha(0.42f));
				Graphics()->LinesDraw(aPathLines, NumPathLines);
				Graphics()->SetColor(PathColor.WithAlpha(0.96f));
				Graphics()->LinesDraw(aFlowDashes, NumFlowDashes);
				Graphics()->SetColor(PathColor.WithAlpha(0.72f + MarkerPulse * 0.24f));
				Graphics()->LinesDraw(aTargetRing, std::size(aTargetRing));
				Graphics()->LinesDraw(aTargetTicks, std::size(aTargetTicks));
				Graphics()->SetColor(Palette.m_AccentAlt.WithAlpha(0.86f));
				Graphics()->LinesDraw(aBounceDiamonds, NumBounceLines);
				Graphics()->LinesEnd();

				m_Width = 300.0f * Graphics()->ScreenAspect();
				m_Height = 300.0f;
				Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);
			}

			if(g_Config.m_ClZzFentBotEnabled && GameClient()->FentBotHasWaypoint())
			{
				float WorldW = 0.0f;
				float WorldH = 0.0f;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &WorldW, &WorldH);
				const float X0 = GameClient()->m_Camera.m_Center.x - WorldW / 2.0f;
				const float Y0 = GameClient()->m_Camera.m_Center.y - WorldH / 2.0f;
				const float X1 = X0 + WorldW;
				const float Y1 = Y0 + WorldH;
				Graphics()->MapScreen(X0, Y0, X1, Y1);

				const vec2 Wp = GameClient()->FentBotWaypoint();
				const float R = 24.0f;
				IGraphics::CLineItem aLines[2] = {
					IGraphics::CLineItem(Wp.x - R, Wp.y, Wp.x + R, Wp.y),
					IGraphics::CLineItem(Wp.x, Wp.y - R, Wp.x, Wp.y + R)};
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				Graphics()->SetColor(0.95f, 0.9f, 0.2f, 0.85f);
				Graphics()->LinesDraw(aLines, 2);
				Graphics()->LinesEnd();

				// restore HUD screen
				m_Width = 300.0f * Graphics()->ScreenAspect();
				m_Height = 300.0f;
				Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);
			}

			if(g_Config.m_ClZzFentBotEnabled && g_Config.m_ClZzFentBotShowPath && GameClient()->FentBotIsActive() && GameClient()->FentBotPlannedPathLen() >= 2)
			{
				float WorldW = 0.0f;
				float WorldH = 0.0f;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &WorldW, &WorldH);
				const float X0 = GameClient()->m_Camera.m_Center.x - WorldW / 2.0f;
				const float Y0 = GameClient()->m_Camera.m_Center.y - WorldH / 2.0f;
				const float X1 = X0 + WorldW;
				const float Y1 = Y0 + WorldH;
				Graphics()->MapScreen(X0, Y0, X1, Y1);

				const int Num = std::clamp(GameClient()->FentBotPlannedPathLen(), 0, (int)CGameClient::FENTBOT_MAX_PATH_POINTS_PUBLIC);
				std::vector<IGraphics::CLineItem> vLines;
				vLines.reserve(Num - 1);
				for(int i = 0; i + 1 < Num; i++)
				{
					const vec2 A = GameClient()->FentBotPlannedPathPoint(i);
					const vec2 B = GameClient()->FentBotPlannedPathPoint(i + 1);
					vLines.emplace_back(A.x, A.y, B.x, B.y);
				}
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				Graphics()->SetColor(0.2f, 0.9f, 0.35f, 0.65f);
				Graphics()->LinesDraw(vLines.data(), vLines.size());
				Graphics()->LinesEnd();

				// restore HUD screen
				m_Width = 300.0f * Graphics()->ScreenAspect();
				m_Height = 300.0f;
				Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);
			}

			if(g_Config.m_ClZzFentBotTrainPreview && GameClient()->FentBotBestPathLen() >= 2)
			{
				float WorldW = 0.0f;
				float WorldH = 0.0f;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom, &WorldW, &WorldH);
				const float X0 = GameClient()->m_Camera.m_Center.x - WorldW / 2.0f;
				const float Y0 = GameClient()->m_Camera.m_Center.y - WorldH / 2.0f;
				const float X1 = X0 + WorldW;
				const float Y1 = Y0 + WorldH;
				Graphics()->MapScreen(X0, Y0, X1, Y1);

				const int Num = std::clamp(GameClient()->FentBotBestPathLen(), 0, (int)CGameClient::FENTBOT_MAX_BEST_POINTS_PUBLIC);
				std::vector<IGraphics::CLineItem> vLines;
				vLines.reserve(Num - 1);
				for(int i = 0; i + 1 < Num; i++)
				{
					const vec2 A = GameClient()->FentBotBestPathPoint(i);
					const vec2 B = GameClient()->FentBotBestPathPoint(i + 1);
					vLines.emplace_back(A.x, A.y, B.x, B.y);
				}
				Graphics()->TextureClear();
				Graphics()->LinesBegin();
				Graphics()->SetColor(0.95f, 0.15f, 0.75f, 0.65f);
				Graphics()->LinesDraw(vLines.data(), vLines.size());
				Graphics()->LinesEnd();

				// restore HUD screen
				m_Width = 300.0f * Graphics()->ScreenAspect();
				m_Height = 300.0f;
				Graphics()->MapScreen(0.0f, 0.0f, m_Width, m_Height);
			}

			if(g_Config.m_ClShowhudHealthAmmo)
			{
				RenderAmmoHealthAndArmor(GameClient()->m_Snap.m_pLocalCharacter);
			}
			if(GameClient()->m_Snap.m_aCharacters[GameClient()->m_Snap.m_LocalClientId].m_HasExtendedData && g_Config.m_ClShowhudDDRace && GameClient()->m_GameInfo.m_HudDDRace)
			{
				RenderPlayerState(GameClient()->m_Snap.m_LocalClientId);
			}
			RenderMovementInformation();
			RenderDDRaceEffects();
		}
		else if(GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			int SpectatorId = GameClient()->m_Snap.m_SpecInfo.m_SpectatorId;
			if(SpectatorId != SPEC_FREEVIEW && g_Config.m_ClShowhudHealthAmmo)
			{
				RenderAmmoHealthAndArmor(&GameClient()->m_Snap.m_aCharacters[SpectatorId].m_Cur);
			}
			if(SpectatorId != SPEC_FREEVIEW &&
				GameClient()->m_Snap.m_aCharacters[SpectatorId].m_HasExtendedData &&
				g_Config.m_ClShowhudDDRace &&
				(!GameClient()->m_MultiViewActivated || GameClient()->m_MultiViewShowHud) &&
				GameClient()->m_GameInfo.m_HudDDRace)
			{
				RenderPlayerState(SpectatorId);
			}
			RenderMovementInformation();
			RenderSpectatorHud();
		}

		if(g_Config.m_ClShowhudTimer)
			RenderGameTimer();
		RenderPauseNotification();
		RenderSuddenDeath();
		if(g_Config.m_ClShowhudScore)
			RenderScoreHud();
		RenderDummyActions();
		RenderWarmupTimer();
		GameClient()->m_TClient.RenderCenterLines();
		RenderLocalTime((m_Width / 7) * 3);
		if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
			RenderConnectionWarning();
		RenderTeambalanceWarning();
		GameClient()->m_Voting.Render();
		if(g_Config.m_ClShowRecord)
			RenderRecord();
	}
	// Keep the watcher plaque in the final HUD layer so map foreground tiles
	// cannot cover it. The custom list remains available even when the regular
	// HUD or its standard spectator counter is disabled.
	if(ShowHud || g_Config.m_ClKnWatcherList)
		RenderSpectatorCount();
	// KernelNet's active-feature panel is useful even when the user hides the
	// standard DDNet HUD. Keep only the optional FPS/prediction text tied to
	// the standard HUD visibility.
	RenderTextInfo(ShowHud);
	RenderCursor();
}

void CHud::OnMessage(int MsgType, void *pRawMsg)
{
	if(MsgType == NETMSGTYPE_SV_DDRACETIME || MsgType == NETMSGTYPE_SV_DDRACETIMELEGACY)
	{
		CNetMsg_Sv_DDRaceTime *pMsg = (CNetMsg_Sv_DDRaceTime *)pRawMsg;

		m_DDRaceTime = pMsg->m_Time;

		m_ShowFinishTime = pMsg->m_Finish != 0;

		if(!m_ShowFinishTime)
		{
			m_TimeCpDiff = (float)pMsg->m_Check / 100;
			m_TimeCpLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
		}
		else
		{
			m_FinishTimeDiff = (float)pMsg->m_Check / 100;
			m_FinishTimeLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
		}
	}
	else if(MsgType == NETMSGTYPE_SV_RECORD || MsgType == NETMSGTYPE_SV_RECORDLEGACY)
	{
		CNetMsg_Sv_Record *pMsg = (CNetMsg_Sv_Record *)pRawMsg;

		// NETMSGTYPE_SV_RACETIME on old race servers
		if(MsgType == NETMSGTYPE_SV_RECORDLEGACY && GameClient()->m_GameInfo.m_DDRaceRecordMessage)
		{
			m_DDRaceTime = pMsg->m_ServerTimeBest; // First value: m_Time

			m_FinishTimeLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);

			if(pMsg->m_PlayerTimeBest) // Second value: m_Check
			{
				m_TimeCpDiff = (float)pMsg->m_PlayerTimeBest / 100;
				m_TimeCpLastReceivedTick = Client()->GameTick(g_Config.m_ClDummy);
			}
		}
		else if(MsgType == NETMSGTYPE_SV_RECORD || GameClient()->m_GameInfo.m_RaceRecordMessage)
		{
			// ignore m_ServerTimeBest, it's handled by the game client
			m_aPlayerRecord[g_Config.m_ClDummy] = (float)pMsg->m_PlayerTimeBest / 100;
		}
	}
}

void CHud::RenderDDRaceEffects()
{
	if(m_DDRaceTime)
	{
		char aBuf[64];
		char aTime[32];
		if(m_ShowFinishTime && m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
		{
			str_time(m_DDRaceTime, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "Finish time: %s", aTime);

			// calculate alpha (4 sec 1 than get lower the next 2 sec)
			float Alpha = 1.0f;
			if(m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 4 < Client()->GameTick(g_Config.m_ClDummy) && m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
			{
				// lower the alpha slowly to blend text out
				Alpha = ((float)(m_FinishTimeLastReceivedTick + Client()->GameTickSpeed() * 6) - (float)Client()->GameTick(g_Config.m_ClDummy)) / (float)(Client()->GameTickSpeed() * 2);
			}

			TextRender()->TextColor(1, 1, 1, Alpha);
			CTextCursor Cursor;
			Cursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(12, aBuf) / 2, 20));
			Cursor.m_FontSize = 12.0f;
			TextRender()->RecreateTextContainer(m_DDRaceEffectsTextContainerIndex, &Cursor, aBuf);
			if(m_FinishTimeDiff != 0.0f && m_DDRaceEffectsTextContainerIndex.Valid())
			{
				if(m_FinishTimeDiff < 0)
				{
					str_time_float(-m_FinishTimeDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
					str_format(aBuf, sizeof(aBuf), "-%s", aTime);
					TextRender()->TextColor(0.5f, 1.0f, 0.5f, Alpha); // green
				}
				else
				{
					str_time_float(m_FinishTimeDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
					str_format(aBuf, sizeof(aBuf), "+%s", aTime);
					TextRender()->TextColor(1.0f, 0.5f, 0.5f, Alpha); // red
				}
				CTextCursor DiffCursor;
				DiffCursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(10, aBuf) / 2, 34));
				DiffCursor.m_FontSize = 10.0f;
				TextRender()->AppendTextContainer(m_DDRaceEffectsTextContainerIndex, &DiffCursor, aBuf);
			}
			if(m_DDRaceEffectsTextContainerIndex.Valid())
			{
				auto OutlineColor = TextRender()->DefaultTextOutlineColor();
				OutlineColor.a *= Alpha;
				TextRender()->RenderTextContainer(m_DDRaceEffectsTextContainerIndex, TextRender()->DefaultTextColor(), OutlineColor);
			}
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
		else if(g_Config.m_ClShowhudTimeCpDiff && !m_ShowFinishTime && m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
		{
			if(m_TimeCpDiff < 0)
			{
				str_time_float(-m_TimeCpDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
				str_format(aBuf, sizeof(aBuf), "-%s", aTime);
			}
			else
			{
				str_time_float(m_TimeCpDiff, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
				str_format(aBuf, sizeof(aBuf), "+%s", aTime);
			}

			// calculate alpha (4 sec 1 than get lower the next 2 sec)
			float Alpha = 1.0f;
			if(m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 4 < Client()->GameTick(g_Config.m_ClDummy) && m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6 > Client()->GameTick(g_Config.m_ClDummy))
			{
				// lower the alpha slowly to blend text out
				Alpha = ((float)(m_TimeCpLastReceivedTick + Client()->GameTickSpeed() * 6) - (float)Client()->GameTick(g_Config.m_ClDummy)) / (float)(Client()->GameTickSpeed() * 2);
			}

			if(m_TimeCpDiff > 0)
				TextRender()->TextColor(1.0f, 0.5f, 0.5f, Alpha); // red
			else if(m_TimeCpDiff < 0)
				TextRender()->TextColor(0.5f, 1.0f, 0.5f, Alpha); // green
			else if(!m_TimeCpDiff)
				TextRender()->TextColor(1, 1, 1, Alpha); // white

			CTextCursor Cursor;
			Cursor.SetPosition(vec2(150 * Graphics()->ScreenAspect() - TextRender()->TextWidth(10, aBuf) / 2, 20));
			Cursor.m_FontSize = 10.0f;
			TextRender()->RecreateTextContainer(m_DDRaceEffectsTextContainerIndex, &Cursor, aBuf);

			if(m_DDRaceEffectsTextContainerIndex.Valid())
			{
				auto OutlineColor = TextRender()->DefaultTextOutlineColor();
				OutlineColor.a *= Alpha;
				TextRender()->RenderTextContainer(m_DDRaceEffectsTextContainerIndex, TextRender()->DefaultTextColor(), OutlineColor);
			}
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
	}
}

void CHud::RenderRecord()
{
	if(GameClient()->m_MapBestTimeSeconds != FinishTime::UNSET && GameClient()->m_MapBestTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
	{
		char aBuf[64];
		TextRender()->Text(5, 75, 6, Localize("Server best:"), -1.0f);
		char aTime[32];
		int64_t TimeCentiseconds = static_cast<int64_t>(GameClient()->m_MapBestTimeSeconds) * 100 + static_cast<int64_t>(GameClient()->m_MapBestTimeMillis) / 10;
		str_time(TimeCentiseconds, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		str_format(aBuf, sizeof(aBuf), "%s%s", GameClient()->m_MapBestTimeSeconds > 3600 ? "" : "   ", aTime);
		TextRender()->Text(53, 75, 6, aBuf, -1.0f);
	}

	if(GameClient()->m_ReceivedDDNetPlayerFinishTimes)
	{
		const int PlayerTimeSeconds = GameClient()->m_aClients[GameClient()->m_aLocalIds[g_Config.m_ClDummy]].m_FinishTimeSeconds;
		if(PlayerTimeSeconds != FinishTime::NOT_FINISHED_MILLIS)
		{
			char aBuf[64];
			TextRender()->Text(5, 82, 6, Localize("Personal best:"), -1.0f);
			char aTime[32];
			const int PlayerTimeMillis = GameClient()->m_aClients[GameClient()->m_aLocalIds[g_Config.m_ClDummy]].m_FinishTimeMillis;
			int64_t TimeCentiseconds = static_cast<int64_t>(PlayerTimeSeconds) * 100 + static_cast<int64_t>(PlayerTimeMillis) / 10;
			str_time(TimeCentiseconds, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "%s%s", PlayerTimeSeconds > 3600 ? "" : "   ", aTime);
			TextRender()->Text(53, 82, 6, aBuf, -1.0f);
		}
	}
	else
	{
		const float PlayerRecord = m_aPlayerRecord[g_Config.m_ClDummy];
		if(PlayerRecord > 0.0f)
		{
			char aBuf[64];
			TextRender()->Text(5, 82, 6, Localize("Personal best:"), -1.0f);
			char aTime[32];
			str_time_float(PlayerRecord, ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
			str_format(aBuf, sizeof(aBuf), "%s%s", PlayerRecord > 3600 ? "" : "   ", aTime);
			TextRender()->Text(53, 82, 6, aBuf, -1.0f);
		}
	}
}
