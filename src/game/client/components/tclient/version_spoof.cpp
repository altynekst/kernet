#include "version_spoof.h"

#include <base/system.h>

#include <engine/shared/config.h>

#include <game/version.h>

#include <algorithm>
#include <random>

struct SSpoofPreset
{
	const char *m_pName;
	const char *m_pFullStr;
	int m_DDNetVersion;
};

static const SSpoofPreset s_aSpoofPresets[] = {
	{"Custom",  "", 0},
	{"DDNet",   "DDNet 19.2 (ef67c53f23176082)",                     19020},
	{"TClient", "TClient 10.8.6 (82d946145072e18f9513ba2427538971)", 19080},
	{"Cactus",  "DDNet 19.2 (ef67c53f23176082)",                     19020},
	{"EClient", "E-Client v1.6.1 (11b25d0734e731f960ae2f0db49d37a8)", 19080},
	{"RClient", "RClient 2.2.2 (15457692727da55839c47254ca16b0e5)",  19080},
	{"FeX",     "FeX 6.1.19.3 (13d551bb30b4195a)",                   19030},
	{"StA",     "DDNet 17.4.2 (aa1ee6709d931d77)",                   17042},
};

int CVersionSpoof::GetPresetCount()
{
	return (int)(sizeof(s_aSpoofPresets) / sizeof(s_aSpoofPresets[0]));
}

const char *CVersionSpoof::GetPresetName(int Preset)
{
	if(Preset < 0 || Preset >= GetPresetCount())
		return "";
	return s_aSpoofPresets[Preset].m_pName;
}

const char *CVersionSpoof::GetFullVersionStr()
{
	if(!g_Config.m_TcSpoofVersion)
		return GAME_NAME " " GAME_RELEASE_VERSION;
	if(g_Config.m_TcSpoofFullStr[0] != '\0')
		return g_Config.m_TcSpoofFullStr;
	return GAME_NAME " " GAME_RELEASE_VERSION;
}

int CVersionSpoof::GetDDNetVersion()
{
	if(g_Config.m_TcSpoofVersion && g_Config.m_TcSpoofDDNetVersion > 0)
		return g_Config.m_TcSpoofDDNetVersion;
	return DDNET_VERSION_NUMBER;
}

void CVersionSpoof::GenerateRandomHash(char *pBuf, int BufSize)
{
	static const char s_aHex[] = "0123456789abcdef";
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> dist(0, 15);
	int Len = std::min(32, BufSize - 1);
	for(int i = 0; i < Len; i++)
		pBuf[i] = s_aHex[dist(gen)];
	pBuf[Len] = '\0';
}

void CVersionSpoof::ApplyPreset(int Preset)
{
	if(Preset < 0 || Preset >= GetPresetCount())
		return;
	const SSpoofPreset &P = s_aSpoofPresets[Preset];
	str_copy(g_Config.m_TcSpoofFullStr, P.m_pFullStr, sizeof(g_Config.m_TcSpoofFullStr));
	g_Config.m_TcSpoofDDNetVersion = P.m_DDNetVersion;
}
