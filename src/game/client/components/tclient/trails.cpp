#include "trails.h"

#include <base/math.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/components/tclient/zz_theme.h>

#include <algorithm>
#include <cmath>

bool CTrails::ShouldPredictPlayer(int ClientId)
{
	if(!GameClient()->Predict())
		return false;
	CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	if(GameClient()->Predict() && (ClientId == GameClient()->m_Snap.m_LocalClientId || (GameClient()->AntiPingPlayers() && !GameClient()->IsOtherTeam(ClientId))) && pChar)
		return true;
	return false;
}

void CTrails::ClearAllHistory()
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
		ClearHistory(i);
}
void CTrails::ClearHistory(int ClientId)
{
	for(int i = 0; i < 200; ++i)
		m_History[ClientId][i] = {{}, -1};
	m_HistoryValid[ClientId] = false;
}
void CTrails::OnReset()
{
	ClearAllHistory();
}

void CTrails::OnRender()
{
	if(!g_Config.m_TcTeeTrail)
		return;

	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	Graphics()->TextureClear();
	const SZZThemePalette ThemePalette = GetZZThemePalette(
		g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	const bool UseLiquidGlassStyle =
		g_Config.m_TcTeeTrailStyle == TRAILSTYLE_LIQUID_GLASS;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const bool Local = GameClient()->m_Snap.m_LocalClientId == ClientId;

		const bool ZoomAllowed = GameClient()->m_Camera.ZoomAllowed();
		if(!g_Config.m_TcTeeTrailOthers && !Local)
			continue;

		if(!Local && !ZoomAllowed)
			continue;

		if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		{
			if(m_HistoryValid[ClientId])
				ClearHistory(ClientId);
			continue;
		}
		else
			m_HistoryValid[ClientId] = true;

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;

		const bool PredictPlayer = ShouldPredictPlayer(ClientId);
		int StartTick;
		const int GameTick = Client()->GameTick(g_Config.m_ClDummy);
		const int PredTick = Client()->PredGameTick(g_Config.m_ClDummy);
		float IntraTick;
		if(PredictPlayer)
		{
			StartTick = PredTick;
			IntraTick = Client()->PredIntraGameTick(g_Config.m_ClDummy);
			if(g_Config.m_TcRemoveAnti)
			{
				StartTick = GameClient()->m_SmoothTick;
				IntraTick = GameClient()->m_SmoothIntraTick;
			}
			if(g_Config.m_TcUnpredOthersInFreeze && !Local && Client()->m_IsLocalFrozen)
			{
				StartTick = GameTick;
			}
		}
		else
		{
			StartTick = GameTick;
			IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
		}

		const vec2 CurServerPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
		const vec2 PrevServerPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev.m_Y);
		m_History[ClientId][GameTick % 200] = {
			mix(PrevServerPos, CurServerPos, IntraTick),
			GameTick,
		};

		// // NOTE: this is kind of a hack to fix 25tps. This fixes flickering when using the speed mode
		// m_History[ClientId][(GameTick + 1) % 200] = m_History[ClientId][GameTick % 200];
		// m_History[ClientId][(GameTick + 2) % 200] = m_History[ClientId][GameTick % 200];

		IGraphics::CLineItem LineItem;
		bool LineMode = g_Config.m_TcTeeTrailWidth == 0;
		const bool LiquidGlass = UseLiquidGlassStyle && !LineMode;

		float Alpha = g_Config.m_TcTeeTrailAlpha / 100.0f;
		// Taken from players.cpp
		if(ClientId == -2)
			Alpha *= g_Config.m_ClRaceGhostAlpha / 100.0f;
		else if(ClientId < 0 || GameClient()->IsOtherTeam(ClientId))
			Alpha *= g_Config.m_ClShowOthersAlpha / 100.0f;

		int TrailLength = g_Config.m_TcTeeTrailLength;
		float Width = g_Config.m_TcTeeTrailWidth;

		static std::vector<CTrailPart> s_Trail;
		static std::vector<int> s_ContinuousSegments;
		static std::vector<float> s_GlassTailAmounts;
		s_Trail.clear();
		s_ContinuousSegments.clear();

		// TODO: figure out why this is required
		if(!PredictPlayer)
			TrailLength += 2;
		bool TrailFull = false;
		// Fill trail list with initial positions
		for(int i = 0; i < TrailLength; i++)
		{
			CTrailPart Part;
			int PosTick = StartTick - i;
			// At the beginning of a connection there is no history before tick 0.
			// Do not turn a negative remainder into an out-of-bounds ring index.
			if(PosTick < 0)
				break;
			if(PredictPlayer)
			{
				if(GameClient()->m_aClients[ClientId].m_aPredTick[PosTick % 200] != PosTick)
					continue;
				Part.m_Pos = GameClient()->m_aClients[ClientId].m_aPredPos[PosTick % 200];
				if(i == TrailLength - 1)
					TrailFull = true;
			}
			else
			{
				if(m_History[ClientId][PosTick % 200].m_Tick != PosTick)
					continue;
				Part.m_Pos = m_History[ClientId][PosTick % 200].m_Pos;
				if(i == TrailLength - 2 || i == TrailLength - 3)
					TrailFull = true;
			}
			Part.m_UnmovedPos = Part.m_Pos;
			Part.m_Tick = PosTick;
			s_Trail.push_back(Part);
		}

		// Trim the ends if intratick is too big
		// this was not trivial to figure out
		int TrimTicks = (int)IntraTick;
		for(int i = 0; i < TrimTicks; i++)
			if((int)s_Trail.size() > 0)
				s_Trail.pop_back();

		// Stuff breaks if we have less than 3 points because we cannot calculate an angle between segments to preserve constant width
		// TODO: Pad the list with generated entries in the same direction as before
		if((int)s_Trail.size() < 3)
			continue;

		if(PredictPlayer)
			s_Trail.at(0).m_Pos = GameClient()->m_aClients[ClientId].m_RenderPos;
		else
			s_Trail.at(0).m_Pos = mix(PrevServerPos, CurServerPos, IntraTick);

		if(TrailFull)
			s_Trail.at(s_Trail.size() - 1).m_Pos = mix(s_Trail.at(s_Trail.size() - 1).m_Pos, s_Trail.at(s_Trail.size() - 2).m_Pos, std::fmod(IntraTick, 1.0f));

		// Set progress
		for(int i = 0; i < (int)s_Trail.size(); i++)
		{
			float Size = float(s_Trail.size() - 1 + TrimTicks);
			CTrailPart &Part = s_Trail.at(i);
			if(i == 0)
				Part.m_Progress = 0.0f;
			else if(i == (int)s_Trail.size() - 1)
				Part.m_Progress = 1.0f;
			else
				Part.m_Progress = ((float)i + IntraTick - 1.0f) / (Size - 1.0f);

			if(LiquidGlass)
			{
				// Glass deliberately follows the active KernelNet theme. Classic
				// trails retain all of their legacy colour modes below.
				Part.m_Col = ZZThemeLerp(ThemePalette.m_Accent,
					ThemePalette.m_AccentAlt,
					std::clamp(0.22f + Part.m_Progress * 0.52f, 0.0f, 1.0f));
			}
			else switch(g_Config.m_TcTeeTrailColorMode)
			{
			case COLORMODE_SOLID:
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcTeeTrailColor));
				break;
			case COLORMODE_TEE:
				if(TeeInfo.m_CustomColoredSkin)
					Part.m_Col = TeeInfo.m_ColorBody;
				else
					Part.m_Col = TeeInfo.m_BloodColor;
				break;
			case COLORMODE_RAINBOW:
			{
				float Cycle = (1.0f / TrailLength) * 0.5f;
				float Hue = std::fmod(((Part.m_Tick + 6361 * ClientId) % 1000000) * Cycle, 1.0f);
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, 0.5f));
				break;
			}
			case COLORMODE_SPEED:
			{
				float Speed = 0.0f;
				if(s_Trail.size() > 3)
				{
					if(i < 2)
						Speed = distance(s_Trail.at(i + 2).m_UnmovedPos, Part.m_UnmovedPos) / absolute(s_Trail.at(i + 2).m_Tick - Part.m_Tick);
					else
						Speed = distance(Part.m_UnmovedPos, s_Trail.at(i - 2).m_UnmovedPos) / absolute(Part.m_Tick - s_Trail.at(i - 2).m_Tick);
				}
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(65280 * ((int)(Speed * Speed / 12.5f) + 1)).UnclampLighting(ColorHSLA::DARKEST_LGT));
				break;
			}
			default:
				dbg_assert(false, "Invalid value for g_Config.m_TcTeeTrailColorMode");
				dbg_break();
			}

			Part.m_Col.a = Alpha;
			if(g_Config.m_TcTeeTrailFade)
				Part.m_Col.a *= 1.0 - Part.m_Progress;

			Part.m_Width = Width;
			if(g_Config.m_TcTeeTrailTaper)
				Part.m_Width = Width * (1.0 - Part.m_Progress);
		}

		// Remove duplicate elements (those with same Pos)
		auto NewEnd = std::unique(s_Trail.begin(), s_Trail.end());
		s_Trail.erase(NewEnd, s_Trail.end());

		if((int)s_Trail.size() < 3)
			continue;

		// Calculate the widths
		for(int i = 0; i < (int)s_Trail.size(); i++)
		{
			CTrailPart &Part = s_Trail.at(i);
			vec2 PrevPos;
			vec2 Pos = s_Trail.at(i).m_Pos;
			vec2 NextPos;

			if(i == 0)
			{
				vec2 Direction = normalize(s_Trail.at(i + 1).m_Pos - Pos);
				PrevPos = Pos - Direction;
			}
			else
				PrevPos = s_Trail.at(i - 1).m_Pos;

			if(i == (int)s_Trail.size() - 1)
			{
				vec2 Direction = normalize(Pos - s_Trail.at(i - 1).m_Pos);
				NextPos = Pos + Direction;
			}
			else
				NextPos = s_Trail.at(i + 1).m_Pos;

			vec2 NextDirection = normalize(NextPos - Pos);
			vec2 PrevDirection = normalize(Pos - PrevPos);

			vec2 Normal = vec2(-PrevDirection.y, PrevDirection.x);
			Part.m_Normal = Normal;
			vec2 Tangent = normalize(NextDirection + PrevDirection);
			if(Tangent == vec2(0.0f, 0.0f))
				Tangent = Normal;

			vec2 PerpVec = vec2(-Tangent.y, Tangent.x);
			Width = Part.m_Width;
			float ScaledWidth = Width / dot(Normal, PerpVec);
			float TopScaled = ScaledWidth;
			float BotScaled = ScaledWidth;
			if(dot(PrevDirection, Tangent) > 0.0f)
				TopScaled = std::min(Width * 3.0f, TopScaled);
			else
				BotScaled = std::min(Width * 3.0f, BotScaled);

			vec2 Top = Pos + PerpVec * TopScaled;
			vec2 Bot = Pos - PerpVec * BotScaled;
			Part.m_Top = Top;
			Part.m_Bot = Bot;

			// Bevel Cap
			if(dot(PrevDirection, NextDirection) < -0.25f)
			{
				Top = Pos + Tangent * Width;
				Bot = Pos - Tangent * Width;

				float Det = PrevDirection.x * NextDirection.y - PrevDirection.y * NextDirection.x;
				if(Det >= 0.0f)
				{
					Part.m_Top = Top;
					Part.m_Bot = Bot;
					if(i > 0)
						s_Trail.at(i).m_Flip = true;
				}
				else // <-Left Direction
				{
					Part.m_Top = Bot;
					Part.m_Bot = Top;
					if(i > 0)
						s_Trail.at(i).m_Flip = true;
				}
			}
		}

		auto SegmentIsContinuous = [&](int Index) {
			const CTrailPart &Part = s_Trail.at(Index);
			const CTrailPart &NextPart = s_Trail.at(Index + 1);
			const float Dist = distance(Part.m_UnmovedPos, NextPart.m_UnmovedPos);
			constexpr float MaxDiff = 120.0f;
			if(Index > 0)
			{
				const CTrailPart &PrevPart = s_Trail.at(Index - 1);
				const float PrevDist = distance(PrevPart.m_UnmovedPos, Part.m_UnmovedPos);
				if(absolute(Dist - PrevDist) > MaxDiff)
					return false;
			}
			if(Index < (int)s_Trail.size() - 2)
			{
				const CTrailPart &NextNextPart = s_Trail.at(Index + 2);
				const float NextDist = distance(NextPart.m_UnmovedPos, NextNextPart.m_UnmovedPos);
				if(absolute(Dist - NextDist) > MaxDiff)
					return false;
			}
			return true;
		};
		s_ContinuousSegments.reserve(s_Trail.size() - 1);
		for(int i = 0; i < (int)s_Trail.size() - 1; ++i)
			if(SegmentIsContinuous(i))
				s_ContinuousSegments.push_back(i);
		if(s_ContinuousSegments.empty())
			continue;

		if(!LiquidGlass)
		{
			if(LineMode)
				Graphics()->LinesBegin();
			else
				Graphics()->QuadsBegin();

			// Keep the classic trail rendering unchanged.
			for(const int i : s_ContinuousSegments)
			{
				const CTrailPart &Part = s_Trail.at(i);
				const CTrailPart &NextPart = s_Trail.at(i + 1);
				if(LineMode)
				{
					Graphics()->SetColor(Part.m_Col);
					LineItem = IGraphics::CLineItem(Part.m_Pos.x, Part.m_Pos.y, NextPart.m_Pos.x, NextPart.m_Pos.y);
					Graphics()->LinesDraw(&LineItem, 1);
				}
				else
				{
					vec2 Top = Part.m_Flip ? Part.m_Bot : Part.m_Top;
					vec2 Bot = Part.m_Flip ? Part.m_Top : Part.m_Bot;
					Graphics()->SetColor4(NextPart.m_Col, NextPart.m_Col, Part.m_Col, Part.m_Col);
					IGraphics::CFreeformItem FreeformItem(NextPart.m_Top, NextPart.m_Bot, Top, Bot);
					Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
				}
			}

			if(LineMode)
				Graphics()->LinesEnd();
			else
				Graphics()->QuadsEnd();
			continue;
		}

		const float GlassIntensity = std::clamp(g_Config.m_TcTeeTrailGlassIntensity / 100.0f, 0.0f, 1.0f);
		const float Refraction = std::clamp(g_Config.m_TcTeeTrailGlassRefraction / 100.0f, 0.0f, 1.0f);
		const int GlassQuality = std::clamp(g_Config.m_TcTeeTrailGlassQuality, (int)GLASSQUALITY_LOW, (int)GLASSQUALITY_HIGH);
		const float GlassTime = Client()->LocalTime() * 2.15f + ClientId * 0.47f;
		s_GlassTailAmounts.resize(s_Trail.size());
		for(int i = 0; i < (int)s_Trail.size(); ++i)
			s_GlassTailAmounts[i] = std::pow(
				std::clamp(1.0f - s_Trail[i].m_Progress, 0.0f, 1.0f), 1.18f);

		auto MixGlassColor = [&](int Index, ColorRGBA Target, float MixAmount, float AlphaScale, float Pulse = 1.0f) {
			const CTrailPart &Part = s_Trail[Index];
			const float Amount = std::clamp(MixAmount, 0.0f, 1.0f);
			return ColorRGBA(
				mix(Part.m_Col.r, Target.r, Amount),
				mix(Part.m_Col.g, Target.g, Amount),
				mix(Part.m_Col.b, Target.b, Amount),
				std::clamp(Part.m_Col.a * s_GlassTailAmounts[Index] * AlphaScale * GlassIntensity * Pulse, 0.0f, 1.0f));
		};
		auto ScaledEdges = [](const CTrailPart &Part, float Scale, float Offset, vec2 &Top, vec2 &Bot) {
			const vec2 Shift = Part.m_Normal * Offset;
			Top = Part.m_Pos + (Part.m_Top - Part.m_Pos) * Scale + Shift;
			Bot = Part.m_Pos + (Part.m_Bot - Part.m_Pos) * Scale + Shift;
		};
		auto DrawGlassRibbon = [&](float Scale, float OffsetFactor, ColorRGBA Target, float MixAmount, float AlphaScale, bool Additive, bool AnimatedOffset, bool AnimatedPulse) {
			if(Additive)
				Graphics()->BlendAdditive();
			else
				Graphics()->BlendNormal();
			Graphics()->QuadsBegin();
			for(const int i : s_ContinuousSegments)
			{
				const CTrailPart &Part = s_Trail.at(i);
				const CTrailPart &NextPart = s_Trail.at(i + 1);
				auto OffsetFor = [&](const CTrailPart &TrailPart) {
					const float Wave = AnimatedOffset ? std::sin(GlassTime + TrailPart.m_Progress * 7.0f) : 1.0f;
					return TrailPart.m_Width * OffsetFactor * Refraction * Wave;
				};
				auto PulseFor = [&](const CTrailPart &TrailPart) {
					return AnimatedPulse ? 0.62f + 0.38f * std::sin(GlassTime * 1.35f + TrailPart.m_Progress * 11.0f) : 1.0f;
				};
				vec2 Top, Bot, NextTop, NextBot;
				ScaledEdges(Part, Scale, OffsetFor(Part), Top, Bot);
				ScaledEdges(NextPart, Scale, OffsetFor(NextPart), NextTop, NextBot);
				if(Part.m_Flip)
					std::swap(Top, Bot);
				const ColorRGBA PartColor = MixGlassColor(i, Target, MixAmount,
					AlphaScale, PulseFor(Part));
				const ColorRGBA NextColor = MixGlassColor(i + 1, Target, MixAmount,
					AlphaScale, PulseFor(NextPart));
				Graphics()->SetColor4(NextColor, NextColor, PartColor, PartColor);
				IGraphics::CFreeformItem FreeformItem(NextTop, NextBot, Top, Bot);
				Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
			}
			Graphics()->QuadsEnd();
			Graphics()->BlendNormal();
		};

		const ColorRGBA GlassShadow = ZZThemeLerp(ThemePalette.m_Background,
			ThemePalette.m_Panel, 0.38f);
		const ColorRGBA GlassHalo = ZZThemeLerp(ThemePalette.m_AccentAlt,
			ThemePalette.m_Text, 0.22f);
		const ColorRGBA GlassBody = ZZThemeLerp(ThemePalette.m_Accent,
			ThemePalette.m_Text, 0.38f);
		const ColorRGBA GlassLens = ZZThemeLerp(ThemePalette.m_AccentAlt,
			ThemePalette.m_Text, 0.62f);
		const ColorRGBA GlassRim = ThemePalette.m_Text;

		// A cool, displaced shadow gives the transparent ribbon apparent depth.
		if(GlassQuality >= GLASSQUALITY_MEDIUM)
		{
			DrawGlassRibbon(1.40f, 0.075f, GlassShadow, 0.78f, 0.13f, false,
				false, false);
			DrawGlassRibbon(1.24f, -0.025f, GlassHalo, 0.42f, 0.075f, true,
				false, false);
		}

		// The translucent body keeps the map visible and uses the selected theme.
		DrawGlassRibbon(1.0f, 0.0f, GlassBody, 0.32f, 0.29f, false, false,
			false);

		// A slowly moving inner lens makes the material feel fluid instead of flat.
		if(GlassQuality >= GLASSQUALITY_MEDIUM && Refraction > 0.001f)
			DrawGlassRibbon(0.62f, 0.20f, GlassLens, 0.56f, 0.105f, false, true,
				false);
		if(GlassQuality >= GLASSQUALITY_HIGH && Refraction > 0.001f)
			DrawGlassRibbon(0.16f, 0.50f, GlassRim, 0.82f, 0.16f, true, true,
				true);

		// Specular rims concentrate light at both glass edges.
		Graphics()->BlendAdditive();
		Graphics()->LinesBegin();
		for(const int i : s_ContinuousSegments)
		{
			const CTrailPart &Part = s_Trail.at(i);
			const CTrailPart &NextPart = s_Trail.at(i + 1);
			const float RimPulse = 0.78f + 0.22f * std::sin(GlassTime * 1.2f + Part.m_Progress * 8.0f);
			Graphics()->SetColor(MixGlassColor(i, GlassRim, 0.86f, 0.34f,
				RimPulse));
			LineItem = IGraphics::CLineItem(Part.m_Top, NextPart.m_Top);
			Graphics()->LinesDraw(&LineItem, 1);
			if(GlassQuality >= GLASSQUALITY_MEDIUM)
			{
				Graphics()->SetColor(MixGlassColor(i, ThemePalette.m_AccentAlt,
					0.52f, 0.15f));
				LineItem = IGraphics::CLineItem(Part.m_Bot, NextPart.m_Bot);
				Graphics()->LinesDraw(&LineItem, 1);
			}
		}
		Graphics()->LinesEnd();
		Graphics()->BlendNormal();
	}
}
