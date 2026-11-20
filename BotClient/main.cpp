// 봇 클라 — 한 프로세스에서 봇 N개가 서버에 붙어 한 판을 끝까지 한다. 플랜 2-b.
//
//   BotClient --map=<파일.mocl> [--port=7791] [--bots=10] [--seed=20261102] [--record=파일] [--lead=3] [--breakRecord] [--breakMirror]
//   BotClient --map=<파일.mocl> --seconds=초 [--measureFrom=초 --measureSeconds=초] ... = 부하 시험. 경기 끝을 기다리지 않고 시간이 되면 끊고 LOADBOT 줄을 낸다.
//   봇이 많으면 서버가 방을 나눠 앉힌다 — 기록은 같은 방 봇끼리만 맞춰 본다. 방 2부터는 기록 이름 뒤에 .room<번호>
//   --measureView = 되감기 측정. Lyra 처럼 상태를 쌓아 0.1초 뒤 보간한 자리를 오차 없이 조준하고, 그 시점을 입력에 실어 보낸다. 기본 꺼짐
//   --ads = 적이 보이면 조준을 누른다(C-4). 서버 조준 상태 ↔ 같은 코드 비교는 이 판에서만 켜진 줄이 생긴다. 기본 꺼짐
//   --crouch = 0.2~2.5초마다 앉기 키를 한 번 누른다(D-5). 서버 앉음 ↔ 같은 코드 비교는 이 판에서만 켜진 줄이 생긴다. 기본 꺼짐
//   --script=startstop [--scriptHold=초] [--scriptWeapon=pistol|rifle|shotgun] = 화면 판정 촬영. 서버 --debug 로 애니 비교 촬영의 시작 자리에 서서, 그 촬영과 같은 경로를 달린다. 쏘지 않는다
//   --script=events [--scriptHold=초] [--scriptWeapon=...] = 9단계 사건 경로(쏘기 · 당겨 쏘기 · 위아래 겨눠 쏘기 · 재장전 · 재장전 중 쏘기 · 무기 바꾸기). 애니 비교 사건 경로와 같은 조각 · 사건 프레임. 부활은 없다.
//   --script=crouch [--scriptHold=초] [--scriptWeapon=...] = 앉기 경로(컨텐츠 D-6 나란히 영상). 애니 비교 앉기 경로(-MOBACScript=crouch)와 같은 조각 · 사건 프레임 — 앉기 · 일어서기 · 조준 · 한 발 · 앉은 데서 점프
//     경로 모드는 받은 자기 몽타주 칸을 상태마다 SCRIPTANIM 줄로 찍는다(Tools/LyraPilot/check_script_anim.py 가 Lyra 기록과 견준다)
//
// 판정은 하지 않는다 — 입력을 보내고 받은 것을 적어 맞춰 볼 뿐이다.
// 1. 봇마다 같은 공용 코드(Shared)로 자기 입력을 여기서도 돌려, 돌아온 상태와 위치·속도·탄창까지 비교한다.
//    서버가 시작 자리로 옮기면(S2C_SPAWN) 그 뒤 입력을 새 자리에서 다시 돌린다.
// 2. 받은 처치·결과를 적어 --record 로 남긴다 — 서버 --record 와 바이트로 비교한다.
// 3. 받은 알림으로 규칙을 검사한다: 무적 중 피해 · 대기·경기 뒤 피해 · 죽은 사람의 발사 · 처치 집계 ↔ 결과 · 도움(받은 피해 · 처치로 따로 셈) ↔ 처치마다 도움 번호 · 결과.
//
// 일부러 틀리기: --breakRecord 는 기록을, --breakMirror 는 여기 쪽 가속을 바꿔 비교가 실패해야 한다.
//   --breakCrouch(--crouch 와 함께)는 앉기 키를 서버에만 보내고 여기 예측엔 안 넣는다 — 앉음 비교(BOT crouch diff)가 실패해야 한다
//   --testNoPawnBlock(서버도 같이)은 사람끼리 막기를 끈다(시험 전용, D83) — 서버 ↔ 같은 코드 위치 차이 0 을 엄격히 본다. 안 주면 막기를 켜고 남 캡슐로 예측 · 보정한다
//   --testPauseAfterSpawn=N 은 부활 직후 N 틱 동안 입력을 만들지도 보내지도 않는다(클라가 멈칫한 꼴 — 서버가 빈 입력으로 움직인다). 이동 막음 비교(BOT moveLock diff)는 0 이어야 한다
//   --melee = 적이 4m 안에 보이면 근접 단추를 누른다(E-3). 근접이 켜지는 입력마다 서버와 같은 식으로 화면 판정해 밀기를 예측에 넣고,
//     서버 근접 결과(S2C_MELEE)로 고친다. 받은 근접 소식의 규칙도 본다(BOT melee · meleeFix · meleeRules 줄). 기본 꺼짐
//   --breakMelee(--melee 와 함께)는 화면 판정 밀기를 반으로 넣는다 — 서버 결과로 고친 수(meleeFix forceFixes)가 화면 맞음 수만큼 늘어야 한다
//   --dash = 0.2~2.5초마다 대시 단추를 한 번 누른다(D-3). 방향(앞 · 뒤 · 왼 · 오른 · 위 보고 앞 · 아래 보고 앞)을 고루 섞고, 켜져 있는 동안 점프 · 재장전 · 근접을 가끔 눌러
//     막기 갈래를 지나가게 한다. 서버 대시 소식(S2C_DASH)으로 예측을 고치고 고친 수(BOT dash dashDiff)를 센다. 기본 꺼짐
//   --breakDash=predict(--dash 와 함께)는 대시 단추를 서버에만 보내고 예측엔 안 넣는다 — 서버가 켠 대시를 예측이 안 켜 dashDiff(startMismatches)가 서야 한다.
//     --breakDash=server 는 반대로 예측에만 넣는다 — 서버 소식 없이 지나간 대시(unconfirmed)가 서야 한다
//   --grenade = 적이 보이고 25m 안이면 0.5~7.5초마다 수류탄 단추(Q)를 한 번 누른다(G-3). 피치를 섞고(그대로 · 하늘 · 발 앞 바닥 · 먼 바닥),
//     부활 기다리는 중 · 경기 끝 단계엔 적 없이도 누른다(서버 죽은 갈래 · 경기 끝 막기). 각본 둘 — 하늘 선이 열린 자리 · 12m 안 적(앞 겨눔)에선
//     쿨다운이 비면 한 번 더 누른다(서버 「안 맞음」 · 「맞아 터짐」 갈래). 서버 수류탄 소식(S2C_GRENADE)으로 예측을 고치고
//     고친 수(BOT grenade grenadeDiff)와 무기 4 피해 · 처치(BOT grenadeHits)를 센다. 기본 꺼짐
//   --breakGrenade=predict|server(--grenade 와 함께) — 수류탄 단추를 서버에만 · 예측에만 보낸다. grenadeDiff 가 서야 한다(startMismatches · unconfirmed)
// 판정 대조(후보 1, 7단계): --judgeClips=<클립 파일> --judgeLog=<파일> [--judgeNoSpread] 면 봇이 화면 역할을 한다.
//   받은 상태로 서버와 같은 식의 몸 자세를 만들어 탄마다 자기 판정을 남긴다. 서버 --rewindBulletLog 줄과 Tools/Regress/check_bot_hits.py 로 견준다
//   --judgeFireAlways 면 적이 안 보여도 방아쇠를 놓지 않는다(10명 전원 연사 비용 측정) — 판정 대조 밖(부하 판)에서도 먹는다(D95)
#include "AnimClips.h"
#include "AnimPose.h"
#include "AnimState.h"
#include "GameProtocol.h"
#include "KillRecord.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "NetClient.h"
#include "Pickups.h"
#include "Prediction.h"
#include "Weapon.h"

#include <timeapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

using namespace mo;

namespace
{
	using Clock = std::chrono::steady_clock;
	constexpr float kTick = 1.f / (float)wire::kTickRate;
	constexpr double kPi = 3.14159265358979323846;
	constexpr double kEyeHeight = 60.0;
	constexpr double kEyeCrouchDrop = 80.0 - 32.0;	// 앉으면 눈을 이만큼 내린다 — Lyra 눈높이 서기 80 · 앉음 32(0-3 실측), 서버가 대신 쓰는 시작점(Room.cpp kAimFallbackCrouchDrop)과 같은 폭
	constexpr double kFireRange = 4000.0;
	// --melee 누름 거리 — 쓸기 350 + 구 반지름 96 + 캡슐 반지름 35 = 손에서 481cm 까지 닿는다. 400 = 맞음 · 빗나감이 둘 다 나오게(제 판단)
	constexpr double kMeleeRange = 400.0;
	constexpr double kStrafeRange = 1200.0;
	// --grenade 누름 거리 — 첫 속력 2500cm/s(Projectile.h B_Grenade 틀) · 신관 2초라 1초 남짓에 닿는 거리(제 판단). 시선 검사(kFireRange 4000) 안이다
	constexpr double kGrenadeBotRange = 2500.0;
	// --grenade 피치 종류 — 그대로(적을 봄 · 0 은 안 씀) · 하늘 · 발 앞 바닥 · 먼 바닥(Think 주석)
	constexpr float kGrenadeKindPitch[4] = { 0.f, 80.f, -60.f, -20.f };
	// --grenade 하늘 각본 — 누른 뒤 이만큼 입력은 다시 안 본다. 600 입력(10초) = 쿨다운 301 의 두 배쯤, 한 판(40초)에 봇마다 몇 번(제 판단)
	constexpr int kGrenadeSkyGap = 600;
	// --grenade 가까운 적 각본 거리 — 12m 면 첫 속력 2500 으로 0.48초, 위로 8.5°(던짐 방향 z 0.15) 오른 몫 ≈ 180 − 113 = 67cm 가 캡슐 반 높이 90 안이다.
	// 옆으로 비키는 몫은 앞 겨눔이 맡는다(제 판단). 다시 보는 간격 = 쿨다운 301 과 같은 300
	constexpr double kGrenadeNearRange = 1200.0;
	constexpr int kGrenadeNearGap = 300;
	constexpr double kPawnCorrectTolerance = 0.1;	// cm·cm/s — 다리 kCorrectTolerance(MOBridgeSubsystem.cpp)와 같은 값. 사람끼리 막기를 켠 판만 고친다
	// 받침 갈래(컨텐츠 B-4) — 이 체력 이하면 같은 층에서 가장 가까운 켜진 받침으로 걷는다. 75 = 최대 100 − 작은 받침 회복 25(0-3 표 · 받침 파일)
	// 작은 받침에 닿아도 회복이 최대에서 안 잘리는 선이다. 받침 수치는 서버만 알아(Epic 맵 값, 리포 밖) 봇은 이 상수로 둔다
	constexpr float kPadSeekHp = 75.f;
	// 같은 층으로 치는 높이 차 — 봇은 곧게만 걸어 다른 층 받침은 못 간다. Expanse 받침 층 사이 높이 차(가장 작은 것 약 750)의 절반 아래
	constexpr double kPadSameFloorZ = 300.0;

	double Length(const Vec3& v)
	{
		return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	}

	bool LoadMap(const char* path, CMapCollision& map)
	{
		FILE* file = std::fopen(path, "rb");
		if (!file) return false;
		std::fseek(file, 0, SEEK_END);
		const long size = std::ftell(file);
		std::fseek(file, 0, SEEK_SET);
		std::vector<char> data((size_t)size);
		const size_t read = std::fread(data.data(), 1, data.size(), file);
		std::fclose(file);
		return read == data.size() && map.LoadFromMemory(data.data(), data.size()) == EMapLoad::Ok;
	}

	constexpr int kBreakDashPredict = 1;	// --breakDash=predict — 예측에 안 넣음
	constexpr int kBreakDashServer = 2;		// --breakDash=server — 서버에 안 보냄
	constexpr int kBreakGrenadePredict = 1;	// --breakGrenade=predict — 예측에 안 넣음
	constexpr int kBreakGrenadeServer = 2;	// --breakGrenade=server — 서버에 안 보냄

	struct Args
	{
		std::string mapPath;
		std::string recordPath;
		int port = 7791;
		int bots = 10;
		int lead = 3;
		uint32_t seed = 20261102;
		bool breakRecord = false;
		bool breakMirror = false;
		bool breakCrouch = false;		// --breakCrouch — 앉기 키를 예측에 안 넣는다(일부러 틀림)
		int testPauseAfterSpawn = 0;		// --testPauseAfterSpawn=N — 부활 직후 N 틱 동안 입력을 안 만듦(빈 입력 이동은 16 틱째부터)
		double seconds = 0;				// 0 이 아니면 부하 시험
		double measureFromSeconds = 0;
		double measureSeconds = 0;
		bool measureView = false;		// 되감기 측정 모드 — 꺼져 있으면 기존 시험과 같은 입력을 보낸다
		bool adsWhenShooting = false;	// --ads — 적이 보이면 조준을 누른다(C-4). 꺼져 있으면 기존 시험과 같은 입력
		bool crouchToggle = false;		// --crouch — 앉기 키를 가끔 누른다(D-5). 꺼져 있으면 기존 시험과 같은 입력
		bool meleeNear = false;			// --melee — 적이 가까이 보이면 근접 단추를 누른다(E-3). 꺼져 있으면 기존 시험과 같은 입력
		bool pawnBlock = true;			// 사람끼리 막기(D83 늘 켬) — 예측이 남 캡슐을 보고 어긋나면 고친다. --testNoPawnBlock(시험 전용 · 서버도 같이)이면 끄고 위치 차이 0 을 엄격히 본다
		bool startAllWeapons = false;	// --testStartAllWeapons(시험 전용 · 서버도 같이) — 세 무기로 부활(무기 줍기 전 동작). 예측의 가방이 서버와 같게(W-2)
		bool push = false;				// --push — 쏘지 않고 가장 가까운 사람 쪽으로 걷는다(F-2 보정 양 측정)
		bool breakMelee = false;			// --breakMelee — 화면 판정 밀기를 반으로(일부러 틀림)
		bool dash = false;				// --dash — 가끔 대시를 누른다(D-3). 꺼져 있으면 기존 시험과 같은 입력
		int breakDash = 0;				// --breakDash=predict|server — 대시 단추를 예측 · 서버 한쪽에만(일부러 틀림)
		bool grenade = false;			// --grenade — 가끔 수류탄을 누른다(G-3). 꺼져 있으면 기존 시험과 같은 입력
		int breakGrenade = 0;			// --breakGrenade=predict|server — 수류탄 단추를 예측 · 서버 한쪽에만(일부러 틀림)
		std::string judgeClips;			// 주면 봇이 화면 역할로 탄마다 자기 판정을 남긴다(후보 1, 7단계). measureView 를 함께 켠다
		std::string judgeLog;
		bool judgeNoSpread = false;		// 서버 --rewindNoSpread 와 같이 쓴다 — 자세만 본다
		bool judgeFireAlways = false;	// 비용 측정 — 적이 보이든 말든 방아쇠를 놓지 않는다(10명 전원 연사). 이름은 판정 대조 때 만든 그대로, 부하 판에서도 먹는다
		std::string script;				// 주면 두뇌 대신 정해진 경로를 달린다
		double scriptHoldSeconds = 4.5;	// 자리에 선 뒤 경로 시작까지 — 촬영 쪽 대기 4초보다 길고, 첫 서 있기 2초를 더해도 제자리 장난 동작(서 있은 지 7.35초)이 나오기 전에 출발해야 합격 영상과 같다. 권총은 6.35초라 촬영 스크립트(record_remote_shot.ps1)가 4.25초를 준다
		uint8_t scriptWeapon = (uint8_t)EWeapon::Rifle;	// --scriptWeapon=pistol|rifle|shotgun — 경로 중 드는 무기(무기별 자세 촬영, 9단계)
		std::string traceInputsPath;	// --traceInputs=파일 — g_traceInputs
		// --testLockstep(L-0-c) — 서버 --testLockstep=N 과 같이 준다. 벽시계로 보내지 않고, 직전 입력을 적용한 틱의 상태를 받은 뒤에 다음 입력을 만든다.
		// 서버는 모든 사람의 다음 입력이 와야 틱을 넘기므로, 입력을 만들 때 가진 최신 상태가 늘 같은 틱이다 — 같은 빌드면 판마다 같은 판
		bool lockstep = false;
	};

	// 경로 안의 사건 — MOBAnimCompareSubsystem.cpp EACEvent 와 같은 뜻. Fire = 방아쇠를 kScriptFireTapTicks 동안 · Hold/Release = 당긴 채 두기/놓기
	// Swap = 경로 무기가 권총이면 소총, 아니면 권총으로 · Back = 경로 무기로. 부활은 봇이 입력으로 낼 길이 없어 없다(그쪽은 부활 효과 능력만 다시 튼다)
	// CrouchOn · CrouchOff = 앉기 단추 한 번(이미 그 상태면 안 누른다 — 그쪽 SetFoeCrouch 와 같다) · AdsOn · AdsOff = 조준 단추를 누른 채 두기 · 놓기(D-6)
	enum class EScriptEvent : uint8_t { None, Fire, Hold, Release, Reload, Swap, Back, CrouchOn, CrouchOff, AdsOn, AdsOff };
	constexpr uint32_t kScriptFireTapTicks = 3;	// 그쪽 FIRE_TAP_SECONDS 0.05초 = 3프레임
	constexpr uint32_t kScriptJumpTicks = 6;	// 그쪽 JUMP_PRESS_SECONDS 0.1초 = 6프레임 — 조각 처음부터 누른다
	// 사건을 넣는 틱 = 기록 프레임 + 이 값. 몽타주 사건은 + 1(ThinkScript 주석). 앉기는 + 0 — 기록 도구가 프레임 끝에 이동 입력과 ToggleCrouch 를 같이 넣어 둘 다 다음 프레임 이동에서 먹는다
	// (기록의 출발 조각 0행 속도 0 · 1행 −20, run_on 72프레임 사건 → 73행 앉음). 봇은 이동을 조각 0틱부터 넣으므로 앉기도 사건 프레임 그 틱에 눌러야 앉기 전에 움직인 횟수가 같다 —
	// + 1 이면 한 번 더 움직여 달리며 앉는 run_on · 일어서는 run_off 에서 5cm 씩 벌어졌다(D-6-d 실측 9.43cm). 점프(kScriptJumpTicks)도 0틱부터라 같은 차례다
	constexpr uint32_t ScriptEventLag(EScriptEvent ev) { return (ev == EScriptEvent::CrouchOn || ev == EScriptEvent::CrouchOff) ? 0u : 1u; }

	// 화면 판정 촬영의 경로 — MOBAnimCompareSubsystem.cpp STARTSTOP_SCRIPT · EVENTS_SCRIPT 와 같은 값. 합격 영상과 나란히 놓으려면 동작이 같아야 한다
	struct ScriptStep
	{
		const char* name;
		uint32_t ticks;	// 합격 영상이 실제로 돈 프레임 수(animcmp_ssp_v_simple.csv) — 그쪽은 시간 합이 조금 모자라 조각마다 1프레임 더 돈다(0.4초 조각만 딱 맞는다)
		bool moving;
		double angle;	// 몸 방향 기준 이동 각도
		float pitchFrom = 0.f;	// 조각 동안 시선 위아래 각(+ 위) — 그쪽처럼 seconds 에 걸쳐 선형으로
		float pitchTo = 0.f;
		float seconds = 0.f;	// 그쪽 조각 길이(초) — 시선 각 보간에만 쓴다. 0 = 처음부터 pitchTo
		EScriptEvent ev0 = EScriptEvent::None;
		uint32_t at0 = 0;		// 조각 시작에서 사건까지 프레임 — Lyra 기록이 실제로 일으킨 프레임(animcmp_ev_<무기>_probe.csv events 칸)
		EScriptEvent ev1 = EScriptEvent::None;
		uint32_t at1 = 0;
		bool jump = false;	// 조각 처음 kScriptJumpTicks 동안 뛰기 단추(그쪽 bJump)
	};
	constexpr ScriptStep kStartStopScript[] = {
		{ "idle", 121, false, 0 },
		{ "fwd", 151, true, 0 }, { "stop_fwd", 121, false, 0 },
		{ "bwd", 151, true, 180 }, { "stop_bwd", 121, false, 0 },
		{ "left", 151, true, -90 }, { "stop_left", 121, false, 0 },
		{ "right", 151, true, 90 }, { "stop_right", 121, false, 0 },
		{ "diag", 151, true, 45 }, { "stop_diag", 121, false, 0 },
		{ "piv_bf_a", 91, true, 180 }, { "piv_bf_b", 91, true, 0 }, { "stop_bf", 121, false, 0 },
		{ "piv_fb_a", 91, true, 0 }, { "piv_fb_b", 91, true, 180 }, { "stop_fb", 121, false, 0 },
		{ "piv_lr_a", 91, true, -90 }, { "piv_lr_b", 91, true, 90 }, { "stop_lr", 121, false, 0 },
		{ "piv_rl_a", 91, true, 90 }, { "piv_rl_b", 91, true, -90 }, { "stop_rl", 121, false, 0 },
		{ "ad1", 24, true, -90 }, { "ad2", 24, true, 90 }, { "ad3", 24, true, -90 },
		{ "ad4", 24, true, 90 }, { "ad5", 24, true, -90 }, { "ad6", 24, true, 90 },
		{ "stop_ad", 121, false, 0 },
	};
	// 9단계 사건 경로 — MOBAnimCompareSubsystem.cpp EVENTS_SCRIPT. 틱 수 · 사건 프레임은 Lyra 기록 animcmp_ev_<무기>_probe.csv pass 0 이 실제로 돈 값(세 무기 같음, 11-12 실측).
	// 그쪽 초 → 프레임은 시간 합이 조금 모자라 대개 1프레임 더 돌고(2.0초 = 121), 0.3초 사건은 17프레임에 났다
	using SE = EScriptEvent;
	constexpr ScriptStep kEventsScript[] = {
		{ "idle", 121, false, 0 },
		{ "fire_stand", 91, false, 0, 0.f, 0.f, 1.5f, SE::Fire, 0 },
		{ "fire_hold", 121, false, 0, 0.f, 0.f, 2.0f, SE::Hold, 0, SE::Release, 29 },
		{ "run_fire", 109, true, 0, 0.f, 0.f, 1.8f, SE::Fire, 17, SE::Fire, 60 },
		{ "stop_f", 61, false, 0 },
		{ "up_fire", 91, false, 0, 0.f, 45.f, 1.5f, SE::Fire, 60 },
		{ "down_fire", 91, false, 0, 45.f, -45.f, 1.5f, SE::Fire, 60 },
		{ "mid", 61, false, 0, -45.f, 0.f, 1.0f },
		{ "reload", 211, false, 0, 0.f, 0.f, 3.5f, SE::Reload, 0 },
		{ "fire_b", 61, false, 0, 0.f, 0.f, 1.0f, SE::Fire, 0 },
		{ "reload_cancel", 121, false, 0, 0.f, 0.f, 2.0f, SE::Reload, 0, SE::Fire, 35 },
		{ "settle_c", 91, false, 0 },
		{ "run_reload", 151, true, 90, 0.f, 0.f, 2.5f, SE::Reload, 12 },
		{ "stop_r", 91, false, 0 },
		{ "swap_out", 121, false, 0, 0.f, 0.f, 2.0f, SE::Swap, 0 },
		{ "swap_back", 121, false, 0, 0.f, 0.f, 2.0f, SE::Back, 0 },
		{ "swap_fire", 91, false, 0, 0.f, 0.f, 1.5f, SE::Swap, 0, SE::Fire, 15 },
		{ "swap_back2", 121, false, 0, 0.f, 0.f, 2.0f, SE::Back, 0 },
		{ "run_swap", 151, true, -90, 0.f, 0.f, 2.5f, SE::Swap, 23, SE::Back, 84 },
		{ "stop_s", 91, false, 0 },
		{ "spawn", 211, false, 0 },		// 그쪽은 여기서 부활 효과를 다시 튼다 — 봇은 서 있기만 한다
		{ "idle_end", 91, false, 0 },
	};
	// 앉기 경로 — MOBAnimCompareSubsystem.cpp CROUCH_SCRIPT. 틱 수 · 사건 프레임은 Lyra 기록 animcmp_crouch_<무기>_probe.csv pass 0 이 실제로 돈 값(세 무기 같음, 11-16 실측 —
	// 합 4,820프레임, C:\tmp\mob\d6d\seg_ticks.py). 앉음 사건은 기록이 그 조각 0프레임에 찍었다(idle_end 포함)
	constexpr ScriptStep kCrouchScript[] = {
		{ "idle", 121, false, 0 },
		{ "c_on", 91, false, 0, 0.f, 0.f, 1.5f, SE::CrouchOn, 0 },
		{ "c_up", 91, false, 0, 0.f, 45.f, 1.5f },
		{ "c_down", 91, false, 0, 45.f, -45.f, 1.5f },
		{ "c_mid", 61, false, 0, -45.f, 0.f, 1.0f },
		{ "c_off", 91, false, 0, 0.f, 0.f, 1.5f, SE::CrouchOff, 0 },
		{ "c_on2", 61, false, 0, 0.f, 0.f, 1.0f, SE::CrouchOn, 0 },
		{ "c_fwd", 151, true, 0 }, { "c_stop_fwd", 121, false, 0 },
		{ "c_bwd", 151, true, 180 }, { "c_stop_bwd", 121, false, 0 },
		{ "c_left", 151, true, -90 }, { "c_stop_left", 121, false, 0 },
		{ "c_right", 151, true, 90 }, { "c_stop_right", 121, false, 0 },
		{ "c_piv_bf_a", 91, true, 180 }, { "c_piv_bf_b", 91, true, 0 }, { "c_stop_bf", 121, false, 0 },
		{ "c_piv_fb_a", 91, true, 0 }, { "c_piv_fb_b", 91, true, 180 }, { "c_stop_fb", 121, false, 0 },
		{ "c_piv_lr_a", 91, true, -90 }, { "c_piv_lr_b", 91, true, 90 }, { "c_stop_lr", 121, false, 0 },
		{ "c_piv_rl_a", 91, true, 90 }, { "c_piv_rl_b", 91, true, -90 }, { "c_stop_rl", 121, false, 0 },
		{ "c_fire", 91, false, 0, 0.f, 0.f, 1.5f, SE::Fire, 17 },
		{ "c_ads_on", 91, false, 0, 0.f, 0.f, 1.5f, SE::AdsOn, 0 },
		{ "c_ads_fwd", 121, true, 0 }, { "c_ads_stop", 121, false, 0 }, { "c_ads_bwd", 121, true, 180 },
		{ "c_ads_off", 121, false, 0, 0.f, 0.f, 2.0f, SE::AdsOff, 29 },
		{ "c_jump", 97, false, 0, 0.f, 0.f, 1.6f, SE::None, 0, SE::None, 0, true },
		{ "c_settle", 61, false, 0 },
		{ "run_on", 151, true, 0, 0.f, 0.f, 2.5f, SE::CrouchOn, 72 },
		{ "run_off", 151, true, 180, 0.f, 0.f, 2.5f, SE::CrouchOff, 72 },
		{ "stop_a", 121, false, 0 },
		{ "start_toggle", 91, true, 90, 0.f, 0.f, 1.5f, SE::CrouchOn, 17 },
		{ "stop_toggle", 121, false, 0, 0.f, 0.f, 2.0f, SE::CrouchOff, 17 },
		{ "piv_toggle_a", 91, true, -90 },
		{ "piv_toggle_b", 91, true, 90, 0.f, 0.f, 1.5f, SE::CrouchOn, 12 },
		{ "ret_left", 30, true, -90 },
		{ "stop_end", 121, false, 0 },
		{ "idle_end", 121, false, 0, 0.f, 0.f, 2.0f, SE::CrouchOff, 0 },
	};
	const Vec3 kScriptHome(-1020.0, -1070.0, 142.0);	// MOBAnimCompareSubsystem.cpp FIXED_HOME
	constexpr float kScriptYaw = 180.f;					// MOBAnimCompareSubsystem.cpp FIXED_YAW

	// 봇이 화면 역할로 쓰는 자세 계산. 봇들이 한 스레드에서 번갈아 쓰므로 한 벌만 둔다.
	// 묶음은 무기마다 하나(서버 Room.h AnimBundleOf 와 같은 이름 = GetWeaponDef(무기).name) — 받은 상태는 같은 PlayerState 의 weapon 묶음으로 푼다
	struct JudgeShared
	{
		CAnimClips clips;
		std::array<CAnimState, (size_t)EWeapon::Count> codecs;
		std::array<CAnimPoseEvaluator, (size_t)EWeapon::Count> evaluators;
		std::vector<BoneTransform> localFrom, localTo, localBlend;
		uint64_t bodiesByWeapon[(size_t)EWeapon::Count] = {};	// 판정에 넣은 몸을 그 틱 묶음별로
		FILE* log = nullptr;
		bool noSpread = false;
		double crouchLift = 0;		// 앉은 메시 올림 — 서버 GetPoseAt 과 같은 값 · 같은 섞기(D64)
	};

	// 서버 Room.cpp NormalizeAxis 와 같은 식
	float NormalizeAxis(float degrees)
	{
		float d = std::fmod(degrees, 360.f);
		if (d > 180.f) d -= 360.f;
		else if (d < -180.f) d += 360.f;
		return d;
	}

	// 받은 상태의 꼬리에서 한 사람치를 읽는다(형식은 GameProtocol.h wire::ReadAnimTail 한 벌 — Lyra 다리도 같은 것을 부른다). false = 꼬리가 없거나 잘렸다
	bool ReadAnimTail(const char* data, int size, int& at, wire::AnimStateWire& out)
	{
		size_t used = 0;
		if (at >= size || !wire::ReadAnimTail(data + at, (size_t)(size - at), out, used))
		{
			at = size;
			return false;
		}
		at += (int)used;
		return true;
	}

	// 대조군 — 서버 숫자가 멀쩡한데 이 값이 나쁘면 송신 쪽 문제다. 측정 구간에만 가리킨다
	std::vector<float>* g_stateGapsMs = nullptr;
	// --traceInputs(시험 전용, L-0-c) — 입력마다 「그때 받아 둔 최신 상태 틱」, 자기 상태마다 「서버가 적용한 입력 번호」. 같은 빌드 두 판이 어디서 갈리나를 줄로 본다
	FILE* g_traceInputs = nullptr;

	double ProcessCpuSeconds()
	{
		FILETIME creation, exit, kernel, user;
		if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
		const auto seconds = [](const FILETIME& t) { return (double)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
		return seconds(kernel) + seconds(user);
	}

	// 받은 알림에서 나온 규칙 위반 수
	struct RuleChecks
	{
		uint64_t kills = 0;
		uint64_t damages = 0;
		uint64_t shots = 0;
		uint64_t shotsWhileDead = 0;
		uint64_t damageOutsidePlaying = 0;
		uint64_t damageWhileImmune = 0;
		uint64_t spawnsSeen = 0;
		uint64_t immuneBulletHits = 0;		// 몸에 맞았는데 피해 0 — 같은 팀 제외. 무적이 실제로 지나갔다는 분모
		uint64_t assists = 0;				// 봇이 따로 센 도움(A-3) — 0 이면 대조가 지나가지 않았다
		uint64_t assistKillDiff = 0;		// 처치마다 서버 도움 번호 ↔ 봇이 센 번호(차례까지) 다름
	};

	// 근접(E-3) — 봇 하나가 센 것. 화면 판정 · 서버 결과 대조 · 받은 근접 소식 규칙
	struct MeleeBotCounts
	{
		uint64_t presses = 0;			// 근접 단추를 누른 입력
		uint64_t judged = 0;			// 그중 예측이 근접을 켠 입력 = 화면 판정 수
		uint64_t screen[4] = {};		// 화면 판정 결과 — wire::EMeleeResult 차례(빗나감 · 같은 팀 · 벽 · 맞음)
		uint64_t mine = 0;				// 내 근접의 서버 결과 소식
		uint64_t mineHitForce = 0;		// 그중 맞음 · 밀기 힘 있음 — 붙어서 맞히면(쓸기 시작부터 겹침) 거리 0 이라 힘도 0 이다
		uint64_t mineHitNoForce = 0;	// 그중 맞음 · 힘 0
		uint64_t agree = 0;				// 화면 판정과 결과 · 대상이 같다
		uint64_t kindDiff = 0;			// 결과가 다르다
		uint64_t targetDiff = 0;		// 결과는 같은데 대상이 다르다
		uint64_t noGuess = 0;			// 화면 판정 없는 입력의 서버 결과 — 예측과 서버가 근접을 켠 입력이 다르다. 0 이어야 한다
		uint64_t guessNoResult = 0;		// 화면 판정했는데 서버 결과 없이 지나감(경기 끝 단계 · 그 사이 죽음)
		uint64_t msgs = 0;				// 받은 근접 소식 전부(남의 것 포함)
		uint64_t hits = 0;				// 그중 맞음
		uint64_t hitsDamaged = 0;		// 맞음 중 피해가 0 보다 큼 — 피해 소식이 하나씩 뒤따라야 한다
		uint64_t hitsZero = 0;			// 맞음 중 피해 0(무적 · 대기 · 경기 뒤)
		uint64_t damageMsgs = 0;		// 무기 칸이 근접인 피해 소식
		uint64_t kills = 0;				// 무기 칸이 근접인 처치 소식
		// 규칙 위반 — 전부 0 이어야 한다
		uint64_t badShape = 0;			// 결과 칸 범위 밖 · 맞음 아닌데 피해나 힘 · 맞음인데 대상 없음 · 빗나감인데 대상 · 피해가 0 도 kMeleeDamage 도 아님
		uint64_t badDead = 0;			// 죽은 뒤 근접
		uint64_t badPost = 0;			// 경기 끝 단계 근접(서버가 막는다)
		uint64_t badDamage = 0;			// 근접 피해 소식이 바로 앞 근접 맞음(공격자 · 대상 · 양 · 틱)과 다르거나 앞 맞음 없이 옴
		uint64_t badZero = 0;			// 경기 중 · 무적 아닌 상대를 맞혔는데 피해 0
		double forceDiffMax = 0;		// 화면 판정 밀기 힘 ↔ 서버 힘 차이의 최댓값(내 근접, 결과가 같은 것 · 다른 것 모두)
	};

	void AddMeleeCounts(MeleeBotCounts& to, const MeleeBotCounts& from)
	{
		to.presses += from.presses;
		to.judged += from.judged;
		for (int i = 0; i < 4; ++i) to.screen[i] += from.screen[i];
		to.mine += from.mine;
		to.mineHitForce += from.mineHitForce;
		to.mineHitNoForce += from.mineHitNoForce;
		to.agree += from.agree;
		to.kindDiff += from.kindDiff;
		to.targetDiff += from.targetDiff;
		to.noGuess += from.noGuess;
		to.guessNoResult += from.guessNoResult;
		to.msgs += from.msgs;
		to.hits += from.hits;
		to.hitsDamaged += from.hitsDamaged;
		to.hitsZero += from.hitsZero;
		to.damageMsgs += from.damageMsgs;
		to.kills += from.kills;
		to.badShape += from.badShape;
		to.badDead += from.badDead;
		to.badPost += from.badPost;
		to.badDamage += from.badDamage;
		to.badZero += from.badZero;
		to.forceDiffMax = std::max(to.forceDiffMax, from.forceDiffMax);
	}

	void AddMeleeFixes(CSelfPrediction::MeleeCounters& to, const CSelfPrediction::MeleeCounters& from)
	{
		to.started += from.started;
		to.pushesGiven += from.pushesGiven;
		to.pushesIgnored += from.pushesIgnored;
		to.results += from.results;
		to.resultsNoSlot += from.resultsNoSlot;
		to.startMismatches += from.startMismatches;
		to.pushHitFixes += from.pushHitFixes;
		to.pushForceFixes += from.pushForceFixes;
		to.unconfirmed += from.unconfirmed;
		to.rerunsMissed += from.rerunsMissed;
	}

	// 대시(D-3) — 봇 하나가 센 것. 누름 · 받은 대시 소식 규칙
	struct DashBotCounts
	{
		uint64_t presses = 0;			// 대시 단추를 누른 입력(봇이 센 것 — 예측 presses 와 같아야 한다)
		uint64_t kinds[6] = {};			// 그중 고른 방향 종류 — 앞 · 뒤 · 왼 · 오른 · 위 보고 앞 · 아래 보고 앞(누르기 한 입력 앞 이동 · 누른 입력 피치)
		uint64_t extraJump = 0, extraReload = 0, extraMelee = 0;	// 켜져 있는 동안(예측 기준) 같이 누른 단추 — 막기 갈래를 지나가게
		uint64_t msgs = 0;				// 받은 대시 소식 전부(남의 것 포함)
		uint64_t mine = 0;				// 그중 내 것 — 합 = 서버 SERVER dash started(봇만 든 판)
		uint64_t mineDir[4] = {};		// 내 것의 방향(EDashDir)
		// 규칙 위반 — 전부 0 이어야 한다
		uint64_t badShape = 0;			// 방향 칸 범위 밖 · 힘 크기가 kDashStrength 가 아님
		uint64_t badDead = 0;			// 죽은 뒤 대시
		uint64_t badPost = 0;			// 경기 끝 단계 대시(서버가 막는다)
	};

	void AddDashCounts(DashBotCounts& to, const DashBotCounts& from)
	{
		to.presses += from.presses;
		for (int i = 0; i < 6; ++i) to.kinds[i] += from.kinds[i];
		to.extraJump += from.extraJump;
		to.extraReload += from.extraReload;
		to.extraMelee += from.extraMelee;
		to.msgs += from.msgs;
		to.mine += from.mine;
		for (int i = 0; i < 4; ++i) to.mineDir[i] += from.mineDir[i];
		to.badShape += from.badShape;
		to.badDead += from.badDead;
		to.badPost += from.badPost;
	}

	void AddDashFixes(CSelfPrediction::DashCounters& to, const CSelfPrediction::DashCounters& from)
	{
		to.presses += from.presses;
		to.started += from.started;
		to.respawnGained += from.respawnGained;
		to.respawnLost += from.respawnLost;
		to.results += from.results;
		to.resultsNoSlot += from.resultsNoSlot;
		to.startMismatches += from.startMismatches;
		to.forceFixes += from.forceFixes;
		to.unconfirmed += from.unconfirmed;
		to.unconfirmedDead += from.unconfirmedDead;
		to.replays += from.replays;
		to.replaysMissed += from.replaysMissed;
		to.meleeInReplay += from.meleeInReplay;
	}

	// 수류탄(G-3) — 봇 하나가 받은 수류탄 소식 · 상태
	struct GrenadeBotCounts
	{
		uint64_t presses = 0;			// 수류탄 단추를 누른 입력(봇이 센 것 — 예측 presses 와 같아야 한다)
		uint64_t kinds[4] = {};			// 그중 피치 종류 — 그대로(적을 봄) · 하늘 +80 · 발 앞 바닥 −60 · 먼 바닥 −20
		uint64_t pressWaiting = 0, pressPost = 0;	// 그중 부활 기다리는 중 · 경기 끝 단계(적 없이 누름)
		uint64_t pressSky = 0;			// 그중 하늘 각본(선이 열린 자리 · 쿨다운 빔 — kinds 하늘 칸에도 든다)
		uint64_t pressNear = 0;			// 그중 가까운 적 각본(kGrenadeNearRange 안 · 쿨다운 빔 — kinds 그대로 칸에도 든다)
		uint64_t msgs = 0;				// 받은 S2C_GRENADE 전부(남의 것 포함)
		uint64_t throws = 0, throwsNoGrenade = 0, boomTimer = 0, boomHit = 0;	// 종류별(던짐 중 번호 0 = 서버 목록이 차 못 만듦)
		uint64_t mine = 0;				// 내 던짐 — 합 = 서버 SERVER grenade thrown(봇만 든 판)
		uint64_t stateMsgs = 0, stateEntries = 0, stateBounced = 0;	// 받은 S2C_GRENADE_STATE · 칸 · 튕김 깃발
		uint64_t stateBeforeJoin = 0;	// 내가 처음 받은 던짐 번호보다 작은(또는 아직 던짐을 못 받은) 모르는 칸 — 들어오기 전에 던진 것이라 규칙 위반이 아니다
		// 무기 4(수류탄 폭발) 피해 · 처치 — mine 합 = 서버 SERVER grenade damageEvents · selfDamage · kills(봇만 든 판)
		uint64_t damageMsgs = 0, killMsgs = 0;			// 받은 것 전부(봇마다 같은 소식)
		uint64_t damageMine = 0, selfDamageMine = 0, killsMine = 0;	// 그중 공격자 = 나(자기 피해 · 자기 처치 포함)
		// 규칙 위반 — 전부 0 이어야 한다
		uint64_t badDead = 0;			// 죽은 뒤 던짐(서버는 죽은 입력에 안 던진다. 터짐은 죽은 뒤에도 정상)
		uint64_t badPost = 0;			// 경기 끝 단계 던짐(서버가 막는다)
		uint64_t badDamage = 0;			// 무기 4 피해 소식이 바로 앞 터짐(던진 사람 · 틱)과 다르거나 터짐 없이 옴
		uint64_t badKind = 0;			// 종류 칸 범위 밖
		uint64_t badState = 0;			// 상태 소식 크기가 머리 + 칸 × count 와 다르거나 count 가 방 크기 밖
		uint64_t badStateId = 0;		// 상태 칸 번호가 받은 던짐 · 터짐으로 본 산 수류탄이 아님(들어온 뒤 던진 것 — 방 안 번호는 1 부터 늘기만 한다)
	};

	void AddGrenadeCounts(GrenadeBotCounts& to, const GrenadeBotCounts& from)
	{
		to.presses += from.presses;
		for (int i = 0; i < 4; ++i) to.kinds[i] += from.kinds[i];
		to.pressWaiting += from.pressWaiting;
		to.pressPost += from.pressPost;
		to.pressSky += from.pressSky;
		to.pressNear += from.pressNear;
		to.damageMsgs += from.damageMsgs;
		to.killMsgs += from.killMsgs;
		to.damageMine += from.damageMine;
		to.selfDamageMine += from.selfDamageMine;
		to.killsMine += from.killsMine;
		to.badDead += from.badDead;
		to.badPost += from.badPost;
		to.badDamage += from.badDamage;
		to.msgs += from.msgs;
		to.throws += from.throws;
		to.throwsNoGrenade += from.throwsNoGrenade;
		to.boomTimer += from.boomTimer;
		to.boomHit += from.boomHit;
		to.mine += from.mine;
		to.stateMsgs += from.stateMsgs;
		to.stateEntries += from.stateEntries;
		to.stateBounced += from.stateBounced;
		to.stateBeforeJoin += from.stateBeforeJoin;
		to.badKind += from.badKind;
		to.badState += from.badState;
		to.badStateId += from.badStateId;
	}

	void AddGrenadeFixes(CSelfPrediction::GrenadeCounters& to, const CSelfPrediction::GrenadeCounters& from)
	{
		to.presses += from.presses;
		to.thrown += from.thrown;
		to.respawnThrown += from.respawnThrown;
		to.respawnLost += from.respawnLost;
		to.replayGained += from.replayGained;
		to.replayLost += from.replayLost;
		to.results += from.results;
		to.resultsNoSlot += from.resultsNoSlot;
		to.startMismatches += from.startMismatches;
		to.unconfirmed += from.unconfirmed;
		to.unconfirmedDead += from.unconfirmedDead;
		to.replays += from.replays;
		to.replaysMissed += from.replaysMissed;
		to.meleeInReplay += from.meleeInReplay;
	}

	// 근접 화면 판정 한 번 — 결과(wire::EMeleeResult) · 처음 닿은 몸의 주인
	struct MeleeGuess
	{
		uint8_t kind = wire::MELEE_NONE;
		uint32_t target = 0;
		Vec3 force;		// 예측에 넣은 밀기 힘(맞음이 아니면 0)
	};

	class CBot
	{
	public:
		CBot(const CMapCollision& map, CMoveSim& sim, const MatchRules& rules, uint32_t seed)
			: _map(map), _rules(rules), _sim(sim), _self(sim, rules, MakeLyraMoveParams().maxWalkSpeed)
		{
			SeedRngs(seed);
			const MoveParams params = MakeLyraMoveParams();		// 서버 CRoom 의 _params 와 같은 값 — 근접 화면 판정의 상대 캡슐
			_capsuleRadius = params.capsuleRadius;
			_capsuleHalfHeight = params.capsuleHalfHeight;
			_crouchLift = sim.CrouchMeshLiftZ();
		}

		bool Connect(int port)
		{
			bool connected = false;
			for (int i = 0; i < 300 && !connected; ++i)
			{
				connected = _net.Connect("127.0.0.1", port);
				if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (!connected) return false;
			wire::Hello hello = wire::Hello();
			hello.protocolVersion = wire::kProtocolVersion;
			return _net.SendPacket(hello, wire::Msg::C2S_HELLO);
		}

		// 반환 false = 끊김
		bool Pump()
		{
			if (!_net.IsOpen()) return false;
			const bool open = _net.Poll([this](const wire::Header& header, const char* data, int size) { OnPacket(header, data, size); });
			if (!open) _net.Close();
			return open;
		}

		void SendNextInput(bool breakMirror)
		{
			if (!_hasSpawn || !_net.IsOpen() || _hasResult) return;
			if (_pauseLeft > 0) { --_pauseLeft; return; }	// 시험 — 예측도 전송도 쉰다(클라 멈칫)
			if (_scripted && !_scriptPlaced)
			{
				if (!_scriptArmed)
				{
					// 경로 무기를 먼저 든다 — 서버는 자리 옮기기에서 자세를 든 무기 묶음으로 처음부터 시작하므로, 교체가 서기 시계를 바꾸지 않는다
					wire::Input arm = wire::Input();
					arm.yaw = kScriptYaw;
					arm.weapon = _scriptWeapon;
					_self.AddInput(arm);
					if (!_net.SendPacket(arm, wire::Msg::C2S_INPUT)) _net.Close();
					_scriptArmed = true;
					return;
				}
				PlaceForScript();
				return;
			}
			wire::Input input = wire::Input();
			if (_scripted) ThinkScript(input);
			else Think(input);
			_self.SetDebugMoveScale(breakMirror ? 0.999 : 1.0);
			// 근접(E-3) — 이 입력이 근접을 켜면 넣기 전에 화면 판정을 해, 맞음이면 밀기를 같이 넣는다(Lyra 화면도 능력 프레임 이동부터 민다)
			MeleeGuess guess;
			Vec3 meleePush;
			const bool meleeStarts = _self.WillMeleeStart(input);
			if (meleeStarts) guess = JudgeMelee(input, meleePush);
			const Vec3* pushArg = meleeStarts && guess.kind == wire::MELEE_HIT ? &meleePush : nullptr;
			SetPawnsForPrediction();
			const bool breakDashPredict = _breakDash == kBreakDashPredict && (input.buttons & wire::BUTTON_DASH) != 0;
			const bool breakGrenadePredict = _breakGrenade == kBreakGrenadePredict && (input.buttons & wire::BUTTON_GRENADE) != 0;
			if ((_breakCrouch && (input.buttons & wire::BUTTON_CROUCH) != 0) || breakDashPredict || breakGrenadePredict)
			{
				wire::Input mine = input;
				if (_breakCrouch) mine.buttons &= (uint8_t)~wire::BUTTON_CROUCH;
				if (breakDashPredict) mine.buttons &= (uint8_t)~wire::BUTTON_DASH;
				if (breakGrenadePredict) mine.buttons &= (uint8_t)~wire::BUTTON_GRENADE;
				_self.AddInput(mine, pushArg);
				input.seq = mine.seq;
			}
			else _self.AddInput(input, pushArg);
			if (_breakDash == kBreakDashServer) input.buttons &= (uint8_t)~wire::BUTTON_DASH;	// 일부러 틀림 — 예측엔 넣고 서버엔 안 보냄
			if (_breakGrenade == kBreakGrenadeServer) input.buttons &= (uint8_t)~wire::BUTTON_GRENADE;
			ClearPawns();
			if (meleeStarts) _meleeGuesses[input.seq] = guess;
			_meleeYaw = input.yaw;		// 서버 player.yaw = 마지막으로 적용한 입력의 요 — 다음 근접의 몸 앞
			if (_scriptStartPending)
			{
				_scriptStartPending = false;
				std::printf("SCRIPT start seq=%u\n", input.seq);	// 경로 첫 틱의 입력 번호 — SCRIPTANIM 의 seq 에서 빼면 경로 몇 번째 틱인지 나온다
			}
			if (_judge && _self.GetLastWeaponEvents().fired) JudgeShot(input, _self.GetLastWeaponEvents());
			if (g_traceInputs)
				std::fprintf(g_traceInputs, "I %u seq=%u st=%u mv=%.6g,%.6g aim=%.6g,%.6g b=%u w=%u at=%.3f,%.3f,%.3f\n", _welcome.playerId, input.seq, _newestStateTick,
					input.moveX, input.moveY, input.yaw, input.pitch, (unsigned)input.buttons, (unsigned)input.weapon, _self.GetMove().location.x, _self.GetMove().location.y, _self.GetMove().location.z);
			if (!_net.SendPacket(input, wire::Msg::C2S_INPUT)) _net.Close();
			_lastSentSeq = input.seq;
		}

		void Close() { _net.Close(); }

		void SetMeasureView(bool on) { _measureView = on; }
		void SetAdsWhenShooting(bool on) { _adsWhenShooting = on; }
		void SetCrouchToggle(bool on, bool breakCrouch) { _crouchToggle = on; _breakCrouch = breakCrouch; }
		void SetMelee(bool on, bool breakPush) { _meleeNear = on; _breakMelee = breakPush; }
		void SetDash(bool on, int breakDash) { _dashPress = on; _breakDash = breakDash; }
		void SetGrenade(bool on, int breakGrenade) { _grenadePress = on; _breakGrenade = breakGrenade; }
		uint64_t GetGrenadeUnconfirmedPost() const { return _grenadeUnconfirmedPost; }
		uint64_t GetGrenadeUnacked() const { return _self.CountGrenadeThrownUnacked(); }
		const DashBotCounts& GetDashCounts() const { return _dashCounts; }
		const CSelfPrediction::DashCounters& GetDashFixes() const { return _self.GetDashCounters(); }
		const GrenadeBotCounts& GetGrenadeCounts() const { return _grenadeCounts; }
		const CSelfPrediction::GrenadeCounters& GetGrenadeFixes() const { return _self.GetGrenadeCounters(); }
		uint64_t GetDashUnconfirmedPost() const { return _dashUnconfirmedPost; }
		void SetPush(bool on) { _push = on; }
		void SetFireAlways(bool on) { _fireAlways = on; }
		void SetPauseAfterSpawn(int ticks) { _pauseAfterSpawn = ticks; }
		uint64_t GetPawnReconciles() const { return _pawnReconciles; }
		uint64_t GetPawnCorrected() const { return _pawnCorrected; }
		uint64_t GetFallDiff() const { return _fallDiff; }
		double GetPawnCorrectSum() const { return _pawnCorrectSum; }
		double GetPawnCorrectMax() const { return _pawnCorrectMax; }
		bool IsScriptDone() const { return _scriptStep == (int)_scriptStepCount; }
		void SetScript(double holdSeconds, uint8_t weapon, const std::string& name)
		{
			_scripted = true;
			_scriptHoldTicks = (uint32_t)std::lround(holdSeconds * wire::kTickRate);
			_scriptWeapon = weapon;
			_scriptCurWeapon = weapon;
			if (name == "events")
			{
				_scriptSteps = kEventsScript;
				_scriptStepCount = sizeof(kEventsScript) / sizeof(kEventsScript[0]);
			}
			else if (name == "crouch")
			{
				_scriptSteps = kCrouchScript;
				_scriptStepCount = sizeof(kCrouchScript) / sizeof(kCrouchScript[0]);
			}
			else
			{
				_scriptSteps = kStartStopScript;
				_scriptStepCount = sizeof(kStartStopScript) / sizeof(kStartStopScript[0]);
			}
		}
		void SetJudge(JudgeShared* judge)
		{
			_judge = judge;
			if (!judge) return;
			_judgeBodies.reserve(wire::kMaxPlayersInRoom);
			_judgePoses.resize(wire::kMaxPlayersInRoom);
			for (std::vector<BodyPose>& pose : _judgePoses) pose.resize(judge->evaluators[0].BodyCount());	// 몸 개수는 클립 파일 하나에 하나 — 묶음마다 같다
		}
		uint64_t GetJudgeShots() const { return _judgeShots; }
		uint64_t GetJudgeBullets() const { return _judgeBullets; }
		uint64_t GetJudgeNoView() const { return _judgeNoView; }
		uint64_t GetJudgeNoSnap() const { return _judgeNoSnap; }
		uint64_t GetJudgeNoAnim() const { return _judgeNoAnim; }
		uint64_t GetJudgeClamped() const { return _judgeClamped; }
		uint64_t GetRecvBytes() const { return _recvBytes; }
		uint64_t GetAckLagSum() const { return _ackLagSum; }
		uint64_t GetAckLagSamples() const { return _ackLagSamples; }
		uint32_t GetAckLagMax() const { return _ackLagMax; }

		uint32_t GetId() const { return _welcome.playerId; }
		uint32_t GetRoomId() const { return _welcome.roomId; }
		uint8_t GetTeam() const { return _welcome.team; }
		bool HasResult() const { return _hasResult; }
		// 기다림 판(--testLockstep) — 난수를 봇 순번이 아니라 서버가 준 자리 번호로 심는다. 접속이 도착한 차례가 판마다 바뀌어도
		// 같은 자리 번호(같은 팀 · 같은 시작 자리)에 같은 난수가 붙는다. 보통 차례(순번 i → 번호 i + 1)면 순번으로 심은 값과 같다
		void SetSeedByPlayerId(uint32_t base) { _seedByPlayerBase = base; }
		// 기다림 판(--testLockstep) — 보낸 입력을 서버가 다 적용한 상태를 받았으면 다음 입력을 만든다. 10초 넘게 못 받으면 그냥 보내고 센다(판정이 FAIL 로 본다)
		bool LockstepWantsInput(Clock::time_point now)
		{
			if (!_hasSpawn || !_net.IsOpen() || _hasResult) return false;
			if (_self.GetSeq() == 0 || _lockstepAcked >= _self.GetSeq())
			{
				_lockstepWaitSince = now;
				return true;
			}
			if (now - _lockstepWaitSince < std::chrono::seconds(10)) return false;
			++_lockstepTimeouts;
			_lockstepWaitSince = now;
			return true;
		}
		uint64_t GetLockstepTimeouts() const { return _lockstepTimeouts; }
		bool IsOpen() const { return _net.IsOpen(); }
		const std::string& GetRecord() const { return _record; }
		const wire::Result& GetResult() const { return _result; }
		const RuleChecks& GetChecks() const { return _checks; }
		const std::map<uint32_t, int>& GetKillTally() const { return _killTally; }
		const std::map<uint32_t, int>& GetDeathTally() const { return _deathTally; }
		const std::map<uint32_t, int>& GetAssistTally() const { return _assistTally; }
		const std::vector<wire::Match>& GetPhaseChanges() const { return _phaseChanges; }
		uint64_t GetCompared() const { return _compared; }
		uint64_t GetPosDiff() const { return _posDiff; }
		uint64_t GetVelDiff() const { return _velDiff; }
		uint64_t GetAmmoDiff() const { return _ammoDiff; }
		uint64_t GetFlagDiff() const { return _flagDiff; }
		uint64_t GetInvDiff() const { return _invDiff; }
		uint64_t GetInvMulti() const { return _invMulti; }
		uint64_t GetWeaponPickupsMine() const { return _weaponPickupsMine; }
		uint64_t GetWeaponSeekStarts() const { return _weaponSeekStarts; }
		uint64_t GetWeaponSeekTicks() const { return _weaponSeekTicks; }
		uint64_t GetWeaponTakesSought() const { return _weaponTakesSought; }
		uint64_t GetWeaponSeekAmmoStarts() const { return _weaponSeekAmmoStarts; }
		uint64_t GetSpareOutTicks() const { return _spareOutTicks; }
		const CSelfPrediction::PickupCounters& GetPickupFixes() const { return _self.GetPickupCounters(); }
		uint64_t GetAdsCompared() const { return _adsCompared; }
		uint64_t GetAdsDiff() const { return _adsDiff; }
		uint64_t GetLockDiff() const { return _lockDiff; }
		uint64_t GetLockCompared() const { return _lockCompared; }
		uint64_t GetAdsInputs() const { return _adsInputs; }
		uint64_t GetCrouchCompared() const { return _crouchCompared; }
		uint64_t GetCrouchDiff() const { return _crouchDiff; }
		uint64_t GetCrouchPresses() const { return _crouchPresses; }
		const MeleeBotCounts& GetMeleeCounts() const { return _meleeCounts; }
		const CSelfPrediction::MeleeCounters& GetMeleeFixes() const { return _self.GetMeleeCounters(); }
		size_t GetMeleePendingDamage() const { return _meleePendingDamage.size(); }
		double GetPosMax() const { return _posMax; }
		uint32_t GetFirstBadSeq() const { return _firstBadSeq; }
		uint32_t GetSeq() const { return _self.GetSeq(); }
		uint64_t GetStates() const { return _states; }
		uint64_t GetDetours() const { return _detours; }
		uint64_t GetPadMsgs() const { return _padMsgs; }
		uint64_t GetPadBad() const { return _padBad; }
		uint64_t GetPadSeekStarts() const { return _padSeekStarts; }
		uint64_t GetPadSeekTicks() const { return _padSeekTicks; }
		uint64_t GetPadHealsMine() const { return _padHealsMine; }
		uint64_t GetPadHealsSought() const { return _padHealsSought; }
		size_t GetPadCount() const { return _pads.size(); }

	private:
		template <class T>
		static T Copy(const char* data, int size)
		{
			T out = T();
			std::memcpy(&out, data, std::min(size, (int)sizeof(T)));
			return out;
		}

		void OnPacket(const wire::Header& header, const char* data, int size)
		{
			_recvBytes += header.size;
			switch ((wire::Msg)header.type)
			{
			case wire::Msg::S2C_WELCOME:
				_welcome = Copy<wire::Welcome>(data, size);
				if (_seedByPlayerBase != 0) SeedRngs(_seedByPlayerBase + _welcome.playerId - 1);
				break;
			case wire::Msg::S2C_STATE:
				OnState(data, size);
				break;
			case wire::Msg::S2C_SHOT:
				OnShot(Copy<wire::Shot>(data, size));
				break;
			case wire::Msg::S2C_DAMAGE:
				OnDamage(Copy<wire::Damage>(data, size));
				break;
			case wire::Msg::S2C_MATCH:
				OnMatch(Copy<wire::Match>(data, size));
				break;
			case wire::Msg::S2C_KILL:
				OnKill(Copy<wire::Kill>(data, size));
				break;
			case wire::Msg::S2C_SPAWN:
				OnSpawn(Copy<wire::Spawn>(data, size));
				break;
			case wire::Msg::S2C_PICKUP:
				OnPickup(Copy<wire::Pickup>(data, size));
				break;
			case wire::Msg::S2C_MELEE:
				OnMelee(Copy<wire::Melee>(data, size));
				break;
			case wire::Msg::S2C_DASH:
				OnDash(Copy<wire::Dash>(data, size));
				break;
			case wire::Msg::S2C_GRENADE:
				OnGrenade(Copy<wire::Grenade>(data, size));
				break;
			case wire::Msg::S2C_GRENADE_STATE:
				OnGrenadeState(data, size);
				break;
			case wire::Msg::S2C_RESULT:
				_result = Copy<wire::Result>(data, size);
				_hasResult = true;
				_record += ResultRecordLines(_result);
				break;
			default:
				break;
			}
		}

		void OnMatch(const wire::Match& match)
		{
			if (match.phase != _phase || _phaseChanges.empty())
			{
				_phaseChanges.push_back(match);
				if (match.phase == (uint8_t)wire::EMatchPhase::Playing)
				{
					// 서버가 처치·사망을 0 으로 돌린다 — 대기 중 떨어져 죽은 것은 결과에 안 들어간다
					_playTick = match.tick;
					_killTally.clear();
					_deathTally.clear();
					_assistTally.clear();	// 서버도 도움 수만 0 으로 돌리고 누가 때렸나(_damagedBy)는 둔다 — 대기 중엔 피해가 없다(damageOutsidePlaying)
				}
				else if (match.phase == (uint8_t)wire::EMatchPhase::PostGame)
				{
					_postTick = match.tick;
				}
			}
			_phase = match.phase;
		}

		void OnSpawn(const wire::Spawn& spawn)
		{
			++_checks.spawnsSeen;
			_spawnTick[spawn.player] = spawn.tick;
			_deadSince.erase(spawn.player);
			if (spawn.player != _welcome.playerId) return;

			_hasSpawn = true;
			_pauseLeft = _pauseAfterSpawn;
			SetPawnsForPrediction();	// 부활 자리에서 다시 돌리는 걸음도 남 캡슐을 본다
			_self.OnSpawn(spawn.x, spawn.y, spawn.z, spawn.lastSeq, spawn.yaw);
			ClearPawns();
			// 근접 몸 앞 = 서버 player.yaw — 부활이 시작 자리 요로 놓는다. 그 뒤 입력을 이미 보냈으면 서버도 그 입력의 요로 바꿔 둔다(마지막 보낸 요 그대로)
			if (_self.GetSeq() <= spawn.lastSeq) _meleeYaw = spawn.yaw;
			// 가진 무기 중에서 고른다(W-3) — 부활 가방은 예측이 서버와 같은 Reset 으로 만들었다(권총 하나, 시험 옵션이면 셋). 난수는 전처럼 한 번 — 세 무기면 칸 차례 = 무기 번호라 고른 값이 전과 같다
			_chosenWeapon = PickOwnedWeapon(_rng.NextFloat());
			_stuckCheckAt = Vec3(spawn.x, spawn.y, spawn.z);
			_stuckTicks = 0;
			_detourLeft = 0;
			_scriptPlaced = false;	// 경기 시작이 시작 자리로 돌려놓으면 다시 옮기고 처음부터
			_scriptArmed = false;	// 부활은 권총으로 돌려놓는다 — 경로 무기를 다시 든다
		}

		void OnKill(const wire::Kill& kill)
		{
			++_checks.kills;
			_record += KillRecordLine(kill);
			if (kill.weapon == wire::kWeaponMelee) ++_meleeCounts.kills;
			if (kill.weapon == wire::kWeaponGrenade)
			{
				++_grenadeCounts.killMsgs;
				if (kill.attacker == _welcome.playerId) ++_grenadeCounts.killsMine;		// 자기 처치도 공격자 = 던진 사람(DetonateGrenade → KillPlayer)
			}
			// 처치 집계 — B_ShooterGameScoring_Base 규칙(서버 KillPlayer 와 따로 적음): 다른 팀이 죽였을 때만 처치 · 죽은 쪽 죽음 +1(떨어짐 · 자기 수류탄은 둘 다 0).
			// 총알 · 근접은 같은 팀에 피해가 없어(ComputeDamage · 근접 같은 팀 칸) 「처치자 있음」과 같았다. 자기 수류탄 처치(G-3)가 처음으로 처치자 = 같은 팀(자기)이다
			const auto attackerState = _players.find(kill.attacker);
			const auto targetState = _players.find(kill.target);
			const bool sameTeam = kill.attacker == kill.target
				|| (attackerState != _players.end() && targetState != _players.end() && attackerState->second.team == targetState->second.team);
			if (kill.attacker != 0 && !sameTeam)
			{
				++_killTally[kill.attacker];
				++_deathTally[kill.target];
			}
			_deadSince[kill.target] = kill.tick;
			// 도움 — ShooterCore AssistProcessor 규칙을 서버와 따로 적는다. 때린 사람 중 처치자를 뺀 전원 +1, 그 사람 기록은 지운다.
			// 처치자가 없으면(떨어짐) 죽은 사람 자신을 뺀다 — 자기 파괴의 처치 소식 시킨 쪽 = 자기 선수 상태(원본 기준, D112)
			std::vector<uint32_t> expect;
			const uint32_t instigator = kill.attacker != 0 ? kill.attacker : kill.target;
			const auto by = _damagedBy.find(kill.target);
			if (by != _damagedBy.end())
			{
				for (const uint32_t attacker : by->second)
				{
					if (attacker != instigator) expect.push_back(attacker);
				}
				_damagedBy.erase(by);
			}
			for (const uint32_t assister : expect) ++_assistTally[assister];
			_checks.assists += expect.size();
			bool same = (size_t)kill.assistCount == expect.size() && expect.size() <= (size_t)(wire::kMaxPlayersInRoom - 1);
			for (size_t i = 0; same && i < expect.size(); ++i) same = kill.assisters[i] == expect[i];
			if (!same) ++_checks.assistKillDiff;
			if (kill.target == _welcome.playerId)
			{
				_self.BeginWaitSpawn();
			}
		}

		void OnDamage(const wire::Damage& damage)
		{
			++_checks.damages;
			// 경기가 끝나는 틱의 앞선 탄은 정상이다 — 그 틱 안에서 승리가 난다
			if (_playTick == 0 || damage.tick < _playTick || (_postTick != 0 && damage.tick > _postTick)) ++_checks.damageOutsidePlaying;
			const auto spawned = _spawnTick.find(damage.target);
			if (spawned != _spawnTick.end() && damage.tick < spawned->second + SecondsToTicks(_rules.spawnImmunitySeconds)) ++_checks.damageWhileImmune;
			// 도움 셈 — 0보다 큰 피해만 알림이 오고(LyraHealthSet), 받은 차례 그대로 든다.
			// 자기 피해(수류탄)도 든다 — AssistProcessor 의 「시킨 쪽 ≠ 맞은 쪽」은 몸 · 선수 상태라 늘 다르다(G-0-b2 실측 · 서버 NoteDamage 와 같은 규칙, D112)
			if (damage.amount > 0)
			{
				std::vector<uint32_t>& by = _damagedBy[damage.target];
				if (std::find(by.begin(), by.end(), damage.attacker) == by.end()) by.push_back(damage.attacker);
			}
			// 근접 피해(E-3) — 바로 앞에 받은 근접 맞음과 공격자 · 대상 · 양 · 틱이 같아야 한다(서버는 근접 → 피해 → 처치 차례로 보낸다)
			if (damage.weapon == wire::kWeaponMelee)
			{
				++_meleeCounts.damageMsgs;
				if (_meleePendingDamage.empty()) ++_meleeCounts.badDamage;
				else
				{
					const wire::Melee& hit = _meleePendingDamage.front();
					if (hit.attacker != damage.attacker || hit.target != damage.target || hit.damage != damage.amount || hit.tick != damage.tick) ++_meleeCounts.badDamage;
					_meleePendingDamage.pop_front();
				}
			}
			// 수류탄 피해(G-3) — 서버는 터짐 → 몸마다 피해(→ 처치) 차례로 보낸다(DetonateGrenade). 같은 틱 다른 수류탄은 그 뒤 터짐이 이어 온다
			if (damage.weapon == wire::kWeaponGrenade)
			{
				GrenadeBotCounts& c = _grenadeCounts;
				++c.damageMsgs;
				if (!_boomSeen || damage.attacker != _lastBoomThrower || damage.tick != _lastBoomTick) ++c.badDamage;
				if (damage.attacker == _welcome.playerId)
				{
					++c.damageMine;
					if (damage.target == _welcome.playerId) ++c.selfDamageMine;
				}
			}
		}

		// 근접 소식(E-3) — 모든 봇이 규칙을 보고, 내 근접이면 화면 판정과 견준 뒤 예측 밀기를 서버 값으로 맞춘다(OnMeleeResult)
		void OnMelee(const wire::Melee& melee)
		{
			MeleeBotCounts& c = _meleeCounts;
			++c.msgs;
			const bool hit = melee.result == wire::MELEE_HIT;
			const bool hasForce = melee.forceX != 0.f || melee.forceY != 0.f;
			if (melee.result > wire::MELEE_HIT
				|| (!hit && (melee.damage != 0.f || hasForce))
				|| (hit && (melee.target == 0 || (melee.damage != 0.f && melee.damage != kMeleeDamage)))
				|| (melee.result == wire::MELEE_NONE && melee.target != 0))
				++c.badShape;
			const auto dead = _deadSince.find(melee.attacker);
			if (dead != _deadSince.end() && melee.tick > dead->second) ++c.badDead;
			if (_postTick != 0 && melee.tick > _postTick) ++c.badPost;		// 경기 끝 틱 경계는 피해 검사(damageOutsidePlaying)와 같다
			if (hit)
			{
				++c.hits;
				if (melee.damage > 0.f)
				{
					++c.hitsDamaged;
					_meleePendingDamage.push_back(melee);
				}
				else
				{
					++c.hitsZero;
					// 피해 0 은 무적일 때만(서버 IsImmune — 경기 중이 아님 · 부활 무적). OnDamage 의 대기 · 무적 피해 검사와 반대 방향
					const bool playing = _playTick != 0 && melee.tick >= _playTick && (_postTick == 0 || melee.tick <= _postTick);
					const auto spawned = _spawnTick.find(melee.target);
					const bool immune = spawned != _spawnTick.end() && melee.tick < spawned->second + SecondsToTicks(_rules.spawnImmunitySeconds);
					if (playing && !immune) ++c.badZero;
				}
			}
			if (melee.attacker != _welcome.playerId) return;
			++c.mine;
			if (hit) ++(hasForce ? c.mineHitForce : c.mineHitNoForce);
			const auto guess = _meleeGuesses.find(melee.seq);
			if (guess == _meleeGuesses.end()) ++c.noGuess;
			else
			{
				if (guess->second.kind != melee.result) ++c.kindDiff;
				else if (guess->second.target != melee.target) ++c.targetDiff;
				else ++c.agree;
				const Vec3 serverForce = hit ? Vec3((double)melee.forceX, (double)melee.forceY, 0.0) : Vec3();
				c.forceDiffMax = std::max(c.forceDiffMax, Length(guess->second.force - serverForce));
				_meleeGuesses.erase(guess);
			}
			SetPawnsForPrediction();	// 그 입력부터 다시 돌리는 걸음도 남 캡슐을 본다 — 근접은 늘 남 옆이다
			_self.OnMeleeResult(melee.seq, hit, Vec3((double)melee.forceX, (double)melee.forceY, 0.0));
			ClearPawns();
		}

		// 대시 소식(D-3) — 받은 것 전부의 규칙을 보고, 내 것이면 예측을 서버 값으로 고친다(다르면 그 입력부터 대시를 다시 고르고 걸음을 다시 돌린다)
		void OnDash(const wire::Dash& dash)
		{
			DashBotCounts& c = _dashCounts;
			++c.msgs;
			const Vec3 force(dash.forceX, dash.forceY, dash.forceZ);
			if (dash.dir > (uint8_t)EDashDir::Right || std::fabs(Length(force) - (double)kDashStrength) > 1e-6) ++c.badShape;	// 힘 = 정규화한 방향 × 세기(DashForce)
			const auto dead = _deadSince.find(dash.player);
			if (dead != _deadSince.end() && dash.tick > dead->second) ++c.badDead;
			if (_postTick != 0 && dash.tick > _postTick) ++c.badPost;		// 근접 · 피해와 같은 경계
			if (dash.player != _welcome.playerId) return;
			++c.mine;
			if (dash.dir <= (uint8_t)EDashDir::Right) ++c.mineDir[dash.dir];
			SetPawnsForPrediction();	// 그 입력부터 다시 돌리는 걸음도 남 캡슐을 본다
			_self.OnDashResult(dash.seq, dash.dir, force);
			ClearPawns();
		}

		void OnGrenade(const wire::Grenade& grenade)
		{
			GrenadeBotCounts& c = _grenadeCounts;
			++c.msgs;
			switch (grenade.kind)
			{
			case wire::GRENADE_THROW:
			{
				++c.throws;
				const auto dead = _deadSince.find(grenade.thrower);
				if (dead != _deadSince.end() && grenade.tick > dead->second) ++c.badDead;
				if (_postTick != 0 && grenade.tick > _postTick) ++c.badPost;		// 대시 · 근접과 같은 경계
				if (grenade.id == 0) ++c.throwsNoGrenade;
				else
				{
					_liveGrenades.insert(grenade.id);
					if (_firstGrenadeId == 0) _firstGrenadeId = grenade.id;
				}
				break;
			}
			case wire::GRENADE_BOOM_TIMER:
			case wire::GRENADE_BOOM_HIT:
				++(grenade.kind == wire::GRENADE_BOOM_HIT ? c.boomHit : c.boomTimer);
				_liveGrenades.erase(grenade.id);
				_lastBoomTick = grenade.tick;
				_lastBoomThrower = grenade.thrower;
				_boomSeen = true;
				return;
			default:
				++c.badKind;
				return;
			}
			if (grenade.thrower != _welcome.playerId) return;
			++c.mine;
			SetPawnsForPrediction();	// 근접 끊음이 바뀌어 다시 돌리는 걸음도 남 캡슐을 본다
			_self.OnGrenadeThrown(grenade.seq);
			ClearPawns();
		}

		// 머리 + 칸 count 개 — 크기가 안 맞으면 칸을 안 읽고 센다
		void OnGrenadeState(const char* data, int size)
		{
			GrenadeBotCounts& c = _grenadeCounts;
			++c.stateMsgs;
			if (size < (int)sizeof(wire::GrenadeStateHead))
			{
				++c.badState;
				return;
			}
			wire::GrenadeStateHead head = wire::GrenadeStateHead();
			std::memcpy(&head, data, sizeof(head));
			if (head.count > wire::kMaxPlayersInRoom || size != (int)(sizeof(head) + sizeof(wire::GrenadeStateEntry) * head.count))
			{
				++c.badState;
				return;
			}
			for (uint8_t i = 0; i < head.count; ++i)
			{
				wire::GrenadeStateEntry e = wire::GrenadeStateEntry();
				std::memcpy(&e, data + sizeof(head) + sizeof(e) * i, sizeof(e));
				++c.stateEntries;
				if (e.flags & wire::GRENADE_STATE_BOUNCED) ++c.stateBounced;
				if (_liveGrenades.count(e.id) != 0) continue;
				// 던짐 소식이 상태보다 먼저 온다(같은 연결, 입력 처리 안에서 보냄) — 처음 받은 던짐 뒤 번호인데 모르면 소식을 놓쳤다
				if (_firstGrenadeId == 0 || e.id < _firstGrenadeId) ++c.stateBeforeJoin;
				else ++c.badStateId;
			}
		}

		void OnShot(const wire::Shot& shot)
		{
			++_checks.shots;
			const auto dead = _deadSince.find(shot.shooter);
			if (dead != _deadSince.end() && shot.tick > dead->second) ++_checks.shotsWhileDead;
			for (int i = 0; i < shot.count; ++i)
			{
				const wire::ShotBullet& bullet = shot.bullets[i];
				if (bullet.hit != (uint8_t)wire::EShotHit::Body || bullet.damage != 0.f) continue;
				const auto shooter = _players.find(shot.shooter);
				const auto target = _players.find(bullet.target);
				if (shooter != _players.end() && target != _players.end() && shooter->second.team != target->second.team) ++_checks.immuneBulletHits;
			}
		}

		void OnState(const char* data, int size)
		{
			if (size < (int)offsetof(wire::State, players)) return;
			++_states;
			const Clock::time_point now = Clock::now();
			if (g_stateGapsMs && _hasStateTime) g_stateGapsMs->push_back(std::chrono::duration<float, std::milli>(now - _stateTime).count());
			_stateTime = now;
			_hasStateTime = true;
			uint32_t stateTick = 0;
			std::memcpy(&stateTick, data + offsetof(wire::State, tick), sizeof(stateTick));
			if (_measureView) UpdateViewClock(stateTick);
			_newestStateTick = stateTick;
			uint8_t count = 0;
			std::memcpy(&count, data + offsetof(wire::State, count), 1);
			int tailAt = (int)offsetof(wire::State, players) + (int)sizeof(wire::PlayerState) * count;
			bool selfSeen = false;
			wire::PlayerState selfState = wire::PlayerState();
			for (int i = 0; i < count; ++i)
			{
				wire::PlayerState p;
				std::memcpy(&p, data + offsetof(wire::State, players) + sizeof(wire::PlayerState) * i, sizeof(p));
				_players[p.id] = p;
				_playerTick[p.id] = stateTick;
				if (p.id == _welcome.playerId) _lockstepAcked = p.lastSeq;
				if (g_traceInputs && p.id == _welcome.playerId)
					std::fprintf(g_traceInputs, "S %u tick=%u lastSeq=%u hp=%.6g flags=%u at=%.3f,%.3f,%.3f\n", p.id, stateTick, p.lastSeq, (double)p.hp, (unsigned)p.flags, (double)p.x, (double)p.y, (double)p.z);
				wire::AnimStateWire anim{};
				const bool hasAnim = (_judge || _scripted) && ReadAnimTail(data, size, tailAt, anim);
				if (_scripted && hasAnim && _scriptPlaced && p.id == _welcome.playerId)
				{
					// 경로 모드 — 받은 자기 몽타주 칸(칸 번호 = 묶음 << 4 | 종류, 시각 1/6000초, 비중 0~255). 서버 무기 · 마지막 적용 입력 번호와 함께
					std::printf("SCRIPTANIM tick=%u seq=%u weapon=%u n=%u", stateTick, p.lastSeq, (unsigned)p.weapon, (unsigned)anim.montageCount);
					for (int m = 0; m < (int)anim.montageCount; ++m) std::printf(" %u:%u:%u", (unsigned)anim.montages[m].montage, (unsigned)anim.montages[m].time, (unsigned)anim.montages[m].weight);
					std::printf("\n");
				}
				if (_measureView) _viewHistory[p.id].Add(p, stateTick, hasAnim ? &anim : nullptr);
				if (p.id == _welcome.playerId)
				{
					// 경로 모드는 비교하지 않는다 — 자리 옮기기는 걷는 상태로, 봇의 OnSpawn 은 떨어지는 상태로 시작해 처음부터 어긋난다
					if (!_scripted) CompareSelf(p);
					selfSeen = true;
					selfState = p;
					const uint32_t ackLag = _self.GetSeq() - p.lastSeq;
					_ackLagSum += ackLag;
					++_ackLagSamples;
					_ackLagMax = std::max(_ackLagMax, ackLag);
				}
			}
			// 남 자리를 이 상태로 다 갱신한 뒤에 고친다 — 다시 돌리는 걸음도 남 캡슐을 본다
			if (selfSeen && _rules.pawnBlock && !_scripted) ReconcileWithPawns(selfState);
		}

		struct Pad
		{
			bool known = false;
			bool available = false;
			Vec3 at;
			uint8_t kind = 0;		// 받침 소식 kind = EPadKind 숫자(W-3) — 체력 받침 · 무기 받침 찾아가기를 가른다
		};

		// 받침 소식(컨텐츠 B-3 · B-4) — 들어올 때 받침마다 현황이 오므로 자리 · 켜짐을 여기서만 안다
		void OnPickup(const wire::Pickup& m)
		{
			++_padMsgs;
			if (m.pad >= m.padCount)
			{
				++_padBad;
				return;
			}
			if (_pads.size() != m.padCount) _pads.resize(m.padCount);
			Pad& pad = _pads[m.pad];
			pad.known = true;
			pad.available = m.available != 0;
			pad.at = Vec3(m.x, m.y, m.z);
			pad.kind = m.kind;
			const bool take = m.reason == wire::PICKUP_TAKE || m.reason == wire::PICKUP_TAKE_RECHECK;
			if (!take || m.picker != _welcome.playerId) return;
			if (m.kind == (uint8_t)EPadKind::Heal)
			{
				++_padHealsMine;
				if (_padSeeking) ++_padHealsSought;	// 찾아가던 중에 받은 회복 — 갈래가 실제로 받침에 닿았다
			}
			else if (m.give != wire::kPickupGiveNone)
			{
				++_weaponPickupsMine;
				if (_weaponSeeking) ++_weaponTakesSought;	// 무기 받침을 찾아가던 중에 집음 — 갈래가 실제로 받침에 닿았다(W-5)
				// 예측 가방 · 탄을 서버 값으로 — 그 입력부터 무기 쪽을 다시 돌린다(W-3). 이 소식은 그 틱 상태보다 먼저 와서 맞춰 보기 전에 고쳐진다
				_self.OnPickup(m.seq, m.reason == wire::PICKUP_TAKE, m.weapon, m.give, m.slot, m.magazine, m.spare);
				// 화면 판정 기록에 줍기 창 — 서버는 seq 입력부터 새 무기 · 탄인데 봇은 이 소식 전까지(sentSeq 까지 보낸 입력) 옛 무기로 쐈다(무기 줍기 플랜 「예측」 — 줍기는 예측하지 않는다).
				// check_bot_hits.py 가 이 창의 탄을 죽은 · 나간 상대처럼 빼고 따로 센다(컨텐츠 D-6)
				if (_judge && _judge->log)
					std::fprintf(_judge->log, "BOTPICKUP shooter=%u seq=%u give=%u weapon=%u sentSeq=%u\n", _welcome.playerId, m.seq, (unsigned)m.give, (unsigned)m.weapon, _lastSentSeq);
				// 서버가 무기를 바꿨으면 고른 무기도 따라 맞춘다 — 「앞 입력과 달라졌을 때만 바꾼다」(Loadout.h)라 옛 번호를 보내도 안 되돌아가지만,
				// 탄이 떨어져 다음 무기로 넘길 때(NextOwnedWeapon) 기준이 든 무기여야 한다. 다리 · 서버 프로브도 같은 규칙(W-2 기록)
				if (m.give == (uint8_t)EGiveResult::NewEquipped) _chosenWeapon = m.weapon;
			}
		}

		// 무기 받침이 주는 무기 — 서버 TryWeaponPickup(Room.cpp)의 종류 → 무기와 같은 짝. 체력 받침은 kNoWeapon
		static uint8_t PadWeapon(uint8_t kind)
		{
			switch ((EPadKind)kind)
			{
			case EPadKind::Pistol: return (uint8_t)EWeapon::Pistol;
			case EPadKind::Rifle: return (uint8_t)EWeapon::Rifle;
			case EPadKind::Shotgun: return (uint8_t)EWeapon::Shotgun;
			case EPadKind::Heal: break;
			}
			return kNoWeapon;
		}

		// 무기 받침 찾아가기(W-5, weapon-pickup-plan.md 「봇」) — 같은 층에서 가장 가까운(수평 거리) 켜진 무기 받침 중
		// 안 가진 무기의 받침, 또는 든 무기의 예비가 바닥났으면 그 무기 받침. 가진 무기 · 든 무기는 서버 상태 값으로 본다(체력 갈래가 서버 체력을 보는 것과 같다). 없으면 nullptr
		const Pad* NearestWantedWeaponPad(const Vec3& at, const wire::PlayerState& self) const
		{
			const Pad* best = nullptr;
			double bestD = 0;
			for (const Pad& pad : _pads)
			{
				const uint8_t weapon = PadWeapon(pad.kind);
				if (!pad.known || !pad.available || weapon == kNoWeapon || std::fabs(pad.at.z - at.z) > kPadSameFloorZ) continue;
				bool owned = false;
				for (size_t s = 0; s < kQuickSlotCount; ++s) owned = owned || CLoadout::UnpackSlot(self.quickSlots, s) == weapon;
				const bool outOfSpare = weapon == self.weapon && self.spare <= 0;
				if (owned && !outOfSpare) continue;
				const double dx = pad.at.x - at.x, dy = pad.at.y - at.y;
				const double d = dx * dx + dy * dy;
				if (!best || d < bestD)
				{
					best = &pad;
					bestD = d;
				}
			}
			return best;
		}

		// 다쳤으면 같은 층에서 가장 가까운 켜진 받침(수평 거리). 없으면 nullptr
		const Pad* NearestOpenPad(const Vec3& at) const
		{
			const Pad* best = nullptr;
			double bestD = 0;
			for (const Pad& pad : _pads)
			{
				if (!pad.known || !pad.available || pad.kind != (uint8_t)EPadKind::Heal || std::fabs(pad.at.z - at.z) > kPadSameFloorZ) continue;	// 무기 받침 찾아가기는 NearestWantedWeaponPad
				const double dx = pad.at.x - at.x, dy = pad.at.y - at.y;
				const double d = dx * dx + dy * dy;
				if (!best || d < bestD)
				{
					best = &pad;
					bestD = d;
				}
			}
			return best;
		}

		// 가진 무기 하나 — r(0 이상 1 미만)을 빠른 칸 차례의 가진 무기 수로 나눈다. 세 무기면 전과 같은 값(r × 3)
		uint8_t PickOwnedWeapon(float r) const
		{
			const CLoadout& bag = _self.GetLoadout();
			uint8_t owned[kQuickSlotCount] = {};
			int count = 0;
			for (size_t i = 0; i < kQuickSlotCount; ++i)
			{
				if (bag.SlotWeapon(i) != kNoWeapon) owned[count++] = bag.SlotWeapon(i);
			}
			if (count == 0) return 0;
			int at = (int)(r * (float)count);
			if (at >= count) at = 0;
			return owned[at];
		}

		// 빠른 칸 차례로 다음 가진 무기 — 세 무기면 전과 같은 값((무기 + 1) % 3). 하나뿐이면 그대로
		uint8_t NextOwnedWeapon(uint8_t from) const
		{
			const CLoadout& bag = _self.GetLoadout();
			int slot = bag.SlotOf(from);
			if (slot < 0) slot = bag.ActiveSlot();
			for (size_t step = 1; step <= kQuickSlotCount; ++step)
			{
				const uint8_t weapon = bag.SlotWeapon(((size_t)slot + step) % kQuickSlotCount);
				if (weapon != kNoWeapon) return weapon;
			}
			return from;
		}

		// 사람끼리 막기(F-2) — 내 예측이 볼 남 캡슐. 남 자리는 마지막으로 받은 상태라 서버가 내 입력을 굴린 순간의 자리와 다르다 — 그 차이가 보정으로 나온다.
		// 산 사람만(서버와 같음). 마지막 상태에 안 실린 사람(나감)은 뺀다
		void SetPawnsForPrediction()
		{
			if (!_rules.pawnBlock) return;
			_pawnScratch.clear();
			for (const auto& entry : _players)
			{
				const wire::PlayerState& o = entry.second;
				if (o.id == _welcome.playerId || (o.flags & wire::STATE_DEAD) || _playerTick[o.id] != _newestStateTick) continue;
				MoveState shape;
				shape.crouched = (o.flags & wire::STATE_CROUCHED) != 0;
				PawnCapsule capsule;
				capsule.center = Vec3(o.x, o.y, o.z);
				capsule.radius = (float)_capsuleRadius;
				capsule.halfHeight = _sim.HalfHeightOf(shape);
				capsule.yaw = o.yaw;
				_pawnScratch.push_back(capsule);
			}
			_sim.SetPawns(_pawnScratch.data(), _pawnScratch.size());
		}
		void ClearPawns()
		{
			if (_rules.pawnBlock) _sim.SetPawns(nullptr, 0);
		}

		// 대시 확인(AckDash)으로 「소식 없이 지나감」이 는 수 중 경기 끝 단계 몫 — 서버는 단계를 틱 첫머리에 바꾸고(TickPhase → 입력) 경기 끝엔 대시를 막는데,
		// 예측은 경기 단계를 모르고 봇은 경기 끝 소식을 받기 전에 보낸 입력에선 누를 수 있다. 정직한 클라도 못 피하는 고침이라 따로 센다(상태 틱 ≥ 경기 끝 틱).
		// 점수로 끝난 틱은 그 틱 입력이 경기 중에 돌아(끝은 그 뒤 처치에서) 서버도 켜므로 이 갈래에 안 든다
		void NoteDashUnconfirmed(uint64_t before)
		{
			const uint64_t added = _self.GetDashCounters().unconfirmed - before;
			if (added != 0 && _postTick != 0 && _newestStateTick >= _postTick) _dashUnconfirmedPost += added;
		}

		// 수류탄(G-3) — 대시와 같은 경기 끝 몫. 죽음 몫(unconfirmedDead)은 이미 따로 세므로 뺀 수로 본다(둘에 겹쳐 들지 않게)
		uint64_t GrenadeUnconfirmedAlive() const
		{
			const CSelfPrediction::GrenadeCounters& g = _self.GetGrenadeCounters();
			return g.unconfirmed - g.unconfirmedDead;
		}
		void NoteGrenadeUnconfirmed(uint64_t before)
		{
			const uint64_t added = GrenadeUnconfirmedAlive() - before;
			if (added != 0 && _postTick != 0 && _newestStateTick >= _postTick) _grenadeUnconfirmedPost += added;
		}

		// 사람끼리 막기를 켠 판 — 같은 코드라도 남 자리를 늦게 알아 어긋난다. 다리(MOBridgeSubsystem.cpp kCorrectTolerance)와 같은 0.1 로 고치고 그 양을 센다
		void ReconcileWithPawns(const wire::PlayerState& p)
		{
			SetPawnsForPrediction();
			const uint64_t unconfirmedBefore = _self.GetDashCounters().unconfirmed;
			const uint64_t grenadeBefore = GrenadeUnconfirmedAlive();
			const CSelfPrediction::ReconcileResult r = _self.Reconcile(p, kPawnCorrectTolerance);		// 처음에 AckMelee · AckDash · AckGrenade
			NoteDashUnconfirmed(unconfirmedBefore);
			NoteGrenadeUnconfirmed(grenadeBefore);
			ClearPawns();
			if (!r.compared) return;
			++_pawnReconciles;
			if (!r.corrected) return;
			++_pawnCorrected;
			_pawnCorrectSum += r.posDiff;
			_pawnCorrectMax = std::max(_pawnCorrectMax, r.posDiff);
		}

		// flagDiff 가 어느 깃발인지 — 부하 판(LOADBOT)은 갈래별 수를 안 적어, 갈린 줄을 봇마다 처음 몇 개만 그대로 적는다(W-5-d 받침 켠 오탈 판 70판 중 4판 flagDiff 1~2)
		static constexpr int FLAG_DIFF_LOG_LIMIT = 3;
		void LogFlagDiff(const char* kind, const wire::PlayerState& p, const CSelfPrediction::Slot& slot, bool server, bool pred)
		{
			if (_flagDiffLogged >= FLAG_DIFF_LOG_LIMIT) return;
			++_flagDiffLogged;
			// predFalling = 예측의 떨어짐(flags 는 서버 깃발 — 0x1 이 서버 떨어짐). 앉기 키는 땅에서만 앉혀 두 쪽 떨어짐이 다르면 앉음도 갈린다
			std::printf("FLAGDIFF bot=%u kind=%s seq=%u server=%d pred=%d phase=%u weapon=%u/%u mag=%d/%d spare=%d/%d slots=%u/%u flags=0x%x predFalling=%d\n", GetId(), kind, p.lastSeq,
				server ? 1 : 0, pred ? 1 : 0, (unsigned)_phase, (unsigned)p.weapon, (unsigned)slot.weapon, (int)p.magazine, (int)slot.magazine, (int)p.spare, (int)slot.spare,
				(unsigned)p.quickSlots, (unsigned)slot.quickSlots, (unsigned)p.flags, slot.move.mode == EMoveMode::Falling ? 1 : 0);
		}

		void CompareSelf(const wire::PlayerState& p)
		{
			// 근접(E-3) — 서버가 지나간 입력 중 결과 없는 근접은 밀기를 지운다(예측 AckMelee). 근접 소식은 같은 연결에서 상태보다 먼저 온다.
			// 대시(D-3)도 같다 — 소식 없이 지나간 대시는 안 켠 것으로 다시 고른다(AckDash). 막기를 켠 판은 뒤 ReconcileWithPawns → Reconcile 이 남 캡슐을 놓고 부른다
			if (!(p.flags & wire::STATE_DEAD))
			{
				_self.AckMelee(p.lastSeq);
				if (!_rules.pawnBlock)
				{
					const uint64_t unconfirmedBefore = _self.GetDashCounters().unconfirmed;
					_self.AckDash(p.lastSeq);
					NoteDashUnconfirmed(unconfirmedBefore);
				}
			}
			// 수류탄(G-3) — 죽음 중에도(서버는 죽은 입력에도 쿨다운을 센다). 대시 확인 뒤 — 대시 막기가 고쳐진 칸으로 고른다. 막기 판은 Reconcile 이 부른다
			if (!_rules.pawnBlock)
			{
				const uint64_t grenadeBefore = GrenadeUnconfirmedAlive();
				_self.AckGrenade(p.lastSeq, (p.flags & wire::STATE_DEAD) != 0);
				NoteGrenadeUnconfirmed(grenadeBefore);
			}
			while (!_meleeGuesses.empty() && _meleeGuesses.begin()->first <= p.lastSeq)
			{
				++_meleeCounts.guessNoResult;
				_meleeGuesses.erase(_meleeGuesses.begin());
			}
			const CSelfPrediction::Slot* found = _self.FindComparable(p);
			if (!found) return;
			const CSelfPrediction::Slot& slot = *found;
			// 와이어는 float — 같은 폭으로 맞춘다
			const float slotX = (float)slot.move.location.x, slotY = (float)slot.move.location.y, slotZ = (float)slot.move.location.z;
			const float slotVx = (float)slot.move.velocity.x, slotVy = (float)slot.move.velocity.y, slotVz = (float)slot.move.velocity.z;
			++_compared;
			const double dx = (double)p.x - slotX, dy = (double)p.y - slotY, dz = (double)p.z - slotZ;
			const double dp = std::sqrt(dx * dx + dy * dy + dz * dz);
			const double vx = (double)p.vx - slotVx, vy = (double)p.vy - slotVy, vz = (double)p.vz - slotVz;
			const double dv = std::sqrt(vx * vx + vy * vy + vz * vz);
			_posMax = std::max(_posMax, dp);
			bool bad = false;
			if (dp > 0) { ++_posDiff; bad = true; }
			if (dv > 0) { ++_velDiff; bad = true; }
			// 경기 끝 단계는 서버가 사격 입력을 지운다 — 그 경계를 입력 번호로는 모르므로 탄은 그 전까지만 본다
			if (_phase < (uint8_t)wire::EMatchPhase::PostGame)
			{
				if (p.magazine != slot.magazine || p.spare != slot.spare || p.weapon != slot.weapon) { ++_ammoDiff; bad = true; }
				if (((p.flags & wire::STATE_RELOADING) != 0) != slot.reloading) { ++_flagDiff; bad = true; LogFlagDiff("reloading", p, slot, (p.flags & wire::STATE_RELOADING) != 0, slot.reloading); }
			}
			// 떨어짐은 이동의 결과 — 사람끼리 막기를 켠 판은 위치처럼 남의 캡슐(예측은 받은 최신 위치)에 따라 갈릴 수 있어 따로 센다(F-2)
			const bool fallDiff = ((p.flags & wire::STATE_FALLING) != 0) != (slot.move.mode == EMoveMode::Falling);
			const bool lockDiff = ((p.flags & wire::STATE_MOVE_LOCKED) != 0) != (slot.lockInputs > 0);
			if (fallDiff) ++_fallDiff;
			if ((p.flags & wire::STATE_MOVE_LOCKED) != 0) ++_lockCompared;
			if (lockDiff) ++_lockDiff;
			if (lockDiff || (fallDiff && !_rules.pawnBlock))
			{
				++_flagDiff;
				if (lockDiff) LogFlagDiff("moveLocked", p, slot, (p.flags & wire::STATE_MOVE_LOCKED) != 0, slot.lockInputs > 0);
				else LogFlagDiff("falling", p, slot, (p.flags & wire::STATE_FALLING) != 0, slot.move.mode == EMoveMode::Falling);
			}
			if (fallDiff || lockDiff) bad = true;
			// 조준 — 경기 끝에도 버튼을 따르므로(D61) 단계와 상관없이 본다. 켜진 줄 수를 같이 세 이 비교가 실제로 돌았는지 본다
			if ((p.flags & wire::STATE_ADS) != 0) ++_adsCompared;
			if (((p.flags & wire::STATE_ADS) != 0) != slot.ads.active) { ++_flagDiff; ++_adsDiff; bad = true; LogFlagDiff("ads", p, slot, (p.flags & wire::STATE_ADS) != 0, slot.ads.active); }
			// 앉음 — 이동 상태라 단계와 상관없이 본다. 켜진 줄 수를 같이 센다.
			// 앉기 키의 결과는 땅에 있나(앉기) · 머리 위가 비었나(일어서기)에 달린 이동의 결과다 — 사람끼리 막기를 켠 판은 떨어짐과 같은 까닭(F-2)으로
			// 남 캡슐에 따라 갈릴 수 있어 따로 센다(W-5-d 막기 켠 판 150판 중 8판 · 갈림을 적은 4판의 4줄 모두 서버 0 · 예측 1, 3줄은 서버가 떨어지는 중). 막기를 끈 판은 flagDiff 에 든다
			if ((p.flags & wire::STATE_CROUCHED) != 0) ++_crouchCompared;
			if (((p.flags & wire::STATE_CROUCHED) != 0) != slot.move.crouched)
			{
				++_crouchDiff;
				bad = true;
				if (!_rules.pawnBlock) ++_flagDiff;
				LogFlagDiff("crouched", p, slot, (p.flags & wire::STATE_CROUCHED) != 0, slot.move.crouched);
			}
			// 가방(W-3) — 줍기 · 부활만 바꾸고 둘 다 경기 끝 단계와 상관없는 길이라 단계와 상관없이 본다. 무기 둘 이상 든 줄 수를 같이 센다(가방 비교가 실제로 돌았나)
			if (CLoadout::UnpackSlot(p.quickSlots, 1) != kNoWeapon) ++_invMulti;
			if (p.quickSlots != slot.quickSlots) { ++_invDiff; bad = true; }
			if (bad && _firstBadSeq == 0) _firstBadSeq = p.lastSeq;
		}

		// Lyra 다리의 틱 시계와 같은 식(MOBridgeSubsystem.cpp HandlePacket) — 받은 상태로 맞추고 30틱 넘게 벌어지면 다시 잡는다
		void UpdateViewClock(uint32_t stateTick)
		{
			const double sample = (double)stateTick - std::chrono::duration<double>(Clock::now().time_since_epoch()).count() * wire::kTickRate;
			if (!_hasViewClock || std::abs(sample - _viewClockOffset) > 30.0)
			{
				_viewClockOffset = sample;
				_hasViewClock = true;
			}
			else
			{
				_viewClockOffset += 0.02 * (sample - _viewClockOffset);
			}
		}

		// Lyra 다리의 상대 보간과 같은 식(MOBridgeSubsystem.cpp TickRemotes) — 측정 봇의 화면에 해당한다
		void BuildShownPlayers(double renderTick)
		{
			_shownPlayers = _players;
			for (const auto& entry : _viewHistory)
			{
				const ViewHistory& history = entry.second;
				const ViewHistory::Snap* from = nullptr;
				const ViewHistory::Snap* to = nullptr;
				const uint32_t available = std::min<uint32_t>(history.count, ViewHistory::kSnaps);
				for (uint32_t back = 0; back < available; ++back)
				{
					const ViewHistory::Snap& s = history.snaps[(history.count - 1 - back) % ViewHistory::kSnaps];
					if ((double)s.tick <= renderTick)
					{
						from = &s;
						break;
					}
					to = &s;
				}
				const ViewHistory::Snap* base = from ? from : to;
				if (!base) continue;
				wire::PlayerState& shown = _shownPlayers[entry.first];
				shown.x = (float)base->at.x;
				shown.y = (float)base->at.y;
				shown.z = (float)base->at.z;
				shown.flags = base->flags;
				if (from && to && Length(to->at - from->at) <= wire::kRemoteTeleportDistance)
				{
					const double span = std::max((double)to->tick - (double)from->tick, 1.0);
					const double alpha = std::min(std::max((renderTick - (double)from->tick) / span, 0.0), 1.0);
					const Vec3 at = from->at + (to->at - from->at) * alpha;
					shown.x = (float)at.x;
					shown.y = (float)at.y;
					shown.z = (float)at.z;
				}
			}
		}

		// 머리나 몸통 둘레의 무작위 점을 겨눈다. 가운데만 쏘면 1틱 어긋나도 맞아서 틱 번호 버그를 못 본다.
		// 71cm = 서 있는 Quinn 의 머리 중심 높이(game_vm3_server.txt 의 REWINDP bone=head, 70.6~71.3).
		// 16cm = 머리 반지름 10.6 + 탄 반지름 5.5. 40cm = 몸통 · 팔 폭. 몸통 높이 10cm 는 캡슐 중심 근처(제 판단)
		Vec3 JudgeAimOffset()
		{
			if (--_judgeAimHold <= 0)
			{
				_judgeAimHold = 8 + (int)(_judgeRng.NextFloat() * 16.f);
				const bool head = _judgeRng.NextFloat() < 0.5f;
				const double radius = head ? 16.0 : 40.0;
				Vec3 p;
				do
				{
					p = Vec3(_judgeRng.NextFloat() * 2.f - 1.f, _judgeRng.NextFloat() * 2.f - 1.f, _judgeRng.NextFloat() * 2.f - 1.f);
				} while (Length(p) > 1.0);
				_judgeAimOffset = p * radius + Vec3(0, 0, head ? 71.0 : 10.0);
			}
			return _judgeAimOffset;
		}

		// 화면 판정 — 서버 ResolveShot 과 같은 식(GetRewoundBody · GetPoseAt)으로 받은 상태에서 몸을 만들고 ResolveBullet 을 그대로 쓴다
		void JudgeShot(const wire::Input& input, const WeaponEvents& fired)
		{
			if (input.viewTick == 0)
			{
				++_judgeNoView;
				return;
			}
			JudgeShared& js = *_judge;
			++_judgeShots;
			_judgeBodies.clear();
			_judgeSpeeds.clear();
			_judgeTargetWeapons.clear();
			_judgeMontages.clear();
			_judgeAds.clear();
			_judgeCrouch.clear();
			_judgeReact.clear();
			_judgeDash.clear();
			for (const auto& entry : _viewHistory)
			{
				if (entry.first == _welcome.playerId || _judgeBodies.size() >= _judgePoses.size()) continue;
				const ViewHistory::Snap* from = entry.second.Find(input.viewTick);
				if (!from)
				{
					++_judgeNoSnap;
					continue;
				}
				if (from->flags & wire::STATE_DEAD) continue;
				if (!from->hasAnim || from->weapon >= (uint8_t)EWeapon::Count)
				{
					++_judgeNoAnim;
					continue;
				}
				BodyPlacement body;
				body.center = from->at;
				body.yawDegrees = from->yaw;
				body.id = entry.first;
				const ViewHistory::Snap* to = input.viewAlpha > 0.f ? entry.second.Find(input.viewTick + 1) : nullptr;
				float poseAlpha = 0.f;
				if (to && to->hasAnim && to->weapon < (uint8_t)EWeapon::Count && Length(to->at - from->at) <= wire::kRemoteTeleportDistance)
				{
					body.center = from->at + (to->at - from->at) * (double)input.viewAlpha;
					body.yawDegrees = from->yaw + input.viewAlpha * NormalizeAxis(to->yaw - from->yaw);
					poseAlpha = input.viewAlpha;
				}

				// 서버 GetPoseAt 과 같은 차례 — 틱마다 그 틱 묶음으로 풀고, 섞기 · 부위는 앞 틱 평가기로(뼈대가 같다)
				CAnimPoseEvaluator& evaluator = js.evaluators[from->weapon];
				AnimPoseInput pose;
				const bool fromCrouched = (from->flags & wire::STATE_CROUCHED) != 0;
				js.codecs[from->weapon].FromWire(from->anim, from->pitch, fromCrouched, pose);		// 앉음은 사람 상태 칸에서(D62 8)
				evaluator.EvaluateLocal(pose, js.localFrom.data());
				const BoneTransform* local = js.localFrom.data();
				float targetAds = pose.adsBlend;
				float targetCrouch = pose.crouchBlend;
				double meshLift = fromCrouched ? js.crouchLift : 0.0;		// 서버 GetPoseAt 과 같은 섞기(D64 2)
				if (poseAlpha > 0.f)
				{
					AnimPoseInput nextPose;
					const bool toCrouched = (to->flags & wire::STATE_CROUCHED) != 0;
					js.codecs[to->weapon].FromWire(to->anim, to->pitch, toCrouched, nextPose);
					targetAds = std::max(targetAds, nextPose.adsBlend);
					targetCrouch = std::max(targetCrouch, nextPose.crouchBlend);
					meshLift += ((toCrouched ? js.crouchLift : 0.0) - meshLift) * (double)poseAlpha;
					js.evaluators[to->weapon].EvaluateLocal(nextPose, js.localTo.data());
					evaluator.BlendLocal(js.localFrom.data(), js.localTo.data(), poseAlpha, js.localBlend.data());
					local = js.localBlend.data();
				}
				std::vector<BodyPose>& out = _judgePoses[_judgeBodies.size()];
				evaluator.BodiesFromLocal(local, out.data(), meshLift);
				++js.bodiesByWeapon[from->weapon];
				body.pose = out.data();
				body.shapes = js.clips.Bodies().data();
				body.shapeCount = (uint32_t)js.clips.Bodies().size();
				_judgeBodies.push_back(body);
				_judgeSpeeds.push_back(from->speed);
				// 9-6 「상대가 몽타주 중」 — 서버 REWIND 줄 montages= 와 같은 정의(그 틱 칸 수, 다음 틱과 섞으면 둘 중 큰 쪽)
				uint8_t montages = from->anim.montageCount;
				if (poseAlpha > 0.f) montages = std::max(montages, to->anim.montageCount);
				_judgeTargetWeapons.push_back(from->weapon);
				_judgeMontages.push_back(montages);
				_judgeAds.push_back(targetAds);
				_judgeCrouch.push_back(targetCrouch);
				// E-3 「상대가 반응 중」 — 서버 REWIND 줄 react= 와 같은 정의(Shared HasHitReactMontage, 다음 틱과 섞으면 둘 중 하나라도)
				_judgeReact.push_back(HasHitReactMontage(from->anim) || (poseAlpha > 0.f && HasHitReactMontage(to->anim)) ? 1 : 0);
				// D-6 「상대가 대시 중」 — 같은 꼴(Shared HasDashMontage, 다음 틱과 섞으면 둘 중 하나라도)
				_judgeDash.push_back(HasDashMontage(from->anim) || (poseAlpha > 0.f && HasDashMontage(to->anim)) ? 1 : 0);
			}

			const uint8_t weapon = _self.GetWeaponIndex();
			const WeaponDef& def = GetWeaponDef((EWeapon)weapon);
			const Vec3 start((double)input.aimX, (double)input.aimY, (double)input.aimZ);
			const Vec3 aim = AimDirection(input.yaw, input.pitch);
			const int count = std::min(def.bulletsPerCartridge, (int32_t)wire::kMaxBulletsInShot);
			for (int i = 0; i < count; ++i)
			{
				CSpreadRng bulletRng(SpreadSeed(input.seq, (uint32_t)i));
				Vec3 dir = SpreadDirection(aim, fired.spreadAngle, fired.spreadMultiplier, def.spreadExponent, bulletRng);
				if (js.noSpread) dir = aim;
				BulletResult result;
				ResolveBullet(_map, _judgeBodies.data(), _judgeBodies.size(), start, start + dir * (double)def.maxDamageRange, def.sweepRadius, result);
				++_judgeBullets;
				if (!js.log) continue;
				const bool hitBody = result.kind == EBulletHit::Body;
				// targetWeapon · montages = 맞힌 상대의 자세 묶음 · 몽타주 칸 수(못 맞히면 0). check_bot_hits.py 가 무기별 · 몽타주 중을 따로 센다
				// targetAds = 맞힌 상대 자세의 조준 묶음 비중(C-5, 못 맞히면 0) — 조준 중인 상대를 맞힌 탄을 따로 센다. targetCrouch = 같은 식의 앉기 묶음 비중(D-5)
				// targetDash = 맞힌 상대 자세에 대시 몽타주가 들었나(컨텐츠 D-6, 못 맞히면 0)
				std::fprintf(js.log, "BOTSHOT seq=%u shooter=%u i=%d weapon=%u kind=%d target=%u weak=%d viewTick=%u alpha=%.4f speed=%.1f targetWeapon=%u montages=%u targetAds=%.3f targetCrouch=%.3f targetReact=%u targetDash=%u\n", input.seq, _welcome.playerId, i, (unsigned)weapon, (int)result.kind,
					hitBody ? _judgeBodies[result.bodyIndex].id : 0u, hitBody && result.weakSpot ? 1 : 0, input.viewTick, input.viewAlpha, hitBody ? _judgeSpeeds[result.bodyIndex] : 0.f,
					hitBody ? (unsigned)_judgeTargetWeapons[result.bodyIndex] : 0u, hitBody ? (unsigned)_judgeMontages[result.bodyIndex] : 0u, hitBody ? _judgeAds[result.bodyIndex] : 0.f, hitBody ? _judgeCrouch[result.bodyIndex] : 0.f, hitBody ? (unsigned)_judgeReact[result.bodyIndex] : 0u,
					hitBody ? (unsigned)_judgeDash[result.bodyIndex] : 0u);
			}
		}

		// 근접 화면 판정(E-3) — 서버 CRoom::TryMelee 와 같은 식. 손 = 걸음 앞 자리 · 앞 입력의 요 · 기본 자세 손(봇은 자기 애니 자세를 안 돌린다 —
		// 서버도 클립 없이 뜨면 같은 손), 상대 = 시점을 보내면 그 시점의 보간 자리(서버 GetRewoundBody 와 같은 식), 아니면 마지막 받은 상태.
		// 몸 = 캡슐 + 서 있는 메시 몸체(자세 없음 — 서버가 클립 없이 뜬 판과 같다). 앉기 키와 같은 입력은 Think 가 근접을 안 누른다(서버는 앉기 뒤 판정)
		MeleeGuess JudgeMelee(const wire::Input& input, Vec3& push)
		{
			++_meleeCounts.judged;
			const MoveState& move = _self.GetMove();
			double local[3];
			MeleeRestHandLocal(move.crouched, _crouchLift, local);
			const Vec3 hand = MeleeHandWorld(move.location, _meleeYaw, local);
			const Vec3 forward = AimDirection(_meleeYaw, 0.f);
			const bool useView = _measureView && input.viewTick != 0;
			_meleeTargets.clear();
			for (const auto& entry : _players)
			{
				if (entry.first == _welcome.playerId) continue;
				const wire::PlayerState& p = entry.second;
				if (p.flags & wire::STATE_DEAD) continue;		// 서버 — 지금 죽어 있으면 그때 살아 있었어도 안 맞는다
				MeleeTarget target;
				target.body.id = entry.first;
				target.body.center = Vec3(p.x, p.y, p.z);
				target.body.yawDegrees = p.yaw;
				bool crouched = (p.flags & wire::STATE_CROUCHED) != 0;
				if (useView)
				{
					const auto history = _viewHistory.find(entry.first);
					const ViewHistory::Snap* from = history != _viewHistory.end() ? history->second.Find(input.viewTick) : nullptr;
					if (!from || (from->flags & wire::STATE_DEAD)) continue;
					target.body.center = from->at;
					target.body.yawDegrees = from->yaw;
					crouched = (from->flags & wire::STATE_CROUCHED) != 0;		// 캡슐 높이 = 앞 틱 앉음(서버와 같다)
					const ViewHistory::Snap* to = input.viewAlpha > 0.f ? history->second.Find(input.viewTick + 1) : nullptr;
					if (to && Length(to->at - from->at) <= wire::kRemoteTeleportDistance)
					{
						target.body.center = from->at + (to->at - from->at) * (double)input.viewAlpha;
						target.body.yawDegrees = from->yaw + input.viewAlpha * NormalizeAxis(to->yaw - from->yaw);
					}
				}
				target.capsuleRadius = _capsuleRadius;
				target.capsuleHalfHeight = crouched ? _capsuleHalfHeight - _crouchLift : _capsuleHalfHeight;
				target.sameTeam = p.team == _welcome.team;
				_meleeTargets.push_back(target);
			}
			MeleeResult result;
			ResolveMelee(_map, _meleeTargets.data(), _meleeTargets.size(), hand, forward, result);
			MeleeGuess guess;
			guess.kind = (uint8_t)result.kind;		// EMeleeHit 차례 = wire::EMeleeResult 차례(Room.cpp static_assert)
			if (result.kind != EMeleeHit::None) guess.target = _meleeTargets[result.targetIndex].body.id;
			++_meleeCounts.screen[guess.kind];
			if (result.kind == EMeleeHit::Hit)
			{
				// 서버와 같이 와이어 float 로 줄인 힘 — 같으면 서버 결과가 와도 다시 돌릴 일이 없다
				const Vec3 exact = MeleePushForce(forward, result.distance);
				const double scale = _breakMelee ? 0.5 : 1.0;
				push = Vec3((float)(exact.x * scale), (float)(exact.y * scale), 0.0);
				guess.force = push;
			}
			return guess;
		}

		// 촬영 시작 자리로 옮긴다. 서버는 자리 옮기기도 입력과 같은 번호 줄에서 받으므로 입력 번호 하나를 받아 쓴다
		void PlaceForScript()
		{
			if (!_welcome.debugAllowed)
			{
				std::printf("SCRIPT 실패 — 서버가 --debug 없이 떴다\n");
				_net.Close();
				return;
			}
			wire::Input reserved = wire::Input();
			_self.AddInput(reserved);
			wire::DebugPlace place = wire::DebugPlace();
			place.seq = reserved.seq;
			place.x = kScriptHome.x;
			place.y = kScriptHome.y;
			place.z = kScriptHome.z;
			place.hp = 100.f;
			if (!_net.SendPacket(place, wire::Msg::C2S_DEBUG_PLACE))
			{
				_net.Close();
				return;
			}
			// 요 = 마지막으로 보낸 입력의 요(서버 대시의 앞 요). 서버 ApplyPlace 는 대시를 안 지운다 — 대본 첫 입력에 대시를 누르면 앞 이동 · 쿨다운이 어긋날 수 있다(대본에 대시가 들 때 D-3-b 에서 본다)
			_self.OnSpawn(kScriptHome.x, kScriptHome.y, kScriptHome.z, place.seq, _meleeYaw);
			_scriptPlaced = true;
			_scriptTick = 0;
			_scriptStep = -1;
			_scriptCurWeapon = _scriptWeapon;
			_scriptFireTapLeft = 0;
			_scriptFireHeld = false;
			_scriptAdsHeld = false;
			_scriptPitch = 0.f;
			std::printf("SCRIPT placed seq=%u\n", place.seq);
		}

		// 정해진 경로 — 틱을 세어 조각을 고른다. 자리에 선 뒤 몇 초는 고른 무기(기본 소총)를 들고 서 있기만 한다.
		// 사건 · 시선은 Lyra 기록의 프레임보다 한 틱 늦게 넣는다 — Lyra 는 사건 프레임의 갱신 뒤에 몽타주를 틀고 다음 갱신부터 무기 층을 바꾸며(대조 도구 AnimCheck 가 그 차례로 튼다, D46),
		// 애니가 읽는 시선 각도 한 프레임 앞 조종 각이다(animcmp_ev_rifle_probe.csv AimPitch, 실측). 서버는 입력 틱의 애니 갱신 앞에 사건을 튼다(Room.cpp PlayAnimEvents)
		// 앉기 사건만 예외 — 이동과 같은 차례라 기록 프레임 그 틱에 누른다(ScriptEventLag)
		void ThinkScript(wire::Input& input)
		{
			input.yaw = kScriptYaw;
			input.pitch = _scriptPitch;
			const uint32_t tick = _scriptTick++;
			float nextPitch = _scriptPitch;
			if (tick == _scriptHoldTicks) _scriptStartPending = true;
			if (tick >= _scriptHoldTicks)
			{
				uint32_t at = tick - _scriptHoldTicks;
				int step = 0;
				const ScriptStep* item = nullptr;
				for (size_t i = 0; i < _scriptStepCount; ++i)
				{
					if (at < _scriptSteps[i].ticks)
					{
						item = &_scriptSteps[i];
						break;
					}
					at -= _scriptSteps[i].ticks;
					++step;
				}
				if (item)
				{
					if (step != _scriptStep)
					{
						_scriptStep = step;
						const auto server = _players.find(_welcome.playerId);
						std::printf("SCRIPT step=%s tick=%u server=%.2f,%.2f,%.2f\n", item->name, tick,
							server != _players.end() ? server->second.x : 0.f, server != _players.end() ? server->second.y : 0.f, server != _players.end() ? server->second.z : 0.f);
					}
					if (item->moving)
					{
						const double radians = ((double)kScriptYaw + item->angle) * 3.14159265358979323846 / 180.0;
						input.moveX = (float)std::cos(radians);
						input.moveY = (float)std::sin(radians);
					}
					if (item->jump && at < kScriptJumpTicks) input.buttons |= wire::BUTTON_JUMP;
					if (item->ev0 != EScriptEvent::None && at == item->at0 + ScriptEventLag(item->ev0)) DoScriptEvent(item->ev0, input);
					if (item->ev1 != EScriptEvent::None && at == item->at1 + ScriptEventLag(item->ev1)) DoScriptEvent(item->ev1, input);
					const float alpha = item->seconds > 0.f ? std::min(std::max((float)at / (float)wire::kTickRate / item->seconds, 0.f), 1.f) : 1.f;
					nextPitch = item->pitchFrom + (item->pitchTo - item->pitchFrom) * alpha;	// FMath::Lerp
				}
				else if (_scriptStep != step)
				{
					_scriptStep = step;
					std::printf("SCRIPT end tick=%u\n", tick);
				}
			}
			if (_scriptFireTapLeft > 0)
			{
				input.buttons |= wire::BUTTON_FIRE;
				--_scriptFireTapLeft;
			}
			if (_scriptFireHeld) input.buttons |= wire::BUTTON_FIRE;
			if (_scriptAdsHeld) input.buttons |= wire::BUTTON_ADS;
			input.weapon = _scriptCurWeapon;
			_scriptPitch = nextPitch;
		}

		void DoScriptEvent(EScriptEvent ev, wire::Input& input)
		{
			switch (ev)
			{
			case EScriptEvent::Fire: _scriptFireTapLeft = kScriptFireTapTicks; break;
			case EScriptEvent::Hold: _scriptFireHeld = true; break;
			case EScriptEvent::Release: _scriptFireHeld = false; break;
			case EScriptEvent::Reload: input.buttons |= wire::BUTTON_RELOAD; break;
			case EScriptEvent::Swap: _scriptCurWeapon = _scriptWeapon == (uint8_t)EWeapon::Pistol ? (uint8_t)EWeapon::Rifle : (uint8_t)EWeapon::Pistol; break;
			case EScriptEvent::Back: _scriptCurWeapon = _scriptWeapon; break;
			case EScriptEvent::CrouchOn:
			case EScriptEvent::CrouchOff:
				if (_self.GetMove().crouched != (ev == EScriptEvent::CrouchOn)) input.buttons |= wire::BUTTON_CROUCH;
				break;
			case EScriptEvent::AdsOn: _scriptAdsHeld = true; break;
			case EScriptEvent::AdsOff: _scriptAdsHeld = false; break;
			case EScriptEvent::None: break;
			}
			std::printf("SCRIPT event=%d tick=%u weapon=%u\n", (int)ev, _scriptTick - 1, (unsigned)_scriptCurWeapon);
		}

		// 두뇌 — 가장 가까운 산 적을 보고 쏘며 다가간다. 길찾기가 없어 막히면 무작위로 돌아간다
		void Think(wire::Input& input)
		{
			const Vec3 at = _self.GetMove().location;
			const Vec3 eye = at + Vec3(0, 0, _self.GetMove().crouched ? kEyeHeight - kEyeCrouchDrop : kEyeHeight);

			if (_measureView && _hasViewClock)
			{
				double renderTick = std::chrono::duration<double>(Clock::now().time_since_epoch()).count() * wire::kTickRate + _viewClockOffset - kViewDelayTicks;
				if (_judge && renderTick > (double)_newestStateTick)
				{
					// Lyra 다리의 bViewClamp 와 같은 식 — 상태가 늦으면 화면은 마지막 받은 자리에 서 있고, 보고하는 시점도 거기까지다
					renderTick = (double)_newestStateTick;
					++_judgeClamped;
				}
				BuildShownPlayers(renderTick);
				const double whole = std::floor(renderTick);
				input.viewTick = renderTick >= 1.0 ? (uint32_t)whole : 0;
				input.viewAlpha = renderTick >= 1.0 ? (float)(renderTick - whole) : 0.f;
			}
			const std::map<uint32_t, wire::PlayerState>& seen = _measureView ? _shownPlayers : _players;

			const wire::PlayerState* enemy = nullptr;
			double nearest = 0;
			for (const auto& entry : seen)
			{
				const wire::PlayerState& p = entry.second;
				if (p.team == _welcome.team || (p.flags & wire::STATE_DEAD)) continue;
				const double d = Length(Vec3(p.x, p.y, p.z) - at);
				if (!enemy || d < nearest)
				{
					enemy = &p;
					nearest = d;
				}
			}

			float yaw = _lastYaw, pitch = 0;
			Vec3 toward(1, 0, 0);
			bool canShoot = false;
			if (enemy)
			{
				const Vec3 target(enemy->x, enemy->y, enemy->z);
				const Vec3 d = (_judge ? target + JudgeAimOffset() : target) - eye;
				const double error = _measureView ? 0.0 : 2.0;	// 도 — 사람 흉내가 아니라 전부 머리에 맞지 않게. 측정은 본 자리를 그대로 조준해 빗나감을 무기 퍼짐으로만 남긴다
				yaw = (float)(std::atan2(d.y, d.x) * 180.0 / kPi + (_rng.NextFloat() - 0.5f) * error);
				pitch = (float)(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 180.0 / kPi + (_rng.NextFloat() - 0.5f) * error);
				const double horizontal = std::sqrt(d.x * d.x + d.y * d.y);
				if (horizontal > 1.0) toward = Vec3(d.x / horizontal, d.y / horizontal, 0);

				if (--_sightWait <= 0)
				{
					_sightWait = 4;		// 광선은 네 틱에 한 번이면 충분하다
					RayHit ray;
					_hasSight = nearest < kFireRange && !_map.Raycast(eye, target, MAP_BLOCK_WEAPON, ray);
				}
				canShoot = _hasSight;
			}
			_lastYaw = yaw;

			// 받침 갈래(컨텐츠 B-4) — 다쳤으면 이동만 같은 층에서 가장 가까운 켜진 받침 쪽. 겨눔 · 사격은 그대로, 옆걸음은 안 한다
			const auto selfState = _players.find(_welcome.playerId);
			const bool hurt = selfState != _players.end() && !(selfState->second.flags & wire::STATE_DEAD) && selfState->second.hp <= kPadSeekHp;
			const Pad* padTarget = hurt ? NearestOpenPad(at) : nullptr;
			if (padTarget)
			{
				const double dx = padTarget->at.x - at.x, dy = padTarget->at.y - at.y;
				const double horizontal = std::sqrt(dx * dx + dy * dy);
				if (horizontal > 1.0) toward = Vec3(dx / horizontal, dy / horizontal, 0);
				if (!_padSeeking) ++_padSeekStarts;
				++_padSeekTicks;
			}
			_padSeeking = padTarget != nullptr;
			// 무기 받침 갈래(W-5) — 체력 받침을 찾아가지 않을 때만(체력이 낮으면 체력 받침이 먼저). 이동만 바꾸는 것은 체력 갈래와 같다
			const bool alive = selfState != _players.end() && !(selfState->second.flags & wire::STATE_DEAD);
			const Pad* weaponTarget = !padTarget && alive ? NearestWantedWeaponPad(at, selfState->second) : nullptr;
			if (alive && selfState->second.spare <= 0) ++_spareOutTicks;	// 든 무기 예비가 바닥난 틱 — 예비 찾아가기가 0 일 때 「안 바닥남」 · 「같은 층에 받침 없음」을 가른다
			if (weaponTarget)
			{
				const double dx = weaponTarget->at.x - at.x, dy = weaponTarget->at.y - at.y;
				const double horizontal = std::sqrt(dx * dx + dy * dy);
				if (horizontal > 1.0) toward = Vec3(dx / horizontal, dy / horizontal, 0);
				if (!_weaponSeeking)
				{
					++_weaponSeekStarts;
					if (PadWeapon(weaponTarget->kind) == selfState->second.weapon) ++_weaponSeekAmmoStarts;	// 든 무기 받침 = 예비가 바닥나 찾아감(나머지는 안 가진 무기)
				}
				++_weaponSeekTicks;
			}
			_weaponSeeking = weaponTarget != nullptr;
			// --push(F-2) — 팀 상관없이 가장 가까운 산 사람 쪽으로 곧장 걷는다. 부딪힘 · 미끄러짐이 계속 일어나야 보정 양을 잴 수 있다
			if (_push)
			{
				const wire::PlayerState* bodyAhead = nullptr;
				double bodyNearest = 0;
				for (const auto& entry : _players)
				{
					const wire::PlayerState& p = entry.second;
					if (p.id == _welcome.playerId || (p.flags & wire::STATE_DEAD) || _playerTick[p.id] != _newestStateTick) continue;
					const double d = Length(Vec3(p.x, p.y, p.z) - at);
					if (!bodyAhead || d < bodyNearest)
					{
						bodyAhead = &p;
						bodyNearest = d;
					}
				}
				if (bodyAhead)
				{
					const double dx = bodyAhead->x - at.x, dy = bodyAhead->y - at.y;
					const double horizontal = std::sqrt(dx * dx + dy * dy);
					if (horizontal > 1.0) toward = Vec3(dx / horizontal, dy / horizontal, 0);
				}
			}
			// --melee 는 옆걸음 없이 곧장 다가간다 — 근접 거리까지 붙어야 근접 갈래가 경기 중에 돈다(옆걸음이면 거의 경기 끝 단계에서만 붙었다)
			const bool strafe = !_push && canShoot && nearest < kStrafeRange && !_padSeeking && !_weaponSeeking && !_meleeNear;

			// 막힘 — 30틱에 30cm 도 못 가면 돌아간다(--push 는 막히는 것이 목적이라 안 돌아간다)
			if (++_stuckTicks >= 30)
			{
				const Vec3 moved = at - _stuckCheckAt;
				if (!_push && !(_self.IsLocked()) && moved.x * moved.x + moved.y * moved.y < 30.0 * 30.0 && _detourLeft <= 0 && !strafe)
				{
					const double angle = _rng.NextFloat() * 2.0 * kPi;
					_detourDir = Vec3(std::cos(angle), std::sin(angle), 0);
					_detourLeft = 60;
					++_detours;
				}
				_stuckCheckAt = at;
				_stuckTicks = 0;
			}

			Vec3 move = toward;
			bool jump = false;
			if (_detourLeft > 0)
			{
				--_detourLeft;
				move = _detourDir;
				jump = _detourLeft > 50;
			}
			else if (strafe)
			{
				if (--_strafeLeft <= 0)
				{
					_strafeLeft = 45 + (int)(_rng.NextFloat() * 60.f);
					_strafeSign = -_strafeSign;
				}
				move = Vec3(-toward.y * _strafeSign, toward.x * _strafeSign, 0);
			}

			input.moveX = (float)move.x;
			input.moveY = (float)move.y;
			input.yaw = yaw;
			input.pitch = pitch;
			input.aimX = (float)eye.x;
			input.aimY = (float)eye.y;
			input.aimZ = (float)eye.z;
			input.camBack = (float)kGrenadeCamBackStanding;	// 카메라 거리(G-3) — G-0 대표값(서서 피치 0). 서버는 던진 입력에만 쓴다
			input.weapon = _chosenWeapon;
			if (jump) input.buttons |= wire::BUTTON_JUMP;

			// 탄을 다 쓰면 다른 무기로
			const CWeaponCycle& cycle = _self.GetWeapon();
			if (cycle.GetMagazine() == 0 && cycle.GetSpare() == 0)
			{
				_chosenWeapon = NextOwnedWeapon(_chosenWeapon);		// 가진 무기 중 다음(W-3)
				input.weapon = _chosenWeapon;
			}
			else if (cycle.GetMagazine() == 0 && !cycle.IsReloading() && !_self.IsWaitingSpawn())
			{
				input.buttons |= wire::BUTTON_RELOAD;
			}
			// 죽어 있어도 방아쇠를 놓지 않는다 — 서버가 거절하는지 보려고. 대기 중에도 쏜다 — 피해 0 인지 보려고
			if (!_push && (canShoot || _self.IsWaitingSpawn() || _fireAlways)) input.buttons |= wire::BUTTON_FIRE;	// --push 는 안 쏜다(아무도 안 죽어야 계속 부딪힌다)
			// --ads — 적이 보이면 겨눈다(사람이 쏠 때 누르는 꼴). 시선은 네 틱마다 다시 보므로 켜고 끄는 일이 잦아 덜 섞인 채 풀기까지 지난다. 난수는 안 쓴다 — 끈 판과 경로가 같다
			if (_adsWhenShooting && canShoot)
			{
				input.buttons |= wire::BUTTON_ADS;
				++_adsInputs;
			}
			// --crouch — 정해 둔 간격마다 앉기 키를 한 번 누른다(누른 입력 하나에만). 간격은 따로 쓰는 난수라 두뇌 난수(_rng)를 안 건드린다.
			// 0.2~2.5초 = D-1 Lyra 기록기의 앉기 누름 간격과 같다. 부활을 기다리는 동안은 세지 않는다(서버가 그 입력을 안 쓴다)
			if (_crouchToggle && !_self.IsWaitingSpawn() && --_crouchWait <= 0)
			{
				_crouchWait = 12 + (int)(_crouchRng.NextFloat() * 138.f);
				input.buttons |= wire::BUTTON_CROUCH;
				++_crouchPresses;
			}
			// --melee — 적이 4m 안에 보이면 근접 단추를 누른다(누르고 있는 꼴 — 켜지는 것은 능력 간격마다). 앉기 키와 같은 입력엔 안 누른다:
			// 서버는 앉기를 근접보다 먼저 처리해 손 높이가 바뀌는데, 화면 판정은 넣기 전 상태로 한다
			if (_meleeNear && enemy && _hasSight && nearest < kMeleeRange && !_self.IsWaitingSpawn() && (input.buttons & wire::BUTTON_CROUCH) == 0)
			{
				input.buttons |= wire::BUTTON_MELEE;
				++_meleeCounts.presses;
			}
			// --dash(D-3) — 정해 둔 간격마다 대시 단추를 한 번 누른다(누른 입력 하나에만). 간격 · 방향 · 같이 누를 단추는 따로 쓰는 난수(_dashRng)라 두뇌 난수(_rng)를 안 건드린다.
			// 간격 12~150 입력 = --crouch 와 같다 — 켜짐 32 · 쿨다운 91 입력보다 짧은 것 · 긴 것이 섞여 켬 · 켜져 있음 · 쿨다운 갈래가 다 돈다.
			// 방향은 능력이 앞 입력의 이동 · 몸 요로 고르므로(SelectDashDir) 누르기 한 입력 앞에서 이동을 그 입력 요 기준 0 · 180 · −90 · +90° 로 놓는다(갈래마다 한가운데).
			// 위 · 아래 보고 앞은 누른 입력의 피치를 +40 · −30 으로(앞 대시 힘은 카메라 앞이다. D-0 기록은 피치 +20 · +45 · 아래). 부활 기다리는 중 · 경기 끝 단계엔 안 누른다 —
			// 서버는 죽은 사람 입력을 대시 앞에서 버리고(dead 로만 센다), 경기 끝은 막는데 예측은 경기 단계를 모른다(다리는 Lyra 입력 막음을 따른다 — D-5)
			if (_dashPress && !_self.IsWaitingSpawn() && _phase != (uint8_t)wire::EMatchPhase::PostGame)
			{
				--_dashWait;
				if (_dashWait == 1)
				{
					_dashKind = std::min(5, (int)(_dashRng.NextFloat() * 6.f));
					static const double kDashKindYaw[6] = { 0.0, 180.0, -90.0, 90.0, 0.0, 0.0 };
					const double angle = ((double)input.yaw + kDashKindYaw[_dashKind]) * kPi / 180.0;
					input.moveX = (float)std::cos(angle);
					input.moveY = (float)std::sin(angle);
				}
				else if (_dashWait <= 0)
				{
					input.buttons |= wire::BUTTON_DASH;
					if (_dashKind == 4) input.pitch = 40.f;
					else if (_dashKind == 5) input.pitch = -30.f;
					++_dashCounts.presses;
					++_dashCounts.kinds[_dashKind];
					_dashWait = 12 + (int)(_dashRng.NextFloat() * 138.f);
				}
				else if (_self.IsDashActive() && _dashRng.NextFloat() < 0.125f)
				{
					// 켜져 있는 동안 — 서버 · 예측 둘 다 막아야 하는 누름(점프 · 재장전 · 근접). 끝 입력이면 막히지 않고 켜진다(그것도 같은 코드로 같아야 한다).
					// 근접은 앉기 키와 같은 입력엔 안 누른다(위 --melee 와 같은 까닭 — 화면 판정이 넣기 전 손 높이로 본다)
					const int pick = std::min(2, (int)(_dashRng.NextFloat() * 3.f));
					if (pick == 0) { input.buttons |= wire::BUTTON_JUMP; ++_dashCounts.extraJump; }
					else if (pick == 1) { input.buttons |= wire::BUTTON_RELOAD; ++_dashCounts.extraReload; }
					else if ((input.buttons & wire::BUTTON_CROUCH) == 0) { input.buttons |= wire::BUTTON_MELEE; ++_dashCounts.extraMelee; }
				}
			}
			// --grenade(G-3, 플랜 123줄) — 적이 보이고 kGrenadeBotRange 안이면 간격마다 수류탄 단추를 한 번 누른다(누른 입력 하나에만). 간격 · 피치 종류는
			// 따로 쓰는 난수(_grenadeRng)라 두뇌 난수(_rng)를 안 건드린다. 간격 30~450 입력 = 쿨다운 301 입력보다 짧은 것 · 긴 것이 섞여 던짐 · 쿨다운 갈래가 다 돈다.
			// 피치 = 그대로(적을 봄) · +80(하늘 — 겨눔 선이 안 맞음. Expanse 봇이 누른 자리 33곳을 MapCheck 로 재 보니 55° 는 4곳 · 80° 는 9곳만 안 맞았다 — 지붕 밑이 많다) · −60(발 앞 바닥 — 선 시작에서 45 안) · −20(먼 바닥 — 맞은 곳을 봄). 대시 누른 입력은 피치를 안 바꾼다(대시 힘이 피치를 따른다)
			// 부활 기다리는 중 · 경기 끝 단계엔 적 없이 누른다 — 서버 죽은 갈래(쿨다운만 돎) · 경기 끝 막기를 지나가게(플랜 177줄 「통계 갈래 각각 1 이상」).
			// 경기 끝 몫은 예측이 단계를 몰라 소식 없이 지나가므로 따로 센다(대시 unconfirmedPost 와 같은 길)
			// 하늘 각본 — 싸움이 나는 한가운데는 높이 17m 쯤 무기 · 폭발 선만 막는 한 면 물체(이동 · 수류탄 몸은 지나감) · 지붕 밑이고 부활 자리도 지붕 밑이라, 간격 누름의 하늘 던짐은 거의 맞는다
			// (G-3-c 파일럿 두 판 — 살아 누른 하늘 16번 전부 맞음 · MapCheck 로 잼). 그래서 서버와 같은 겨눔 선(사람 몸 뺌)을 내 맵에 대 보고 열렸고 쿨다운이 비었으면
			// 하늘을 한 번 누른다 — 서버 「안 맞음」 갈래(플랜 177줄 「0 이면 각본을 더한다」). 누른 뒤 kGrenadeSkyGap 입력은 다시 안 본다.
			// 가까운 적 각본 — 간격 누름의 「그대로」는 적이 멀어(25m 안) 몸에 바로 맞는 일이 드물다(두 판 0). kGrenadeNearRange 안에 보이면 몸 가운데를 앞 겨눔으로 보고 한 번 누른다 — 서버 「맞아 터짐」 갈래
			if (_grenadePress)
			{
				const bool waiting = _self.IsWaitingSpawn();
				const bool post = _phase == (uint8_t)wire::EMatchPhase::PostGame;
				const bool ready = !waiting && !post && (input.buttons & wire::BUTTON_DASH) == 0 && !_self.IsDashActive() && _self.GetGrenadeCooldownLeft() == 0;
				const bool nearPress = --_grenadeNearWait <= 0 && ready && enemy && _hasSight && nearest < kGrenadeNearRange;
				const bool skyPress = --_grenadeSkyWait <= 0 && ready && !nearPress && SkyLineOpen(input);
				const bool timed = --_grenadeWait <= 0 && (waiting || post || (enemy && _hasSight && nearest < kGrenadeBotRange));
				if (nearPress || skyPress || timed)
				{
					const int kind = nearPress ? 0 : skyPress ? 1 : std::min(3, (int)(_grenadeRng.NextFloat() * 4.f));
					if (kind != 0 && (input.buttons & wire::BUTTON_DASH) == 0) input.pitch = kGrenadeKindPitch[kind];
					if (nearPress)
					{
						// 옆걸음(걷기 최대 600cm/s, Movement.h)이면 3m 에서도 날아가는 0.12초에 72cm 비켜 캡슐(반지름 35)을 빗나간다(첫 각본 두 판 맞아 터짐 0) — 적 속도로 앞을 겨눈다.
						// 앞 시간 = 수평 거리 ÷ 첫 속력 2500 + 3틱(상태가 늦게 온 몫 · 서버가 입력을 받는 틈, 제 판단)
						const Vec3 target(enemy->x, enemy->y, enemy->z);
						const Vec3 v(enemy->vx, enemy->vy, enemy->vz);
						const Vec3 d0 = target - eye;
						const double lead = std::sqrt(d0.x * d0.x + d0.y * d0.y) / 2500.0 + 3.0 / (double)wire::kTickRate;
						const Vec3 d = target + Vec3(v.x * lead, v.y * lead, 0.0) - eye;
						input.yaw = (float)(std::atan2(d.y, d.x) * 180.0 / kPi);
						input.pitch = (float)(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * 180.0 / kPi);
					}
					input.buttons |= wire::BUTTON_GRENADE;
					GrenadeBotCounts& c = _grenadeCounts;
					++c.presses;
					++c.kinds[kind];
					if (waiting) ++c.pressWaiting;
					if (post) ++c.pressPost;
					if (skyPress)
					{
						++c.pressSky;
						_grenadeSkyWait = kGrenadeSkyGap;
					}
					if (nearPress)
					{
						++c.pressNear;
						_grenadeNearWait = kGrenadeNearGap;
					}
					if (timed) _grenadeWait = 30 + (int)(_grenadeRng.NextFloat() * 420.f);
				}
			}
		}

		// 하늘 각본의 겨눔 선 — 서버 ThrowGrenade 와 같은 식(카메라 = 눈 − 시선 × 카메라 거리 · 선 450~3150 · 무기 채널). 사람 몸은 안 본다
		bool SkyLineOpen(const wire::Input& input) const
		{
			Rotator control;
			control.pitch = kGrenadeKindPitch[1];
			control.yaw = input.yaw;
			const Vec3 aim(input.aimX, input.aimY, input.aimZ);
			const Vec3 camera = aim - AimDirection(input.yaw, kGrenadeKindPitch[1]) * ClampGrenadeCamBack(input.camBack);
			Vec3 start, end;
			GrenadeTraceSegment(camera, control, start, end);
			RayHit ray;
			return !_map.Raycast(start, end, MAP_BLOCK_WEAPON, ray);
		}

		const CMapCollision& _map;
		const MatchRules& _rules;
		CMoveSim& _sim;			// 봇끼리 나눠 쓴다 — 사람끼리 막기의 남 캡슐은 넣기 · 다시 돌리기 동안만 걸고 바로 뗀다
		// 난수 다섯 — 씨앗 하나에서 갈라 심는다(전부터의 값). 기다림 판은 환영 때 자리 번호로 다시 심는다(SetSeedByPlayerId)
		void SeedRngs(uint32_t seed)
		{
			_rng = CSpreadRng(seed);
			_judgeRng = CSpreadRng(seed ^ 0x5A5A1234u);
			_crouchRng = CSpreadRng(seed ^ 0x3C3C5678u);
			_dashRng = CSpreadRng(seed ^ 0x6A6A9ABCu);
			_grenadeRng = CSpreadRng(seed ^ 0x4E4E2B1Du);
		}
		uint32_t _seedByPlayerBase = 0;
		CSpreadRng _rng;
		CNetClient _net;
		wire::Welcome _welcome = wire::Welcome();
		wire::Result _result = wire::Result();
		bool _hasResult = false;
		std::string _record;
		std::map<uint32_t, wire::PlayerState> _players;
		std::map<uint32_t, uint32_t> _spawnTick;
		std::map<uint32_t, uint32_t> _deadSince;
		std::map<uint32_t, int> _killTally;
		std::map<uint32_t, int> _deathTally;
		std::map<uint32_t, int> _assistTally;
		std::map<uint32_t, std::vector<uint32_t>> _damagedBy;	// 맞은 사람 → 0보다 큰 피해를 준 사람(받은 차례). 판 중간에 나간 사람은 못 지운다 — 그 사람이 때렸다면 처치 번호 대조가 다름으로 잡는다
		std::vector<wire::Match> _phaseChanges;
		uint8_t _phase = (uint8_t)wire::EMatchPhase::Waiting;
		uint32_t _playTick = 0;
		uint32_t _postTick = 0;
		RuleChecks _checks;

		CSelfPrediction _self;
		uint8_t _chosenWeapon = 0;
		bool _hasSpawn = false;

		bool _scripted = false;
		bool _scriptPlaced = false;
		bool _scriptArmed = false;
		uint32_t _scriptHoldTicks = 0;
		uint8_t _scriptWeapon = (uint8_t)EWeapon::Rifle;
		uint32_t _scriptTick = 0;
		int _scriptStep = -1;
		const ScriptStep* _scriptSteps = kStartStopScript;
		size_t _scriptStepCount = sizeof(kStartStopScript) / sizeof(kStartStopScript[0]);
		uint8_t _scriptCurWeapon = (uint8_t)EWeapon::Rifle;	// 사건 경로에서 지금 든 무기(Swap · Back)
		uint32_t _scriptFireTapLeft = 0;	// 한 발 사건의 방아쇠가 남은 틱
		bool _scriptFireHeld = false;
		bool _scriptAdsHeld = false;		// 앉기 경로의 조준 누름(D-6)
		float _scriptPitch = 0.f;			// 앞 틱의 조종 시선 각 — 이번 입력에 싣는다(한 틱 늦게)
		bool _scriptStartPending = false;

		float _lastYaw = 0;
		int _sightWait = 0;
		bool _hasSight = false;
		Vec3 _stuckCheckAt;
		int _stuckTicks = 0;
		int _detourLeft = 0;
		Vec3 _detourDir;
		int _strafeLeft = 0;
		int _strafeSign = 1;
		uint64_t _detours = 0;

		// 받침(컨텐츠 B-4) — 서버 받침 번호 차례
		std::vector<Pad> _pads;
		bool _padSeeking = false;
		bool _weaponSeeking = false;	// 무기 받침 찾아가는 중(W-5)
		uint64_t _weaponSeekStarts = 0, _weaponSeekTicks = 0, _weaponTakesSought = 0, _weaponSeekAmmoStarts = 0, _spareOutTicks = 0;
		uint64_t _padMsgs = 0, _padBad = 0, _padSeekStarts = 0, _padSeekTicks = 0, _padHealsMine = 0, _padHealsSought = 0;

		// 측정 모드 — 화면에 해당하는 상태 기록. 6틱 지연 + 왕복 300ms(18틱) + 여유로 32칸
		struct ViewHistory
		{
			static constexpr uint32_t kSnaps = 32;
			struct Snap
			{
				uint32_t tick = 0;
				Vec3 at;
				uint8_t flags = 0;
				float yaw = 0, pitch = 0, speed = 0;
				uint8_t weapon = 0;			// 이 틱 상태를 만든 자세 묶음 — 서버는 무기를 바꾼 틱에 묶음도 바꿔 보낸다
				bool hasAnim = false;
				wire::AnimStateWire anim;
			};
			std::array<Snap, kSnaps> snaps;
			uint32_t count = 0;
			void Add(const wire::PlayerState& p, uint32_t tick, const wire::AnimStateWire* anim)
			{
				Snap& s = snaps[count % kSnaps];
				s.tick = tick;
				s.at = Vec3(p.x, p.y, p.z);
				s.flags = p.flags;
				s.yaw = p.yaw;
				s.pitch = p.pitch;
				s.speed = (float)std::sqrt(p.vx * p.vx + p.vy * p.vy);
				s.weapon = p.weapon;
				s.hasAnim = anim != nullptr;
				if (anim) s.anim = *anim;
				++count;
			}
			const Snap* Find(uint32_t tick) const
			{
				const uint32_t available = std::min<uint32_t>(count, kSnaps);
				for (uint32_t back = 0; back < available; ++back)
				{
					const Snap& s = snaps[(count - 1 - back) % kSnaps];
					if (s.tick == tick) return &s;
					if (s.tick < tick) break;
				}
				return nullptr;
			}
		};
		static constexpr double kViewDelayTicks = 6.0;	// Lyra 다리 kRemoteDelayTicks 와 같은 값(0.1초)
		bool _measureView = false;
		bool _hasViewClock = false;
		double _viewClockOffset = 0;
		std::map<uint32_t, ViewHistory> _viewHistory;
		std::map<uint32_t, wire::PlayerState> _shownPlayers;
		uint32_t _newestStateTick = 0;
		JudgeShared* _judge = nullptr;
		CSpreadRng _judgeRng;
		int _judgeAimHold = 0;
		Vec3 _judgeAimOffset;
		std::vector<BodyPlacement> _judgeBodies;
		std::vector<float> _judgeSpeeds;
		std::vector<uint8_t> _judgeTargetWeapons, _judgeMontages;	// _judgeBodies 와 같은 차례
		std::vector<float> _judgeAds;	// 같은 차례 — 상대 자세의 조준 묶음 비중(C-5, 다음 틱과 섞으면 둘 중 큰 쪽)
		std::vector<float> _judgeCrouch;	// 같은 차례 — 상대 자세의 앉기 묶음 비중(D-5, 같은 식)
		std::vector<uint8_t> _judgeReact;	// 같은 차례 — 상대 자세에 맞은 쪽 반응 몽타주가 들었나(E-3 17-d)
		std::vector<uint8_t> _judgeDash;	// 같은 차례 — 상대 자세에 대시 몽타주가 들었나(컨텐츠 D-6)
		std::vector<std::vector<BodyPose>> _judgePoses;
		uint64_t _judgeShots = 0, _judgeBullets = 0, _judgeNoView = 0, _judgeNoSnap = 0, _judgeNoAnim = 0, _judgeClamped = 0, _recvBytes = 0;
		uint64_t _ackLagSum = 0, _ackLagSamples = 0;
		uint32_t _ackLagMax = 0;

		uint64_t _states = 0;
		Clock::time_point _stateTime;
		bool _hasStateTime = false;
		uint64_t _compared = 0, _posDiff = 0, _velDiff = 0, _ammoDiff = 0, _flagDiff = 0;
		uint64_t _invDiff = 0, _invMulti = 0, _weaponPickupsMine = 0;	// 가방(W-3) — 빠른 칸이 서버와 다른 비교 줄 · 무기 둘 이상 든 비교 줄 · 내가 집은 무기 받침
		uint32_t _lockstepAcked = 0;		// 받은 자기 상태의 lastSeq — 서버가 적용한 마지막 입력 번호(기다림 판)
		Clock::time_point _lockstepWaitSince = Clock::now();
		uint64_t _lockstepTimeouts = 0;
		uint32_t _lastSentSeq = 0;		// 마지막으로 보낸 입력 번호 — 화면 판정 기록 BOTPICKUP 의 줍기 창 끝(컨텐츠 D-6)
		uint64_t _lockDiff = 0, _lockCompared = 0;		// 이동 막음(부활 직후) 깃발이 서버와 다른 줄 — flagDiff 안에도 든다
		uint64_t _adsCompared = 0, _adsDiff = 0, _adsInputs = 0;	// 서버가 조준 켜짐으로 보낸 비교 줄 · 그중 어긋남 · 조준을 누른 입력
		bool _adsWhenShooting = false;		// --ads
		uint64_t _crouchCompared = 0, _crouchDiff = 0, _crouchPresses = 0;	// 서버가 앉음으로 보낸 비교 줄 · 그중 어긋남 · 앉기 키를 누른 입력
		bool _crouchToggle = false;		// --crouch
		bool _breakCrouch = false;		// --breakCrouch
		int _pauseAfterSpawn = 0, _pauseLeft = 0;	// --testPauseAfterSpawn — 부활마다 쉴 틱 수 · 남은 수
		CSpreadRng _crouchRng;
		int _crouchWait = 0;
		// 근접(E-3)
		bool _meleeNear = false;		// --melee
		bool _breakMelee = false;		// --breakMelee
		float _meleeYaw = 0;			// 서버 player.yaw — 마지막으로 보낸 입력의 요(부활 직후는 시작 자리 요). 근접 몸 앞
		double _capsuleRadius = 0, _capsuleHalfHeight = 0, _crouchLift = 0;
		// 사람끼리 막기(F-2) — rules.pawnBlock 이면 예측이 남 캡슐(마지막으로 받은 상태)을 보고, 서버와 어긋나면 Reconcile 로 고친다
		bool _push = false;				// --push — 쏘지 않고 가장 가까운 사람(팀 상관없이) 쪽으로 걷는다. 옆걸음 · 돌아가기 없음
		bool _fireAlways = false;		// --judgeFireAlways — 적이 보이든 말든 방아쇠를 놓지 않는다. 판정 대조 · 부하 판 어느 쪽에서나(D95 한계 측정 = 전원 연사)
		std::vector<PawnCapsule> _pawnScratch;
		std::map<uint32_t, uint32_t> _playerTick;		// 사람마다 마지막으로 상태에 실린 틱 — 나간 사람을 남 캡슐에서 빼려고
		uint64_t _pawnReconciles = 0, _pawnCorrected = 0, _fallDiff = 0;
		double _pawnCorrectSum = 0, _pawnCorrectMax = 0;
		MeleeBotCounts _meleeCounts;
		std::map<uint32_t, MeleeGuess> _meleeGuesses;		// 입력 번호 → 화면 판정. 서버 결과가 오거나 서버가 결과 없이 지나가면 지운다
		std::deque<wire::Melee> _meleePendingDamage;		// 피해 소식이 뒤따라야 할 근접 맞음
		std::vector<MeleeTarget> _meleeTargets;
		// 대시(D-3)
		bool _dashPress = false;		// --dash
		int _breakDash = 0;				// --breakDash — kBreakDashPredict · kBreakDashServer
		CSpreadRng _dashRng;
		int _dashWait = 30;				// 다음 누름까지 남은 입력(첫 누름은 부활 뒤 30입력째 — 처음부터 방향을 고른 누름이 되게 1 을 지나간다)
		int _dashKind = 0;				// 이번 누름의 방향 종류(DashBotCounts::kinds 차례)
		DashBotCounts _dashCounts;
		// 수류탄(G-3)
		bool _grenadePress = false;		// --grenade
		int _breakGrenade = 0;			// --breakGrenade — kBreakGrenadePredict · kBreakGrenadeServer
		CSpreadRng _grenadeRng;
		int _grenadeWait = 30;			// 다음 누름까지 남은 입력(대시와 같은 첫 30)
		int _grenadeSkyWait = 0;		// 하늘 각본을 다시 볼 때까지 남은 입력
		int _grenadeNearWait = 0;		// 가까운 적 각본을 다시 볼 때까지 남은 입력
		uint64_t _grenadeUnconfirmedPost = 0;	// 소식 없이 지나간 던짐(죽음 몫 뺀 것) 중 경기 끝 단계 몫(NoteGrenadeUnconfirmed)
		uint32_t _lastBoomTick = 0, _lastBoomThrower = 0;	// 마지막 터짐 소식 — 무기 4 피해 소식은 그 바로 뒤에 온다(DetonateGrenade 차례)
		bool _boomSeen = false;
		GrenadeBotCounts _grenadeCounts;
		std::set<uint32_t> _liveGrenades;		// 받은 던짐 − 받은 터짐 — 상태 칸 번호 검사(G-3). 던진 사람이 나가 서버가 소식 없이 지운 것은 남는다(검사엔 해 없음)
		uint32_t _firstGrenadeId = 0;			// 처음 받은 던짐의 수류탄 번호(0 = 아직) — 그보다 작은 번호는 들어오기 전에 던진 것
		uint64_t _dashUnconfirmedPost = 0;	// 그중 경기 끝 단계 몫(NoteDashUnconfirmed)
		double _posMax = 0;
		uint32_t _firstBadSeq = 0;
		int _flagDiffLogged = 0;		// FLAGDIFF 줄을 적은 수(LogFlagDiff — 봇마다 FLAG_DIFF_LOG_LIMIT 까지)
	};

	bool ParseArgs(int argc, char** argv, Args& out)
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string s = argv[i];
			const auto value = [&s](const char* key, std::string& result)
			{
				const size_t n = std::strlen(key);
				if (s.compare(0, n, key) != 0) return false;
				result = s.substr(n);
				return true;
			};
			std::string v;
			if (value("--map=", v)) out.mapPath = v;
			else if (value("--record=", v)) out.recordPath = v;
			else if (value("--port=", v)) out.port = std::atoi(v.c_str());
			else if (value("--bots=", v)) out.bots = std::atoi(v.c_str());
			else if (value("--lead=", v)) out.lead = std::atoi(v.c_str());
			else if (value("--seed=", v)) out.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
			else if (value("--seconds=", v)) out.seconds = std::atof(v.c_str());
			else if (value("--measureFrom=", v)) out.measureFromSeconds = std::atof(v.c_str());
			else if (value("--measureSeconds=", v)) out.measureSeconds = std::atof(v.c_str());
			else if (value("--judgeClips=", v)) { out.judgeClips = v; out.measureView = true; }
			else if (value("--judgeLog=", v)) out.judgeLog = v;
			else if (value("--script=", v)) out.script = v;
			else if (value("--scriptHold=", v)) out.scriptHoldSeconds = std::atof(v.c_str());
			else if (value("--scriptWeapon=", v))
			{
				out.scriptWeapon = (uint8_t)EWeapon::Count;
				for (uint8_t weapon = 0; weapon < (uint8_t)EWeapon::Count; ++weapon)
				{
					if (v == GetWeaponDef((EWeapon)weapon).name) out.scriptWeapon = weapon;
				}
				if (out.scriptWeapon == (uint8_t)EWeapon::Count)
				{
					std::printf("모르는 무기 %s\n", v.c_str());
					return false;
				}
			}
			else if (s == "--judgeNoSpread") out.judgeNoSpread = true;
			else if (s == "--judgeFireAlways") out.judgeFireAlways = true;
			else if (s == "--measureView") out.measureView = true;
			else if (s == "--ads") out.adsWhenShooting = true;
			else if (s == "--crouch") out.crouchToggle = true;
			else if (s == "--melee") out.meleeNear = true;
			else if (s == "--dash") out.dash = true;
			else if (s == "--grenade") out.grenade = true;
			else if (value("--breakGrenade=", v))
			{
				if (v == "predict") out.breakGrenade = kBreakGrenadePredict;
				else if (v == "server") out.breakGrenade = kBreakGrenadeServer;
				else
				{
					std::printf("모르는 수류탄 틀림 %s (predict · server)\n", v.c_str());
					return false;
				}
			}
			else if (value("--breakDash=", v))
			{
				if (v == "predict") out.breakDash = kBreakDashPredict;
				else if (v == "server") out.breakDash = kBreakDashServer;
				else
				{
					std::printf("모르는 대시 틀림 %s (predict · server)\n", v.c_str());
					return false;
				}
			}
			else if (s == "--testNoPawnBlock") out.pawnBlock = false;
			else if (s == "--testStartAllWeapons") out.startAllWeapons = true;
			else if (s == "--push") out.push = true;
			else if (s == "--breakMelee") out.breakMelee = true;
			else if (s == "--breakRecord") out.breakRecord = true;
			else if (s == "--breakMirror") out.breakMirror = true;
			else if (s.rfind("--traceInputs=", 0) == 0) out.traceInputsPath = s.substr(14);
			else if (s == "--testLockstep") out.lockstep = true;
			else if (s == "--breakCrouch") out.breakCrouch = true;
			else if (s.rfind("--testPauseAfterSpawn=", 0) == 0) out.testPauseAfterSpawn = std::atoi(s.c_str() + 22);
			else
			{
				std::printf("모르는 인자 %s\n", s.c_str());
				return false;
			}
		}
		if (!out.script.empty() && out.script != "startstop" && out.script != "events" && out.script != "crouch")
		{
			std::printf("모르는 경로 %s\n", out.script.c_str());
			return false;
		}
		return !out.mapPath.empty();
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Args args;
	if (!ParseArgs(argc, argv, args))
	{
		std::printf("쓰는 법: BotClient --map=<파일.mocl> [--port=7791] [--bots=10] [--seed=N] [--record=파일] [--lead=3] [--breakRecord] [--breakMirror]\n");
		return 2;
	}
	CMapCollision map;
	if (!LoadMap(args.mapPath.c_str(), map))
	{
		std::printf("맵을 읽지 못함\n");
		return 2;
	}
	const MoveParams params = MakeLyraMoveParams();
	CMoveSim sim(map, params);	// 틱 사이에 들고 가는 것이 MoveState 뿐이라 봇끼리 나눠 쓴다
	MatchRules rules;
	rules.pawnBlock = args.pawnBlock;
	rules.startAllWeapons = args.startAllWeapons;

	JudgeShared judge;
	const bool judging = !args.judgeClips.empty();
	if (judging)
	{
		FILE* file = std::fopen(args.judgeClips.c_str(), "rb");
		std::vector<char> data;
		if (file)
		{
			std::fseek(file, 0, SEEK_END);
			data.resize((size_t)std::ftell(file));
			std::fseek(file, 0, SEEK_SET);
			if (std::fread(data.data(), 1, data.size(), file) != data.size()) data.clear();
			std::fclose(file);
		}
		bool loaded = !data.empty() && judge.clips.LoadFromMemory(data.data(), data.size()) == EClipLoad::Ok;
		for (size_t weapon = 0; loaded && weapon < (size_t)EWeapon::Count; ++weapon)
		{
			const char* bundle = GetWeaponDef((EWeapon)weapon).name;
			loaded = judge.evaluators[weapon].Init(judge.clips, bundle) && judge.codecs[weapon].Init(judge.clips, bundle);
		}
		if (!loaded)
		{
			std::printf("클립 파일을 읽지 못함 %s\n", args.judgeClips.c_str());
			return 2;
		}
		judge.localFrom.resize(judge.evaluators[0].BoneCount());
		judge.localTo.resize(judge.evaluators[0].BoneCount());
		judge.localBlend.resize(judge.evaluators[0].BoneCount());
		judge.noSpread = args.judgeNoSpread;
		judge.crouchLift = sim.CrouchMeshLiftZ();
		if (!args.judgeLog.empty()) judge.log = std::fopen(args.judgeLog.c_str(), "w");
	}

	std::vector<std::unique_ptr<CBot>> bots;
	for (int i = 0; i < args.bots; ++i)
	{
		bots.emplace_back(new CBot(map, sim, rules, args.seed + 100 + (uint32_t)i));
		bots.back()->SetMeasureView(args.measureView);
		bots.back()->SetAdsWhenShooting(args.adsWhenShooting);
		bots.back()->SetCrouchToggle(args.crouchToggle, args.breakCrouch);
		bots.back()->SetMelee(args.meleeNear, args.breakMelee);
		bots.back()->SetDash(args.dash, args.breakDash);
		bots.back()->SetGrenade(args.grenade, args.breakGrenade);
		bots.back()->SetPush(args.push);
		bots.back()->SetFireAlways(args.judgeFireAlways);
		bots.back()->SetPauseAfterSpawn(args.testPauseAfterSpawn);
		if (args.lockstep) bots.back()->SetSeedByPlayerId(args.seed + 100);
		bots.back()->SetJudge(judging ? &judge : nullptr);
		if (!args.script.empty()) bots.back()->SetScript(args.scriptHoldSeconds, args.scriptWeapon, args.script);
		if (!bots.back()->Connect(args.port))
		{
			std::printf("BOT %d 접속 실패\n", i);
			return 1;
		}
	}

	if (!args.traceInputsPath.empty()) g_traceInputs = std::fopen(args.traceInputsPath.c_str(), "wb");
	timeBeginPeriod(1);
	const auto start = Clock::now();
	uint64_t sentTicks = 0;
	uint64_t lockstepSends = 0;		// 기다림 판에서 부른 SendNextInput 수(봇 합)
	uint64_t lastReportSecond = 0;
	bool allDone = false;
	std::vector<float> stateGapsMs;
	std::vector<float> loopGapsMs;
	if (args.measureSeconds > 0)
	{
		const size_t expected = (size_t)(args.measureSeconds * (double)wire::kTickRate * 1.2) * (size_t)args.bots;
		stateGapsMs.reserve(expected);
		loopGapsMs.reserve((size_t)(args.measureSeconds * 1200.0));
	}
	double cpuBegin = 0, cpuEnd = 0;
	bool measureOpened = false, measureClosed = false;
	Clock::time_point lastLoop = start;
	while (!allDone)
	{
		const Clock::time_point loopNow = Clock::now();
		const double elapsed = std::chrono::duration<double>(loopNow - start).count();
		if (args.measureSeconds > 0)
		{
			if (!measureOpened && elapsed >= args.measureFromSeconds)
			{
				measureOpened = true;
				cpuBegin = ProcessCpuSeconds();
				g_stateGapsMs = &stateGapsMs;
			}
			if (measureOpened && !measureClosed && elapsed >= args.measureFromSeconds + args.measureSeconds)
			{
				measureClosed = true;
				cpuEnd = ProcessCpuSeconds();
				g_stateGapsMs = nullptr;
			}
			if (measureOpened && !measureClosed) loopGapsMs.push_back(std::chrono::duration<float, std::milli>(loopNow - lastLoop).count());
		}
		lastLoop = loopNow;
		if (args.seconds > 0 && elapsed >= args.seconds) break;
		if (args.lockstep)
		{
			for (const std::unique_ptr<CBot>& bot : bots)
			{
				if (!bot->LockstepWantsInput(loopNow)) continue;
				bot->SendNextInput(args.breakMirror);
				++lockstepSends;
			}
		}
		else
		{
			const uint64_t target = (uint64_t)(elapsed * wire::kTickRate) + (uint64_t)args.lead;
			while (sentTicks < target)
			{
				for (const std::unique_ptr<CBot>& bot : bots) bot->SendNextInput(args.breakMirror);
				++sentTicks;
			}
		}
		allDone = true;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			bot->Pump();
			if (bot->IsOpen() && !bot->HasResult()) allDone = false;
		}
		const uint64_t second = (uint64_t)elapsed;
		if (second >= lastReportSecond + 30)
		{
			lastReportSecond = second;
			std::printf("PROGRESS %llus kills=%llu shots=%llu compared=%llu\n", (unsigned long long)second, (unsigned long long)bots[0]->GetChecks().kills, (unsigned long long)bots[0]->GetChecks().shots, (unsigned long long)bots[0]->GetCompared());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	timeEndPeriod(1);
	if (g_traceInputs) std::fclose(g_traceInputs);
	g_traceInputs = nullptr;
	size_t botsOpen = 0;
	for (const std::unique_ptr<CBot>& bot : bots)
	{
		if (bot->IsOpen()) ++botsOpen;
		bot->Close();
	}

	// ─── 부하 시험 — 끝까지 안 가므로 결과 대조는 없다. 도중 값이 서버와 같은지만 ──
	if (!args.script.empty())
	{
		const bool done = bots.front()->IsScriptDone();
		std::printf("SCRIPT result %s\n", done ? "PASS" : "FAIL");
		return done ? 0 : 1;
	}
	if (args.seconds > 0)
	{
		uint64_t compared = 0, posDiff = 0, velDiff = 0, ammoDiff = 0, flagDiff = 0, states = 0, pawnReconciles = 0, invDiff = 0, crouchDiff = 0, fallDiff = 0;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			compared += bot->GetCompared();
			invDiff += bot->GetInvDiff();
			crouchDiff += bot->GetCrouchDiff();
			fallDiff += bot->GetFallDiff();
			posDiff += bot->GetPosDiff();
			velDiff += bot->GetVelDiff();
			ammoDiff += bot->GetAmmoDiff();
			flagDiff += bot->GetFlagDiff();
			states += bot->GetStates();
			pawnReconciles += bot->GetPawnReconciles();
		}
		if (judging)
		{
			uint64_t shots = 0, bullets = 0, noView = 0, noSnap = 0, noAnim = 0, clamped = 0, recvBytes = 0;
			for (const std::unique_ptr<CBot>& bot : bots)
			{
				shots += bot->GetJudgeShots();
				bullets += bot->GetJudgeBullets();
				noView += bot->GetJudgeNoView();
				noSnap += bot->GetJudgeNoSnap();
				noAnim += bot->GetJudgeNoAnim();
				clamped += bot->GetJudgeClamped();
				recvBytes += bot->GetRecvBytes();
			}
			if (judge.log) std::fclose(judge.log);
			std::printf("JUDGE shots=%llu bullets=%llu noView=%llu noSnap=%llu noAnim=%llu clampedThinks=%llu recvBytesPerBotPerSec=%.0f\n", (unsigned long long)shots, (unsigned long long)bullets, (unsigned long long)noView, (unsigned long long)noSnap, (unsigned long long)noAnim, (unsigned long long)clamped, (double)recvBytes / (double)bots.size() / args.seconds);
			std::printf("JUDGE bodies %s=%llu %s=%llu %s=%llu\n", GetWeaponDef((EWeapon)0).name, (unsigned long long)judge.bodiesByWeapon[0], GetWeaponDef((EWeapon)1).name, (unsigned long long)judge.bodiesByWeapon[1], GetWeaponDef((EWeapon)2).name, (unsigned long long)judge.bodiesByWeapon[2]);
		}
		uint64_t ackLagSum = 0, ackLagSamples = 0;
		uint32_t ackLagMax = 0;
		for (const std::unique_ptr<CBot>& bot : bots)
		{
			ackLagSum += bot->GetAckLagSum();
			ackLagSamples += bot->GetAckLagSamples();
			ackLagMax = std::max(ackLagMax, bot->GetAckLagMax());
		}
		std::printf("BOTLAG ackLagAvg=%.2f maxAckLag=%u samples=%llu\n", ackLagSamples ? (double)ackLagSum / (double)ackLagSamples : 0.0, ackLagMax, (unsigned long long)ackLagSamples);
		std::sort(stateGapsMs.begin(), stateGapsMs.end());
		std::sort(loopGapsMs.begin(), loopGapsMs.end());
		const auto percentile = [](const std::vector<float>& sorted, double p) { return sorted.empty() ? 0.f : sorted[(size_t)((sorted.size() - 1) * p)]; };
		// 위치 · 속도 기준은 아래 「맞춰 보기」 판정과 같다 — 사람끼리 막기를 켠 판은 어긋남이 재는 대상(남 자리를 늦게 안다)이라 빼고,
		// 대신 맞춰 본 줄이 있어야 하고 예측의 이상한 겹침은 0. 막기를 끈 판(--testNoPawnBlock)만 위치 · 속도 차이 0 을 엄격히 본다
		const MoveCounters& predCount = sim.GetCounters();
		const bool moveOk = args.pawnBlock ? (pawnReconciles > 0 && predCount.pawnOddSimplex == 0) : (posDiff == 0 && velDiff == 0);
		const bool pass = compared > 0 && moveOk && ammoDiff == 0 && flagDiff == 0 && invDiff == 0 && botsOpen == bots.size();
		std::printf("LOADBOT bots=%d open=%zu compared=%llu posDiff=%llu velDiff=%llu ammoDiff=%llu flagDiff=%llu states=%llu stateGaps=%zu stateGapP50Ms=%.2f stateGapP99Ms=%.2f stateGapMaxMs=%.2f loopP99Ms=%.2f loopMaxMs=%.2f cpuPercent=%.1f pawnBlock=%d pawnReconciles=%llu predOdd=%llu invDiff=%llu crouchDiff=%llu fallDiff=%llu\n",
			args.bots, botsOpen, (unsigned long long)compared, (unsigned long long)posDiff, (unsigned long long)velDiff, (unsigned long long)ammoDiff, (unsigned long long)flagDiff, (unsigned long long)states, stateGapsMs.size(),
			percentile(stateGapsMs, 0.5), percentile(stateGapsMs, 0.99), percentile(stateGapsMs, 1.0), percentile(loopGapsMs, 0.99), percentile(loopGapsMs, 1.0), args.measureSeconds > 0 ? (cpuEnd - cpuBegin) / args.measureSeconds * 100.0 : 0.0,
			args.pawnBlock ? 1 : 0, (unsigned long long)pawnReconciles, (unsigned long long)predCount.pawnOddSimplex, (unsigned long long)invDiff, (unsigned long long)crouchDiff, (unsigned long long)fallDiff);
		std::printf("RESULT %s\n", pass ? "PASS" : "FAIL");
		return pass ? 0 : 1;
	}

	// ─── 맞춰 보기 ────────────────────────────────────────
	bool ok = true;
	const CBot& first = *bots[0];
	std::map<uint32_t, const CBot*> firstOfRoom;
	std::map<uint32_t, int> botsOfRoom;
	for (const std::unique_ptr<CBot>& bot : bots)
	{
		firstOfRoom.emplace(bot->GetRoomId(), bot.get());
		++botsOfRoom[bot->GetRoomId()];
	}
	std::string record = first.GetRecord();
	for (const auto& room : firstOfRoom)
	{
		std::string roomRecord = room.second->GetRecord();
		if (args.breakRecord && !roomRecord.empty()) roomRecord[roomRecord.size() / 2] = roomRecord[roomRecord.size() / 2] == '1' ? '2' : '1';
		if (args.recordPath.empty()) continue;
		FILE* file = std::fopen(RoomRecordPath(args.recordPath, room.first).c_str(), "wb");
		if (file)
		{
			std::fwrite(roomRecord.data(), 1, roomRecord.size(), file);
			std::fclose(file);
		}
	}

	uint64_t compared = 0, posDiff = 0, velDiff = 0, ammoDiff = 0, flagDiff = 0, states = 0, detours = 0, lockstepTimeouts = 0;
	uint64_t adsCompared = 0, adsDiff = 0, adsInputs = 0, lockDiff = 0, lockCompared = 0;
	uint64_t crouchCompared = 0, crouchDiff = 0, crouchPresses = 0;
	uint64_t pawnReconciles = 0, pawnCorrected = 0, fallDiff = 0;
	double pawnCorrectSum = 0, pawnCorrectMax = 0;
	MeleeBotCounts melee;
	CSelfPrediction::MeleeCounters meleeFixes;
	DashBotCounts dash;
	CSelfPrediction::DashCounters dashFixes;
	GrenadeBotCounts grenade;
	CSelfPrediction::GrenadeCounters grenadeFixes;
	uint64_t grenadeUnconfirmedPost = 0, grenadeUnacked = 0;
	uint64_t dashUnconfirmedPost = 0;
	uint64_t meleePendingLeft = 0;
	uint64_t padMsgs = 0, padBad = 0, padSeekStarts = 0, padSeekTicks = 0, padHealsMine = 0, padHealsSought = 0;
	uint64_t invDiff = 0, invMulti = 0, weaponPickupsMine = 0, weaponSeekStarts = 0, weaponSeekTicks = 0, weaponTakesSought = 0, weaponSeekAmmoStarts = 0, spareOutTicks = 0;
	CSelfPrediction::PickupCounters pickupFixes;
	size_t padCountMax = 0;
	uint64_t shotsWhileDead = 0, damageOutside = 0, damageImmune = 0, recordDiff = 0, tallyDiff = 0, noResult = 0, immuneHits = 0;
	uint64_t assistsCounted = 0, assistKillDiff = 0, assistDiff = 0;
	double posMax = 0;
	uint32_t firstBad = 0;
	int firstBadBot = -1;
	for (size_t i = 0; i < bots.size(); ++i)
	{
		const CBot& bot = *bots[i];
		compared += bot.GetCompared();
		posDiff += bot.GetPosDiff();
		velDiff += bot.GetVelDiff();
		ammoDiff += bot.GetAmmoDiff();
		flagDiff += bot.GetFlagDiff();
		adsCompared += bot.GetAdsCompared();
		adsDiff += bot.GetAdsDiff();
		lockDiff += bot.GetLockDiff();
		lockCompared += bot.GetLockCompared();
		adsInputs += bot.GetAdsInputs();
		crouchCompared += bot.GetCrouchCompared();
		crouchDiff += bot.GetCrouchDiff();
		crouchPresses += bot.GetCrouchPresses();
		pawnReconciles += bot.GetPawnReconciles();
		pawnCorrected += bot.GetPawnCorrected();
		fallDiff += bot.GetFallDiff();
		pawnCorrectSum += bot.GetPawnCorrectSum();
		pawnCorrectMax = std::max(pawnCorrectMax, bot.GetPawnCorrectMax());
		AddMeleeCounts(melee, bot.GetMeleeCounts());
		AddMeleeFixes(meleeFixes, bot.GetMeleeFixes());
		AddDashCounts(dash, bot.GetDashCounts());
		AddDashFixes(dashFixes, bot.GetDashFixes());
		AddGrenadeCounts(grenade, bot.GetGrenadeCounts());
		AddGrenadeFixes(grenadeFixes, bot.GetGrenadeFixes());
		grenadeUnconfirmedPost += bot.GetGrenadeUnconfirmedPost();
		grenadeUnacked += bot.GetGrenadeUnacked();
		dashUnconfirmedPost += bot.GetDashUnconfirmedPost();
		meleePendingLeft += bot.GetMeleePendingDamage();
		states += bot.GetStates();
		detours += bot.GetDetours();
		lockstepTimeouts += bot.GetLockstepTimeouts();
		padMsgs += bot.GetPadMsgs();
		padBad += bot.GetPadBad();
		padSeekStarts += bot.GetPadSeekStarts();
		padSeekTicks += bot.GetPadSeekTicks();
		padHealsMine += bot.GetPadHealsMine();
		padHealsSought += bot.GetPadHealsSought();
		invDiff += bot.GetInvDiff();
		invMulti += bot.GetInvMulti();
		weaponPickupsMine += bot.GetWeaponPickupsMine();
		weaponSeekStarts += bot.GetWeaponSeekStarts();
		weaponSeekTicks += bot.GetWeaponSeekTicks();
		weaponTakesSought += bot.GetWeaponTakesSought();
		weaponSeekAmmoStarts += bot.GetWeaponSeekAmmoStarts();
		spareOutTicks += bot.GetSpareOutTicks();
		{
			const CSelfPrediction::PickupCounters& fixes = bot.GetPickupFixes();
			pickupFixes.received += fixes.received;
			pickupFixes.rerun += fixes.rerun;
			pickupFixes.noSlot += fixes.noSlot;
			pickupFixes.resultDiffs += fixes.resultDiffs;
			pickupFixes.ammoFixes += fixes.ammoFixes;
			pickupFixes.meleeInWindow += fixes.meleeInWindow;
		}
		padCountMax = std::max(padCountMax, bot.GetPadCount());
		posMax = std::max(posMax, bot.GetPosMax());
		if (firstBad == 0 && bot.GetFirstBadSeq() != 0)
		{
			firstBad = bot.GetFirstBadSeq();
			firstBadBot = (int)i;
		}
		shotsWhileDead += bot.GetChecks().shotsWhileDead;
		damageOutside += bot.GetChecks().damageOutsidePlaying;
		damageImmune += bot.GetChecks().damageWhileImmune;
		immuneHits += bot.GetChecks().immuneBulletHits;
		assistsCounted += bot.GetChecks().assists;
		assistKillDiff += bot.GetChecks().assistKillDiff;
		if (bot.GetRecord() != firstOfRoom[bot.GetRoomId()]->GetRecord()) ++recordDiff;
		if (!bot.HasResult())
		{
			++noResult;
			continue;
		}
		const wire::Result& result = bot.GetResult();
		for (int p = 0; p < result.count; ++p)
		{
			const wire::PlayerResult& player = result.players[p];
			const auto killIt = bot.GetKillTally().find(player.id);
			const auto deathIt = bot.GetDeathTally().find(player.id);
			const int kills = killIt == bot.GetKillTally().end() ? 0 : killIt->second;
			const int deaths = deathIt == bot.GetDeathTally().end() ? 0 : deathIt->second;
			if (kills != player.kills || deaths != player.deaths) ++tallyDiff;
			const auto assistIt = bot.GetAssistTally().find(player.id);
			const int assists = assistIt == bot.GetAssistTally().end() ? 0 : assistIt->second;
			if (assists != player.assists) ++assistDiff;
		}
	}

	const wire::Result& result = first.GetResult();
	std::printf("BOT bots=%d inputsSent=%llu statesSeen=%llu detours=%llu\n", args.bots, args.lockstep ? (unsigned long long)lockstepSends : (unsigned long long)sentTicks * bots.size(), (unsigned long long)states, (unsigned long long)detours);
	std::printf("BOT lockstep on=%d sends=%llu timeouts=%llu\n", args.lockstep ? 1 : 0, (unsigned long long)lockstepSends, (unsigned long long)lockstepTimeouts);
	std::printf("BOT 단계:");
	for (const wire::Match& match : first.GetPhaseChanges()) std::printf(" [%u 틱=%u 남은초=%.2f 목표=%u]", match.phase, match.tick, match.secondsLeft, match.targetScore);
	std::printf("\n");
	std::printf("BOT 서버 ↔ 같은 코드: compared=%llu posDiff=%llu posMax=%.5f velDiff=%llu ammoDiff=%llu flagDiff=%llu 처음 어긋난 봇=%d 번호=%u\n", (unsigned long long)compared, (unsigned long long)posDiff, posMax, (unsigned long long)velDiff, (unsigned long long)ammoDiff, (unsigned long long)flagDiff, firstBadBot, firstBad);
	std::printf("BOT ads inputs=%llu comparedOn=%llu diff=%llu (diff 는 flagDiff 안에도 든다)\n", (unsigned long long)adsInputs, (unsigned long long)adsCompared, (unsigned long long)adsDiff);
	std::printf("BOT crouch presses=%llu comparedOn=%llu diff=%llu (diff 는 막기를 끈 판만 flagDiff 안에도 든다)\n", (unsigned long long)crouchPresses, (unsigned long long)crouchCompared, (unsigned long long)crouchDiff);
	// 부활 직후 이동 막음 — comparedOn = 서버가 막음으로 보낸 비교 줄. 서버 빈 입력 이동(SERVER idleMoves)이 막음을 따로 줄이면 diff 가 선다
	std::printf("BOT moveLock comparedOn=%llu diff=%llu (diff 는 flagDiff 안에도 든다)\n", (unsigned long long)lockCompared, (unsigned long long)lockDiff);
	// 사람끼리 막기(F-2) — 꺼진 판은 0. 예측 쪽 계수는 봇끼리 나눠 쓰는 sim 하나의 합(다시 돌린 걸음 포함)
	const MoveCounters& predCount = sim.GetCounters();
	// fallDiff — 떨어짐 깃발이 서버와 다른 줄 수. 막기를 끈 판은 flagDiff 에도 들고, 켠 판은 위치 차이처럼 여기만 센다
	std::printf("BOT pawn block=%d push=%d reconciles=%llu corrected=%llu correctAvg=%.4f correctMax=%.4f predHits=%llu predStartPen=%llu predOdd=%llu predTouch=%llu fallDiff=%llu\n", args.pawnBlock ? 1 : 0, args.push ? 1 : 0,
		(unsigned long long)pawnReconciles, (unsigned long long)pawnCorrected, pawnCorrected ? pawnCorrectSum / (double)pawnCorrected : 0.0, pawnCorrectMax,
		(unsigned long long)predCount.pawnHits, (unsigned long long)predCount.pawnStartPenetrating, (unsigned long long)predCount.pawnOddSimplex, (unsigned long long)predCount.pawnTouchStart, (unsigned long long)fallDiff);
	// 수류탄 셈 둘(G-3) — 아래 출력 · 판정이 같이 쓴다. 뺄셈이 음이 되면 큰 수로 돌아 판정이 갈린다(세는 자리가 빠졌다는 뜻)
	const uint64_t grenadeFinal = grenadeFixes.thrown + grenadeFixes.respawnThrown - grenadeFixes.respawnLost + grenadeFixes.replayGained - grenadeFixes.replayLost - grenadeUnacked;
	const uint64_t grenadeDiff = grenadeFixes.startMismatches + grenadeFixes.unconfirmed - grenadeFixes.unconfirmedDead - grenadeUnconfirmedPost;
	// 근접(E-3) — mine 합 = 서버 SERVER melee starts(봇만 든 판). 소식 · 규칙 줄은 봇마다 같은 소식을 따로 받으므로 봇 수만큼 겹친다
	{
		const auto u = [](uint64_t v) { return (unsigned long long)v; };
		std::printf("BOT melee presses=%llu judged=%llu screen none=%llu sameTeam=%llu blocked=%llu hit=%llu | mine=%llu hitForce=%llu hitNoForce=%llu agree=%llu kindDiff=%llu targetDiff=%llu forceDiffMax=%.4f noGuess=%llu guessNoResult=%llu\n",
			u(melee.presses), u(melee.judged), u(melee.screen[0]), u(melee.screen[1]), u(melee.screen[2]), u(melee.screen[3]), u(melee.mine), u(melee.mineHitForce), u(melee.mineHitNoForce), u(melee.agree), u(melee.kindDiff), u(melee.targetDiff), melee.forceDiffMax, u(melee.noGuess), u(melee.guessNoResult));
		std::printf("BOT meleeFix started=%llu pushesGiven=%llu pushesIgnored=%llu results=%llu noSlot=%llu startMismatches=%llu hitFixes=%llu forceFixes=%llu unconfirmed=%llu rerunsMissed=%llu\n",
			u(meleeFixes.started), u(meleeFixes.pushesGiven), u(meleeFixes.pushesIgnored), u(meleeFixes.results), u(meleeFixes.resultsNoSlot), u(meleeFixes.startMismatches), u(meleeFixes.pushHitFixes), u(meleeFixes.pushForceFixes), u(meleeFixes.unconfirmed), u(meleeFixes.rerunsMissed));
		std::printf("BOT meleeRules msgs=%llu hits=%llu damaged=%llu zero=%llu damageMsgs=%llu kills=%llu pendingLeft=%llu | bad shape=%llu dead=%llu post=%llu damage=%llu zero=%llu\n",
			u(melee.msgs), u(melee.hits), u(melee.hitsDamaged), u(melee.hitsZero), u(melee.damageMsgs), u(melee.kills), u(meleePendingLeft), u(melee.badShape), u(melee.badDead), u(melee.badPost), u(melee.badDamage), u(melee.badZero));
		// 대시(D-3) — mine 합 = 서버 SERVER dash started(봇만 든 판). dashDiff = 서버 소식으로 고친 대시(켬 어긋남 + 방향 · 힘 + 소식 없이 지나감 − 그중 경기 끝 몫 unconfirmedPost), 봇 판 기대 0.
		// pressed = 봇이 누른 입력, predPresses = 예측이 센 누름(같아야 한다). kinds = 앞 · 뒤 · 왼 · 오른 · 위 보고 앞 · 아래 보고 앞, dir = 서버가 고른 방향(앞 · 뒤 · 왼 · 오른)
		// respawn = 부활 다시 돌리기에서 켜짐이 바뀐 칸(늘어남/줄어듦). 서버 켬 = mine = predStarted + 늘어남 − 줄어듦 + startMismatches − unconfirmed − unconfirmedDead(죽은 줄 모르고 켠 몫)
		std::printf("BOT dash pressed=%llu predPresses=%llu predStarted=%llu respawn=+%llu/-%llu kinds=%llu/%llu/%llu/%llu/%llu/%llu extra jump=%llu reload=%llu melee=%llu | mine=%llu dir=%llu/%llu/%llu/%llu results=%llu noSlot=%llu | dashDiff=%llu startMismatches=%llu forceFixes=%llu unconfirmed=%llu unconfirmedPost=%llu unconfirmedDead=%llu replays=%llu replaysMissed=%llu meleeInReplay=%llu\n",
			u(dash.presses), u(dashFixes.presses), u(dashFixes.started), u(dashFixes.respawnGained), u(dashFixes.respawnLost), u(dash.kinds[0]), u(dash.kinds[1]), u(dash.kinds[2]), u(dash.kinds[3]), u(dash.kinds[4]), u(dash.kinds[5]),
			u(dash.extraJump), u(dash.extraReload), u(dash.extraMelee), u(dash.mine), u(dash.mineDir[0]), u(dash.mineDir[1]), u(dash.mineDir[2]), u(dash.mineDir[3]), u(dashFixes.results), u(dashFixes.resultsNoSlot),
			u(dashFixes.startMismatches + dashFixes.forceFixes + dashFixes.unconfirmed - dashUnconfirmedPost), u(dashFixes.startMismatches), u(dashFixes.forceFixes), u(dashFixes.unconfirmed), u(dashUnconfirmedPost), u(dashFixes.unconfirmedDead), u(dashFixes.replays), u(dashFixes.replaysMissed), u(dashFixes.meleeInReplay));
		std::printf("BOT dashRules msgs=%llu | bad shape=%llu dead=%llu post=%llu\n", u(dash.msgs), u(dash.badShape), u(dash.badDead), u(dash.badPost));
		// 수류탄(G-3) — mine 합 = 서버 SERVER grenade thrown(봇만 든 판). 소식 · 상태 · 피해 받은 수(msgs)는 봇마다 같은 소식을 따로 받으므로 봇 수 배.
		// pressed = 봇이 누른 입력, predPresses = 예측이 센 누름(같아야 한다). kinds = 피치 그대로 · 하늘 · 발 앞 바닥 · 먼 바닥, waiting · post = 부활 대기 · 경기 끝에 누름.
		// finalThrown = 예측 칸마다 마지막으로 정한 던짐의 합 = predThrown + 부활 다시 돌리기(+/−) + 다시 고르기(+/−) − 판 끝에 아직 확인 안 된 몫(unacked) — mine 과 같아야 한다(sameFinal).
		// grenadeDiff = 서버 소식으로 고친 던짐(어긋남 + 소식 없이 지나감 − 죽음을 늦게 안 몫 unconfirmedDead − 경기 끝 몫 unconfirmedPost), 봇 판 기대 0
		std::printf("BOT grenade pressed=%llu predPresses=%llu kinds=%llu/%llu/%llu/%llu waiting=%llu post=%llu sky=%llu near=%llu | predThrown=%llu respawn=+%llu/-%llu replay=+%llu/-%llu unacked=%llu finalThrown=%llu mine=%llu sameFinal=%d results=%llu noSlot=%llu | grenadeDiff=%llu startMismatches=%llu unconfirmed=%llu unconfirmedDead=%llu unconfirmedPost=%llu replays=%llu replaysMissed=%llu meleeInReplay=%llu\n",
			u(grenade.presses), u(grenadeFixes.presses), u(grenade.kinds[0]), u(grenade.kinds[1]), u(grenade.kinds[2]), u(grenade.kinds[3]), u(grenade.pressWaiting), u(grenade.pressPost), u(grenade.pressSky), u(grenade.pressNear),
			u(grenadeFixes.thrown), u(grenadeFixes.respawnThrown), u(grenadeFixes.respawnLost), u(grenadeFixes.replayGained), u(grenadeFixes.replayLost), u(grenadeUnacked), u(grenadeFinal), u(grenade.mine), grenadeFinal == grenade.mine ? 1 : 0,
			u(grenadeFixes.results), u(grenadeFixes.resultsNoSlot), u(grenadeDiff), u(grenadeFixes.startMismatches), u(grenadeFixes.unconfirmed), u(grenadeFixes.unconfirmedDead), u(grenadeUnconfirmedPost),
			u(grenadeFixes.replays), u(grenadeFixes.replaysMissed), u(grenadeFixes.meleeInReplay));
		// 무기 4 — mine 합 = 서버 SERVER grenade damageEvents · selfDamage · kills(봇만 든 판). msgs 는 봇 수 배
		std::printf("BOT grenadeHits damageMsgs=%llu killMsgs=%llu | mine damage=%llu selfDamage=%llu kills=%llu | bad damage=%llu\n",
			u(grenade.damageMsgs), u(grenade.killMsgs), u(grenade.damageMine), u(grenade.selfDamageMine), u(grenade.killsMine), u(grenade.badDamage));
		std::printf("BOT grenadeWire msgs=%llu throws=%llu noGrenade=%llu boom timer=%llu hit=%llu state msgs=%llu entries=%llu bounced=%llu beforeJoin=%llu | bad kind=%llu state=%llu stateId=%llu dead=%llu post=%llu\n",
			u(grenade.msgs), u(grenade.throws), u(grenade.throwsNoGrenade), u(grenade.boomTimer), u(grenade.boomHit), u(grenade.stateMsgs), u(grenade.stateEntries), u(grenade.stateBounced),
			u(grenade.stateBeforeJoin), u(grenade.badKind), u(grenade.badState), u(grenade.badStateId), u(grenade.badDead), u(grenade.badPost));
	}
	// 받침(컨텐츠 B-4) — healsMine 합 = 서버 SERVER pickups(봇만 든 판). healsSought = 찾아가던 중에 받은 회복(갈래가 실제로 닿았나)
	std::printf("BOT pads count=%zu msgs=%llu bad=%llu seekStarts=%llu seekTicks=%llu healsMine=%llu healsSought=%llu\n", padCountMax, (unsigned long long)padMsgs,
		(unsigned long long)padBad, (unsigned long long)padSeekStarts, (unsigned long long)padSeekTicks, (unsigned long long)padHealsMine, (unsigned long long)padHealsSought);
	// 무기 받침(W-3) — pickupsMine 합 = 서버 SERVER weaponPickups new + noSwitch + ammo(봇만 든 판). 예측이 받은 수 received = rerun + noSlot,
	// invMulti = 무기 둘 이상 든 비교 줄(가방 비교가 실제로 돌았나). 세는 자리를 받음 · 다시 돌림 · 칸 없음으로 나눠 0 이 「안 지나감」인지 가린다.
	// seekStarts · seekTicks · sought = 무기 받침 찾아가기(W-5) 시작 수 · 틱 수 · 찾아가던 중에 집은 수(갈래가 실제로 닿았나).
	// seekAmmoStarts = 그중 든 무기 예비가 바닥나 찾아간 수, spareOutTicks = 산 채로 든 무기 예비가 0 인 틱
	std::printf("BOT weapons pickupsMine=%llu received=%llu rerun=%llu noSlot=%llu resultDiffs=%llu ammoFixes=%llu meleeInWindow=%llu invDiff=%llu invMulti=%llu seekStarts=%llu seekTicks=%llu sought=%llu seekAmmoStarts=%llu spareOutTicks=%llu\n",
		(unsigned long long)weaponPickupsMine, (unsigned long long)pickupFixes.received, (unsigned long long)pickupFixes.rerun, (unsigned long long)pickupFixes.noSlot,
		(unsigned long long)pickupFixes.resultDiffs, (unsigned long long)pickupFixes.ammoFixes, (unsigned long long)pickupFixes.meleeInWindow, (unsigned long long)invDiff, (unsigned long long)invMulti,
		(unsigned long long)weaponSeekStarts, (unsigned long long)weaponSeekTicks, (unsigned long long)weaponTakesSought, (unsigned long long)weaponSeekAmmoStarts, (unsigned long long)spareOutTicks);
	std::printf("BOT 규칙: kills=%llu damages=%llu shots=%llu spawns=%llu | 죽은 뒤 발사=%llu 대기·경기 뒤 피해=%llu 무적 중 피해=%llu | 적이 맞았는데 피해 0(무적 분모)=%llu\n",
		(unsigned long long)first.GetChecks().kills, (unsigned long long)first.GetChecks().damages, (unsigned long long)first.GetChecks().shots, (unsigned long long)first.GetChecks().spawnsSeen,
		(unsigned long long)shotsWhileDead, (unsigned long long)damageOutside, (unsigned long long)damageImmune, (unsigned long long)immuneHits);
	std::printf("BOT 기록: 줄 바이트=%zu 봇끼리 다름=%llu 결과 못 받음=%llu 처치 집계 ↔ 결과 다름=%llu\n", record.size(), (unsigned long long)recordDiff, (unsigned long long)noResult, (unsigned long long)tallyDiff);
	std::printf("BOT 도움: 봇이 센 도움=%llu 처치마다 번호 다름=%llu 결과 다름=%llu (봇 %zu명 합 — 봇마다 같은 판을 따로 센다)\n",
		(unsigned long long)assistsCounted, (unsigned long long)assistKillDiff, (unsigned long long)assistDiff, bots.size());
	std::printf("BOT 결과: 점수 %u:%u 목표 %u 이긴 팀 %u 틱 %u\n", result.teamScoreA, result.teamScoreB, result.targetScore, result.winningTeam, result.tick);
	for (int p = 0; p < result.count; ++p)
	{
		std::printf("SCORE id=%u team=%u kills=%u deaths=%u assists=%u\n", result.players[p].id, result.players[p].team, result.players[p].kills, result.players[p].deaths, result.players[p].assists);
	}

	for (const auto& room : firstOfRoom)
	{
		const wire::Result& roomResult = room.second->GetResult();
		std::printf("BOT 방 %u: 봇 %d 점수 %u:%u 이긴 팀 %u 기록 %zu바이트\n", room.first, botsOfRoom[room.first], roomResult.teamScoreA, roomResult.teamScoreB, roomResult.winningTeam, room.second->GetRecord().size());
	}

	if (noResult != 0 || recordDiff != 0 || tallyDiff != 0) ok = false;
	if (assistKillDiff != 0 || assistDiff != 0) ok = false;
	// 사람끼리 막기를 켠 판은 위치 · 속도 어긋남이 재는 대상이다(남 자리를 늦게 안다) — 대신 맞춰 본 줄이 있어야 하고 옮기지 않은 갈래는 0
	if (compared == 0 || (!args.pawnBlock && (posDiff != 0 || velDiff != 0)) || ammoDiff != 0 || flagDiff != 0) ok = false;
	if (args.pawnBlock && (pawnReconciles == 0 || predCount.pawnOddSimplex != 0)) ok = false;
	if (args.push && args.pawnBlock && predCount.pawnHits == 0) ok = false;		// 막기를 켜고 밀었는데 예측이 한 번도 안 닿음 = 갈래가 안 돎
	if (padBad != 0) ok = false;
	if (invDiff != 0 || pickupFixes.noSlot != 0 || pickupFixes.resultDiffs != 0 || pickupFixes.ammoFixes != 0 || pickupFixes.received != weaponPickupsMine) ok = false;
	// 근접 — 규칙 위반 · 피해 소식 짝 · 켠 입력 어긋남은 늘 0. --melee 판은 내 근접 결과가 하나 이상(갈래가 실제로 돌았다)
	if (melee.badShape != 0 || melee.badDead != 0 || melee.badPost != 0 || melee.badDamage != 0 || melee.badZero != 0 || meleePendingLeft != 0) ok = false;
	if (melee.noGuess != 0 || meleeFixes.startMismatches != 0 || meleeFixes.rerunsMissed != 0 || meleeFixes.pushesIgnored != 0) ok = false;
	if (args.meleeNear && melee.mine == 0) ok = false;
	// 대시(D-3) — 소식 규칙 · 고친 수 0(경기 끝 몫 빼고) · 받은 내 소식 = 예측이 받은 수(칸 있음 + 없음). --dash 면 서버가 한 번 이상 켜고, 누른 수가 봇 · 예측 같고,
	// 서버 켬 = 예측 켬(입력 때 + 부활 다시 돌리기 늘어남 − 줄어듦 + 서버 소식으로 켬 − 소식 없이 지나감) — 세는 자리가 빠지면 갈린다
	if (dash.badShape != 0 || dash.badDead != 0 || dash.badPost != 0) ok = false;
	if (dashFixes.startMismatches != 0 || dashFixes.forceFixes != 0 || dashFixes.unconfirmed != dashUnconfirmedPost || dashFixes.results + dashFixes.resultsNoSlot != dash.mine) ok = false;
	if (args.dash && (dash.mine == 0 || dash.presses != dashFixes.presses || dashFixes.started + dashFixes.respawnGained + dashFixes.startMismatches != dash.mine + dashFixes.respawnLost + dashFixes.unconfirmed + dashFixes.unconfirmedDead)) ok = false;
	// 수류탄(G-3) — 소식 · 피해 규칙 · 고친 수 0(죽음 · 경기 끝 몫 빼고) · 받은 내 소식 = 예측이 받은 수(칸 있음 + 없음) · 예측 칸에 남은 던짐 = 내 소식.
	// --grenade 면 서버가 한 번 이상 던지고 누른 수가 봇 · 예측 같고, 상태 소식을 받았다(플랜 177줄)
	if (grenade.badKind != 0 || grenade.badState != 0 || grenade.badStateId != 0 || grenade.badDead != 0 || grenade.badPost != 0 || grenade.badDamage != 0) ok = false;
	if (grenadeDiff != 0 || grenadeFixes.resultsNoSlot != 0 || grenadeFixes.replaysMissed != 0 || grenadeFixes.results + grenadeFixes.resultsNoSlot != grenade.mine || grenadeFinal != grenade.mine) ok = false;
	if (args.grenade && (grenade.mine == 0 || grenade.presses != grenadeFixes.presses || grenade.stateMsgs == 0)) ok = false;
	if (shotsWhileDead != 0 || damageOutside != 0 || damageImmune != 0) ok = false;
	if (lockstepTimeouts != 0) ok = false;		// 기다림 판이 한도에 걸려 벽시계로 보냄 — 판마다 같다는 보장이 깨졌다
	std::printf("RESULT %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
