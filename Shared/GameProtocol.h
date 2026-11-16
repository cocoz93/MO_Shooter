#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 서버와 클라(봇·Lyra)가 주고받는 와이어 형식. 리틀 엔디언, 1바이트 정렬, 좌표는 언리얼 그대로 cm.
// - 왜: 메시지마다 잃어도 되는지를 처음부터 적어 둔다. 지금은 TCP 라 잃지 않지만 5단계 UDP 비교 때 이 구분을 그대로 쓴다.
// - 소켓은 모른다. 구조체 정의와 애니 꼬리 쓰기 · 읽기 식뿐이라 언리얼 안에서도 컴파일된다.
namespace mo
{
	namespace wire
	{
		constexpr uint32_t kProtocolVersion = 6;		// 6 = 애니 꼬리에 조준 묶음 비중 · 겹침 칸 번호에 조준 전환 12개(컨텐츠 C-3, D60) · 조준 버튼 BUTTON_ADS · 상태 STATE_ADS(C-4, D61)
														// 5 = 애니 꼬리에 몽타주 칸 · 덮기 · 왼손 비중(9단계 조각 4, D51)과 대기 몸짓 칸 눈금 1/3000초(D47)
		constexpr int kTickRate = 60;
		constexpr int kMaxPlayersInRoom = 10;
		constexpr int kMaxBulletsInShot = 9;	// 샷건 한 발의 탄 수
		constexpr double kRemoteTeleportDistance = 200.0;	// 한 틱에 이만큼 뛰면 부활·순간이동 — 화면도 되감기도 보간하지 않는다

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
			BUTTON_ADS = 1 << 3,	// 누르는 동안 켠다 — 그 입력부터 조준(C-4, D61)
		};

		enum EStateFlag : uint8_t
		{
			STATE_FALLING = 1 << 0,
			STATE_RELOADING = 1 << 1,
			STATE_DEAD = 1 << 2,
			STATE_IMMUNE = 1 << 3,		// 경기 전 대기·부활 직후·경기 끝 — 맞아도 피해 없음
			STATE_MOVE_LOCKED = 1 << 4,	// 부활 직후 — 이동 입력을 안 받는다
			STATE_ADS = 1 << 5,			// 조준 능력이 켜져 있다(GA_ADS) — 걸음 속도 · 퍼짐 · 조준 자세가 이 값을 따른다
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
			uint32_t viewTick;		// 쏠 때 화면에 그리던 틱의 정수 부분. 0 = 시점 없음 → 서버는 현재 자리로 판정
			float viewAlpha;		// viewTick 에서 다음 틱까지의 비율 0~1 — 틱 간격이 달리기로 약 8cm 라 정수 틱만으로는 모자란다
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

		// 애니 상태 한 사람치. S2C_STATE 의 players[count] 바로 뒤에 같은 차례로 붙는다(꼬리, 클립 파일을 준 서버만).
		// - 머리 뒤에 겹침 칸 layerCount 개, 그 뒤에 몽타주 칸 montageCount 개만 싣는다 — 사람마다 22 + 4 × layerCount + 4 × montageCount 바이트, 최대 50(D35 · D51 · D60).
		//   쓰고 읽는 식은 아래 WriteAnimTail · ReadAnimTail 한 벌이다(서버가 쓰고 봇 · 다리가 읽는다)
		// - 앞부분(State)만 읽는 쪽은 count 로 끝을 정하므로 꼬리를 무시해도 깨지지 않는다
		inline constexpr int kAnimLayersOnWire = 4;
		inline constexpr int kAnimMontagesOnWire = 3;			// 몽타주 칸 최대 = Lyra 기록 세 무기의 동시 최대(D45). CAnimState 의 kAnimMontageMax 와 같다
		inline constexpr int kAnimMontageKindBits = 4;			// 몽타주 칸 번호 = 묶음 번호 << 4 | 종류 — 묶음 16개 · 종류 16개까지
		inline constexpr uint16_t kAnimTimeNone = 0xFFFF;		// 대기 몸짓 · 착지 충격을 안 하는 중

		struct AnimLayerWire
		{
			int8_t clip;			// 쌓기 칸 번호 0~28 = 점프 5 · 허리 전환 12 · 조준 전환 12(AnimClips.h StackClipIndex), -1 = 걷기 · 서기 자리
			uint16_t time;			// 1/6000초 칸
			uint8_t fade;			// 아래 칸을 덮은 정도 0~255
		};

		// 자세에 들어가는 몽타주 한 칸(서버가 사건 앞 목록을 그 차례 그대로 싣는다)
		struct AnimMontageWire
		{
			uint8_t montage;		// 묶음 번호(클립 파일 차례) << 4 | 종류(EMontageKind)
			uint16_t time;			// 몽타주 시각, 1/6000초 칸
			uint8_t weight;			// 몽타주 비중 0~255. 0 으로 반올림되는 칸은 안 싣는다
		};

		struct AnimStateWire
		{
			uint8_t weights[5];		// 서기 · 앞 · 뒤 · 왼 · 오른 비중 0~255. 받는 쪽이 합을 1 로 맞춘다
			uint8_t step;			// 몇 번째 걸음인가
			uint16_t stepFrac;		// 그 걸음의 진행 0~65535
			uint16_t idleTime;		// 서기 클립 시각, 1/6000초 칸
			uint8_t aimBlend;		// 255 = 허리 사격 조준만, 0 = 느슨한 조준만
			uint8_t adsBlend;		// 조준 묶음 비중 0~255(D60) — 0 = 허리 서기 · 걸음 클립만, 255 = 조준 클립만. 걸음 번호 · 진행률 · 서기 시각은 두 묶음이 같이 쓴다
			uint16_t fidgetTime;	// 대기 몸짓 시각, 1/3000초 칸(권총 몸짓 13.8초가 1/6000초 칸 최대 10.92초를 넘어서 — D47). kAnimTimeNone = 안 함
			uint16_t recoveryTime;	// 착지 충격 시각. kAnimTimeNone = 안 함
			int16_t plantDrop;		// 발 디딤 내림 0.01cm 칸
			uint8_t overrideWeight;	// 쏜 뒤 윗몸 덮기 비중 0~255(D45). 받는 쪽은 쏜 뒤 시간을 세지 않고 이 값을 쓴다(D51)
			uint8_t handLeftAlpha;	// 왼손 붙이기 비중 0~255 — 앞 자세의 몽타주 곡선에서 나와 받는 쪽이 셀 수 없다(D51)
			uint8_t montageCount;	// 뒤에 실린 몽타주 칸 수
			uint8_t layerCount;		// 뒤에 실린 겹침 칸 수
			AnimLayerWire layers[kAnimLayersOnWire];
			AnimMontageWire montages[kAnimMontagesOnWire];		// 겹침 칸 layerCount 개 바로 뒤에 붙는다
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
		static_assert(sizeof(AnimStateWire) == 50, "패킷 크기 어림(D35 · D51 · D60)과 같아야 한다");
		inline constexpr size_t kAnimStateHeadSize = offsetof(AnimStateWire, layers);
		static_assert(kAnimStateHeadSize == 22, "머리 = 18 + 덮기 · 왼손 · 몽타주 칸 수(D51) + 조준 묶음 비중(D60)");
		inline constexpr size_t kMaxStatePacketSize = sizeof(State) + sizeof(AnimStateWire) * kMaxPlayersInRoom;		// 꼬리까지 넣은 최대
		static_assert(kMaxStatePacketSize <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");

		// 꼬리 한 사람치의 바이트 수
		inline size_t AnimTailSize(const AnimStateWire& anim)
		{
			return kAnimStateHeadSize + sizeof(AnimLayerWire) * anim.layerCount + sizeof(AnimMontageWire) * anim.montageCount;
		}

		// 꼬리 한 사람치를 out 에 쓴다 — 머리 · 겹침 칸 · 몽타주 칸 차례. 쓴 바이트 수(AnimTailSize)를 돌려준다
		inline size_t WriteAnimTail(const AnimStateWire& anim, void* out)
		{
			uint8_t* at = static_cast<uint8_t*>(out);
			const size_t layerBytes = sizeof(AnimLayerWire) * anim.layerCount;
			std::memcpy(at, &anim, kAnimStateHeadSize);
			std::memcpy(at + kAnimStateHeadSize, anim.layers, layerBytes);
			std::memcpy(at + kAnimStateHeadSize + layerBytes, anim.montages, sizeof(AnimMontageWire) * anim.montageCount);
			return AnimTailSize(anim);
		}

		// 받은 바이트에서 꼬리 한 사람치를 읽는다. false = 머리나 칸이 잘렸거나 칸 수가 상한 밖(그때 out · used 는 뜻이 없다)
		inline bool ReadAnimTail(const void* data, size_t size, AnimStateWire& out, size_t& used)
		{
			const uint8_t* at = static_cast<const uint8_t*>(data);
			if (size < kAnimStateHeadSize) return false;
			std::memcpy(&out, at, kAnimStateHeadSize);
			if (out.layerCount > kAnimLayersOnWire || out.montageCount > kAnimMontagesOnWire) return false;
			const size_t layerBytes = sizeof(AnimLayerWire) * out.layerCount;
			const size_t montageBytes = sizeof(AnimMontageWire) * out.montageCount;
			if (size < kAnimStateHeadSize + layerBytes + montageBytes) return false;
			std::memcpy(out.layers, at + kAnimStateHeadSize, layerBytes);
			std::memcpy(out.montages, at + kAnimStateHeadSize + layerBytes, montageBytes);
			used = kAnimStateHeadSize + layerBytes + montageBytes;
			return true;
		}
		static_assert(sizeof(Shot) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
		static_assert(sizeof(Result) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
	}
}
