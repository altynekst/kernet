#include "zz_visual_effects.h"

#include <base/color.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>

#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include "zz_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
	float Hash01(uint32_t Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7feb352dU;
		Value ^= Value >> 15;
		Value *= 0x846ca68bU;
		Value ^= Value >> 16;
		return (Value & 0x00ffffffU) / 16777215.0f;
	}

	float SmoothFactor(float DeltaTime, float Response)
	{
		return 1.0f - std::exp(-std::clamp(DeltaTime, 0.0f, 0.05f) * Response);
	}
} // namespace

void CZZWorldPostProcess::OnRender()
{
	const bool VideoRendering =
#if defined(CONF_VIDEORECORDER)
		Client()->State() == IClient::STATE_DEMOPLAYBACK &&
		IVideo::Current() && IVideo::Current()->IsRecording();
#else
		false;
#endif
	if((Client()->State() != IClient::STATE_ONLINE && !VideoRendering) ||
		!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzPostProcessing ||
		g_Config.m_ClZzVisualEffectsIntensity <= 0 ||
		g_Config.m_ClZzVisualPreset == 3)
		return;

	const float Strength =
		std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	float BloomIntensity = 0.24f;
	float BloomThreshold = 0.82f;
	float BloomScatter = 0.54f;
	float Exposure = 0.99f;
	float Saturation = 1.025f;
	float Contrast = 1.018f;
	float Warmth = 0.04f;
	float Vignette = 0.025f;
	float LightDepth = 0.035f;
	float Sharpening = 0.18f;
	int BlurPasses = 1;

	if(g_Config.m_ClZzVisualPreset == 1)
	{
		BloomIntensity = 0.44f;
		BloomThreshold = 0.68f;
		BloomScatter = 0.68f;
		Exposure = 1.0f;
		Saturation = 1.065f;
		Contrast = 1.045f;
		Warmth = 0.11f;
		Vignette = 0.045f;
		LightDepth = 0.075f;
		Sharpening = 0.27f;
		BlurPasses = 2;
	}
	else if(g_Config.m_ClZzVisualPreset == 2)
	{
		BloomIntensity = 0.64f;
		BloomThreshold = 0.56f;
		BloomScatter = 0.80f;
		Exposure = 1.035f;
		Saturation = 1.12f;
		Contrast = 1.075f;
		Warmth = 0.17f;
		Vignette = 0.065f;
		LightDepth = 0.115f;
		Sharpening = 0.36f;
		BlurPasses = 3;
	}

	BloomIntensity *= Strength;
	BloomThreshold = mix(0.92f, BloomThreshold, Strength);
	const float UserSpread =
		std::clamp(g_Config.m_ClZzPostBloomSpread / 100.0f, 0.2f, 1.0f);
	BloomScatter = std::clamp(
		BloomScatter * mix(0.72f, 1.18f, UserSpread), 0.2f, 1.0f);
	Exposure *= g_Config.m_ClZzPostExposure / 100.0f;
	Saturation = mix(1.0f, Saturation, Strength);
	Contrast = mix(1.0f, Contrast, Strength);
	Warmth *= Strength;
	Vignette *= Strength;
	const bool DynamicLighting = g_Config.m_ClZzVisualDynamicLights != 0;
	const float AmbientLight = DynamicLighting ?
		1.0f - LightDepth *
			(g_Config.m_ClZzPostLightDepth / 100.0f) * Strength :
		1.0f;
	Sharpening *= Strength;
	Graphics()->PostProcess(BloomIntensity, BloomThreshold, BloomScatter,
		Exposure, Saturation, Contrast, Warmth, Vignette, AmbientLight,
		Sharpening, BlurPasses);
}

void CZZVisualEffects::OnReset()
{
	m_PaletteInitialized = false;
	m_CursorTrailInitialized = false;
	m_NextClickBurst = 0;
	for(SClickBurst &Burst : m_aClickBursts)
		Burst.m_Age = -1.0f;
	m_FreezeAmount = 0.0f;
	m_FreezeImpact = 0.0f;
	m_FreezePulseElapsed = 0.0f;
	m_WasFrozen = false;
	m_LastVisualPreset = -1;
	m_FeatureWatchInitialized = false;
	m_NextFeatureToast = 0;
	for(SFeatureToast &Toast : m_aFeatureToasts)
		Toast.m_Age = -1.0f;
}

bool CZZVisualEffects::OnInput(const IInput::CEvent &Event)
{
	if(Event.m_Key != KEY_MOUSE_1 || (Event.m_Flags & IInput::FLAG_PRESS) == 0 ||
		!g_Config.m_ClZzVisualEffects || !g_Config.m_ClZzClickBursts)
		return false;
	const bool MenuActive = GameClient()->m_Menus.IsActive();
	if(!MenuActive && g_Config.m_ClZzClickGui == 0)
		return false;

	const float ScreenW = (float)Graphics()->ScreenWidth();
	const float ScreenH = (float)Graphics()->ScreenHeight();
	if(ScreenW <= 1.0f || ScreenH <= 1.0f)
		return false;
	vec2 Pos;
	if(MenuActive)
	{
		const CUIRect *pUiScreen = Ui()->Screen();
		if(pUiScreen->w <= 1.0f || pUiScreen->h <= 1.0f)
			return false;
		Pos = (Ui()->MousePos() - vec2(pUiScreen->x, pUiScreen->y)) /
		      vec2(pUiScreen->w, pUiScreen->h) * vec2(ScreenW, ScreenH);
	}
	else
	{
		const vec2 WindowSize((float)Graphics()->WindowWidth(),
			(float)Graphics()->WindowHeight());
		if(WindowSize.x <= 1.0f || WindowSize.y <= 1.0f)
			return false;
		Pos = Input()->NativeMousePos() / WindowSize * vec2(ScreenW, ScreenH);
	}

	SClickBurst &Burst = m_aClickBursts[m_NextClickBurst];
	Burst.m_Pos = Pos;
	Burst.m_Age = 0.0f;
	Burst.m_Rotation = Hash01((uint32_t)m_NextClickBurst * 101U +
				   (uint32_t)(LocalTime() * 1000.0f)) *
			   2.0f * pi;
	m_NextClickBurst = (m_NextClickBurst + 1) % CLICK_BURST_COUNT;
	return false;
}

void CZZVisualEffects::RenderMenuParticles(float ScreenW, float ScreenH,
	float Time, float Strength)
{
	if(!g_Config.m_ClZzMenuParticles || g_Config.m_ClZzMenuParticleAmount <= 0)
		return;

	IGraphics::CQuadItem aAccentParticles[40];
	IGraphics::CQuadItem aAltParticles[40];
	IGraphics::CLineItem aTrails[24];
	int NumAccent = 0;
	int NumAlt = 0;
	int NumTrails = 0;
	const int ParticleCount = 12 + g_Config.m_ClZzMenuParticleAmount * 68 / 100;
	for(int i = 0; i < ParticleCount; ++i)
	{
		const float SeedX = Hash01((uint32_t)i * 37U + 5U);
		const float SeedY = Hash01((uint32_t)i * 61U + 17U);
		const float SeedSpeed = Hash01((uint32_t)i * 89U + 23U);
		const float Speed = 8.0f + SeedSpeed * 22.0f;
		const float Direction = (i % 3) == 0 ? -1.0f : 1.0f;
		const float TravelH = ScreenH + 40.0f;
		float Y = fmodf(SeedY * TravelH + Time * Speed, TravelH);
		if(Direction < 0.0f)
			Y = TravelH - Y;
		Y -= 20.0f;
		const float Wave =
			sinf(Time * (0.30f + SeedSpeed * 0.55f) + SeedY * 14.0f) *
			(9.0f + SeedSpeed * 22.0f);
		const float X = SeedX * ScreenW + Wave;
		const float Size = 1.2f + Hash01((uint32_t)i * 113U + 31U) * 3.2f;
		const IGraphics::CQuadItem Particle(X, Y, Size, Size);
		if((i & 1) == 0 && NumAccent < (int)std::size(aAccentParticles))
			aAccentParticles[NumAccent++] = Particle;
		else if(NumAlt < (int)std::size(aAltParticles))
			aAltParticles[NumAlt++] = Particle;
		if(i % 4 == 0 && NumTrails < (int)std::size(aTrails))
			aTrails[NumTrails++] = IGraphics::CLineItem(
				X + Size * 0.5f, Y, X + Size * 0.5f - Wave * 0.035f,
				Y - Direction * (5.0f + SeedSpeed * 8.0f));
	}

	Graphics()->QuadsBegin();
	Graphics()->SetColor(m_Accent.WithAlpha(0.18f * Strength));
	Graphics()->QuadsDrawTL(aAccentParticles, NumAccent);
	Graphics()->SetColor(m_AccentAlt.WithAlpha(0.14f * Strength));
	Graphics()->QuadsDrawTL(aAltParticles, NumAlt);
	Graphics()->QuadsEnd();
	if(NumTrails > 0)
	{
		Graphics()->LinesBegin();
		Graphics()->SetColor(m_AccentAlt.WithAlpha(0.12f * Strength));
		Graphics()->LinesDraw(aTrails, NumTrails);
		Graphics()->LinesEnd();
	}
}

void CZZVisualEffects::RenderCursorTrail(float ScreenW, float ScreenH,
	float Time, float Strength)
{
	if(!g_Config.m_ClZzCursorTrail)
	{
		m_CursorTrailInitialized = false;
		return;
	}

	vec2 MousePos;
	if(GameClient()->m_Menus.IsActive())
	{
		const CUIRect *pUiScreen = Ui()->Screen();
		if(pUiScreen->w <= 1.0f || pUiScreen->h <= 1.0f)
			return;
		MousePos = (Ui()->MousePos() - vec2(pUiScreen->x, pUiScreen->y)) /
			   vec2(pUiScreen->w, pUiScreen->h) * vec2(ScreenW, ScreenH);
	}
	else
	{
		const vec2 WindowSize((float)Graphics()->WindowWidth(),
			(float)Graphics()->WindowHeight());
		if(WindowSize.x <= 1.0f || WindowSize.y <= 1.0f)
			return;
		MousePos = Input()->NativeMousePos() / WindowSize * vec2(ScreenW, ScreenH);
	}
	const int TrailPoints =
		10 + g_Config.m_ClZzCursorTrailLength * (CURSOR_TRAIL_POINTS - 10) / 100;
	if(!m_CursorTrailInitialized || distance(MousePos, m_aCursorTrail[0]) >
						std::max(ScreenW, ScreenH) * 0.45f)
	{
		std::fill(std::begin(m_aCursorTrail), std::end(m_aCursorTrail), MousePos);
		m_CursorTrailInitialized = true;
		return;
	}

	m_aCursorTrail[0] = MousePos;
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	for(int i = 1; i < CURSOR_TRAIL_POINTS; ++i)
	{
		const float Response = 46.0f - std::min(i, 40) * 0.45f;
		m_aCursorTrail[i] += (m_aCursorTrail[i - 1] - m_aCursorTrail[i]) *
				     SmoothFactor(DeltaTime, Response);
	}

	float TotalLength = 0.0f;
	for(int i = 1; i < TrailPoints; ++i)
		TotalLength += distance(m_aCursorTrail[i - 1], m_aCursorTrail[i]);
	const float Energy = std::clamp(TotalLength / 42.0f, 0.0f, 1.0f);
	if(Energy <= 0.01f)
		return;

	vec2 aNormals[CURSOR_TRAIL_POINTS];
	float aFades[CURSOR_TRAIL_POINTS];
	for(int Index = 0; Index < TrailPoints; ++Index)
	{
		const int Prev = std::max(0, Index - 1);
		const int Next = std::min(TrailPoints - 1, Index + 1);
		const vec2 Tangent = m_aCursorTrail[Prev] - m_aCursorTrail[Next];
		const float TangentLength = length(Tangent);
		if(TangentLength <= 0.001f)
			aNormals[Index] = vec2(0.0f, 1.0f);
		else
		{
			const vec2 Direction = Tangent / TangentLength;
			aNormals[Index] = vec2(-Direction.y, Direction.x);
		}
		aFades[Index] = std::pow(1.0f - (float)Index / (TrailPoints - 1), 1.15f);
	}

	IGraphics::CFreeformItem aGlow[CURSOR_TRAIL_POINTS];
	IGraphics::CFreeformItem aCore[CURSOR_TRAIL_POINTS];
	int NumSegments = 0;
	const float BaseHalfWidth = 0.75f + g_Config.m_ClZzCursorTrailSize * 0.0525f;
	for(int i = 0; i < TrailPoints - 1; ++i)
	{
		const vec2 Point = m_aCursorTrail[i];
		const vec2 NextPoint = m_aCursorTrail[i + 1];
		if(distance(Point, NextPoint) <= 0.04f)
			continue;
		const float Fade = aFades[i];
		const float NextFade = aFades[i + 1];
		const vec2 Normal = aNormals[i];
		const vec2 NextNormal = aNormals[i + 1];
		const float Width = BaseHalfWidth * Fade;
		const float NextWidth = BaseHalfWidth * NextFade;
		aGlow[NumSegments] = IGraphics::CFreeformItem(
			NextPoint + NextNormal * NextWidth * 2.25f,
			NextPoint - NextNormal * NextWidth * 2.25f,
			Point + Normal * Width * 2.25f, Point - Normal * Width * 2.25f);
		aCore[NumSegments] = IGraphics::CFreeformItem(
			NextPoint + NextNormal * NextWidth, NextPoint - NextNormal * NextWidth,
			Point + Normal * Width, Point - Normal * Width);
		++NumSegments;
	}
	if(NumSegments == 0)
		return;

	IGraphics::CQuadItem aAccentSparks[12];
	IGraphics::CQuadItem aAltSparks[12];
	int NumAccentSparks = 0;
	int NumAltSparks = 0;
	const int SparkCount = g_Config.m_ClZzCursorTrailParticles * 24 / 100;
	for(int i = 0; i < SparkCount; ++i)
	{
		const int Index =
			std::clamp(2 + i * (TrailPoints - 4) / std::max(1, SparkCount), 2,
				TrailPoints - 2);
		const float LocalMovement =
			distance(m_aCursorTrail[Index - 1], m_aCursorTrail[Index + 1]);
		if(LocalMovement <= 0.18f)
			continue;
		const float Fade = 1.0f - (float)Index / (TrailPoints - 1);
		const float Seed = Hash01((uint32_t)i * 71U + 19U);
		const float Jitter = sinf(Time * (4.0f + Seed * 3.0f) + Seed * 18.0f) *
				     BaseHalfWidth * (0.35f + Seed * 0.85f);
		const vec2 SparkPos = m_aCursorTrail[Index] + aNormals[Index] * Jitter;
		const float Size = (0.75f + Seed * 1.7f) * (0.45f + Fade) *
				   (0.75f + g_Config.m_ClZzCursorTrailSize / 180.0f);
		const IGraphics::CQuadItem Spark(SparkPos.x - Size * 0.5f,
			SparkPos.y - Size * 0.5f, Size, Size);
		if((i & 1) == 0 && NumAccentSparks < (int)std::size(aAccentSparks))
			aAccentSparks[NumAccentSparks++] = Spark;
		else if(NumAltSparks < (int)std::size(aAltSparks))
			aAltSparks[NumAltSparks++] = Spark;
	}

	Graphics()->QuadsBegin();
	Graphics()->SetColor(m_Accent.WithAlpha(0.10f * Strength * Energy));
	Graphics()->QuadsDrawFreeform(aGlow, NumSegments);
	Graphics()->SetColor(ZZThemeLerp(m_Accent, m_AccentAlt, 0.42f)
			.WithAlpha(0.40f * Strength * Energy));
	Graphics()->QuadsDrawFreeform(aCore, NumSegments);
	Graphics()->SetColor(m_Accent.WithAlpha(0.52f * Strength * Energy));
	Graphics()->QuadsDrawTL(aAccentSparks, NumAccentSparks);
	Graphics()->SetColor(m_AccentAlt.WithAlpha(0.42f * Strength * Energy));
	Graphics()->QuadsDrawTL(aAltSparks, NumAltSparks);
	Graphics()->QuadsEnd();
}

void CZZVisualEffects::RenderClickBursts(float ScreenW, float ScreenH,
	float Strength)
{
	if(!g_Config.m_ClZzClickBursts)
	{
		for(SClickBurst &Burst : m_aClickBursts)
			Burst.m_Age = -1.0f;
		return;
	}
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	const float BurstStrength =
		Strength *
		std::clamp(g_Config.m_ClZzClickBurstStrength / 100.0f, 0.1f, 1.0f);
	const float SpeedScale =
		std::clamp(g_Config.m_ClZzAnimationSpeed / 100.0f, 0.25f, 2.0f);
	const float SizeScale = 0.90f + g_Config.m_ClZzClickBurstStrength * 0.0025f;
	constexpr float Duration = 0.68f;

	Graphics()->LinesBegin();
	for(int BurstIndex = 0; BurstIndex < CLICK_BURST_COUNT; ++BurstIndex)
	{
		SClickBurst &Burst = m_aClickBursts[BurstIndex];
		if(Burst.m_Age < 0.0f)
			continue;
		Burst.m_Age += DeltaTime * SpeedScale;
		const float Progress = Burst.m_Age / Duration;
		if(Progress >= 1.0f)
		{
			Burst.m_Age = -1.0f;
			continue;
		}
		const float Remaining = 1.0f - Progress;
		const float Ease = 1.0f - Remaining * Remaining * Remaining;
		const float Alpha = Remaining * Remaining * BurstStrength;
		const float Radius = (5.0f + Ease * 37.0f) * SizeScale;
		IGraphics::CLineItem aOuterRing[16];
		for(int i = 0; i < (int)std::size(aOuterRing); ++i)
		{
			const float Step = 2.0f * pi / std::size(aOuterRing);
			const float A0 = Burst.m_Rotation + i * Step + Progress * 0.22f;
			const float A1 = A0 + Step * (0.48f + Progress * 0.30f);
			const vec2 P0 = Burst.m_Pos + direction(A0) * Radius;
			const vec2 P1 = Burst.m_Pos + direction(A1) * Radius;
			aOuterRing[i] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
		}
		Graphics()->SetColor(
			(BurstIndex & 1 ? m_AccentAlt : m_Accent).WithAlpha(0.82f * Alpha));
		Graphics()->LinesDraw(aOuterRing, std::size(aOuterRing));

		const float InnerProgress =
			std::clamp((Progress - 0.07f) / 0.93f, 0.0f, 1.0f);
		const float InnerRemaining = 1.0f - InnerProgress;
		const float InnerEase =
			1.0f - InnerRemaining * InnerRemaining * InnerRemaining;
		const float InnerRadius = (3.0f + InnerEase * 26.0f) * SizeScale;
		IGraphics::CLineItem aInnerRing[12];
		for(int i = 0; i < (int)std::size(aInnerRing); ++i)
		{
			const float Step = 2.0f * pi / std::size(aInnerRing);
			const float A0 = -Burst.m_Rotation + i * Step - InnerProgress * 0.38f;
			const float A1 = A0 + Step * 0.62f;
			aInnerRing[i] =
				IGraphics::CLineItem(Burst.m_Pos + direction(A0) * InnerRadius,
					Burst.m_Pos + direction(A1) * InnerRadius);
		}
		Graphics()->SetColor((BurstIndex & 1 ? m_Accent : m_AccentAlt)
				.WithAlpha(0.52f * Alpha * InnerProgress));
		Graphics()->LinesDraw(aInnerRing, std::size(aInnerRing));

		IGraphics::CLineItem aRays[10];
		const float RayLife = sinf(Progress * pi);
		for(int i = 0; i < (int)std::size(aRays); ++i)
		{
			const float A = Burst.m_Rotation + i * 2.0f * pi / std::size(aRays) +
					Progress * 0.35f;
			const vec2 Dir = direction(A);
			const vec2 P0 = Burst.m_Pos + Dir * (Radius * (0.45f + Progress * 0.18f));
			const vec2 P1 =
				Burst.m_Pos + Dir * (Radius + (7.0f + RayLife * 8.0f) * SizeScale);
			aRays[i] = IGraphics::CLineItem(P0.x, P0.y, P1.x, P1.y);
		}
		Graphics()->SetColor((BurstIndex & 1 ? m_Accent : m_AccentAlt)
				.WithAlpha(0.56f * Alpha * RayLife));
		Graphics()->LinesDraw(aRays, std::size(aRays));
	}
	Graphics()->LinesEnd();

	Graphics()->QuadsBegin();
	for(int BurstIndex = 0; BurstIndex < CLICK_BURST_COUNT; ++BurstIndex)
	{
		const SClickBurst &Burst = m_aClickBursts[BurstIndex];
		if(Burst.m_Age < 0.0f)
			continue;
		const float Progress = std::clamp(Burst.m_Age / Duration, 0.0f, 1.0f);
		const float Remaining = 1.0f - Progress;
		const float Alpha = Remaining * Remaining * BurstStrength;
		const float Ease = 1.0f - Remaining * Remaining * Remaining;
		const float Flash = std::clamp(1.0f - Progress * 3.8f, 0.0f, 1.0f);
		Graphics()->SetColor(ZZThemeLerp(m_Accent, m_AccentAlt, 0.5f)
				.WithAlpha(0.16f * Flash * BurstStrength));
		Graphics()->DrawCircle(Burst.m_Pos.x, Burst.m_Pos.y,
			(7.0f + Ease * 12.0f) * SizeScale, 16);
		Graphics()->SetColor((BurstIndex & 1 ? m_AccentAlt : m_Accent)
				.WithAlpha(0.30f * Flash * BurstStrength));
		Graphics()->DrawCircle(Burst.m_Pos.x, Burst.m_Pos.y,
			(3.0f + Flash * 4.5f) * SizeScale, 12);

		IGraphics::CQuadItem aSparks[12];
		for(int i = 0; i < (int)std::size(aSparks); ++i)
		{
			const float Seed =
				Hash01((uint32_t)BurstIndex * 79U + (uint32_t)i * 31U + 7U);
			const float A =
				Burst.m_Rotation + i * 2.0f * pi / std::size(aSparks) + Seed * 0.34f;
			const float Dist = (8.0f + Ease * (30.0f + Seed * 20.0f)) * SizeScale;
			const vec2 Pos = Burst.m_Pos + direction(A) * Dist;
			const float Size =
				(1.3f + Seed * 2.5f) * (0.48f + Remaining * 0.52f) * SizeScale;
			aSparks[i] = IGraphics::CQuadItem(Pos.x - Size * 0.5f,
				Pos.y - Size * 0.5f, Size, Size);
		}
		Graphics()->SetColor(
			(BurstIndex & 1 ? m_AccentAlt : m_Accent).WithAlpha(0.72f * Alpha));
		Graphics()->QuadsDrawTL(aSparks, std::size(aSparks));
	}
	Graphics()->QuadsEnd();
}

void CZZVisualEffects::ApplyVisualPreset()
{
	if(m_LastVisualPreset < 0)
	{
		m_LastVisualPreset = g_Config.m_ClZzVisualPreset;
		return;
	}
	if(m_LastVisualPreset == g_Config.m_ClZzVisualPreset)
		return;
	m_LastVisualPreset = g_Config.m_ClZzVisualPreset;
	switch(g_Config.m_ClZzVisualPreset)
	{
	case 0:
		g_Config.m_ClZzVisualEffectsIntensity = 38;
		g_Config.m_ClZzPostProcessing = 1;
		g_Config.m_ClZzPostExposure = 100;
		g_Config.m_ClZzPostBloomSpread = 52;
		g_Config.m_ClZzPostLightDepth = 12;
		g_Config.m_ClZzMenuParticleAmount = 18;
		g_Config.m_ClZzCursorTrailParticles = 18;
		g_Config.m_ClZzClickBurstStrength = 42;
		g_Config.m_ClZzVisualWorldAtmosphere = 0;
		g_Config.m_ClZzVisualOtherTrails = 0;
		g_Config.m_ClZzVisualDynamicLights = 0;
		g_Config.m_ClZzVisualPlayerPresence = 1;
		g_Config.m_ClZzVisualAmbientSweep = 0;
		g_Config.m_ClZzVisualFocusBrackets = 1;
		break;
	case 1:
		g_Config.m_ClZzVisualEffectsIntensity = 62;
		g_Config.m_ClZzPostProcessing = 1;
		g_Config.m_ClZzPostExposure = 100;
		g_Config.m_ClZzPostBloomSpread = 68;
		g_Config.m_ClZzPostLightDepth = 28;
		g_Config.m_ClZzMenuParticleAmount = 45;
		g_Config.m_ClZzCursorTrailParticles = 45;
		g_Config.m_ClZzClickBurstStrength = 62;
		g_Config.m_ClZzVisualWorldAtmosphere = 1;
		g_Config.m_ClZzVisualOtherTrails = 1;
		g_Config.m_ClZzVisualDynamicLights = 1;
		g_Config.m_ClZzVisualPlayerPresence = 1;
		g_Config.m_ClZzVisualAmbientSweep = 1;
		g_Config.m_ClZzVisualFocusBrackets = 1;
		break;
	case 2:
		g_Config.m_ClZzVisualEffectsIntensity = 88;
		g_Config.m_ClZzPostProcessing = 1;
		g_Config.m_ClZzPostExposure = 104;
		g_Config.m_ClZzPostBloomSpread = 82;
		g_Config.m_ClZzPostLightDepth = 48;
		g_Config.m_ClZzMenuParticleAmount = 78;
		g_Config.m_ClZzCursorTrailParticles = 82;
		g_Config.m_ClZzClickBurstStrength = 90;
		g_Config.m_ClZzVisualWorldAtmosphere = 1;
		g_Config.m_ClZzVisualOtherTrails = 1;
		g_Config.m_ClZzVisualDynamicLights = 1;
		g_Config.m_ClZzVisualPlayerPresence = 1;
		g_Config.m_ClZzVisualAmbientSweep = 1;
		g_Config.m_ClZzVisualFocusBrackets = 1;
		break;
	default:
		g_Config.m_ClZzVisualEffectsIntensity = 34;
		g_Config.m_ClZzPostProcessing = 0;
		g_Config.m_ClZzPostExposure = 100;
		g_Config.m_ClZzPostBloomSpread = 40;
		g_Config.m_ClZzPostLightDepth = 0;
		g_Config.m_ClZzMenuParticleAmount = 8;
		g_Config.m_ClZzCursorTrailParticles = 12;
		g_Config.m_ClZzClickBurstStrength = 35;
		g_Config.m_ClZzVisualWorldAtmosphere = 0;
		g_Config.m_ClZzVisualOtherTrails = 0;
		g_Config.m_ClZzVisualDynamicLights = 0;
		g_Config.m_ClZzVisualPlayerPresence = 0;
		g_Config.m_ClZzVisualAmbientSweep = 0;
		g_Config.m_ClZzVisualFocusBrackets = 0;
		break;
	}
}

void CZZVisualEffects::UpdateFeatureToasts(float DeltaTime)
{
	const int aCurrent[] = {
		g_Config.m_ClZzAimbotEnabled, g_Config.m_ClZzAutoUnfreeze,
		g_Config.m_ClZzBalanceBot, g_Config.m_ClZzFngAutoLaser,
		g_Config.m_ClZzFlyHelperEnabled};
	const char *apNames[] = {"Aimbot", "Auto unfreeze", "Balance bot",
		"FNG laser", "Fly helper"};
	if(!m_FeatureWatchInitialized)
	{
		std::copy(std::begin(aCurrent), std::end(aCurrent),
			std::begin(m_aFeatureWatch));
		m_FeatureWatchInitialized = true;
	}
	else if(g_Config.m_ClZzVisualFeatureToasts)
	{
		for(int i = 0; i < (int)std::size(aCurrent); ++i)
		{
			if(aCurrent[i] == m_aFeatureWatch[i])
				continue;
			SFeatureToast &Toast = m_aFeatureToasts[m_NextFeatureToast];
			str_copy(Toast.m_aTitle, apNames[i]);
			Toast.m_Enabled = aCurrent[i] != 0;
			Toast.m_Age = 0.0f;
			m_NextFeatureToast = (m_NextFeatureToast + 1) % FEATURE_TOAST_COUNT;
			m_aFeatureWatch[i] = aCurrent[i];
		}
	}
	else
	{
		std::copy(std::begin(aCurrent), std::end(aCurrent),
			std::begin(m_aFeatureWatch));
	}
	for(SFeatureToast &Toast : m_aFeatureToasts)
	{
		if(Toast.m_Age >= 0.0f)
		{
			Toast.m_Age += DeltaTime;
			if(Toast.m_Age > 3.6f)
				Toast.m_Age = -1.0f;
		}
	}
}

void CZZVisualEffects::RenderInterfaceHud(float ScreenW, float ScreenH,
	float Strength)
{
	(void)ScreenH;
	(void)Strength;
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	UpdateFeatureToasts(DeltaTime);

	int VisibleIndex = 0;
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	for(const SFeatureToast &Toast : m_aFeatureToasts)
	{
		if(Toast.m_Age < 0.0f)
			continue;
		const float FadeIn = std::clamp(Toast.m_Age / 0.2f, 0.0f, 1.0f);
		const float FadeOut = std::clamp((3.6f - Toast.m_Age) / 0.40f, 0.0f, 1.0f);
		const float Amount = FadeIn * FadeOut;
		char aText[64];
		str_format(aText, sizeof(aText), "%s %s", Toast.m_aTitle,
			Toast.m_Enabled ? "enabled" : "disabled");
		const float FontSize = 20.0f;
		const float Width = TextRender()->TextWidth(FontSize, aText);
		const float X = ScreenW - Width - 22.0f + (1.0f - Amount) * 18.0f;
		const float Y = 54.0f + VisibleIndex * 29.0f;
		const ColorRGBA StateColor = Toast.m_Enabled ? ColorRGBA(0.30f, 0.88f, 0.58f, 1.0f) : ColorRGBA(0.72f, 0.74f, 0.80f, 1.0f);
		TextRender()->TextColor(StateColor.WithAlpha(Amount));
		TextRender()->Text(X, Y, FontSize, aText);
		++VisibleIndex;
	}
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CZZVisualEffects::RenderAmbientOverlay(float ScreenW, float ScreenH,
	float Time, float Strength)
{
	if(g_Config.m_ClZzVisualAmbientSweep)
	{
		const float BandHeight = std::clamp(ScreenH * 0.085f, 48.0f, 92.0f);
		const float Travel = ScreenH + BandHeight;
		const float Y = fmodf(Time * 34.0f, Travel) - BandHeight;
		const float Alpha = 0.018f * Strength;
		Graphics()->DrawRect(0.0f, Y, ScreenW, BandHeight,
			m_Accent.WithAlpha(Alpha), IGraphics::CORNER_NONE,
			0.0f);
		Graphics()->DrawRect(0.0f, Y + BandHeight * 0.48f, ScreenW, 1.0f,
			m_AccentAlt.WithAlpha(Alpha * 2.2f),
			IGraphics::CORNER_NONE, 0.0f);
	}

	if(!g_Config.m_ClZzVisualFocusBrackets)
		return;
	const float Pulse = 0.5f + 0.5f * sinf(Time * 1.7f);
	const float Radius = 42.0f + Pulse * 4.0f;
	const float Segment = 11.0f;
	const float Thickness = 1.5f;
	const float CenterX = ScreenW * 0.5f;
	const float CenterY = ScreenH * 0.5f;
	const ColorRGBA Primary =
		m_Accent.WithAlpha((0.095f + Pulse * 0.035f) * Strength);
	const ColorRGBA Secondary =
		m_AccentAlt.WithAlpha((0.075f + Pulse * 0.025f) * Strength);

	Graphics()->DrawRect(CenterX - Radius, CenterY - Radius, Segment, Thickness,
		Primary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX - Radius, CenterY - Radius, Thickness, Segment,
		Primary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX + Radius - Segment, CenterY - Radius, Segment,
		Thickness, Secondary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX + Radius - Thickness, CenterY - Radius,
		Thickness, Segment, Secondary, IGraphics::CORNER_NONE,
		0.0f);
	Graphics()->DrawRect(CenterX - Radius, CenterY + Radius - Thickness, Segment,
		Thickness, Secondary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX - Radius, CenterY + Radius - Segment, Thickness,
		Segment, Secondary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX + Radius - Segment,
		CenterY + Radius - Thickness, Segment, Thickness,
		Primary, IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(CenterX + Radius - Thickness,
		CenterY + Radius - Segment, Thickness, Segment, Primary,
		IGraphics::CORNER_NONE, 0.0f);
}

void CZZVisualEffects::OnRender()
{
	const float ScreenW = (float)Graphics()->ScreenWidth();
	const float ScreenH = (float)Graphics()->ScreenHeight();
	if(ScreenW <= 0.0f || ScreenH <= 0.0f)
		return;

	if(!g_Config.m_ClZzVisualEffects ||
		g_Config.m_ClZzVisualEffectsIntensity <= 0)
	{
		m_CursorTrailInitialized = false;
		for(SClickBurst &Burst : m_aClickBursts)
			Burst.m_Age = -1.0f;
		return;
	}

	ApplyVisualPreset();

	const float Strength =
		std::clamp(g_Config.m_ClZzVisualEffectsIntensity / 100.0f, 0.0f, 1.0f);
	const float Time = LocalTime() * (g_Config.m_ClZzAnimationSpeed / 100.0f);
	const SZZThemePalette Palette = GetZZThemePalette(
		g_Config.m_ClZzTheme,
		color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true)));
	if(!m_PaletteInitialized)
	{
		m_Accent = Palette.m_Accent;
		m_AccentAlt = Palette.m_AccentAlt;
		m_PaletteInitialized = true;
	}
	else
	{
		const float Blend =
			SmoothFactor(Client()->RenderFrameTime(),
				7.0f * (g_Config.m_ClZzAnimationSpeed / 100.0f));
		m_Accent = ZZThemeLerp(m_Accent, Palette.m_Accent, Blend);
		m_AccentAlt = ZZThemeLerp(m_AccentAlt, Palette.m_AccentAlt, Blend);
	}
	Graphics()->MapScreen(0.0f, 0.0f, ScreenW, ScreenH);
	Graphics()->TextureClear();

	const bool MenuActive = GameClient()->m_Menus.IsActive();
	const bool ClickGuiActive = g_Config.m_ClZzClickGui != 0;
	if(MenuActive)
	{
		if(g_Config.m_ClZzVisualEffectsMenu)
		{
			RenderMenuParticles(ScreenW, ScreenH, Time, Strength);
			const float SegmentW = std::clamp(ScreenW * 0.18f, 120.0f, 360.0f);
			const float TravelW = ScreenW + SegmentW;
			const float TopX = fmodf(Time * 175.0f, TravelW) - SegmentW;
			const float BottomX = ScreenW - fmodf(Time * 135.0f, TravelW);
			const float SegmentH = std::clamp(ScreenH * 0.16f, 80.0f, 220.0f);
			const float TravelH = ScreenH + SegmentH;
			const float LeftY = fmodf(Time * 115.0f, TravelH) - SegmentH;
			const float RightY = ScreenH - fmodf(Time * 155.0f, TravelH);
			const float Pulse = 0.5f + 0.5f * sinf(Time * 3.2f);
			Graphics()->DrawRect(
				0.0f, 0.0f, ScreenW, 2.0f,
				m_Accent.WithAlpha((0.13f + Pulse * 0.08f) * Strength),
				IGraphics::CORNER_NONE, 0.0f);
			Graphics()->DrawRect(
				0.0f, ScreenH - 2.0f, ScreenW, 2.0f,
				m_AccentAlt.WithAlpha((0.10f + Pulse * 0.06f) * Strength),
				IGraphics::CORNER_NONE, 0.0f);
			Graphics()->DrawRect(TopX, 0.0f, SegmentW, 3.0f,
				m_Accent.WithAlpha(0.66f * Strength),
				IGraphics::CORNER_ALL, 1.5f);
			Graphics()->DrawRect(BottomX, ScreenH - 3.0f, SegmentW, 3.0f,
				m_AccentAlt.WithAlpha(0.52f * Strength),
				IGraphics::CORNER_ALL, 1.5f);
			Graphics()->DrawRect(0.0f, LeftY, 3.0f, SegmentH,
				m_AccentAlt.WithAlpha(0.42f * Strength),
				IGraphics::CORNER_ALL, 1.5f);
			Graphics()->DrawRect(ScreenW - 3.0f, RightY, 3.0f, SegmentH,
				m_Accent.WithAlpha(0.46f * Strength),
				IGraphics::CORNER_ALL, 1.5f);
		}
		RenderClickBursts(ScreenW, ScreenH, Strength);
		RenderCursorTrail(ScreenW, ScreenH, Time, Strength);
		return;
	}
	if(ClickGuiActive)
	{
		RenderClickBursts(ScreenW, ScreenH, Strength);
		RenderCursorTrail(ScreenW, ScreenH, Time, Strength);
		return;
	}
	m_CursorTrailInitialized = false;
	for(SClickBurst &Burst : m_aClickBursts)
		Burst.m_Age = -1.0f;

	const bool VideoRendering =
#if defined(CONF_VIDEORECORDER)
		Client()->State() == IClient::STATE_DEMOPLAYBACK &&
		IVideo::Current() && IVideo::Current()->IsRecording();
#else
		false;
#endif
	if((Client()->State() != IClient::STATE_ONLINE && !VideoRendering) ||
		!g_Config.m_ClZzVisualEffectsGameplay)
		return;

	const float Pulse = 0.5f + 0.5f * sinf(Time * 2.1f);
	const float Corner = std::clamp(
		std::min(ScreenW, ScreenH) * (0.032f + Pulse * 0.003f), 20.0f, 44.0f);
	const float Inset = 12.0f;
	const float ThinAlpha = (0.055f + Pulse * 0.045f) * Strength;
	const ColorRGBA Primary = m_Accent.WithAlpha(ThinAlpha);
	const ColorRGBA Secondary = m_AccentAlt.WithAlpha(ThinAlpha * 0.86f);
	Graphics()->DrawRect(Inset, Inset, Corner, 2.0f, Primary,
		IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(Inset, Inset, 2.0f, Corner, Primary,
		IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(ScreenW - Inset - Corner, Inset, Corner, 2.0f, Secondary,
		IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(ScreenW - Inset - 2.0f, Inset, 2.0f, Corner, Secondary,
		IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(Inset, ScreenH - Inset - 2.0f, Corner, 2.0f, Secondary,
		IGraphics::CORNER_NONE, 0.0f);
	Graphics()->DrawRect(ScreenW - Inset - Corner, ScreenH - Inset - 2.0f, Corner,
		2.0f, Primary, IGraphics::CORNER_NONE, 0.0f);
	const float CenterMarkWidth = std::clamp(ScreenW * 0.045f, 36.0f, 92.0f);
	Graphics()->DrawRect(
		(ScreenW - CenterMarkWidth) * 0.5f, Inset, CenterMarkWidth, 1.5f,
		m_Accent.WithAlpha(ThinAlpha * 0.78f), IGraphics::CORNER_ALL, 0.75f);
	Graphics()->DrawRect((ScreenW - CenterMarkWidth) * 0.5f,
		ScreenH - Inset - 1.5f, CenterMarkWidth, 1.5f,
		m_AccentAlt.WithAlpha(ThinAlpha * 0.68f),
		IGraphics::CORNER_ALL, 0.75f);
	RenderAmbientOverlay(ScreenW, ScreenH, Time, Strength);
	RenderInterfaceHud(ScreenW, ScreenH, Strength);

	bool Frozen = false;
	if(g_Config.m_ClZzVisualEffectsFreeze &&
		GameClient()->m_Snap.m_pLocalCharacter)
	{
		const int LocalId = GameClient()->m_Snap.m_LocalClientId;
		if(LocalId >= 0 && LocalId < MAX_CLIENTS)
		{
			Frozen = GameClient()->m_aClients[LocalId].m_DeepFrozen ||
				 GameClient()->m_aClients[LocalId].m_LiveFrozen ||
				 GameClient()->m_aClients[LocalId].m_FreezeEnd >
					 Client()->GameTick(g_Config.m_ClDummy) ||
				 GameClient()->m_aClients[LocalId].m_Predicted.m_IsInFreeze != 0;
		}
	}
	const float DeltaTime = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.05f);
	if(Frozen)
	{
		if(!m_WasFrozen)
		{
			m_FreezeImpact = 1.0f;
			m_FreezePulseElapsed = 0.0f;
		}
		else
		{
			m_FreezePulseElapsed += DeltaTime;
			if(m_FreezePulseElapsed >= 2.0f)
			{
				m_FreezeImpact = 1.0f;
				m_FreezePulseElapsed = fmodf(m_FreezePulseElapsed, 2.0f);
			}
		}
	}
	else
	{
		m_FreezePulseElapsed = 0.0f;
	}
	m_WasFrozen = Frozen;
	m_FreezeAmount += ((Frozen ? 1.0f : 0.0f) - m_FreezeAmount) *
			  SmoothFactor(DeltaTime, Frozen ? 10.0f : 5.0f);
	m_FreezeImpact += (0.0f - m_FreezeImpact) * SmoothFactor(DeltaTime, 4.5f);
	if(m_FreezeImpact < 0.001f)
		m_FreezeImpact = 0.0f;
	if(m_FreezeAmount <= 0.005f)
		return;

	const float FreezePulse = 0.5f + 0.5f * sinf(Time * 6.5f);
	const ColorRGBA IceColor =
		ZZThemeLerp(ColorRGBA(0.22f, 0.74f, 1.0f, 1.0f), m_AccentAlt, 0.16f);
	const ColorRGBA IceHighlight =
		ZZThemeLerp(ColorRGBA(0.82f, 0.96f, 1.0f, 1.0f), m_Accent, 0.08f);
	const float FreezeStrength = Strength * m_FreezeAmount;
	Graphics()->DrawRect(
		0.0f, 0.0f, ScreenW, ScreenH,
		IceColor.WithAlpha((0.018f + m_FreezeImpact * 0.075f) * FreezeStrength),
		IGraphics::CORNER_NONE, 0.0f);
	for(int Pass = 0; Pass < 3; ++Pass)
	{
		const float Edge = 3.0f + Pass * 4.0f;
		const float Alpha = (0.11f - Pass * 0.023f + FreezePulse * 0.055f +
					    m_FreezeImpact * 0.12f) *
				    FreezeStrength;
		const ColorRGBA EdgeColor =
			(Pass == 0 ? IceHighlight : IceColor).WithAlpha(Alpha);
		Graphics()->DrawRect(0.0f, Pass * 3.0f, ScreenW, Edge, EdgeColor,
			IGraphics::CORNER_NONE, 0.0f);
		Graphics()->DrawRect(0.0f, ScreenH - Pass * 3.0f - Edge, ScreenW, Edge,
			EdgeColor, IGraphics::CORNER_NONE, 0.0f);
		Graphics()->DrawRect(Pass * 3.0f, Edge, Edge, ScreenH - Edge * 2.0f,
			EdgeColor, IGraphics::CORNER_NONE, 0.0f);
		Graphics()->DrawRect(ScreenW - Pass * 3.0f - Edge, Edge, Edge,
			ScreenH - Edge * 2.0f, EdgeColor,
			IGraphics::CORNER_NONE, 0.0f);
	}

	IGraphics::CFreeformItem aIceShards[24];
	IGraphics::CLineItem aIceGlints[24];
	for(int i = 0; i < (int)std::size(aIceShards); ++i)
	{
		const float SeedA = Hash01((uint32_t)i * 67U + 11U);
		const float SeedB = Hash01((uint32_t)i * 103U + 37U);
		const int Side = i & 3;
		const float Inset = 5.0f + SeedB * 18.0f +
				    sinf(Time * (1.6f + SeedA) + SeedB * 12.0f) * 3.5f;
		vec2 Center;
		vec2 Inward;
		if(Side == 0)
		{
			Center = vec2(SeedA * ScreenW, Inset);
			Inward = vec2(0.0f, 1.0f);
		}
		else if(Side == 1)
		{
			Center = vec2(ScreenW - Inset, SeedA * ScreenH);
			Inward = vec2(-1.0f, 0.0f);
		}
		else if(Side == 2)
		{
			Center = vec2(SeedA * ScreenW, ScreenH - Inset);
			Inward = vec2(0.0f, -1.0f);
		}
		else
		{
			Center = vec2(Inset, SeedA * ScreenH);
			Inward = vec2(1.0f, 0.0f);
		}
		const vec2 Across(-Inward.y, Inward.x);
		const float Size = 2.0f + SeedB * 4.5f + m_FreezeImpact * 3.0f;
		aIceShards[i] = IGraphics::CFreeformItem(
			Center - Inward * Size * 1.4f, Center + Across * Size,
			Center - Across * Size, Center + Inward * Size * 1.8f);
		const vec2 GlintEnd =
			Center + Inward * (8.0f + SeedA * 13.0f + m_FreezeImpact * 12.0f);
		aIceGlints[i] = IGraphics::CLineItem(Center, GlintEnd);
	}
	Graphics()->QuadsBegin();
	Graphics()->SetColor(
		IceColor.WithAlpha((0.18f + FreezePulse * 0.14f) * FreezeStrength));
	Graphics()->QuadsDrawFreeform(aIceShards, std::size(aIceShards));
	Graphics()->QuadsEnd();
	Graphics()->LinesBegin();
	Graphics()->SetColor(IceHighlight.WithAlpha((0.16f + m_FreezeImpact * 0.38f) *
						    FreezeStrength));
	Graphics()->LinesDraw(aIceGlints, std::size(aIceGlints));
	Graphics()->LinesEnd();

	IGraphics::CLineItem aCracks[20];
	for(int i = 0; i < (int)std::size(aCracks); ++i)
	{
		const float Seed = Hash01((uint32_t)i * 149U + 53U);
		const int Side = i & 3;
		vec2 Start;
		vec2 Inward;
		if(Side == 0)
		{
			Start = vec2(Seed * ScreenW, 0.0f);
			Inward = vec2(0.0f, 1.0f);
		}
		else if(Side == 1)
		{
			Start = vec2(ScreenW, Seed * ScreenH);
			Inward = vec2(-1.0f, 0.0f);
		}
		else if(Side == 2)
		{
			Start = vec2(Seed * ScreenW, ScreenH);
			Inward = vec2(0.0f, -1.0f);
		}
		else
		{
			Start = vec2(0.0f, Seed * ScreenH);
			Inward = vec2(1.0f, 0.0f);
		}
		const vec2 Across(-Inward.y, Inward.x);
		const vec2 End = Start +
				 Inward * (16.0f + Seed * 36.0f + m_FreezeImpact * 22.0f) +
				 Across * sinf(Seed * 31.0f + Time) * 8.0f;
		aCracks[i] = IGraphics::CLineItem(Start, End);
	}
	Graphics()->LinesBegin();
	Graphics()->SetColor(IceHighlight.WithAlpha((0.10f + m_FreezeImpact * 0.24f) *
						    FreezeStrength));
	Graphics()->LinesDraw(aCracks, std::size(aCracks));
	Graphics()->LinesEnd();
}
