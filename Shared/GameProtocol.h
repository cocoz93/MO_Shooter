#pragma once

#include <cstdint>

// 서버와 클라(봇·Lyra)가 주고받는 와이어 형식. 리틀 엔디언, 1바이트 정렬, 좌표는 언리얼 그대로 cm.
// - 왜: 메시지마다 잃어도 되는지를 처음부터 적어 둔다. 지금은 TCP 라 잃지 않지만 5단계 UDP 비교 때 이 구분을 그대로 쓴다.
// - 소켓은 모른다. 구조체 정의뿐이라 언리얼 안에서도 컴파일된다.
namespace mo
{
	namespace wire
	{
		constexpr uint32_t kProtocolVersion = 2;
		constexpr int kTickRate = 60;
		constexpr int kMaxPlayersInRoom = 10;
		constexpr int kMaxBulletsInShot = 9;	// 샷건 한 발의 탄 수

		enum class Msg : uint16_t
		{
			// 클라 → 서버
			C2S_HELLO = 1,			// 잃으면 안 됨
			C2S_INPUT = 2,			// 잃어도 됨 — 다음 입력이 곧 온다(잃으면 그 틱만큼 입력이 빔)
			C2S_DEBUG_PLACE = 3,	// 잃으면 안 됨 — 서버를 --debug 로 띄웠을 때만 받는다

			// 서버 → 클라
			S2C_WELCOME = 100,		// 잃으면 안 됨
			S2C_STATE = 101,		// 잃어도 됨 — 다음 틱 상태가 덮는다
			S2C_SHOT = 102,			// 잃어도 됨 — 화면 효과용
			S2C_DAMAGE = 103,		// 잃으면 안 됨 — 체력 변화 기록
			S2C_MATCH = 104,		// 잃으면 안 됨 — 경기 단계·점수. 단계가 바뀔 때와 1초마다
			S2C_KILL = 105,			// 잃으면 안 됨 — 처치 기록
			S2C_SPAWN = 106,		// 잃으면 안 됨 — 시작 자리. 클라는 여기서 위치를 다시 잡는다
			S2C_RESULT = 107,		// 잃으면 안 됨 — 경기 끝의 점수 기록
		};

		enum EButton : uint8_t
		{
			BUTTON_JUMP = 1 << 0,	// 누르는 동안 켠다
			BUTTON_FIRE = 1 << 1,	// 누르는 동안 켠다
			BUTTON_RELOAD = 1 << 2,	// 누른 입력 하나에만 켠다
		};

		enum EStateFlag : uint8_t
		{
			STATE_FALLING = 1 << 0,
			STATE_RELOADING = 1 << 1,
			STATE_DEAD = 1 << 2,
			STATE_IMMUNE = 1 << 3,		// 경기 전 대기·부활 직후·경기 끝 — 맞아도 피해 없음
			STATE_MOVE_LOCKED = 1 << 4,	// 부활 직후 — 이동 입력을 안 받는다
		};

		enum class EMatchPhase : uint8_t { Waiting, Warmup, Playing, PostGame, Finished };

		constexpr uint8_t kTeamNone = 0;	// 무승부·아직 안 끝남
		constexpr uint8_t kTeamA = 1;		// 번호는 Lyra 와 같다(B_TeamSetup_TwoTeams 1·2)
		constexpr uint8_t kTeamB = 2;

		enum class EShotHit : uint8_t { None, Map, Body };

#pragma pack(push, 1)
		struct Header
		{
			uint16_t size;	// 머리말 포함 전체 — ServerCore 의 MsgHeader 와 같은 4바이트
			uint16_t type;	// Msg
		};

		struct Hello
		{
			Header h;
			uint32_t protocolVersion;
		};

		// 입력 하나 = 서버 한 틱(1/60초). 서버는 틱마다 하나씩 꺼낸다
		struct Input
		{
			Header h;
			uint32_t seq;			// 1부터 하나씩 늘린다
			float moveX, moveY;		// 월드 축. 길이 1 이하
			float yaw, pitch;		// 도
			float aimX, aimY, aimZ;	// 사격 시작점 — Lyra 는 카메라 쪽에서 쏜다
			uint8_t buttons;		// EButton
			uint8_t weapon;			// EWeapon
		};

		// 시험용 — 입력과 같은 순서로 처리한다
		struct DebugPlace
		{
			Header h;
			uint32_t seq;
			double x, y, z;
			double vx, vy, vz;
			float hp;
			uint8_t falling;
		};

		struct Welcome
		{
			Header h;
			uint32_t playerId;
			uint32_t tick;
			uint8_t team;
			uint8_t debugAllowed;
			uint32_t roomId;		// 서버 안의 방 번호 — 시험 클라가 같은 방끼리 기록을 맞춰 본다
		};

		struct PlayerState
		{
			uint32_t id;
			uint32_t lastSeq;		// 이 상태까지 반영한 입력 — 클라 보정의 기준
			float x, y, z;			// 캡슐 중심
			float vx, vy, vz;
			float yaw, pitch;
			float hp;
			int16_t magazine, spare;
			uint8_t flags;			// EStateFlag
			uint8_t weapon;
			uint8_t team;
			int8_t moveX, moveY;	// 누름 방향(127 = 1.0) — 남의 캐릭터 애니가 가속으로 읽는다. 판정에는 안 쓴다
		};

		// 보낼 때는 count 만큼만 잘라 보낸다
		struct State
		{
			Header h;
			uint32_t tick;
			uint8_t count;
			PlayerState players[kMaxPlayersInRoom];
		};

		struct ShotBullet
		{
			float x, y, z;			// 맞은 점. 빗나가면 사거리 끝
			float damage;			// 같은 팀이면 0
			uint32_t target;		// 몸을 맞혔을 때만 뜻이 있다
			uint8_t hit;			// EShotHit
			uint8_t weakSpot;
		};

		// 보낼 때는 count 만큼만 잘라 보낸다
		struct Shot
		{
			Header h;
			uint32_t tick;
			uint32_t shooter;
			float startX, startY, startZ;
			uint8_t weapon;
			uint8_t count;
			ShotBullet bullets[kMaxBulletsInShot];
		};

		struct Damage
		{
			Header h;
			uint32_t tick;
			uint32_t attacker;
			uint32_t target;
			float amount;
			float hpAfter;
			uint8_t weapon;
			uint8_t weakSpot;
		};

		struct Match
		{
			Header h;
			uint32_t tick;
			float secondsLeft;		// 지금 단계가 끝날 때까지
			uint16_t targetScore;
			uint16_t teamScoreA, teamScoreB;
			uint8_t phase;			// EMatchPhase
			uint8_t winningTeam;	// PostGame 부터 뜻이 있다. kTeamNone = 무승부
		};

		struct Kill
		{
			Header h;
			uint32_t tick;
			uint32_t attacker;		// 0 = 떨어져 죽음
			uint32_t target;
			uint16_t teamScoreA, teamScoreB;
			uint16_t attackerKills;
			uint16_t targetDeaths;
			uint8_t weapon;
			uint8_t weakSpot;
		};

		struct Spawn
		{
			Header h;
			uint32_t tick;
			uint32_t player;
			uint32_t lastSeq;		// 이 번호까지의 입력은 부활 전에 처리됐다 — 그 뒤 입력부터 새 자리에서 다시 계산한다
			double x, y, z;		// float 로 줄이면 클라가 같은 입력을 돌려도 어긋난다
			float yaw;
			uint8_t startIndex;
		};

		struct PlayerResult
		{
			uint32_t id;
			uint16_t kills;
			uint16_t deaths;
			uint8_t team;
		};

		// 보낼 때는 count 만큼만 잘라 보낸다
		struct Result
		{
			Header h;
			uint32_t tick;
			uint16_t targetScore;
			uint16_t teamScoreA, teamScoreB;
			uint8_t winningTeam;
			uint8_t count;
			PlayerResult players[kMaxPlayersInRoom];
		};
#pragma pack(pop)

		static_assert(sizeof(Header) == 4, "ServerCore MsgHeader 와 같아야 한다");
		static_assert(sizeof(State) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
		static_assert(sizeof(Shot) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
		static_assert(sizeof(Result) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
	}
}
