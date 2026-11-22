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
		constexpr uint32_t kProtocolVersion = 17;		// 17 = 애니 꼬리 걷기 자리 나누기(L-13-a) — 쌓기 칸 번호 -2(서기 자리) · 바탕 자리 baseSlot · weights 방향 넷이 움직임 자리 방향 비중 · 16 =상태 꼬리 장치 발 높이 footLift · 장치 알파 plantAlpha(L-12-e) · 15 = 상태 꼬리 다리 IK 비중 legIKAlpha(L-12-c) · 14 = 몽타주 칸 빼기 진행 blendOut · 입력 끝 누름 차례 pressOrder · 받침 까닭 PICKUP_FAIL_FULL(L-4)
														// 13 = 수류탄 단추 BUTTON_GRENADE 를 보냄 · 입력 끝 카메라 거리 camBack · 수류탄 소식 S2C_GRENADE · 수류탄 상태 S2C_GRENADE_STATE(컨텐츠 G-3)
														// 12 = 대시 단추 BUTTON_DASH 를 보냄 · 대시 소식 S2C_DASH(컨텐츠 D-3)
														// 11 = 상태의 빠른 칸 quickSlots · 받침 소식의 종류 · 줍기 결과 · 무기 · 칸 · 탄 · 입력 번호 · 시험 입력의 탄 정하기 · 죽음(컨텐츠 W-2 · W-3)
														// 10 = 근접 단추 BUTTON_MELEE · 근접 결과 S2C_MELEE · 피해 · 처치 소식의 무기 번호 근접 kWeaponMelee(컨텐츠 E-3)
														// 9 = 체력 받침 상태 S2C_PICKUP(컨텐츠 B-3)
														// 7 = 애니 꼬리에 앉기 묶음 비중 · 겹침 칸 번호에 앉은 전환 14개(컨텐츠 D-3, D62) · 앉기 버튼 BUTTON_CROUCH · 상태 STATE_CROUCHED(D-5)
														// 6 = 애니 꼬리에 조준 묶음 비중 · 겹침 칸 번호에 조준 전환 12개(컨텐츠 C-3, D60) · 조준 버튼 BUTTON_ADS · 상태 STATE_ADS(C-4, D61)
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
			S2C_PICKUP = 108,		// 잃으면 안 됨 — 체력 받침 상태. 집음 · 다시 켜짐 때와, 들어올 때 · 경기 시작에 받침마다 하나(컨텐츠 B-3)
			S2C_MELEE = 109,		// 잃으면 안 됨 — 근접 결과. 서버가 근접 능력을 켤 때마다 방 전원에게. 공격자 화면은 이것으로 제 밀기 예측을 고친다(컨텐츠 E-3)
			S2C_DASH = 110,			// 잃으면 안 됨 — 대시 켬. 서버가 대시를 켤 때마다 방 전원에게. 켠 사람 화면은 이것으로 제 대시 예측(켬 · 방향 · 힘)을 고친다(컨텐츠 D-3)
			S2C_GRENADE = 111,		// 잃으면 안 됨 — 수류탄 던짐 · 터짐(EGrenadeEvent). 방 전원에게. 던진 사람 화면은 던짐으로 제 쿨다운 예측을 고친다(컨텐츠 G-3)
			S2C_GRENADE_STATE = 112,	// 잃어도 됨 — 산 수류탄마다 자리 · 회전 · 이번 틱 부딪힘. 산 수류탄이 있는 틱에만 상태 패킷 바로 뒤에(다음 틱이 덮는다)
		};

		enum EButton : uint8_t
		{
			BUTTON_JUMP = 1 << 0,	// 누르는 동안 켠다
			BUTTON_FIRE = 1 << 1,	// 누르는 동안 켠다
			BUTTON_RELOAD = 1 << 2,	// 누른 입력 하나에만 켠다
			BUTTON_ADS = 1 << 3,	// 누르는 동안 켠다 — 그 입력부터 조준(C-4, D61)
			BUTTON_CROUCH = 1 << 4,	// 누른 입력 하나에만 켠다 — 앉기 키 한 번 = ALyraCharacter::ToggleCrouch 한 번(D-5)
			BUTTON_MELEE = 1 << 5,	// Lyra 근접 입력이 눌림을 낸 입력에 켠다 — 근접 능력이 꺼져 있으면 그 입력에 켜지고, 켜진 동안의 누름은 버린다(E-3)
			BUTTON_DASH = 1 << 6,	// Lyra 대시 입력이 눌림을 낸 입력에 켠다 — 켜짐 · 쿨다운 중이면 버린다(서버 규칙 D-2). 보내는 쪽 = 봇 --dash(D-3) · 다리 Shift(D-5)
			BUTTON_GRENADE = 1 << 7,	// Q 를 누른 입력 하나에만 켠다 — 쿨다운 · 대시 중이면 버린다(서버 규칙 G-2). 판 13 · 카메라 거리는 입력 camBack(G-3). 보내는 쪽 = 봇 --grenade(G-3) · 다리 Q(G-5)
		};

		// S2C_PICKUP 의 까닭 — 화면 효과를 낼지 다리가 이것으로 가른다
		enum EPickupReason : uint8_t
		{
			PICKUP_TAKE = 0,			// 겹침이 시작돼 집음(ALyraWeaponSpawner::OnOverlapBegin)
			PICKUP_TAKE_RECHECK = 1,	// 다시 켜진 0.25초 뒤 이미 서 있던 사람이 집음(CheckForExistingOverlaps)
			PICKUP_READY = 2,			// 대기가 끝나 다시 켜짐(ResetCoolDown)
			PICKUP_SYNC = 3,			// 지금 상태만 — 들어올 때 · 경기 시작(받침을 새로 놓음). 화면 효과 없음
			PICKUP_FAIL_FULL = 4,		// 무기 받침을 집으려다 예비 가득이라 못 집음(B_WeaponSpawner GiveWeapon 실패 길 — Lyra 는 「예비 탄약 가득 참」을 띄운다, W-0 FAILMSG).
										// 집으려던 사람에게만 · 받침 상태는 그대로 · give = Full 과 그 무기 · 칸 · 탄을 싣는다(L-4). 경기 끝 단계 · 빈 칸 없음 실패는 Lyra 알림 기록이 없어 안 보낸다
		};

		enum EStateFlag : uint8_t
		{
			STATE_FALLING = 1 << 0,
			STATE_RELOADING = 1 << 1,
			STATE_DEAD = 1 << 2,
			STATE_IMMUNE = 1 << 3,		// 경기 전 대기·부활 직후·경기 끝 — 맞아도 피해 없음
			STATE_MOVE_LOCKED = 1 << 4,	// 부활 직후 — 이동 입력을 안 받는다
			STATE_ADS = 1 << 5,			// 조준 능력이 켜져 있다(GA_ADS) — 걸음 속도 · 퍼짐 · 조준 자세가 이 값을 따른다
			STATE_CROUCHED = 1 << 6,	// 앉아 있다(bIsCrouched) — 캡슐 반높이 · 앉은 걸음 · 퍼짐 · 앉은 자세 · 메시 올림이 이 값을 따른다. 받는 쪽 애니도 앉음을 여기서 읽는다(D62 8)
		};

		enum class EMatchPhase : uint8_t { Waiting, Warmup, Playing, PostGame, Finished };

		constexpr uint8_t kTeamNone = 0;	// 무승부·아직 안 끝남
		constexpr uint8_t kTeamA = 1;		// 번호는 Lyra 와 같다(B_TeamSetup_TwoTeams 1·2)
		constexpr uint8_t kTeamB = 2;

		// 피해 · 처치 소식의 무기 번호 — 0~2 = EWeapon, 이 값 = 근접(무기 셋 뒤 번호, 컨텐츠 E-3). EWeapon::Count 와 같은지는 서버가 검사한다
		constexpr uint8_t kWeaponMelee = 3;
		constexpr uint8_t kWeaponGrenade = 4;	// 수류탄 폭발(컨텐츠 G-2) — 근접 다음 번호. 서버가 검사한다. 받는 쪽(봇 · 다리) 셈은 G-3 · G-5

		// S2C_MELEE 의 결과 — Shared EMeleeHit 와 같은 차례(서버가 검사한다)
		enum EMeleeResult : uint8_t
		{
			MELEE_NONE = 0,		// 아무 몸에도 안 닿음
			MELEE_SAME_TEAM = 1,	// 처음 닿은 몸이 같은 팀 — 피해 · 밀기 · 반응 없음
			MELEE_BLOCKED = 2,		// 손 → 닿은 점 선이 벽에 막힘
			MELEE_HIT = 3,
		};

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
			float camBack;			// 카메라에서 사격 시작점(aim)까지 시선 방향 거리(cm) — 수류탄 겨눔 선이 카메라에서 시작한다(G-3). 서버가 정직한 Lyra 값이 드는 범위로 자른다(ClampGrenadeCamBack · L-3-c)
			uint8_t pressOrder;		// 이 입력에 켜진 근접 · 대시 · 수류탄 · 재장전의 누른 차례(L-4 · 판 14) — 2비트씩 k 번째 칸 = k 번째로 누른 단추(EPressKey). 쓰고 읽기는 아래 MakePressOrder · ReadPressOrder
		};

		// 시험 입력 DebugPlace 의 덧붙임(flags) — 0 이면 자리 · 체력만(전부터의 뜻)
		enum EDebugPlaceFlag : uint8_t
		{
			DEBUG_PLACE_SET_AMMO = 1 << 0,	// 놓은 뒤 ammoWeapon 의 탄창 · 예비를 magazine · spare 로 — 무기 줍기 대조(W-2). Lyra 기록기가 쏘는 대신 탄 표식을 바꾼 단계와 같다
			DEBUG_PLACE_KILL = 1 << 1,		// 놓은 뒤 스스로 죽음 — Lyra 기록기의 DamageSelfDestruct(처치자 없음)
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
			uint8_t flags;			// EDebugPlaceFlag
			uint8_t ammoWeapon;		// DEBUG_PLACE_SET_AMMO 의 무기(EWeapon)
			int16_t magazine, spare;
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
			uint8_t quickSlots;		// 빠른 칸 셋(W-3) — 칸 i 가 비트 2i · 2i+1, 0 = 빈 칸 · 1 + 무기 번호. 읽고 쓰기는 CLoadout::PackSlots · UnpackSlot(Loadout.h). 든 칸 = weapon 이 든 칸
		};

		// 애니 상태 한 사람치. S2C_STATE 의 players[count] 바로 뒤에 같은 차례로 붙는다(꼬리, 클립 파일을 준 서버만).
		// - 머리 뒤에 겹침 칸 layerCount 개, 그 뒤에 몽타주 칸 montageCount 개만 싣는다 — 사람마다 30 + 4 × layerCount + 5 × montageCount 바이트, 최대 61(D35 · D51 · D60 · D62 · 몽타주 칸 빼기 진행 L-4 · 다리 IK 비중 L-12-c · 장치 발 높이 · 알파 L-12-e · 바탕 자리 L-13-a).
		//   쓰고 읽는 식은 아래 WriteAnimTail · ReadAnimTail 한 벌이다(서버가 쓰고 봇 · 다리가 읽는다)
		// - 앞부분(State)만 읽는 쪽은 count 로 끝을 정하므로 꼬리를 무시해도 깨지지 않는다
		inline constexpr int kAnimLayersOnWire = 4;
		inline constexpr int kAnimMontagesOnWire = 3;			// 몽타주 칸 최대 = Lyra 기록 세 무기의 동시 최대(D45). CAnimState 의 kAnimMontageMax 와 같다
		inline constexpr int kAnimMontageKindBits = 4;			// 몽타주 칸 번호 = 묶음 번호 << 4 | 종류 — 묶음 16개 · 종류 16개까지
		inline constexpr uint16_t kAnimTimeNone = 0xFFFF;		// 대기 몸짓 · 착지 충격을 안 하는 중

		struct AnimLayerWire
		{
			int8_t clip;			// 쌓기 칸 번호 0~42 = 점프 5 · 허리 전환 12 · 조준 전환 12 · 앉은 전환 12 · 서기 안 앉기 · 일어서기(AnimClips.h StackClipIndex), -1 = 움직임 자리 · -2 = 서기 자리(판 17, L-13-a)
			uint16_t time;			// 1/6000초 칸
			uint8_t fade;			// 아래 칸을 덮은 정도 0~255
		};

		// 자세에 들어가는 몽타주 한 칸(서버가 사건 앞 목록을 그 차례 그대로 싣는다)
		struct AnimMontageWire
		{
			uint8_t montage;		// 묶음 번호(클립 파일 차례) << 4 | 종류(EMontageKind)
			uint16_t time;			// 몽타주 시각, 1/6000초 칸
			uint8_t weight;			// 몽타주 비중 0~255. 0 으로 반올림되는 칸은 안 싣는다
			uint8_t blendOut;		// 빼기 진행(L-4 · 판 14) — 0 = 빼기 비중 표가 없거나 빠지는 중이 아님, 1~255 = 선형 진행 0~1(엔진 FAlphaBlend 의 선형 진행).
									// 엔진은 빼기 비중 표(대시 넷 = FastFeet)가 걸린 몽타주를 이 진행으로 뼈마다 빼는데(D103) 서버 되감기 판정이 와이어 기록으로 자세를 만들어 와이어에 싣는다.
									// 받는 쪽 자세 계산(AnimPose ApplySlot)이 이 진행으로 뼈마다 뺀다(L-6-b · D130) — 시작 진행은 1 로 본다(엔진과 다른 칸 = CAnimState::ProfileStartMisses)
		};

		struct AnimStateWire
		{
			uint8_t weights[5];		// [0] = 서기 비중(속도로 섞은 것) 0~255 · [1~4] = 움직임 자리 앞 · 뒤 · 왼 · 오른 비중(합 255 근처, 받는 쪽이 합 1 로 맞춘다 — 판 17, L-13-a)
			uint8_t step;			// 몇 번째 걸음인가
			uint16_t stepFrac;		// 그 걸음의 진행 0~65535
			uint16_t idleTime;		// 서기 클립 시각, 1/6000초 칸
			uint8_t aimBlend;		// 255 = 허리 사격 조준만, 0 = 느슨한 조준만
			uint8_t adsBlend;		// 조준 묶음 비중 0~255(D60) — 0 = 허리 서기 · 걸음 클립만, 255 = 조준 클립만. 걸음 번호 · 진행률 · 서기 시각은 두 묶음이 같이 쓴다
			uint8_t crouchBlend;	// 앉기 묶음 비중 0~255(D62 2) — 조준보다 먼저 나눈다. 앉음 자체는 사람 상태 칸에서 읽는다(D62 8)
			uint16_t fidgetTime;	// 대기 몸짓 시각, 1/3000초 칸(권총 몸짓 13.8초가 1/6000초 칸 최대 10.92초를 넘어서 — D47). kAnimTimeNone = 안 함
			uint16_t recoveryTime;	// 착지 충격 시각. kAnimTimeNone = 안 함
			int16_t plantDrop;		// 발 디딤 내림 0.01cm 칸
			int16_t footLift[2];	// 발 붙이기 장치 발 높이 스프링(왼 · 오른, 알파 곱하기 전) 0.01cm 칸(L-12-e) — 장치 상태라 받는 쪽이 셀 수 없다
			uint8_t plantAlpha;		// 발 붙이기 장치 켜짐 0~255(L-12-e) — 받는 쪽이 장치 다리 몫을 이 비중으로 덧붙이고, plantDrop 을 나눠 알파 곱하기 전 골반 내림을 얻는다
			uint8_t overrideWeight;	// 쏜 뒤 윗몸 덮기 비중 0~255(D45). 받는 쪽은 쏜 뒤 시간을 세지 않고 이 값을 쓴다(D51)
			uint8_t handLeftAlpha;	// 왼손 붙이기 비중 0~255 — 앞 자세의 몽타주 곡선에서 나와 받는 쪽이 셀 수 없다(D51)
			uint8_t legIKAlpha;		// 다리 IK 비중 0~255 = 1 − 앞 자세의 DisableLegIK 곡선(L-12-c · D150) — 왼손 비중과 같은 까닭으로 싣는다
			int8_t baseSlot;		// 겹침 칸이 없을 때 바탕 자리 -1 = 움직임 · -2 = 서기(판 17, L-13-a) — 쌓기가 걷기 자리 한 칸으로 줄면 칸을 안 싣고 그 번호만 싣는다
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
			uint8_t assistCount;		// 도움 받은 사람 수(ShooterCore AssistProcessor — 이 사람에게 피해를 준 사람 중 처치자를 뺀 전원)
			uint32_t assisters[kMaxPlayersInRoom - 1];	// 보낼 때는 assistCount 만큼만 잘라 보낸다. 떨어져 죽으면 처치자가 없어 죽은 사람 빼고 전원까지
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
			uint16_t assists;		// 처치 도움(A-2) — kills · deaths 와 같이 경기 시작에 0
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

		// 받침 하나의 상태. 받침 파일(--pads=)은 서버만 읽는다 — 자리를 같이 실어 다리는 맵의 받침 액터를, 봇은 갈 곳을 이것으로 안다
		struct Pickup
		{
			Header h;
			uint32_t tick;			// 이 일이 난 틱
			uint32_t readyTick;		// 다시 켜지는 틱. 켜져 있으면 0
			uint32_t cooldownTicks;	// 정한 대기(초) × 틱 수 — Lyra 대기 표시 CoolDownPercentage 의 분모(CoolDownTime)
			uint32_t picker;		// 집은 사람. 0 = 집음이 아님
			float heal;				// 실제로 오른 체력(최대에서 잘린 뒤). 집음이 아니면 0
			float hpAfter;			// 집은 사람의 회복 뒤 체력. 집음이 아니면 0
			float x, y, z;			// 받침 자리(받침 파일 값)
			uint8_t pad;			// 받침 번호 = 서버 받침 파일의 줄 차례
			uint8_t padCount;		// 서버 받침 수 — 받는 쪽이 칸을 미리 잡는다(kMaxPickupPads 255 라 한 바이트)
			uint8_t available;		// 1 = 켜짐
			uint8_t reason;			// EPickupReason
			// 아래는 W-3 — 받침 종류는 늘, 줍기 칸은 무기 받침을 집었을 때만(그 밖은 give = kPickupGiveNone · 나머지 0)
			uint8_t kind;			// 받침 종류 EPadKind(Pickups.h) 숫자 — 0 체력 · 1 권총 · 2 소총 · 3 샷건. 봇이 체력 받침만 찾아가려고
			uint8_t give;			// 줍기 결과 EGiveResult(Loadout.h) 숫자 — 집은 것만 오므로 새 무기(듦) · 새 무기(재장전 중이라 안 듦) · 탄 채움 셋 중 하나
			uint8_t weapon;			// 집은 무기 번호
			uint8_t slot;			// 그 무기가 든 빠른 칸
			int16_t magazine, spare;	// 집은 뒤 그 무기 탄창 · 예비
			uint32_t seq;			// 집은 사람이 서버에서 마지막으로 쓴 입력 번호 — 그 입력 뒤에 집었다(다시 검사로 집었으면 그 틱까지 쓴 입력). 예측이 이 입력부터 무기 쪽을 다시 돌린다
		};
		inline constexpr uint8_t kPickupGiveNone = 0xFF;

		// 근접 한 번(E-3) — 맞음 · 같은 팀 · 벽 · 빗나감 모두 보낸다(공격자 몽타주는 늘 돈다). 피해는 따로 S2C_DAMAGE(무기 번호 kWeaponMelee)로도 간다
		struct Melee
		{
			Header h;
			uint32_t tick;
			uint32_t attacker;
			uint32_t seq;			// 공격자 입력 번호 — 공격자 화면은 이 입력의 밀기 예측을 서버 값으로 바꾼다
			uint32_t target;		// 처음 닿은 몸의 주인. 0 = 안 닿음
			float distance;			// 쓸기 시작 → 처음 닿은 때 구 중심. 맞음이면 밀기 세기가 이것에 비례한다
			float forceX, forceY;	// 서버가 넣은 밀기 힘(맞음일 때만, 아니면 0). 서버도 이 float 값 그대로 넣는다 — 공격자 화면이 같은 값으로 다시 돌린다
			float damage;			// 준 피해. 무적이면 0
			uint8_t result;			// EMeleeResult
			uint8_t weapon;			// 근접을 켠 무기(몽타주 묶음)
		};

		// 대시 한 번(D-3) — 켬만 보낸다(방향 없음 · 쿨다운 · 켜져 있음 · 죽음 · 경기 끝은 안 보낸다 — 서버도 아무것도 안 했다). 서버는 그 입력을 돈 틱의 상태보다 먼저 보낸다
		struct Dash
		{
			Header h;
			uint32_t tick;
			uint32_t player;
			uint32_t seq;					// 켠 사람의 입력 번호 — 그 사람 화면은 이 입력의 대시를 서버 값으로 바꾼다(CSelfPrediction::OnDashResult)
			double forceX, forceY, forceZ;	// 서버가 넣은 대시 힘(cm/s). double — 서버가 넣는 값 그대로라야 같은 입력 다시 돌리기가 끝자리까지 같다(근접 밀기는 서버도 float 를 넣는다)
			uint8_t dir;					// 고른 방향(EDashDir — 앞 · 뒤 · 왼 · 오른) = 몽타주
		};

		// S2C_GRENADE 의 종류
		enum EGrenadeEvent : uint8_t
		{
			GRENADE_THROW = 0,			// 던짐 — 능력이 켜졌다(쿨다운 시작). 자리 · 속도 · 회전 = 생긴 수류탄
			GRENADE_BOOM_TIMER = 1,		// 신관으로 터짐 — 자리 · 회전 = 터진 걸음 뒤
			GRENADE_BOOM_HIT = 2,		// 다른 팀 사람에 맞아 터짐
		};

		// 수류탄 소식 하나(G-3) — 던짐은 그 입력 처리 안에서 보내 이 틱 상태보다 먼저 간다(던진 사람 예측의 확인 AckGrenade 가 소식을 먼저 봐야 한다)
		struct Grenade
		{
			Header h;
			uint32_t tick;
			uint32_t id;				// 방 안 수류탄 번호(1 부터). 던짐인데 0 = 능력은 켜졌지만 목록이 차 수류탄을 못 만듦(grenadeListFull — 쿨다운 · 끊기는 그대로라 예측은 고쳐야 한다)
			uint32_t thrower;
			uint32_t seq;				// 던진 사람의 입력 번호(던진 입력) — 그 사람 화면은 이 입력을 던짐으로 고친다(CSelfPrediction::OnGrenadeThrown)
			float x, y, z;				// 뿌리 자리
			float vx, vy, vz;			// 속도(던짐만, 터짐은 0)
			float pitch, yaw, roll;		// 회전(도)
			uint8_t kind;				// EGrenadeEvent
			uint8_t team;				// 던질 때 팀
		};

		// S2C_GRENADE_STATE 의 칸 깃발
		enum EGrenadeStateFlag : uint8_t
		{
			GRENADE_STATE_BOUNCED = 1 << 0,	// 이번 틱에 튕겼다 — hitX/Y/Z = 첫 튕김의 맞은 자리(소리용. Lyra 소리가 어느 사건을 따르는지는 G-5 에서 견준다)
			GRENADE_STATE_STOPPED = 1 << 1,	// 멈춤(더 안 걷는다)
		};

		struct GrenadeStateEntry
		{
			uint32_t id;
			float x, y, z;				// 이번 틱 걸음 뒤 뿌리 자리
			float pitch, yaw, roll;
			float hitX, hitY, hitZ;		// GRENADE_STATE_BOUNCED 일 때만
			uint8_t flags;				// EGrenadeStateFlag
		};

		// 수류탄 상태(G-3) — 머리 뒤에 칸 count 개(보낸 크기 = 머리 + 칸 × count). 그 틱에 터진 수류탄은 목록에서 빠져 안 든다(터짐은 S2C_GRENADE)
		struct GrenadeStateHead
		{
			Header h;
			uint32_t tick;
			uint8_t count;				// kMaxPlayersInRoom 이하 — 산 수류탄은 사람당 하나(쿨다운 301 입력 > 수명 121 틱)
		};
#pragma pack(pop)

		static_assert(sizeof(Header) == 4, "ServerCore MsgHeader 와 같아야 한다");
		static_assert(sizeof(State) <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
		static_assert(sizeof(AnimStateWire) == 61, "패킷 크기 어림(D35 · D51 · D60 · D62 · L-4 · L-12-c · L-12-e · L-13-a)과 같아야 한다");
		inline constexpr size_t kAnimStateHeadSize = offsetof(AnimStateWire, layers);
		static_assert(kAnimStateHeadSize == 30, "머리 = 18 + 덮기 · 왼손 · 몽타주 칸 수(D51) + 조준 묶음 비중(D60) + 앉기 묶음 비중(D62) + 다리 IK 비중(L-12-c) + 장치 발 높이 둘 · 알파(L-12-e) + 바탕 자리(L-13-a)");
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
		static_assert(sizeof(Pickup) == 56, "받침 소식 크기 — 칸을 바꾸면 다리 · 봇도 같이 다시 빌드");
		static_assert(sizeof(Melee) == 38, "근접 소식 크기, 칸을 바꾸면 다리 · 봇도 같이 다시 빌드");
		static_assert(sizeof(Dash) == 41, "대시 소식 크기, 칸을 바꾸면 다리 · 봇도 같이 다시 빌드");
		static_assert(sizeof(Grenade) == 58, "수류탄 소식 크기, 칸을 바꾸면 다리 · 봇도 같이 다시 빌드");
		static_assert(sizeof(GrenadeStateEntry) == 41 && sizeof(GrenadeStateHead) == 9, "수류탄 상태 크기, 칸을 바꾸면 다리 · 봇도 같이 다시 빌드");
		inline constexpr size_t kMaxGrenadeStatePacketSize = sizeof(GrenadeStateHead) + sizeof(GrenadeStateEntry) * kMaxPlayersInRoom;
		static_assert(kMaxGrenadeStatePacketSize <= 1458, "ServerCore 패킷 한계(MAX_PACKET_SIZE) 안");
		static_assert(sizeof(Input) == 51, "입력 크기 — 칸을 바꾸면 다리 · 봇도 같이 다시 빌드(판 번호를 올린다)");

		// 한 입력 안 누름 차례(L-4 · 판 14). 근접 · 대시 · 수류탄 · 재장전은 넷 다 누른 입력 하나에만 켜지는 단추라 한 입력에 둘 이상 켜질 수 있다.
		// 그때 Lyra 는 눌린 차례대로 켜고 차례가 결과를 바꾼다(L-1-c 기록 — 근접 + 대시 피해 100 ↔ 0 · 재장전 + Q 채움 ↔ 안 채움 · 근접 + Q 몽타주).
		// 서버 · 예측은 아래 AbilityOrder 로 이 차례를 따른다(L-5-a)
		enum EPressKey : uint8_t
		{
			PRESS_MELEE = 0,
			PRESS_DASH = 1,
			PRESS_GRENADE = 2,
			PRESS_RELOAD = 3,
			kPressKeyCount = 4,
		};
		inline constexpr uint8_t kPressKeyButtons[kPressKeyCount] = { BUTTON_MELEE, BUTTON_DASH, BUTTON_GRENADE, BUTTON_RELOAD };

		// keys[0..count) = 누른 차례의 단추 → pressOrder. 2비트씩 아래부터 — k 번째 칸 = k 번째로 누른 단추. 남는 칸은 0
		inline uint8_t MakePressOrder(const uint8_t* keys, int count)
		{
			uint8_t order = 0;
			for (int k = 0; k < count && k < kPressKeyCount; ++k) order = (uint8_t)(order | ((keys[k] & 3u) << (2 * k)));
			return order;
		}

		// buttons 에 켜진 넷 중 몇 개인가(n)와 그 차례(out[0..n)). 돌려주는 값 = n, 차례를 못 믿으면 -1.
		// n ≤ 1 이면 차례가 뜻이 없어 pressOrder 를 안 보고 켜진 하나만 담는다. n ≥ 2 면 앞 n 칸이 켜진 단추를 한 번씩 다 담고 남는 칸이 0 일 때만 믿는다
		// (보내는 쪽이 차례를 안 채운 0 은 n ≥ 2 에서 「근접 근접」이 되어 -1 — 판 13 까지의 고정 차례로 돌아갈 자리)
		inline int ReadPressOrder(uint8_t buttons, uint8_t pressOrder, uint8_t (&out)[kPressKeyCount])
		{
			int pressed = 0;
			uint8_t single = 0;
			for (uint8_t key = 0; key < kPressKeyCount; ++key)
			{
				if ((buttons & kPressKeyButtons[key]) == 0) continue;
				++pressed;
				single = key;
			}
			if (pressed <= 1)
			{
				out[0] = single;
				return pressed;
			}
			uint8_t seen = 0;
			for (int k = 0; k < pressed; ++k)
			{
				const uint8_t key = (uint8_t)((pressOrder >> (2 * k)) & 3u);
				if ((buttons & kPressKeyButtons[key]) == 0 || (seen & (1u << key)) != 0) return -1;
				seen = (uint8_t)(seen | (1u << key));
				out[k] = key;
			}
			if (pressed < kPressKeyCount && (pressOrder >> (2 * pressed)) != 0) return -1;
			return pressed;
		}

		// 능력 자리에서 켜는 차례(L-5-a) — 근접 · 대시 · 수류탄 중 켜진 것을 켤 차례로 out[0..n) 에 담고 n 을 돌려준다. 서버 ApplyInput 과 예측 StepAbilities 가 같이 쓴다.
		// 차례를 믿으면(ReadPressOrder ≥ 0) 실린 차례, 못 믿으면 판 13 고정 차례(대시 → 수류탄 → 근접 — D102 · D112 ①).
		// 재장전은 무기 순환(걸음 뒤)이 받으므로 빼고, 믿는 차례에서 수류탄보다 먼저 눌렸는지만 reloadBeforeGrenade 로 넘긴다(던지면 그 재장전은 켜졌다 바로 끊긴 것 — L-1-c reload_q)
		inline int AbilityOrder(uint8_t buttons, uint8_t pressOrder, uint8_t (&out)[3], bool& reloadBeforeGrenade)
		{
			reloadBeforeGrenade = false;
			uint8_t order[kPressKeyCount];
			const int pressed = ReadPressOrder(buttons, pressOrder, order);
			int n = 0;
			if (pressed < 0)
			{
				const uint8_t fixed[3] = { PRESS_DASH, PRESS_GRENADE, PRESS_MELEE };
				for (int k = 0; k < 3; ++k)
				{
					if ((buttons & kPressKeyButtons[fixed[k]]) != 0) out[n++] = fixed[k];
				}
				return n;
			}
			bool reloadSeen = false;
			for (int k = 0; k < pressed; ++k)
			{
				if (order[k] == PRESS_RELOAD)
				{
					reloadSeen = true;
					continue;
				}
				if (order[k] == PRESS_GRENADE && reloadSeen) reloadBeforeGrenade = true;
				out[n++] = order[k];
			}
			return n;
		}
	}
}
