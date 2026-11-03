#pragma once

#include "GameProtocol.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Weapon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// 방 하나. 방 안 상태는 게임 스레드 하나만 만지므로 잠금이 없다.
// - 왜: 플랜 확정값 = 게임 스레드 N개에 방을 고정 배정. 소켓은 모르고 IRoomSender 로만 내보낸다(4단계에서 방을 늘려도 이 파일은 그대로).
namespace mo
{
	class IRoomSender
	{
	public:
		virtual ~IRoomSender() = default;
		virtual void SendToSession(int64_t session, const void* data, int size) = 0;
		virtual void OnKillRecorded(const wire::Kill& kill) { (void)kill; }		// 서버 쪽 점수 기록
	};

	struct RoomStats
	{
		uint64_t ticks = 0;
		uint64_t inputsReceived = 0;
		uint64_t inputsApplied = 0;
		uint64_t inputsDroppedFull = 0;		// 입력 대기열이 꽉 차서 버림
		uint64_t inputsDroppedOldSeq = 0;	// 번호가 앞선 입력보다 작거나 같음
		uint64_t inputsWhileDead = 0;		// 죽어 있는 동안 온 입력 — 번호만 기록하고 안 쓴다
		uint64_t starvedTicks = 0;			// 입력이 안 와서 그 틱에 꺼낼 것이 없음
		uint64_t idleMoves = 0;				// 입력 없이 오래 지나 빈 입력으로 움직임(공중에 멈추지 않게)
		uint64_t debugPlaces = 0;
		uint64_t statesSent = 0;
		uint64_t shots = 0;
		uint64_t firesBlockedPostGame = 0;	// 경기 끝 단계의 발사 입력 — GE_BlockAbilityInput
		uint64_t bullets = 0;
		uint64_t bulletsHitMap = 0;
		uint64_t bulletsHitBody = 0;
		uint64_t bulletsHitImmune = 0;		// 몸에 맞았지만 무적이라 피해 없음
		uint64_t bulletsMissed = 0;
		uint64_t damageEvents = 0;
		uint64_t kills = 0;					// 맞아서 죽음
		uint64_t falls = 0;					// 떨어져 죽음
		uint64_t spawns = 0;
		uint64_t spawnsFromFallback = 0;	// 적이 없거나 자리가 모두 찜돼 무작위·대체 자리로
		uint64_t phaseChanges = 0;

		// 권위 확인 — 일부러 보낸 나쁜 입력을 사유별로 센다. 정직한 클라는 여기 일곱이 전부 0 이어야 한다
		uint64_t inputsRejectedBadValue = 0;	// NaN·무한대 — 통째로 버림
		uint64_t movesOverLength = 0;			// 이동 입력 길이가 1을 넘음 — 1로 줄여 씀
		uint64_t weaponIdRejected = 0;			// 없는 무기 번호 — 든 무기 그대로
		uint64_t debugPlacesRejected = 0;		// 시험 입력이 꺼진 서버에 온 자리 옮기기
		uint64_t shotsAimReplaced = 0;			// 사격 시작점이 내 위치에서 허용 범위 밖 — 서버가 아는 눈높이로 바꿔 쏜다
		uint64_t firesRejectedSwap = 0;			// 무기를 바꿔 발사 간격을 건너뛰려 함
		uint64_t shotsAimPulledBack = 0;		// 사격 시작점이 벽 너머 — 벽 앞으로 당겨 쏜다

		uint64_t shotsAimStaleAfterSpawn = 0;	// 부활 소식을 듣기 전에 보낸 입력의 시작점 — 정직해도 생긴다. 바꿔 쏘는 것은 같다

		// 버튼을 누르고 있으면 정직해도 쌓인다 — 거절이 아니라 무기 규칙이 안 쏜 횟수
		uint64_t firesTooSoon = 0;				// 발사 간격이 안 지남
		uint64_t firesEmpty = 0;				// 탄창 0

		// 틱 안에서 쓴 시간 — 어느 구간이 가장 오래 걸렸나
		uint64_t timeInputsNs = 0;				// 입력 처리·이동
		uint64_t timeShotsNs = 0;				// 광선 판정(결과 보내기 포함)
		uint64_t timeStateNs = 0;				// 상태 조립·보내기
	};

	struct RoomResult
	{
		wire::Result packet;
		int packetSize = 0;
	};

	class CRoom
	{
	public:
		CRoom(const CMapCollision& map, const MoveParams& params, const MatchRules& rules, uint32_t seed, bool allowDebug, IRoomSender& sender, uint32_t roomId);

		// 반환 = 방 안에서 쓰는 번호. 자리가 없거나 경기가 끝나 가면 0. 환영과 시작 자리는 여기서 보낸다
		uint32_t AddPlayer(int64_t session);
		void RemovePlayer(int64_t session);
		bool HasPlayer(int64_t session) const { return Find(session) != nullptr; }
		size_t GetPlayerCount() const { return _players.size(); }

		void OnInput(int64_t session, const wire::Input& input);
		void OnDebugPlace(int64_t session, const wire::DebugPlace& place);

		// 경기 단계 → 부활 → 입력 적용(이동 → 무기) → 사격 판정 → 상태 전송. 사격은 모두 움직인 뒤에 판정한다 — 처리 순서가 결과를 바꾸지 않게
		void Tick();

		wire::EMatchPhase GetPhase() const { return _phase; }
		const RoomResult& GetResult() const { return _result; }		// Finished 부터 뜻이 있다
		const RoomStats& GetStats() const { return _stats; }

	private:
		static constexpr int kInputQueueSize = 32;		// 0.53초치
		static constexpr int kMaxInputTokens = 8;		// 입력이 밀렸을 때 한 틱에 더 꺼낼 수 있는 한도
		static constexpr int kIdleMoveAfterTicks = 15;	// 입력이 이만큼 안 오면 빈 입력으로 움직임

		struct QueuedInput
		{
			wire::Input input;
			wire::DebugPlace place;
			bool isPlace;
		};

		struct Player
		{
			uint32_t id = 0;
			int64_t session = 0;
			uint8_t team = 0;
			MoveState move;
			std::array<CWeaponCycle, (size_t)EWeapon::Count> weapons;
			CWeaponFireGate fireGate;
			uint8_t weapon = 0;
			float hp = 0;
			float yaw = 0, pitch = 0;
			int8_t moveX = 0, moveY = 0;	// 마지막 누름 방향 — 상태 패킷에 실어 보낸다
			bool jumpHeld = false;
			bool dead = false;
			uint32_t respawnTick = 0;		// dead 일 때
			uint32_t immuneUntilTick = 0;
			int moveLockInputs = 0;			// 틱이 아니라 입력 개수로 센다 — 클라가 같은 입력을 돌려 보면 같은 결과가 나오게
			uint16_t kills = 0;
			uint16_t deaths = 0;
			uint32_t lastSeq = 0;
			uint32_t lastQueuedSeq = 0;
			int starvedInRow = 0;
			int tokens = 1;
			std::array<QueuedInput, kInputQueueSize> queue;
			int queueHead = 0;
			int queueCount = 0;
		};

		struct PendingShot
		{
			uint32_t shooter;
			uint8_t weapon;
			Vec3 start;
			Vec3 aimDir;
			float spreadAngle;
			float spreadMultiplier;
		};

		struct StartSlot
		{
			SpawnPoint point;
			uint32_t claimedBy = 0;		// ALyraPlayerStart::ClaimingController
		};

		Player* Find(int64_t session);
		const Player* Find(int64_t session) const;
		Player* FindById(uint32_t id);
		bool PushQueue(Player& player, const QueuedInput& item, uint32_t seq);
		void TickPhase();
		void EnterPhase(wire::EMatchPhase phase);
		void TickRespawns();
		void TickClaims();
		void TickPlayerInputs(Player& player);
		void ApplyInput(Player& player, const wire::Input& input);
		void ApplyPlace(Player& player, const wire::DebugPlace& place);
		bool IsStartOccupied(const StartSlot& slot) const;
		int ChooseStart(const Player& player);
		void SpawnPlayer(Player& player);
		void KillPlayer(Player& target, Player* attacker, uint8_t weapon, bool weakSpot);
		bool IsImmune(const Player& player) const;
		Vec3 ResolveAimStart(const Player& player, const Vec3& locationBeforeMove, const Vec3& aim);
		void ResolveShots();
		void ResolveShot(const PendingShot& shot);
		void HandleVictory(uint8_t team);
		void SendMatch();
		void SendState();
		void SendToAll(const void* data, int size);
		void BuildResult();
		uint8_t ChooseTeam() const;

		const CMapCollision& _map;
		const MoveParams _params;
		const MatchRules _rules;
		const bool _allowDebug;
		IRoomSender& _sender;
		const uint32_t _roomId;
		CMoveSim _moveSim;
		CSpreadRng _rng;
		CSpreadRng _spawnRng;		// 탄 난수와 따로 — 시험 클라가 탄 난수를 그대로 따라갈 수 있게
		uint32_t _tick = 0;
		uint32_t _nextPlayerId = 1;
		wire::EMatchPhase _phase = wire::EMatchPhase::Waiting;
		uint32_t _phaseEndTick = 0;
		uint16_t _targetScore = 0;
		uint16_t _teamScoreA = 0;
		uint16_t _teamScoreB = 0;
		uint8_t _winningTeam = wire::kTeamNone;
		std::vector<std::unique_ptr<Player>> _players;
		std::vector<StartSlot> _starts;
		std::vector<PendingShot> _pendingShots;
		std::vector<BodyPlacement> _bodies;		// 사격 판정 때마다 다시 채운다 — 매번 할당하지 않으려고 멤버로 둔다
		std::vector<Player*> _bodyOwners;
		RoomResult _result;
		RoomStats _stats;
	};
}
