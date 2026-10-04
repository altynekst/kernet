/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_VERSION_H
#define GAME_VERSION_H

#include <game/client/components/bestclient/version.h>

// ddnet
#define GAME_NAME "DDNet"
#define DDNET_VERSION_NUMBER 19080
extern const char *GIT_SHORTREV_HASH;
#ifndef GAME_RELEASE_VERSION_INTERNAL
#define GAME_RELEASE_VERSION_INTERNAL 19.8
#endif
#define GAME_RELEASE_VERSION STRINGIFY(GAME_RELEASE_VERSION_INTERNAL)

// teeworlds
#define CLIENT_VERSION7 0x0705
#define GAME_VERSION "0.6.4, " GAME_RELEASE_VERSION
#define GAME_NETVERSION "0.6 626fce9a778df4d4"
#define GAME_NETVERSION7 "0.7 802f1be60a05665f"

// TClient
#ifndef TCLIENT_VERSION
#define TCLIENT_VERSION "10.8.7"
#endif

// BestClient identity
#ifndef UI_CLIENT_NAME
#define UI_CLIENT_NAME "BestClient"
#endif
#ifndef UI_CLIENT_RELEASE_VERSION
#define UI_CLIENT_RELEASE_VERSION BESTCLIENT_VERSION
#endif

// Server/network identity (sent to servers)
#ifndef SERVER_CLIENT_NAME
#define SERVER_CLIENT_NAME "BestClient"
#endif
#ifndef SERVER_CLIENT_RELEASE_VERSION
#define SERVER_CLIENT_RELEASE_VERSION BESTCLIENT_VERSION
#endif

#ifndef BestClient_VERSION
#define BestClient_VERSION BESTCLIENT_VERSION
#endif

// Legacy macros used throughout the codebase. Default them to the UI identity.
#define CLIENT_NAME UI_CLIENT_NAME
#define CLIENT_RELEASE_VERSION UI_CLIENT_RELEASE_VERSION

#endif
