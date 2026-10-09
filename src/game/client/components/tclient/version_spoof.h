#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_VERSION_SPOOF_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_VERSION_SPOOF_H

class CVersionSpoof
{
public:
	static const char *GetFullVersionStr();
	static int GetDDNetVersion();
	static void GenerateRandomHash(char *pBuf, int BufSize);
	static void ApplyPreset(int Preset);
	static const char *GetPresetName(int Preset);
	static int GetPresetCount();
};

#endif
