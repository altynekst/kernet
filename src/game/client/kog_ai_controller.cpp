#include "kog_ai_controller.h"

#include "gameclient.h"
#include "prediction/entities/character.h"

#include <base/io.h>
#include <base/math.h>
#include <base/system.h>

#include <engine/storage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <vector>

namespace
{
constexpr int KOG_AI_ACTION_MOVE_LEFT = 0;
constexpr int KOG_AI_ACTION_MOVE_RIGHT = 1;
constexpr int KOG_AI_ACTION_JUMP = 2;
constexpr int KOG_AI_ACTION_HOOK = 3;
constexpr int KOG_AI_ACTION_ATTACK = 4;
constexpr int KOG_AI_ACTION_RETREAT = 5;
constexpr int KOG_AI_ACTION_IDLE = 6;
constexpr int KOG_AI_ACTION_COUNT = 7;

constexpr int KOG_AI_STYLE_AGGRESSIVE = 0;
constexpr int KOG_AI_STYLE_CAREFUL = 1;
constexpr int KOG_AI_STYLE_BALANCED = 2;

constexpr int KOG_AI_MAX_NEAREST_ENEMIES = 3;
constexpr int KOG_AI_FEATURE_DIM = 24;
constexpr int KOG_AI_MAX_DATASET_SAMPLES = 60000;

float Clamp01(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

const char *ActionName(int Action)
{
	switch(Action)
	{
	case KOG_AI_ACTION_MOVE_LEFT: return "move_left";
	case KOG_AI_ACTION_MOVE_RIGHT: return "move_right";
	case KOG_AI_ACTION_JUMP: return "jump";
	case KOG_AI_ACTION_HOOK: return "hook";
	case KOG_AI_ACTION_ATTACK: return "attack";
	case KOG_AI_ACTION_RETREAT: return "retreat";
	default: return "idle";
	}
}

const char *StyleName(int Style)
{
	switch(Style)
	{
	case KOG_AI_STYLE_AGGRESSIVE: return "aggressive";
	case KOG_AI_STYLE_CAREFUL: return "careful";
	default: return "balanced";
	}
}

void Softmax(const std::array<float, KOG_AI_ACTION_COUNT> &Logits, std::array<float, KOG_AI_ACTION_COUNT> &OutProbs)
{
	float MaxV = Logits[0];
	for(int i = 1; i < KOG_AI_ACTION_COUNT; i++)
		MaxV = std::max(MaxV, Logits[i]);

	float Sum = 0.0f;
	for(int i = 0; i < KOG_AI_ACTION_COUNT; i++)
	{
		OutProbs[i] = expf(Logits[i] - MaxV);
		Sum += OutProbs[i];
	}
	if(Sum <= 0.0f)
	{
		for(float &P : OutProbs)
			P = 1.0f / (float)KOG_AI_ACTION_COUNT;
		return;
	}
	const float InvSum = 1.0f / Sum;
	for(float &P : OutProbs)
		P *= InvSum;
}

int ArgMax(const std::array<float, KOG_AI_ACTION_COUNT> &Values)
{
	int Best = 0;
	for(int i = 1; i < KOG_AI_ACTION_COUNT; i++)
	{
		if(Values[i] > Values[Best])
			Best = i;
	}
	return Best;
}

int SampleByProbability(std::mt19937 &Rng, const std::array<float, KOG_AI_ACTION_COUNT> &Probabilities)
{
	const float R = std::uniform_real_distribution<float>(0.0f, 1.0f)(Rng);
	float Prefix = 0.0f;
	for(int i = 0; i < KOG_AI_ACTION_COUNT; i++)
	{
		Prefix += Probabilities[i];
		if(R <= Prefix)
			return i;
	}
	return KOG_AI_ACTION_IDLE;
}
}

class CKoGAIController::CImpl
{
public:
	struct SEnemyInfo
	{
		int m_ClientId = -1;
		vec2 m_Pos = vec2(0.0f, 0.0f);
		vec2 m_Vel = vec2(0.0f, 0.0f);
		float m_Distance = std::numeric_limits<float>::max();
		bool m_Visible = false;
		bool m_Frozen = false;
	};

	struct SState
	{
		bool m_Valid = false;
		bool m_Alive = true;
		int m_Tick = 0;
		int m_LocalScore = 0;
		vec2 m_LocalPos = vec2(0.0f, 0.0f);
		vec2 m_LocalVel = vec2(0.0f, 0.0f);
		float m_HpNorm = 1.0f;
		float m_IncomingDamageNorm = 0.0f;
		float m_DangerNorm = 0.0f;
		bool m_HasTarget = false;
		int m_TargetId = -1;
		vec2 m_TargetPos = vec2(0.0f, 0.0f);
		vec2 m_TargetDir = vec2(1.0f, 0.0f);
		float m_TargetDistNorm = 1.0f;
		bool m_TargetVisible = false;
		bool m_TargetFrozen = false;
		std::array<SEnemyInfo, KOG_AI_MAX_NEAREST_ENEMIES> m_aNearestEnemies{};
		std::array<bool, MAX_CLIENTS> m_aEnemyFrozen{};
		std::array<float, KOG_AI_FEATURE_DIM> m_aFeatures{};
	};

	struct SDecision
	{
		int m_Action = KOG_AI_ACTION_IDLE;
		vec2 m_AimDir = vec2(1.0f, 0.0f);
		std::array<float, KOG_AI_ACTION_COUNT> m_aActionProb{};
		bool m_UsedModel = false;
		float m_Epsilon = 0.0f;
	};

	struct SRuntime
	{
		bool m_HasPrevState = false;
		bool m_HasPrevDecision = false;
		SState m_PrevState{};
		SDecision m_PrevDecision{};
		int m_NextDecisionTick = -1;
		float m_LastReward = 0.0f;
	};

	struct SDatasetSample
	{
		std::array<float, KOG_AI_FEATURE_DIM> m_aFeatures{};
		int m_Action = KOG_AI_ACTION_IDLE;
		float m_Reward = 0.0f;
	};

	class CKoGAIStateExtractor
	{
	public:
		explicit CKoGAIStateExtractor(CGameClient *pGameClient) :
			m_pGameClient(pGameClient)
		{
		}

		void OnReset()
		{
		}

		bool Extract(int DummyIndex, const vec2 &FallbackAimDir, float IncomingDamageNorm, SState &OutState) const
		{
			const int LocalId = m_pGameClient->m_Snap.m_LocalClientId;
			if(LocalId < 0 || LocalId >= MAX_CLIENTS || !m_pGameClient->m_Snap.m_aCharacters[LocalId].m_Active)
				return false;

			OutState = {};
			OutState.m_Valid = true;
			OutState.m_Tick = m_pGameClient->Client()->GameTick(DummyIndex);
			OutState.m_LocalPos = m_pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Pos;
			OutState.m_LocalVel = m_pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Vel;
			OutState.m_IncomingDamageNorm = Clamp01(IncomingDamageNorm);
			OutState.m_DangerNorm = ComputeDanger(OutState.m_LocalPos);
			OutState.m_Alive = m_pGameClient->m_Snap.m_aCharacters[LocalId].m_Active;
			OutState.m_LocalScore = m_pGameClient->m_Snap.m_pLocalInfo ? m_pGameClient->m_Snap.m_pLocalInfo->m_Score : 0;

			if(m_pGameClient->m_Snap.m_pLocalCharacter)
			{
				const int Hp = std::clamp(m_pGameClient->m_Snap.m_pLocalCharacter->m_Health, 0, 10);
				const int Armor = std::clamp(m_pGameClient->m_Snap.m_pLocalCharacter->m_Armor, 0, 10);
				OutState.m_HpNorm = Clamp01((float)(Hp + Armor) / 20.0f);
			}
			else
			{
				OutState.m_HpNorm = 1.0f;
			}

			struct SCandidate
			{
				int m_Id;
				float m_Dist;
				bool m_Visible;
				bool m_Frozen;
			};
			std::vector<SCandidate> vCandidates;
			vCandidates.reserve(MAX_CLIENTS);

			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(!IsEnemy(LocalId, i))
					continue;

				const vec2 EnemyPos = m_pGameClient->m_aClients[i].m_RegularPredicted.m_Pos;
				const vec2 Delta = EnemyPos - OutState.m_LocalPos;
				const float Dist = length(Delta);
				if(Dist < 1.0f)
					continue;

				const bool Frozen = IsFrozen(i, OutState.m_Tick);
				const bool Visible = IsVisible(OutState.m_LocalPos, EnemyPos);
				OutState.m_aEnemyFrozen[i] = Frozen;
				vCandidates.push_back({i, Dist, Visible, Frozen});
			}

			std::sort(vCandidates.begin(), vCandidates.end(), [](const SCandidate &A, const SCandidate &B) {
				return A.m_Dist < B.m_Dist;
			});

			for(int k = 0; k < KOG_AI_MAX_NEAREST_ENEMIES; k++)
			{
				if(k >= (int)vCandidates.size())
					break;
				const int Cid = vCandidates[k].m_Id;
				OutState.m_aNearestEnemies[k].m_ClientId = Cid;
				OutState.m_aNearestEnemies[k].m_Pos = m_pGameClient->m_aClients[Cid].m_RegularPredicted.m_Pos;
				OutState.m_aNearestEnemies[k].m_Vel = m_pGameClient->m_aClients[Cid].m_RegularPredicted.m_Vel;
				OutState.m_aNearestEnemies[k].m_Distance = vCandidates[k].m_Dist;
				OutState.m_aNearestEnemies[k].m_Visible = vCandidates[k].m_Visible;
				OutState.m_aNearestEnemies[k].m_Frozen = vCandidates[k].m_Frozen;
			}

			if(!vCandidates.empty())
			{
				const int TargetId = vCandidates[0].m_Id;
				const vec2 TargetPos = m_pGameClient->m_aClients[TargetId].m_RegularPredicted.m_Pos;
				vec2 AimDir = normalize(TargetPos - OutState.m_LocalPos);
				if(length(AimDir) < 0.001f)
					AimDir = FallbackAimDir;

				OutState.m_HasTarget = true;
				OutState.m_TargetId = TargetId;
				OutState.m_TargetPos = TargetPos;
				OutState.m_TargetDir = AimDir;
				OutState.m_TargetDistNorm = Clamp01(vCandidates[0].m_Dist / 900.0f);
				OutState.m_TargetVisible = vCandidates[0].m_Visible;
				OutState.m_TargetFrozen = vCandidates[0].m_Frozen;
			}
			else
			{
				OutState.m_HasTarget = false;
				OutState.m_TargetDir = FallbackAimDir;
				OutState.m_TargetDistNorm = 1.0f;
			}

			BuildFeatureVector(OutState);
			return true;
		}

	private:
		CGameClient *m_pGameClient = nullptr;

		bool IsEnemy(int LocalId, int ClientId) const
		{
			if(ClientId < 0 || ClientId >= MAX_CLIENTS || ClientId == LocalId)
				return false;
			if(!m_pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
				return false;
			if(m_pGameClient->m_aClients[ClientId].m_Spec || m_pGameClient->m_aClients[ClientId].m_Paused)
				return false;

			if(m_pGameClient->IsTeamPlay() &&
				m_pGameClient->m_aClients[LocalId].m_Team != TEAM_SPECTATORS &&
				m_pGameClient->m_aClients[ClientId].m_Team == m_pGameClient->m_aClients[LocalId].m_Team)
			{
				return false;
			}
			return true;
		}

		bool IsFrozen(int ClientId, int Tick) const
		{
			if(ClientId < 0 || ClientId >= MAX_CLIENTS)
				return false;
			const auto &Client = m_pGameClient->m_aClients[ClientId];
			return Client.m_DeepFrozen || Client.m_LiveFrozen || Client.m_FreezeEnd > Tick;
		}

		bool IsVisible(const vec2 &From, const vec2 &To) const
		{
			vec2 Hit;
			return m_pGameClient->Collision()->IntersectLine(From, To, &Hit, nullptr) == 0;
		}

		bool IsFreezeTile(int Tile) const
		{
			return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
		}

		float ComputeDanger(const vec2 &LocalPos) const
		{
			static const vec2 s_aOffsets[] = {
				vec2(0.0f, 0.0f),
				vec2(24.0f, 0.0f),
				vec2(-24.0f, 0.0f),
				vec2(0.0f, 24.0f),
				vec2(0.0f, -24.0f),
				vec2(48.0f, 0.0f),
				vec2(-48.0f, 0.0f),
				vec2(0.0f, 48.0f),
				vec2(0.0f, -48.0f),
			};

			float Score = 0.0f;
			float MaxScore = 0.0f;
			for(const vec2 &Off : s_aOffsets)
			{
				const int Pure = m_pGameClient->Collision()->GetPureMapIndex(LocalPos + Off);
				if(Pure < 0)
					continue;
				const int Tile = m_pGameClient->Collision()->GetTileIndex(Pure);
				const int FTile = m_pGameClient->Collision()->GetFrontTileIndex(Pure);
				float LocalScore = 0.0f;
				if(Tile == TILE_DEATH || FTile == TILE_DEATH || IsFreezeTile(Tile) || IsFreezeTile(FTile))
					LocalScore = 1.0f;
				else if(Tile == TILE_SOLID || FTile == TILE_SOLID || Tile == TILE_NOHOOK || FTile == TILE_NOHOOK)
					LocalScore = 0.35f;
				Score += LocalScore;
				MaxScore += 1.0f;
			}
			if(MaxScore <= 0.0f)
				return 0.0f;
			return Clamp01(Score / MaxScore);
		}

		void BuildFeatureVector(SState &State) const
		{
			const float MapW = (float)std::max(1, m_pGameClient->Collision()->GetWidth()) * 32.0f;
			const float MapH = (float)std::max(1, m_pGameClient->Collision()->GetHeight()) * 32.0f;

			int f = 0;
			State.m_aFeatures[f++] = std::clamp((State.m_LocalPos.x / MapW) * 2.0f - 1.0f, -1.0f, 1.0f);
			State.m_aFeatures[f++] = std::clamp((State.m_LocalPos.y / MapH) * 2.0f - 1.0f, -1.0f, 1.0f);
			State.m_aFeatures[f++] = std::clamp(State.m_LocalVel.x / 1200.0f, -1.0f, 1.0f);
			State.m_aFeatures[f++] = std::clamp(State.m_LocalVel.y / 1200.0f, -1.0f, 1.0f);
			State.m_aFeatures[f++] = Clamp01(State.m_HpNorm);
			State.m_aFeatures[f++] = Clamp01(State.m_IncomingDamageNorm);
			State.m_aFeatures[f++] = Clamp01(State.m_DangerNorm);
			State.m_aFeatures[f++] = State.m_HasTarget ? 1.0f : 0.0f;
			State.m_aFeatures[f++] = State.m_TargetDistNorm;
			State.m_aFeatures[f++] = std::clamp(State.m_TargetDir.x, -1.0f, 1.0f);
			State.m_aFeatures[f++] = std::clamp(State.m_TargetDir.y, -1.0f, 1.0f);
			State.m_aFeatures[f++] = State.m_TargetVisible ? 1.0f : 0.0f;

			for(int k = 0; k < KOG_AI_MAX_NEAREST_ENEMIES; k++)
			{
				const auto &Enemy = State.m_aNearestEnemies[k];
				if(Enemy.m_ClientId < 0)
				{
					State.m_aFeatures[f++] = 0.0f;
					State.m_aFeatures[f++] = 0.0f;
					State.m_aFeatures[f++] = 1.0f;
					State.m_aFeatures[f++] = 0.0f;
					continue;
				}
				const vec2 Delta = Enemy.m_Pos - State.m_LocalPos;
				State.m_aFeatures[f++] = std::clamp(Delta.x / 900.0f, -1.0f, 1.0f);
				State.m_aFeatures[f++] = std::clamp(Delta.y / 900.0f, -1.0f, 1.0f);
				State.m_aFeatures[f++] = Clamp01(Enemy.m_Distance / 900.0f);
				State.m_aFeatures[f++] = Enemy.m_Frozen ? 1.0f : 0.0f;
			}
		}
	};

	class CKoGAILearningModule
	{
	public:
		explicit CKoGAILearningModule(CGameClient *pGameClient) :
			m_pGameClient(pGameClient),
			m_Rng((uint32_t)time_get())
		{
			ResetModel();
		}

		void OnResetRuntime()
		{
		}

		void ResetModel()
		{
			m_ModelReady = false;
			m_Updates = 0;
			std::fill(m_aWeights.begin(), m_aWeights.end(), 0.0f);
			std::fill(m_aBias.begin(), m_aBias.end(), 0.0f);
		}

		void EnsureModel()
		{
			if(m_ModelReady)
				return;
			std::uniform_real_distribution<float> Dist(-0.06f, 0.06f);
			for(float &W : m_aWeights)
				W = Dist(m_Rng);
			for(float &B : m_aBias)
				B = 0.0f;
			m_ModelReady = true;
		}

		void InferLogits(const std::array<float, KOG_AI_FEATURE_DIM> &Features, std::array<float, KOG_AI_ACTION_COUNT> &OutLogits) const
		{
			if(!m_ModelReady)
			{
				for(float &L : OutLogits)
					L = 0.0f;
				return;
			}

			for(int a = 0; a < KOG_AI_ACTION_COUNT; a++)
			{
				float Sum = m_aBias[a];
				const int Base = a * KOG_AI_FEATURE_DIM;
				for(int j = 0; j < KOG_AI_FEATURE_DIM; j++)
					Sum += m_aWeights[Base + j] * Features[j];
				OutLogits[a] = Sum;
			}
		}

		void LearnOnline(const std::array<float, KOG_AI_FEATURE_DIM> &Features, const std::array<float, KOG_AI_ACTION_COUNT> &ActionProb, int Action, float Reward, int Difficulty, bool Enabled)
		{
			if(!Enabled)
				return;
			if(Action < 0 || Action >= KOG_AI_ACTION_COUNT)
				return;

			EnsureModel();
			const float Lr = 0.0035f + 0.0012f * (float)(std::clamp(Difficulty, 1, 3) - 1);
			const float RewardClamped = std::clamp(Reward, -2.5f, 2.5f);
			for(int a = 0; a < KOG_AI_ACTION_COUNT; a++)
			{
				const float OneHot = (a == Action) ? 1.0f : 0.0f;
				const float Delta = (OneHot - ActionProb[a]) * RewardClamped * Lr;
				const int Base = a * KOG_AI_FEATURE_DIM;
				for(int j = 0; j < KOG_AI_FEATURE_DIM; j++)
				{
					const float NewW = m_aWeights[Base + j] + Delta * Features[j];
					m_aWeights[Base + j] = std::clamp(NewW, -4.0f, 4.0f);
				}
				m_aBias[a] = std::clamp(m_aBias[a] + Delta * 0.5f, -3.0f, 3.0f);
			}
			m_Updates++;

			m_Dataset.push_back({Features, Action, Reward});
			while((int)m_Dataset.size() > KOG_AI_MAX_DATASET_SAMPLES)
				m_Dataset.pop_front();
		}

		bool SaveModel(const char *pFile, char *pErr, int ErrSize) const
		{
			if(!m_ModelReady)
			{
				str_copy(pErr, "model is not initialized", ErrSize);
				return false;
			}
			if(!pFile || pFile[0] == '\0')
			{
				str_copy(pErr, "empty model filename", ErrSize);
				return false;
			}

			m_pGameClient->Storage()->CreateFolder("kog_ai", IStorage::TYPE_SAVE);
			IOHANDLE Handle = m_pGameClient->Storage()->OpenFile(pFile, IOFLAG_WRITE, IStorage::TYPE_SAVE);
			if(!Handle)
			{
				str_copy(pErr, "failed to open model file for writing", ErrSize);
				return false;
			}

			const int Version = 1;
			const int FeatureDim = KOG_AI_FEATURE_DIM;
			const int ActionCount = KOG_AI_ACTION_COUNT;
			io_write(Handle, "KOGAI01", 8);
			io_write(Handle, &Version, (int)sizeof(Version));
			io_write(Handle, &FeatureDim, (int)sizeof(FeatureDim));
			io_write(Handle, &ActionCount, (int)sizeof(ActionCount));
			io_write(Handle, &m_Updates, (int)sizeof(m_Updates));
			io_write(Handle, m_aWeights.data(), (int)(m_aWeights.size() * sizeof(float)));
			io_write(Handle, m_aBias.data(), (int)(m_aBias.size() * sizeof(float)));
			io_close(Handle);
			return true;
		}

		bool LoadModel(const char *pFile, char *pErr, int ErrSize)
		{
			if(!pFile || pFile[0] == '\0')
			{
				str_copy(pErr, "empty model filename", ErrSize);
				return false;
			}

			IOHANDLE Handle = m_pGameClient->Storage()->OpenFile(pFile, IOFLAG_READ, IStorage::TYPE_SAVE);
			if(!Handle)
			{
				str_copy(pErr, "failed to open model file", ErrSize);
				return false;
			}

			char aMagic[8];
			int Version = 0;
			int FeatureDim = 0;
			int ActionCount = 0;
			int Updates = 0;

			auto ReadExact = [&](void *pDst, int Size) -> bool {
				return io_read(Handle, pDst, Size) == Size;
			};

			if(!ReadExact(aMagic, (int)sizeof(aMagic)) || mem_comp(aMagic, "KOGAI01", 8) != 0 ||
				!ReadExact(&Version, (int)sizeof(Version)) || Version != 1 ||
				!ReadExact(&FeatureDim, (int)sizeof(FeatureDim)) || FeatureDim != KOG_AI_FEATURE_DIM ||
				!ReadExact(&ActionCount, (int)sizeof(ActionCount)) || ActionCount != KOG_AI_ACTION_COUNT ||
				!ReadExact(&Updates, (int)sizeof(Updates)) ||
				!ReadExact(m_aWeights.data(), (int)(m_aWeights.size() * sizeof(float))) ||
				!ReadExact(m_aBias.data(), (int)(m_aBias.size() * sizeof(float))))
			{
				io_close(Handle);
				str_copy(pErr, "invalid model file", ErrSize);
				return false;
			}

			io_close(Handle);
			m_ModelReady = true;
			m_Updates = Updates;
			return true;
		}

		bool ExportDataset(const char *pFile, char *pErr, int ErrSize) const
		{
			if(!pFile || pFile[0] == '\0')
			{
				str_copy(pErr, "empty dataset filename", ErrSize);
				return false;
			}
			if(m_Dataset.empty())
			{
				str_copy(pErr, "dataset is empty", ErrSize);
				return false;
			}

			m_pGameClient->Storage()->CreateFolder("kog_ai", IStorage::TYPE_SAVE);
			IOHANDLE Handle = m_pGameClient->Storage()->OpenFile(pFile, IOFLAG_WRITE, IStorage::TYPE_SAVE);
			if(!Handle)
			{
				str_copy(pErr, "failed to open dataset file for writing", ErrSize);
				return false;
			}

			const int Version = 1;
			const int FeatureDim = KOG_AI_FEATURE_DIM;
			const int ActionCount = KOG_AI_ACTION_COUNT;
			const int SampleCount = (int)m_Dataset.size();
			io_write(Handle, "KOGDS01", 8);
			io_write(Handle, &Version, (int)sizeof(Version));
			io_write(Handle, &FeatureDim, (int)sizeof(FeatureDim));
			io_write(Handle, &ActionCount, (int)sizeof(ActionCount));
			io_write(Handle, &SampleCount, (int)sizeof(SampleCount));
			for(const auto &Sample : m_Dataset)
			{
				io_write(Handle, &Sample.m_Action, (int)sizeof(Sample.m_Action));
				io_write(Handle, &Sample.m_Reward, (int)sizeof(Sample.m_Reward));
				io_write(Handle, Sample.m_aFeatures.data(), (int)(Sample.m_aFeatures.size() * sizeof(float)));
			}
			io_close(Handle);
			return true;
		}

		bool TrainOffline(const char *pFile, int Epochs, char *pErr, int ErrSize)
		{
			if(!pFile || pFile[0] == '\0')
			{
				str_copy(pErr, "empty dataset filename", ErrSize);
				return false;
			}

			IOHANDLE Handle = m_pGameClient->Storage()->OpenFile(pFile, IOFLAG_READ, IStorage::TYPE_SAVE);
			if(!Handle)
			{
				str_copy(pErr, "failed to open dataset file", ErrSize);
				return false;
			}

			char aMagic[8];
			int Version = 0;
			int FeatureDim = 0;
			int ActionCount = 0;
			int SampleCount = 0;

			auto ReadExact = [&](void *pDst, int Size) -> bool {
				return io_read(Handle, pDst, Size) == Size;
			};

			if(!ReadExact(aMagic, (int)sizeof(aMagic)) || mem_comp(aMagic, "KOGDS01", 8) != 0 ||
				!ReadExact(&Version, (int)sizeof(Version)) || Version != 1 ||
				!ReadExact(&FeatureDim, (int)sizeof(FeatureDim)) || FeatureDim != KOG_AI_FEATURE_DIM ||
				!ReadExact(&ActionCount, (int)sizeof(ActionCount)) || ActionCount != KOG_AI_ACTION_COUNT ||
				!ReadExact(&SampleCount, (int)sizeof(SampleCount)) || SampleCount <= 0)
			{
				io_close(Handle);
				str_copy(pErr, "invalid dataset header", ErrSize);
				return false;
			}

			std::vector<SDatasetSample> vSamples;
			vSamples.reserve(std::clamp(SampleCount, 1, 200000));
			for(int i = 0; i < SampleCount; i++)
			{
				SDatasetSample Sample;
				if(!ReadExact(&Sample.m_Action, (int)sizeof(Sample.m_Action)) ||
					!ReadExact(&Sample.m_Reward, (int)sizeof(Sample.m_Reward)) ||
					!ReadExact(Sample.m_aFeatures.data(), (int)(Sample.m_aFeatures.size() * sizeof(float))))
				{
					break;
				}
				if(Sample.m_Action < 0 || Sample.m_Action >= KOG_AI_ACTION_COUNT)
					continue;
				vSamples.push_back(Sample);
			}
			io_close(Handle);

			if(vSamples.empty())
			{
				str_copy(pErr, "dataset has no valid samples", ErrSize);
				return false;
			}

			EnsureModel();
			Epochs = std::clamp(Epochs, 1, 200);
			std::vector<int> vIndices(vSamples.size());
			for(int i = 0; i < (int)vIndices.size(); i++)
				vIndices[i] = i;

			for(int Ep = 0; Ep < Epochs; Ep++)
			{
				std::shuffle(vIndices.begin(), vIndices.end(), m_Rng);
				for(int Idx : vIndices)
				{
					const SDatasetSample &Sample = vSamples[Idx];
					std::array<float, KOG_AI_ACTION_COUNT> Logits;
					std::array<float, KOG_AI_ACTION_COUNT> Probs;
					InferLogits(Sample.m_aFeatures, Logits);
					Softmax(Logits, Probs);

					const float Weight = std::clamp(0.5f + Sample.m_Reward, 0.1f, 3.0f);
					const float Lr = 0.0008f;
					for(int a = 0; a < KOG_AI_ACTION_COUNT; a++)
					{
						const float OneHot = (a == Sample.m_Action) ? 1.0f : 0.0f;
						const float Delta = (OneHot - Probs[a]) * Weight * Lr;
						const int Base = a * KOG_AI_FEATURE_DIM;
						for(int j = 0; j < KOG_AI_FEATURE_DIM; j++)
						{
							const float NewW = m_aWeights[Base + j] + Delta * Sample.m_aFeatures[j];
							m_aWeights[Base + j] = std::clamp(NewW, -4.0f, 4.0f);
						}
						m_aBias[a] = std::clamp(m_aBias[a] + Delta * 0.5f, -3.0f, 3.0f);
					}
					m_Updates++;
				}
			}

			str_format(pErr, ErrSize, "offline train done (samples=%d epochs=%d)", (int)vSamples.size(), Epochs);
			return true;
		}

		bool IsModelReady() const { return m_ModelReady; }
		int UpdateCount() const { return m_Updates; }
		int DatasetSize() const { return (int)m_Dataset.size(); }

	private:
		CGameClient *m_pGameClient = nullptr;
		bool m_ModelReady = false;
		int m_Updates = 0;
		std::array<float, KOG_AI_ACTION_COUNT * KOG_AI_FEATURE_DIM> m_aWeights{};
		std::array<float, KOG_AI_ACTION_COUNT> m_aBias{};
		std::deque<SDatasetSample> m_Dataset;
		std::mt19937 m_Rng;
	};

	class CKoGAIDecisionEngine
	{
	public:
		CKoGAIDecisionEngine() :
			m_Rng((uint32_t)(time_get() ^ 0x51CEB00u))
		{
		}

		void OnReset()
		{
		}

		SDecision Decide(const SState &State, int Difficulty, int AggressionLevel, int Style, const CKoGAILearningModule &LearningModule)
		{
			SDecision Out;
			Difficulty = std::clamp(Difficulty, 1, 3);
			AggressionLevel = std::clamp(AggressionLevel, 0, 100);
			Style = std::clamp(Style, KOG_AI_STYLE_AGGRESSIVE, KOG_AI_STYLE_BALANCED);

			std::array<float, KOG_AI_ACTION_COUNT> Heuristics{};
			std::array<float, KOG_AI_ACTION_COUNT> ModelLogits{};
			std::array<float, KOG_AI_ACTION_COUNT> FinalLogits{};

			BuildHeuristics(State, AggressionLevel, Style, Heuristics);
			LearningModule.InferLogits(State.m_aFeatures, ModelLogits);

			const float Difficulty01 = (float)(Difficulty - 1) / 2.0f;
			const float Aggression01 = (float)AggressionLevel / 100.0f;
			const float ModelWeight = std::clamp(0.25f + Difficulty01 * 0.40f + Aggression01 * 0.15f, 0.20f, 0.85f);
			for(int a = 0; a < KOG_AI_ACTION_COUNT; a++)
				FinalLogits[a] = Heuristics[a] * (1.0f - ModelWeight) + ModelLogits[a] * ModelWeight;

			Softmax(FinalLogits, Out.m_aActionProb);

			float Epsilon = 0.28f - Difficulty01 * 0.14f + (1.0f - Aggression01) * 0.08f;
			if(Style == KOG_AI_STYLE_AGGRESSIVE)
				Epsilon -= 0.03f;
			else if(Style == KOG_AI_STYLE_CAREFUL)
				Epsilon += 0.05f;
			Epsilon = std::clamp(Epsilon, 0.03f, 0.35f);

			const float Rand = std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Rng);
			if(Rand < Epsilon)
				Out.m_Action = SampleByProbability(m_Rng, Out.m_aActionProb);
			else
				Out.m_Action = ArgMax(Out.m_aActionProb);

			Out.m_UsedModel = true;
			Out.m_Epsilon = Epsilon;
			Out.m_AimDir = State.m_HasTarget ? State.m_TargetDir : vec2(1.0f, 0.0f);
			if(Out.m_Action == KOG_AI_ACTION_RETREAT && State.m_HasTarget)
				Out.m_AimDir = -State.m_TargetDir;
			if(length(Out.m_AimDir) < 0.001f)
				Out.m_AimDir = vec2(1.0f, 0.0f);
			Out.m_AimDir = normalize(Out.m_AimDir);
			return Out;
		}

	private:
		void BuildHeuristics(const SState &State, int AggressionLevel, int Style, std::array<float, KOG_AI_ACTION_COUNT> &OutHeuristics) const
		{
			for(float &H : OutHeuristics)
				H = 0.0f;

			const float Aggression01 = (float)AggressionLevel / 100.0f;
			const int TowardAction = State.m_TargetDir.x >= 0.0f ? KOG_AI_ACTION_MOVE_RIGHT : KOG_AI_ACTION_MOVE_LEFT;
			const int AwayAction = TowardAction == KOG_AI_ACTION_MOVE_RIGHT ? KOG_AI_ACTION_MOVE_LEFT : KOG_AI_ACTION_MOVE_RIGHT;

			if(!State.m_HasTarget)
			{
				OutHeuristics[KOG_AI_ACTION_IDLE] += 0.35f;
				OutHeuristics[KOG_AI_ACTION_MOVE_LEFT] += 0.25f;
				OutHeuristics[KOG_AI_ACTION_MOVE_RIGHT] += 0.25f;
			}
			else
			{
				OutHeuristics[TowardAction] += 0.60f;
				if(State.m_TargetDistNorm < 0.20f)
				{
					OutHeuristics[KOG_AI_ACTION_ATTACK] += 1.70f;
					OutHeuristics[KOG_AI_ACTION_JUMP] += 0.45f;
					OutHeuristics[KOG_AI_ACTION_HOOK] += State.m_TargetVisible ? 0.50f : -0.20f;
				}
				else if(State.m_TargetDistNorm < 0.45f)
				{
					OutHeuristics[TowardAction] += 0.75f;
					OutHeuristics[KOG_AI_ACTION_HOOK] += State.m_TargetVisible ? 0.95f : 0.10f;
					OutHeuristics[KOG_AI_ACTION_ATTACK] += State.m_TargetVisible ? 0.75f : 0.15f;
				}
				else
				{
					OutHeuristics[TowardAction] += 1.00f;
					OutHeuristics[KOG_AI_ACTION_HOOK] += State.m_TargetVisible ? 0.30f : -0.20f;
				}
			}

			if(State.m_HpNorm < 0.30f)
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 2.30f;
				OutHeuristics[AwayAction] += 0.60f;
				OutHeuristics[KOG_AI_ACTION_ATTACK] -= 0.90f;
			}
			else if(State.m_HpNorm < 0.50f)
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 1.15f;
			}

			if(State.m_IncomingDamageNorm > 0.08f)
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 1.10f;
				OutHeuristics[KOG_AI_ACTION_JUMP] += 0.50f;
			}
			if(State.m_DangerNorm > 0.45f)
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 1.55f;
				OutHeuristics[KOG_AI_ACTION_JUMP] += 0.95f;
				OutHeuristics[KOG_AI_ACTION_IDLE] -= 1.00f;
				OutHeuristics[KOG_AI_ACTION_ATTACK] -= 0.80f;
			}
			if(State.m_TargetFrozen)
			{
				OutHeuristics[KOG_AI_ACTION_ATTACK] -= 0.70f;
				OutHeuristics[KOG_AI_ACTION_HOOK] -= 0.50f;
			}

			if(Style == KOG_AI_STYLE_AGGRESSIVE)
			{
				OutHeuristics[KOG_AI_ACTION_ATTACK] += 0.95f + Aggression01 * 0.40f;
				OutHeuristics[KOG_AI_ACTION_HOOK] += 0.45f;
				OutHeuristics[KOG_AI_ACTION_RETREAT] -= 0.55f;
			}
			else if(Style == KOG_AI_STYLE_CAREFUL)
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 0.80f + (1.0f - Aggression01) * 0.50f;
				OutHeuristics[KOG_AI_ACTION_JUMP] += 0.45f;
				OutHeuristics[KOG_AI_ACTION_ATTACK] -= 0.30f;
			}
			else
			{
				OutHeuristics[KOG_AI_ACTION_RETREAT] += 0.25f;
				OutHeuristics[KOG_AI_ACTION_ATTACK] += 0.20f;
			}

			OutHeuristics[KOG_AI_ACTION_ATTACK] += Aggression01 * 0.65f;
			OutHeuristics[KOG_AI_ACTION_RETREAT] += (1.0f - Aggression01) * 0.30f;
			OutHeuristics[KOG_AI_ACTION_IDLE] += (1.0f - Aggression01) * 0.10f;
		}

		std::mt19937 m_Rng;
	};

	class CKoGAIActionExecutor
	{
	public:
		explicit CKoGAIActionExecutor(CGameClient *pGameClient) :
			m_pGameClient(pGameClient)
		{
			Reset();
		}

		void Reset()
		{
			std::fill(std::begin(m_aLastJumpTick), std::end(m_aLastJumpTick), -1000000);
			std::fill(std::begin(m_aLastFireTick), std::end(m_aLastFireTick), -1000000);
			std::fill(std::begin(m_aLastHookStartTick), std::end(m_aLastHookStartTick), -1000000);
			std::fill(std::begin(m_aHookHoldUntilTick), std::end(m_aHookHoldUntilTick), -1000000);
			std::fill(std::begin(m_aFireReleasePending), std::end(m_aFireReleasePending), false);
			std::fill(std::begin(m_aInjectedFireState), std::end(m_aInjectedFireState), -1);
		}

		void Apply(int DummyIndex, int Difficulty, int ReactionTicks, const SState &State, const SDecision &Decision, CNetObj_PlayerInput *pInput)
		{
			if(!pInput)
				return;

			const int Tick = m_pGameClient->Client()->GameTick(DummyIndex);
			const int LocalId = m_pGameClient->m_Snap.m_LocalClientId;
			CCharacter *pPredChar = LocalId >= 0 ? m_pGameClient->m_RegularPredictedWorld.GetCharacterById(LocalId) : nullptr;

			// Release fire pulse generated on previous tick.
			if(m_aFireReleasePending[DummyIndex] &&
				(pInput->m_Fire & 1) != 0 &&
				pInput->m_Fire == m_aInjectedFireState[DummyIndex])
			{
				pInput->m_Fire = (pInput->m_Fire + 1) & INPUT_STATE_MASK;
				m_aFireReleasePending[DummyIndex] = false;
				m_aInjectedFireState[DummyIndex] = -1;
			}

			vec2 AimDir = Decision.m_AimDir;
			if(length(AimDir) < 0.001f)
				AimDir = vec2(1.0f, 0.0f);
			AimDir = normalize(AimDir);
			pInput->m_TargetX = (int)round_to_int(AimDir.x * 1000.0f);
			pInput->m_TargetY = (int)round_to_int(AimDir.y * 1000.0f);
			if(!pInput->m_TargetX && !pInput->m_TargetY)
				pInput->m_TargetX = 1;
			m_pGameClient->m_Controls.m_aMousePos[DummyIndex] = AimDir * 1000.0f;

			int DesiredDirection = 0;
			switch(Decision.m_Action)
			{
			case KOG_AI_ACTION_MOVE_LEFT:
				DesiredDirection = -1;
				break;
			case KOG_AI_ACTION_MOVE_RIGHT:
				DesiredDirection = 1;
				break;
			case KOG_AI_ACTION_RETREAT:
				if(State.m_HasTarget)
					DesiredDirection = State.m_TargetDir.x >= 0.0f ? -1 : 1;
				break;
			case KOG_AI_ACTION_ATTACK:
			case KOG_AI_ACTION_HOOK:
				if(State.m_HasTarget)
				{
					if(State.m_TargetDir.x > 0.2f)
						DesiredDirection = 1;
					else if(State.m_TargetDir.x < -0.2f)
						DesiredDirection = -1;
				}
				break;
			default:
				break;
			}
			pInput->m_Direction = DesiredDirection;

			const int JumpCooldown = std::clamp(ReactionTicks / 2 + (3 - std::clamp(Difficulty, 1, 3)), 3, 10);
			if((Decision.m_Action == KOG_AI_ACTION_JUMP || (Decision.m_Action == KOG_AI_ACTION_RETREAT && State.m_DangerNorm > 0.55f)) &&
				Tick - m_aLastJumpTick[DummyIndex] >= JumpCooldown)
			{
				const bool CanJump = !pPredChar || pPredChar->Core()->m_Jumps > 0;
				if(CanJump)
				{
					pInput->m_Jump = (pInput->m_Jump + 2) | 1;
					m_aLastJumpTick[DummyIndex] = Tick;
				}
			}

			const int HookCooldown = std::clamp(ReactionTicks + 2, 6, 20);
			if(Decision.m_Action == KOG_AI_ACTION_HOOK && State.m_HasTarget && Tick - m_aLastHookStartTick[DummyIndex] >= HookCooldown)
			{
				m_aLastHookStartTick[DummyIndex] = Tick;
				m_aHookHoldUntilTick[DummyIndex] = Tick + std::clamp(ReactionTicks / 2, 2, 5);
			}
			pInput->m_Hook = Tick <= m_aHookHoldUntilTick[DummyIndex] ? 1 : 0;

			const int FireCooldown = std::clamp(ReactionTicks / 2 + (3 - std::clamp(Difficulty, 1, 3)) * 2, 4, 15);
			if(Decision.m_Action == KOG_AI_ACTION_ATTACK && State.m_HasTarget && State.m_TargetVisible)
			{
				const bool WeaponReady = !pPredChar || pPredChar->GetReloadTimer() <= 0;
				if(WeaponReady && Tick - m_aLastFireTick[DummyIndex] >= FireCooldown && !m_aFireReleasePending[DummyIndex])
				{
					int Fire = (pInput->m_Fire + 1) & INPUT_STATE_MASK;
					if((Fire & 1) == 0)
						Fire = (Fire + 1) & INPUT_STATE_MASK;
					pInput->m_Fire = Fire;
					m_aLastFireTick[DummyIndex] = Tick;
					m_aFireReleasePending[DummyIndex] = true;
					m_aInjectedFireState[DummyIndex] = Fire;
				}
			}
		}

	private:
		CGameClient *m_pGameClient = nullptr;
		std::array<int, NUM_DUMMIES> m_aLastJumpTick{};
		std::array<int, NUM_DUMMIES> m_aLastFireTick{};
		std::array<int, NUM_DUMMIES> m_aLastHookStartTick{};
		std::array<int, NUM_DUMMIES> m_aHookHoldUntilTick{};
		std::array<bool, NUM_DUMMIES> m_aFireReleasePending{};
		std::array<int, NUM_DUMMIES> m_aInjectedFireState{};
	};

	explicit CImpl(CGameClient *pGameClient) :
		m_pGameClient(pGameClient),
		m_StateExtractor(pGameClient),
		m_LearningModule(pGameClient),
		m_ActionExecutor(pGameClient),
		m_Rng((uint32_t)(time_get() ^ 0xA17B0A1u))
	{
		OnReset();
	}

	void OnReset()
	{
		m_StateExtractor.OnReset();
		m_DecisionEngine.OnReset();
		m_LearningModule.OnResetRuntime();
		m_ActionExecutor.Reset();
		for(auto &Runtime : m_aRuntime)
			Runtime = {};
	}

	void PrintStatus()
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "enabled=%d difficulty=%d reaction_ms=%d aggression=%d style=%s learn=%d log=%d model_ready=%d updates=%d dataset=%d",
			g_Config.m_ClZzKogAiEnabled,
			g_Config.m_ClZzKogAiDifficulty,
			g_Config.m_ClZzKogAiReactionTime,
			g_Config.m_ClZzKogAiAggression,
			StyleName(g_Config.m_ClZzKogAiStyle),
			g_Config.m_ClZzKogAiLearning,
			g_Config.m_ClZzKogAiLog,
			m_LearningModule.IsModelReady() ? 1 : 0,
			m_LearningModule.UpdateCount(),
			m_LearningModule.DatasetSize());
		m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
	}

	void SaveModel(const char *pFile)
	{
		char aErr[256];
		if(m_LearningModule.SaveModel(pFile, aErr, sizeof(aErr)))
		{
			char aMsg[256];
			str_format(aMsg, sizeof(aMsg), "model saved: %s", pFile);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
		else
		{
			char aMsg[320];
			str_format(aMsg, sizeof(aMsg), "save failed: %s", aErr);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
	}

	void LoadModel(const char *pFile)
	{
		char aErr[256];
		if(m_LearningModule.LoadModel(pFile, aErr, sizeof(aErr)))
		{
			char aMsg[256];
			str_format(aMsg, sizeof(aMsg), "model loaded: %s", pFile);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
		else
		{
			char aMsg[320];
			str_format(aMsg, sizeof(aMsg), "load failed: %s", aErr);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
	}

	void ExportDataset(const char *pFile)
	{
		char aErr[256];
		if(m_LearningModule.ExportDataset(pFile, aErr, sizeof(aErr)))
		{
			char aMsg[256];
			str_format(aMsg, sizeof(aMsg), "dataset exported: %s", pFile);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
		else
		{
			char aMsg[320];
			str_format(aMsg, sizeof(aMsg), "dataset export failed: %s", aErr);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
		}
	}

	void TrainOffline(const char *pFile, int Epochs)
	{
		char aMsg[256];
		if(m_LearningModule.TrainOffline(pFile, Epochs, aMsg, sizeof(aMsg)))
		{
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", "offline training interface ready; you can keep collecting datasets with zz_kog_ai_export_dataset");
		}
		else
		{
			char aErr[320];
			str_format(aErr, sizeof(aErr), "offline train failed: %s", aMsg);
			m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aErr);
		}
	}

	void ResetModel()
	{
		m_LearningModule.ResetModel();
		m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", "model reset");
	}

	bool ProcessInput(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &OrigInput, bool SaveInBlockActive)
	{
		if(!g_Config.m_ClZzKogAiEnabled)
			return false;
		if(SaveInBlockActive || !pInput)
			return false;
		if(!m_pGameClient->m_Snap.m_pLocalCharacter || m_pGameClient->m_Snap.m_SpecInfo.m_Active)
			return false;
		if(m_pGameClient->Client()->State() != IClient::STATE_ONLINE)
			return false;
		if(!str_find_nocase(m_pGameClient->m_GameInfo.m_aGameType, "gores") &&
			!str_find_nocase(m_pGameClient->m_GameInfo.m_aGameType, "kog"))
		{
			return false;
		}

		const int DummyIndex = g_Config.m_ClDummy;
		const int LocalId = m_pGameClient->m_Snap.m_LocalClientId;
		if(LocalId < 0 || LocalId >= MAX_CLIENTS || !m_pGameClient->m_Snap.m_aCharacters[LocalId].m_Active)
			return false;

		const int Hp = std::clamp(m_pGameClient->m_Snap.m_pLocalCharacter->m_Health, 0, 10);
		const int Armor = std::clamp(m_pGameClient->m_Snap.m_pLocalCharacter->m_Armor, 0, 10);
		const float HpNormNow = Clamp01((float)(Hp + Armor) / 20.0f);

		vec2 FallbackAimDir((float)OrigInput.m_TargetX, (float)OrigInput.m_TargetY);
		if(length(FallbackAimDir) < 0.001f)
			FallbackAimDir = vec2(1.0f, 0.0f);
		FallbackAimDir = normalize(FallbackAimDir);

		SRuntime &Runtime = m_aRuntime[DummyIndex];
		const float IncomingDamageNorm = Runtime.m_HasPrevState ? std::max(0.0f, Runtime.m_PrevState.m_HpNorm - HpNormNow) : 0.0f;

		SState State;
		if(!m_StateExtractor.Extract(DummyIndex, FallbackAimDir, IncomingDamageNorm, State))
			return false;

		const int Difficulty = std::clamp(g_Config.m_ClZzKogAiDifficulty, 1, 3);
		const int Aggression = std::clamp(g_Config.m_ClZzKogAiAggression, 0, 100);
		const int Style = std::clamp(g_Config.m_ClZzKogAiStyle, KOG_AI_STYLE_AGGRESSIVE, KOG_AI_STYLE_BALANCED);
		const bool OnlineLearning = g_Config.m_ClZzKogAiLearning != 0;

		if(Runtime.m_HasPrevState && Runtime.m_HasPrevDecision)
		{
			const float Reward = EvaluateReward(Runtime.m_PrevState, Runtime.m_PrevDecision, State);
			Runtime.m_LastReward = Reward;
			m_LearningModule.LearnOnline(Runtime.m_PrevState.m_aFeatures, Runtime.m_PrevDecision.m_aActionProb, Runtime.m_PrevDecision.m_Action, Reward, Difficulty, OnlineLearning);
		}

		m_LearningModule.EnsureModel();
		const bool NeedNewDecision = !Runtime.m_HasPrevDecision || State.m_Tick >= Runtime.m_NextDecisionTick;
		if(NeedNewDecision)
		{
			Runtime.m_PrevDecision = m_DecisionEngine.Decide(State, Difficulty, Aggression, Style, m_LearningModule);
			Runtime.m_HasPrevDecision = true;
			const int ReactionTicks = CalcReactionTicks(Difficulty);
			Runtime.m_NextDecisionTick = State.m_Tick + ReactionTicks;

			if(g_Config.m_ClZzKogAiLog)
			{
				char aMsg[256];
				str_format(aMsg, sizeof(aMsg),
					"state hp=%.2f in_dmg=%.2f danger=%.2f target=%d dist=%.2f action=%s eps=%.2f reward=%.3f",
					State.m_HpNorm, State.m_IncomingDamageNorm, State.m_DangerNorm, State.m_TargetId, State.m_TargetDistNorm,
					ActionName(Runtime.m_PrevDecision.m_Action), Runtime.m_PrevDecision.m_Epsilon, Runtime.m_LastReward);
				m_pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "kog_ai", aMsg);
			}
		}

		const int ReactionTicks = std::max(1, Runtime.m_NextDecisionTick - State.m_Tick);
		m_ActionExecutor.Apply(DummyIndex, Difficulty, ReactionTicks, State, Runtime.m_PrevDecision, pInput);

		Runtime.m_PrevState = State;
		Runtime.m_HasPrevState = true;
		return true;
	}

private:
	float EvaluateReward(const SState &Prev, const SDecision &PrevDecision, const SState &Cur) const
	{
		float Reward = Cur.m_Alive ? 0.01f : 0.0f;

		const float HpLoss = std::max(0.0f, Prev.m_HpNorm - Cur.m_HpNorm);
		Reward -= HpLoss * 1.45f;

		if(Prev.m_Alive && !Cur.m_Alive)
			Reward -= 1.0f;

		if(Cur.m_LocalScore > Prev.m_LocalScore)
			Reward += std::clamp((float)(Cur.m_LocalScore - Prev.m_LocalScore) * 0.25f, 0.0f, 1.5f);
		else if(Cur.m_LocalScore < Prev.m_LocalScore)
			Reward -= std::clamp((float)(Prev.m_LocalScore - Cur.m_LocalScore) * 0.10f, 0.0f, 1.0f);

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(Cur.m_aEnemyFrozen[i] && !Prev.m_aEnemyFrozen[i])
				Reward += 0.18f;
		}

		if((PrevDecision.m_Action == KOG_AI_ACTION_ATTACK || PrevDecision.m_Action == KOG_AI_ACTION_HOOK) &&
			Cur.m_HasTarget && Cur.m_TargetVisible && Cur.m_TargetDistNorm < 0.25f)
		{
			Reward += 0.04f;
		}

		Reward -= Cur.m_DangerNorm * 0.025f;
		return std::clamp(Reward, -2.0f, 2.0f);
	}

	int CalcReactionTicks(int Difficulty)
	{
		const int Ms = std::clamp(g_Config.m_ClZzKogAiReactionTime, 20, 500);
		const int TicksPerSec = std::max(1, m_pGameClient->Client()->GameTickSpeed());
		const int Base = std::max(1, (Ms * TicksPerSec) / 1000);
		int JitterMin = -1;
		int JitterMax = 1;
		if(Difficulty <= 1)
		{
			JitterMin = 0;
			JitterMax = 2;
		}
		else if(Difficulty >= 3)
		{
			JitterMin = -2;
			JitterMax = 1;
		}
		const int Jitter = std::uniform_int_distribution<int>(JitterMin, JitterMax)(m_Rng);
		return std::max(1, Base + Jitter);
	}

	CGameClient *m_pGameClient = nullptr;
	CKoGAIStateExtractor m_StateExtractor;
	CKoGAIDecisionEngine m_DecisionEngine;
	CKoGAILearningModule m_LearningModule;
	CKoGAIActionExecutor m_ActionExecutor;
	std::array<SRuntime, NUM_DUMMIES> m_aRuntime{};
	std::mt19937 m_Rng;
};

CKoGAIController::CKoGAIController(CGameClient *pGameClient) :
	m_pGameClient(pGameClient),
	m_pImpl(std::make_unique<CImpl>(pGameClient))
{
}

CKoGAIController::~CKoGAIController() = default;

void CKoGAIController::OnConsoleInit()
{
	m_pGameClient->Console()->Register("zz_kog_ai_status", "", CFGFLAG_CLIENT, ConStatus, this, "KoG AI: print status and runtime stats");
	m_pGameClient->Console()->Register("zz_kog_ai_save_model", "?r[file]", CFGFLAG_CLIENT, ConSaveModel, this, "KoG AI: save model to save dir");
	m_pGameClient->Console()->Register("zz_kog_ai_load_model", "?r[file]", CFGFLAG_CLIENT, ConLoadModel, this, "KoG AI: load model from save dir");
	m_pGameClient->Console()->Register("zz_kog_ai_export_dataset", "?r[file]", CFGFLAG_CLIENT, ConExportDataset, this, "KoG AI: export online learning dataset");
	m_pGameClient->Console()->Register("zz_kog_ai_train_offline", "?r[file] ?i[epochs]", CFGFLAG_CLIENT, ConTrainOffline, this, "KoG AI: offline train from exported dataset");
	m_pGameClient->Console()->Register("zz_kog_ai_reset_model", "", CFGFLAG_CLIENT, ConResetModel, this, "KoG AI: reset policy model");
}

void CKoGAIController::OnReset()
{
	m_pImpl->OnReset();
}

bool CKoGAIController::ProcessInput(CNetObj_PlayerInput *pInput, const CNetObj_PlayerInput &OrigInput, bool SaveInBlockActive)
{
	return m_pImpl->ProcessInput(pInput, OrigInput, SaveInBlockActive);
}

void CKoGAIController::ConStatus(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	pSelf->m_pImpl->PrintStatus();
}

void CKoGAIController::ConSaveModel(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	const char *pFile = pResult->NumArguments() > 0 ? pResult->GetString(0) : g_Config.m_ClZzKogAiModel;
	if(!pFile || !pFile[0])
		pFile = "kog_ai/model.bin";
	pSelf->m_pImpl->SaveModel(pFile);
}

void CKoGAIController::ConLoadModel(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	const char *pFile = pResult->NumArguments() > 0 ? pResult->GetString(0) : g_Config.m_ClZzKogAiModel;
	if(!pFile || !pFile[0])
		pFile = "kog_ai/model.bin";
	pSelf->m_pImpl->LoadModel(pFile);
}

void CKoGAIController::ConExportDataset(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	const char *pFile = pResult->NumArguments() > 0 ? pResult->GetString(0) : "kog_ai/dataset.bin";
	if(!pFile || !pFile[0])
		pFile = "kog_ai/dataset.bin";
	pSelf->m_pImpl->ExportDataset(pFile);
}

void CKoGAIController::ConTrainOffline(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	const char *pFile = pResult->NumArguments() > 0 ? pResult->GetString(0) : "kog_ai/dataset.bin";
	if(!pFile || !pFile[0])
		pFile = "kog_ai/dataset.bin";
	const int Epochs = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : 12;
	pSelf->m_pImpl->TrainOffline(pFile, Epochs);
}

void CKoGAIController::ConResetModel(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	auto *pSelf = static_cast<CKoGAIController *>(pUserData);
	pSelf->m_pImpl->ResetModel();
}
