#include <game/client/components/menus.h>
#include <game/client/gameclient.h>

#include <engine/shared/config.h>

#include <game/localization.h>

void CMenus::CreateIdentityKey()
{
	if(g_Config.m_ClZzDevIdentityId[0] == '\0')
	{
		PopupMessage(Localize("Identity"),
			Localize("Enter an Identity ID before generating a key."),
			Localize("Ok"));
		return;
	}
	char aError[128];
	if(!GameClient()->m_DeveloperIdentity.GenerateKey(aError, sizeof(aError)))
	{
		PopupMessage(Localize("Identity key error"), aError, Localize("Ok"));
		return;
	}
	g_Config.m_ClZzDevIdentityPublish = 0;
	PopupMessage(Localize("Identity key created"),
		Localize("Private key saved. Badge publishing is disabled until "
			 "registration."),
		Localize("Ok"));
}

void CMenus::PopupConfirmReplaceIdentityKey() { CreateIdentityKey(); }
