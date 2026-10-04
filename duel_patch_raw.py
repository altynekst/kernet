import sys

gamecontext_cpp = 'src/game/server/gamecontext.cpp'
with open(gamecontext_cpp, 'r', encoding='utf-8') as f:
    text = f.read()

# 1. Add Call to TickDuels
tick_call = '	UpdatePlayerMaps();\n'
tick_duels = '	TickDuels();\n'
if tick_duels not in text:
    text = text.replace(tick_call, tick_call + tick_duels)

# 2. Add EndDuel on disconnect
drop_func = 'void CGameContext::OnClientDrop(int ClientId, const char *pReason)\n{\n'
drop_end = '''	if(m_apPlayers[ClientId] && m_apPlayers[ClientId]->m_InDuel)
	{
		int OpponentId = m_apPlayers[ClientId]->m_DuelOpponent;
		if(m_apPlayers[OpponentId] && m_apPlayers[OpponentId]->m_InDuel)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Дуэль: игрок '%s' ragequitнул! '%s' победил! Счёт: %d-%d", Server()->ClientName(ClientId), Server()->ClientName(OpponentId), m_apPlayers[OpponentId]->m_DuelScore, m_apPlayers[ClientId]->m_DuelScore);
			SendChat(-1, TEAM_ALL, aBuf);
			EndDuel(OpponentId, ClientId, OpponentId);
		}
	}
'''
if "ragequitнул" not in text:
    text = text.replace(drop_func, drop_func + drop_end)

# 3. Add Registering chat commands
reg_cmds = 'void CGameContext::RegisterChatCommands()\n{\n'
regs = '''	Console()->Register("duel", "s[player] ?s[side]", CFGFLAG_CHAT, ConDuel, this, "Challenge a player");
	Console()->Register("accept", "?s[player]", CFGFLAG_CHAT, ConAcceptDuel, this, "Accept a duel");\n'''
if "duel" not in text and 'Register("duel"' not in text:
    text = text.replace(reg_cmds, reg_cmds + regs)

# 4. Add Registering rcon commands
reg_rcons = 'void CGameContext::RegisterDDRaceCommands()\n{\n'
regs2 = '''	Console()->Register("setduelspawn_left1", "v[x] v[y]", CFGFLAG_SERVER, ConSetDuelSpawnLeft1, this, "Set duel left spawn 1");
	Console()->Register("setduelspawn_left2", "v[x] v[y]", CFGFLAG_SERVER, ConSetDuelSpawnLeft2, this, "Set duel left spawn 2");
	Console()->Register("setduelspawn_right1", "v[x] v[y]", CFGFLAG_SERVER, ConSetDuelSpawnRight1, this, "Set duel right spawn 1");
	Console()->Register("setduelspawn_right2", "v[x] v[y]", CFGFLAG_SERVER, ConSetDuelSpawnRight2, this, "Set duel right spawn 2");\n'''
if "setduelspawn_left1" not in text:
    text = text.replace(reg_rcons, reg_rcons + regs2)

with open(gamecontext_cpp, 'w', encoding='utf-8') as f:
    f.write(text)

ddracechat_cpp = 'src/game/server/ddracechat.cpp'
with open(ddracechat_cpp, 'r', encoding='utf-8') as f:
    text = f.read()

impl = '''
void CGameContext::ConSetDuelSpawnLeft1(IConsole::IResult *pResult, void *pUserData) {
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_DuelSpawns[0][0] = vec2(pResult->GetFloat(0), pResult->GetFloat(1));
}
void CGameContext::ConSetDuelSpawnLeft2(IConsole::IResult *pResult, void *pUserData) {
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_DuelSpawns[0][1] = vec2(pResult->GetFloat(0), pResult->GetFloat(1));
}
void CGameContext::ConSetDuelSpawnRight1(IConsole::IResult *pResult, void *pUserData) {
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_DuelSpawns[1][0] = vec2(pResult->GetFloat(0), pResult->GetFloat(1));
}
void CGameContext::ConSetDuelSpawnRight2(IConsole::IResult *pResult, void *pUserData) {
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_DuelSpawns[1][1] = vec2(pResult->GetFloat(0), pResult->GetFloat(1));
}

void CGameContext::StartDuel(int Client1, int Client2, int Side1)
{
	CPlayer *pP1 = m_apPlayers[Client1];
	CPlayer *pP2 = m_apPlayers[Client2];
	if(!pP1 || !pP2) return;

	auto OptTeam = m_pController->Teams().GetFirstEmptyTeam();
	if(!OptTeam.has_value() || OptTeam.value() == 0)
	{
		SendChatTarget(Client1, "Дуэль: Нет свободных команд.");
		SendChatTarget(Client2, "Дуэль: Нет свободных команд.");
		return;
	}
	int FreeTeam = OptTeam.value();

	pP1->m_InDuel = true;
	pP2->m_InDuel = true;
	pP1->m_DuelOpponent = Client2;
	pP2->m_DuelOpponent = Client1;
	pP1->m_DuelScore = 0;
	pP2->m_DuelScore = 0;
	pP1->m_DuelSide = Side1;
	pP2->m_DuelSide = 1 - Side1;

	pP1->m_DuelSavedTeam = pP1->GetTeam();
	pP2->m_DuelSavedTeam = pP2->GetTeam();
	
	if(pP1->GetCharacter())
		pP1->m_DuelSavedTee.Save(pP1->GetCharacter(), false);
	if(pP2->GetCharacter())
		pP2->m_DuelSavedTee.Save(pP2->GetCharacter(), false);

	m_pController->Teams().SetForceCharacterTeam(Client1, FreeTeam);
	m_pController->Teams().SetForceCharacterTeam(Client2, FreeTeam);
	m_pController->Teams().SetTeamLock(FreeTeam, true);

	SendChatTarget(Client1, "Вы начали дуэль!");
	SendChatTarget(Client2, "Вы начали дуэль!");

	NextDuelRound(Client1, Client2);
}

void CGameContext::NextDuelRound(int Client1, int Client2)
{
	CPlayer *pP1 = m_apPlayers[Client1];
	CPlayer *pP2 = m_apPlayers[Client2];
	if(!pP1 || !pP2) return;

	pP1->m_DuelFrozenTicks = 0;
	pP2->m_DuelFrozenTicks = 0;

	if(pP1->GetCharacter())
	{
		pP1->GetCharacter()->Unfreeze();
		pP1->GetCharacter()->SetPosition(m_DuelSpawns[pP1->m_DuelSide][0]);
		pP1->GetCharacter()->ResetVelocity();
	}
	else
	{
		pP1->ForceSpawn(m_DuelSpawns[pP1->m_DuelSide][0]);
	}

	if(pP2->GetCharacter())
	{
		pP2->GetCharacter()->Unfreeze();
		pP2->GetCharacter()->SetPosition(m_DuelSpawns[pP2->m_DuelSide][1]);
		pP2->GetCharacter()->ResetVelocity();
	}
	else
	{
		pP2->ForceSpawn(m_DuelSpawns[pP2->m_DuelSide][1]);
	}
}

void CGameContext::EndDuel(int Client1, int Client2, int Winner)
{
	CPlayer *pP1 = m_apPlayers[Client1];
	CPlayer *pP2 = m_apPlayers[Client2];

	if(pP1)
	{
		pP1->m_InDuel = false;
		if (pP1->GetCharacter()) pP1->KillCharacter(WEAPON_GAME, false);
		m_pController->Teams().SetForceCharacterTeam(Client1, TEAM_FLOCK);
		pP1->m_DuelSavedTee.Load(pP1->ForceSpawn(pP1->m_DuelSavedTee.GetPos()), pP1->m_DuelSavedTeam);
	}
	if(pP2)
	{
		pP2->m_InDuel = false;
		if (pP2->GetCharacter()) pP2->KillCharacter(WEAPON_GAME, false);
		m_pController->Teams().SetForceCharacterTeam(Client2, TEAM_FLOCK);
		pP2->m_DuelSavedTee.Load(pP2->ForceSpawn(pP2->m_DuelSavedTee.GetPos()), pP2->m_DuelSavedTeam);
	}

	if(pP1 && pP2 && Winner != -1)
	{
		int Loser = (Winner == Client1) ? Client2 : Client1;
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "Дуэль завершена! %s победил %s со счетом %d-%d",
			Server()->ClientName(Winner), Server()->ClientName(Loser),
			m_apPlayers[Winner]->m_DuelScore, m_apPlayers[Loser]->m_DuelScore);
		SendChat(-1, TEAM_ALL, aBuf);
	}
}

void CGameContext::TickDuels()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pPlayer = m_apPlayers[i];
		if(!pPlayer || !pPlayer->m_InDuel) continue;

		int OpponentId = pPlayer->m_DuelOpponent;
		if(i > OpponentId) continue;

		CPlayer *pOpponent = m_apPlayers[OpponentId];
		if(!pOpponent || !pOpponent->m_InDuel) continue;

		CCharacter *pChr1 = pPlayer->GetCharacter();
		CCharacter *pChr2 = pOpponent->GetCharacter();

		if(pChr1)
		{
			if(pChr1->m_FreezeTime > 0) pPlayer->m_DuelFrozenTicks++;
			else pPlayer->m_DuelFrozenTicks = 0;
		}
		if(pChr2)
		{
			if(pChr2->m_FreezeTime > 0) pOpponent->m_DuelFrozenTicks++;
			else pOpponent->m_DuelFrozenTicks = 0;
		}

		int Fps = Server()->TickSpeed();
		bool P1Frozen = (pPlayer->m_DuelFrozenTicks >= 3 * Fps);
		bool P2Frozen = (pOpponent->m_DuelFrozenTicks >= 3 * Fps);
		
		bool Restart = false;

		if(P1Frozen && P2Frozen)
		{
			SendChatTarget(i, "Дуэль: Ничья (оба в фризе 3 секунды)!");
			SendChatTarget(OpponentId, "Дуэль: Ничья (оба в фризе 3 секунды)!");
			Restart = true;
		}
		else if(P1Frozen)
		{
			SendChatTarget(i, "Дуэль: Вы находились в фризе 3 секунды. Противник получает очко.");
			SendChatTarget(OpponentId, "Дуэль: Противник находился в фризе 3 секунды! Вы получаете очко.");
			pOpponent->m_DuelScore++;
			Restart = true;
		}
		else if(P2Frozen)
		{
			SendChatTarget(OpponentId, "Дуэль: Вы находились в фризе 3 секунды. Противник получает очко.");
			SendChatTarget(i, "Дуэль: Противник находился в фризе 3 секунды! Вы получаете очко.");
			pPlayer->m_DuelScore++;
			Restart = true;
		}
		
		bool P1Dead = !pChr1;
		bool P2Dead = !pChr2;

		if(P1Dead || P2Dead)
		{
			if(P1Dead && P2Dead)
			{
				SendChatTarget(i, "Дуэль: Вы оба умерли. Ничья.");
				SendChatTarget(OpponentId, "Дуэль: Вы оба умерли. Ничья.");
			}
			else if(P1Dead)
			{
				pOpponent->m_DuelScore++;
				SendChatTarget(OpponentId, "Дуэль: Противник умер! Вы получаете очко.");
			}
			else
			{
				pPlayer->m_DuelScore++;
				SendChatTarget(i, "Дуэль: Противник умер! Вы получаете очко.");
			}
			Restart = true;
		}

		if(Restart)
		{
			if(pPlayer->m_DuelScore >= 10 || pOpponent->m_DuelScore >= 10)
			{
				int Winner = pPlayer->m_DuelScore >= 10 ? i : OpponentId;
				EndDuel(i, OpponentId, Winner);
			}
			else
			{
				NextDuelRound(i, OpponentId);
			}
		}
		else if (pPlayer->m_InDuel)
		{
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "%s: %d\\n%s: %d                                                                                                                                                                                                                            ", Server()->ClientName(i), pPlayer->m_DuelScore, Server()->ClientName(OpponentId), pOpponent->m_DuelScore);
			SendBroadcast(aBuf, i, false);
			SendBroadcast(aBuf, OpponentId, false);
		}
	}
}

void CGameContext::ConDuel(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientId = pResult->m_ClientId;
	if(ClientId == -1) return;
	CPlayer *pPlayer = pSelf->m_apPlayers[ClientId];
	if(!pPlayer) return;

	if(pPlayer->m_InDuel)
	{
		pSelf->SendChatTarget(ClientId, "Вы уже в дуэли.");
		return;
	}
	if(!pPlayer->GetCharacter() || pPlayer->GetCharacter()->m_FreezeTime > 0)
	{
		pSelf->SendChatTarget(ClientId, "Нельзя начать дуэль, пока вы находитесь во фризе или мертвы.");
		return;
	}
	vec2 Vel = pPlayer->GetCharacter()->GetCore().m_Vel;
	if (length(Vel) > 0.1f)
	{
		pSelf->SendChatTarget(ClientId, "Замрите на месте, чтобы кинуть дуэль.");
		return;
	}

	const char *pName = pResult->GetString(0);
	int TargetId = -1;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i != ClientId && pSelf->m_apPlayers[i] && str_comp(pSelf->Server()->ClientName(i), pName) == 0)
		{
			TargetId = i;
			break;
		}
	}

	if(TargetId == -1)
	{
		pSelf->SendChatTarget(ClientId, "Игрок не найден.");
		return;
	}

	int Side = 0;
	if(pResult->NumArguments() > 1 && str_comp_nocase(pResult->GetString(1), "right") == 0)
		Side = 1;

	pSelf->m_apPlayers[TargetId]->m_aDuelRequests[ClientId] = Side + 1;
	pSelf->SendChatTarget(ClientId, "Заявка на дуэль отправлена.");
	pSelf->SendChatTarget(TargetId, "Вам кинули дуэль! Напишите /accept чтобы принять.");
}

void CGameContext::ConAcceptDuel(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientId = pResult->m_ClientId;
	if(ClientId == -1) return;
	CPlayer *pPlayer = pSelf->m_apPlayers[ClientId];
	if(!pPlayer) return;

	if(pPlayer->m_InDuel) return;
	if(!pPlayer->GetCharacter() || pPlayer->GetCharacter()->m_FreezeTime > 0) return;

	int TargetId = -1;
	if(pResult->NumArguments() == 0)
	{
		int Found = 0;
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(pPlayer->m_aDuelRequests[i] > 0)
			{
				Found++;
				TargetId = i;
			}
		}
		if(Found > 1)
		{
			pSelf->SendChatTarget(ClientId, "У вас несколько заявок. Укажите ник: /accept (ник).");
			return;
		}
		if(Found == 0)
		{
			pSelf->SendChatTarget(ClientId, "У вас нет заявок.");
			return;
		}
	}
	else
	{
		const char *pName = pResult->GetString(0);
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(pSelf->m_apPlayers[i] && str_comp(pSelf->Server()->ClientName(i), pName) == 0)
			{
				TargetId = i;
				break;
			}
		}
		if(TargetId == -1 || pPlayer->m_aDuelRequests[TargetId] == 0)
		{
			pSelf->SendChatTarget(ClientId, "Заявка от этого игрока не найдена.");
			return;
		}
	}

	int Side1 = pPlayer->m_aDuelRequests[TargetId] - 1;
	pPlayer->m_aDuelRequests[TargetId] = 0;

	CPlayer *pTarget = pSelf->m_apPlayers[TargetId];
	if(!pTarget || !pTarget->GetCharacter() || pTarget->GetCharacter()->m_FreezeTime > 0)
	{
		pSelf->SendChatTarget(ClientId, "Противник сейчас не может дуэлиться (вышел, в фризе).");
		return;
	}

	pSelf->StartDuel(TargetId, ClientId, Side1);
}
'''
if "TickDuels" not in text:
    text += impl

with open(ddracechat_cpp, 'w', encoding='utf-8') as f:
    f.write(text)

