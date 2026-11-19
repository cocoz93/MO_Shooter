#pragma once

#include "AnimState.h"
#include "Dash.h"
#include "GameProtocol.h"
#include "Loadout.h"
#include "MapCollision.h"
#include "MatchRules.h"
#include "Movement.h"
#include "Pickups.h"
#include "Weapon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
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

	// 되감기 한도 18틱(60Hz 기준 0.3초). 넘으면 한도까지만 되감는다.
	// - 왜 상수인가: 회선 추정값이 아니라 게임 규칙이다. 맞는 쪽이 얼마나 과거 자리에서 맞아도 되는지 정하는 선이라,
	//   지연에 맞춰 늘리면 일부러 지연을 올린 쪽이 이득을 본다. 쏠 때마다 되감는 양은 입력의 viewTick 으로 따로 달라진다.
	// - 왜 0.3초인가: 되감는 양 = 왕복 지연 + 화면 지연 0.1초 라서 왕복 200ms 까지 정확히 맞는다.
	//   200ms 는 시험 최대 150ms + 여유 50ms 로 고른 시작값이다. 실제 회선 자료에서 나온 값은 아니다.
	// - 대가: 달리는 상대는 최대 약 1.4m 뒤 자리에서 맞을 수 있다. 왕복 200ms 를 넘는 쪽은 앞질러 쏴야 한다.
	constexpr uint32_t kRewindLimitTicks = 18;

	struct RewindOptions
	{
		uint32_t limitTicks = kRewindLimitTicks;	// 시험 전용 --rewindLimitTicks. 0 = 되감기 끔
		bool shadow = false;						// 측정 전용 — 같은 탄을 현재 자리로도 계산해 비교한다
		bool log = false;							// 대조 전용 — 쏠 때마다 되감은 자리를 찍는다
		bool bulletLog = false;						// 측정 전용 — 탄마다 결과 한 줄(BULLET). 봇 판정과 (쏜 사람, 입력 번호, 탄 번호)로 잇는다
		bool noSpread = false;		// 측정 전용 — 무기 퍼짐을 빼고 조준점 그대로 쏜다. 퍼짐(3~14도)이 21m 교전 거리의 몸 크기(약 2도)보다 커서 되감기 효과를 가리는지 가르려고
		uint32_t montageShiftTicks = 0;	// 일부러 틀림 전용 --rewindMontageShift=틱 — 판정이 되감기 기록을 풀 때 몽타주 시각만 민다(Room.cpp JudgeAnim).
										// 기록 · 보내는 상태는 그대로라 화면 대조(VIEWPOSE) · 봇 오탈이 몽타주 중 쏨에서만 벌어져야 한다(9단계 플랜 9-5 · 9-6)
	};

	// 무기마다 자세 묶음이 하나 — 묶음 이름 = 무기 이름(9단계). 샷건 묶음은 소총 클립 그대로다(D45)
	inline const char* AnimBundleOf(uint8_t weapon) { return GetWeaponDef((EWeapon)weapon).name; }

	// 서버가 애니 상태에 넘기는 사건(9단계 조각 4). 틱 안에서 생긴 차례대로 쌓아 그 틱 애니 갱신 앞에 몽타주로 튼다
	// 근접(E-3) — Melee = 공격자 근접 몽타주, HitBack · HitFront = 맞은 쪽 반응(둘 중 무작위, 서버가 정해 쌓는다)
	enum class EAnimEvent : uint8_t { Spawn, Swap, Fire, Reload, Melee, HitBack, HitFront, Count };

	// 클립을 주면 몸 판정이 서 있는 자세 대신 그 시점의 애니 자세를 쓴다(D32). 비어 있으면 지금까지와 같다
	struct AnimOptions
	{
		const CAnimClips* clips = nullptr;	// 프로세스가 들고 있다. 방보다 오래 산다
		double boundRadius = 0.0;			// CAnimPoseEvaluator::ComputeBoundRadius
	};

	// 체력 받침(컨텐츠 B) — main 이 --pads= 로 읽어 들고 있다. 방보다 오래 산다. 없으면 받침 없는 판
	struct PickupOptions
	{
		const std::vector<PickupPad>* pads = nullptr;
	};

	// 시점 입력의 네 갈래. 값을 돌려주는 함수로 두어 시험이 갈래를 전수로 볼 수 있게 한다
	enum class ERewindKind : uint8_t
	{
		NoView,		// viewTick == 0 — 현재 자리로 판정
		Future,		// 아직 안 온 틱 — 현재 자리로 자른다
		Clamped,	// 한도보다 과거 — 한도 시점으로 자른다
		Ok,
	};
	ERewindKind ClassifyRewind(uint32_t nowTick, uint32_t viewTick, float viewAlpha, uint32_t limitTicks);

	struct RoomStats
	{
		uint64_t ticks = 0;
		uint64_t inputsReceived = 0;
		uint64_t inputsApplied = 0;
		uint64_t inputsDroppedFull = 0;		// 입력 대기열이 꽉 차서 버림
		uint64_t inputsDroppedOldSeq = 0;	// 번호가 앞선 입력보다 작거나 같음
		uint64_t firesDroppedSeqGap = 0;	// 입력 번호를 건너뛴 입력의 사격을 버림(D15)
		uint64_t inputsWhileDead = 0;		// 죽어 있는 동안 온 입력 — 번호만 기록하고 안 쓴다
		uint64_t starvedTicks = 0;			// 입력이 안 와서 그 틱에 꺼낼 것이 없음
		uint64_t idleMoves = 0;				// 입력 없이 오래 지나 빈 입력으로 움직임(공중에 멈추지 않게)
		uint64_t debugPlaces = 0;
		uint64_t debugAmmoSets = 0;			// 시험 입력이 탄을 정함(DEBUG_PLACE_SET_AMMO)
		uint64_t debugKills = 0;			// 시험 입력이 죽임(DEBUG_PLACE_KILL)
		uint64_t statesSent = 0;
		uint64_t shots = 0;
		uint64_t firesBlockedPostGame = 0;	// 경기 끝 단계의 발사 입력 — GE_BlockAbilityInput
		uint64_t adsInputs = 0;				// 조준이 켜진 채 적용한 입력(C-4)
		uint64_t shotsAimed = 0;			// 조준 비율 1 로 쏜 발 — 퍼짐이 조준 배수를 탄 발(C-4)
		uint64_t crouchPresses = 0;			// 앉기 키가 든 입력(D-5) — ToggleCrouch 를 부른 수
		uint64_t crouchedInputs = 0;		// 이동 뒤 앉아 있는 입력(D-5) — 앉은 걸음 · 퍼짐 · 자세가 돈 입력 수
		uint64_t shotsCrouched = 0;			// 앉은 채 쏜 발(D-5) — 퍼짐 계산에 앉음이 들어간 발
		uint64_t pawnBlockMoves = 0;		// 사람끼리 막기(F-2)로 남의 캡슐을 넘겨 굴린 입력 — 시험 전용 --testNoPawnBlock 판은 0
		uint64_t pawnBlockOthers = 0;		// 그 입력들에 넘긴 캡슐 수 합(산 사람만 — Lyra 는 죽으면 캡슐 충돌을 끈다)
		uint64_t pawnHits = 0;				// 그중 이동 쓸기가 사람에 먼저 닿음(CMoveSim pawnHits 차이)
		uint64_t pawnStartPenetrating = 0;	// 그중 시작부터 겹침 — 정확한 식으로 겹친 것은 정확한 식, 단정밀도로만 닿은 것은 엔진 IsDone 갈래(pawnTouchStart)
		uint64_t pawnOddSimplex = 0;		// 엔진 계산을 옮기지 않은 갈래를 탐 — 0 이어야 한다(Movement.cpp SweepPawnLikeChaos)
		uint64_t pawnTouchStart = 0;		// 사람 쓸기가 처음부터 여유 안(λ = 0)으로 끝나 엔진처럼 겹침 맞음으로 돌려준 횟수(고르기 전 — 막은 것만이 아님)
		uint64_t bullets = 0;
		uint64_t bulletsHitMap = 0;
		uint64_t bulletsHitBody = 0;
		uint64_t bulletsHitImmune = 0;		// 몸에 맞았지만 무적이라 피해 없음
		uint64_t bulletsMissed = 0;
		uint64_t damageEvents = 0;
		uint64_t kills = 0;					// 맞아서 죽음
		uint64_t assists = 0;				// 처치 도움(A-1) — 죽음마다 처치자를 뺀, 그 사람에게 피해를 준 사람 수
		uint64_t pickups = 0;				// 받침을 집음(B-2) — 겹침 시작으로 · 다시 검사로 둘 다
		uint64_t pickupsFromRecheck = 0;	// 그중 다시 켜진 뒤 다시 검사로 집은 것(CheckForExistingOverlaps)
		uint64_t pickupsFullHealth = 0;		// 켜진 받침에 닿았지만 체력이 가득 차 안 집음(GiveWeapon 이 false)
		uint64_t padRespawns = 0;			// 대기가 끝나 다시 켜짐
		uint64_t padRechecks = 0;			// 다시 켜진 뒤 서 있는 사람을 다시 본 횟수
		// 무기 받침(W-2) — 위 pickups · pickupsFromRecheck 는 체력 받침만 센다(회귀 「받침갈래」가 체력 회복 수와 맞춰 본다). 대기 · 다시 검사는 같은 규칙이라 위 둘이 같이 센다
		uint64_t weaponPickupsNew = 0;			// 없던 무기를 넣고 바로 듦
		uint64_t weaponPickupsNoSwitch = 0;		// 없던 무기 — 재장전 중이라 넣기만(IgnorePickUpWhenReload)
		uint64_t weaponPickupsAmmo = 0;			// 가진 무기 예비를 기본값까지 채움
		uint64_t weaponPickupsFull = 0;			// 가진 무기 예비가 이미 기본값 — 실패, 받침 안 꺼짐
		uint64_t weaponPickupsNoSlot = 0;		// 빈 칸 없음 — 무기 셋 · 칸 셋이라 0 이어야 한다
		uint64_t weaponPickupsPostGame = 0;		// 경기 끝 단계라 실패(GiveWeapon 의 IsPhaseActive(PostGame)) — 받침 안 꺼짐
		uint64_t weaponPickupsFromRecheck = 0;	// 집은 것(새 무기 · 탄) 중 다시 검사로 집은 것
		uint64_t weaponSwitchRejected = 0;		// 입력이 안 가진 무기를 고름 — 든 무기 그대로(빈 칸 키)
		uint64_t falls = 0;					// 떨어져 죽음
		uint64_t spawns = 0;
		uint64_t spawnsFromFallback = 0;	// 적이 없거나 자리가 모두 찜돼 무작위·대체 자리로
		uint64_t phaseChanges = 0;

		// 근접(컨텐츠 E-3) — 켬(meleeStarts) = 맞음 + 같은 팀 + 벽 + 빗나감 = 보낸 S2C_MELEE. 되감기 네 갈래 합도 켬
		uint64_t meleePresses = 0;			// 근접 단추가 든 입력
		uint64_t meleeStarts = 0;
		uint64_t meleeWhileActive = 0;		// 켜져 있어 버린 누름(bRetriggerInstancedAbility = False)
		uint64_t meleeBlockedPostGame = 0;	// 경기 끝 단계의 누름 — GE_BlockAbilityInput
		uint64_t meleeHits = 0;
		uint64_t meleeSameTeam = 0;
		uint64_t meleeBlocked = 0;			// 손 → 닿은 점 선이 벽에 막힘
		uint64_t meleeMisses = 0;
		uint64_t meleePushes = 0;			// 넣은 밀기 = 맞음
		uint64_t meleeHitImmune = 0;		// 맞았지만 무적 — 피해 0, 밀기 · 반응은 넣는다(제 판단, Lyra 기록 없음)
		uint64_t meleeDamageEvents = 0;		// 근접이 보낸 S2C_DAMAGE — damageEvents 에도 든다
		uint64_t meleeKills = 0;			// 근접으로 죽음 — kills 에도 든다
		uint64_t meleeTargets = 0;			// 판정에 넣은 몸 수(켬마다 그때 살아 있던 남)
		uint64_t meleePoses = 0;			// 그중 애니 자세로 판정한 몸 — 사격 자세 통계(poseTargets 등)와 따로 센다
		uint64_t meleePoseSkipped = 0;		// 그중 쓸기 근처에도 없어 자세를 안 구한 몸(캡슐은 그대로 본다)
		uint64_t meleeHandFallback = 0;		// 앞 틱 자세가 없어(애니 없음 · 그 틱 기록 없음) 권총 기본 자세 손으로 — 애니를 켠 판에서는 부활 첫 입력 말고 0 이어야 한다
		uint64_t meleeFiresBlocked = 0;		// 근접이 켜져 있어 막은 발사 누름
		uint64_t meleeReloadCancels = 0;	// 근접이 켜지며 끊은 재장전(탄이 안 들어감)
		uint64_t meleeReloadMontageCancels = 0;	// 근접 몽타주가 먼저 끈 재장전 칸(쏘기의 reloadCancels 와 같은 길)
		uint64_t meleeRewindNoView = 0;
		uint64_t meleeRewindFuture = 0;
		uint64_t meleeRewindClamped = 0;
		uint64_t meleeRewindOk = 0;

		// 대시(컨텐츠 D-2) — 누름(dashPresses) = 켬 + 방향 없음 + 쿨다운 + 켜져 있음 + 죽음 + 경기 끝. 갈래마다 따로 센다(0 이 「안 지나감」인지 가리려고)
		uint64_t dashPresses = 0;			// 대시 단추가 든 입력(서버가 만든 빈 입력 제외)
		uint64_t dashStarted = 0;
		uint64_t dashNoDirection = 0;		// 앞 입력 · 몸 요로 고른 방향이 0 — 쿨다운 없이 취소
		uint64_t dashOnCooldown = 0;
		uint64_t dashWhileActive = 0;
		uint64_t dashDead = 0;
		uint64_t dashPostGame = 0;			// 경기 끝 단계 — GE_BlockAbilityInput
		uint64_t dashFiresBlocked = 0;		// 대시가 앞 입력부터 켜져 있어 막은 쥔 사격 입력
		uint64_t dashReloadsBlocked = 0;	// 대시가 켜져 있어(켠 입력 포함) 막은 재장전 누름
		uint64_t dashMeleeBlocked = 0;		// 같은 까닭으로 막은 근접 누름
		uint64_t dashJumpsBlocked = 0;		// 같은 까닭으로 막은 점프 누름(버튼이 눌리는 입력)
		uint64_t dashAdsBlocked = 0;		// 같은 까닭으로 막은 조준 누름(버튼이 눌리는 입력)
		uint64_t dashFireCancels = 0;		// 켜는 입력에 끊은 사격 능력(그 입력에 쏜 발 포함)
		uint64_t dashReloadCancels = 0;		// 켜는 입력에 끊은 재장전(탄이 안 들어감)
		uint64_t dashMeleeCancels = 0;		// 켜는 입력에 끊은 근접
		uint64_t dashMeleePushRemoved = 0;	// 그중 밀기 힘이 있어 지움(RemoveRootForce — 이미 끝난 힘이면 아무것도 안 한다)
		uint64_t dashJumpCancels = 0;		// 켜는 입력에 끊은 점프(ReleaseJump)
		uint64_t dashAdsCancels = 0;		// 켜는 입력에 끈 조준
		uint64_t dashUncrouches = 0;		// 앉은 채 켜서 일어서기를 바람
		uint64_t dashLiftoffs = 0;			// 이동 계수 rootForceLiftoffs 가 그 입력 걸음에서 는 수(위를 보고 앞 대시 — 걷기 → 낙하)
		uint64_t dashMontage[4] = {};		// 켠 방향(EDashDir — 앞 · 뒤 · 왼 · 오른)
		uint64_t dashMsgs = 0;				// 보낸 S2C_DASH — started 와 같아야 한다(세는 자리를 따로 둬 보내는 길이 빠졌는지 가린다)

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

		// 되감기 — 네 갈래 합 = shots
		uint64_t rewindNoView = 0;
		uint64_t rewindFuture = 0;
		uint64_t rewindClamped = 0;
		uint64_t rewindOk = 0;
		uint64_t rewindTicksSum = 0;			// 되감은 틱 합 — 평균이 왕복 + 6틱 근처인지 본다
		// --rewindShadow 일 때만 — 같은 탄을 그 시점 자리와 현재 자리로 둘 다 계산. 넷 합 = bullets
		uint64_t hitsBoth = 0;
		uint64_t hitsRewindOnly = 0;
		uint64_t hitsNowOnly = 0;
		uint64_t hitsNeither = 0;
		uint64_t rewindOnlyBehindWall = 0;		// 되감기로만 맞았고, 쏜 자리에서 대상의 현재 몸 중심까지 맵이 가로막음

		// 애니 판정 — --clips 일 때만. 셋 합 = poseTargets
		uint64_t poseTargets = 0;				// 탄마다 판정 대상이 된 몸 수
		uint64_t poseEvals = 0;					// 자세를 새로 계산
		uint64_t poseReused = 0;				// 같은 틱에 같은 대상 · 같은 시점을 구해 둔 것을 씀
		uint64_t poseSkipped = 0;				// 탄이 몸 근처에도 안 와 자세를 안 구함
		uint64_t animUpdates = 0;				// 애니 상태 갱신 — 살아 있는 사람 × 틱. 입력이 몇 개 왔든 틱당 1회
		uint64_t poseEvalsByWeapon[(size_t)EWeapon::Count] = {};	// poseEvals 를 그 틱 자세 묶음(무기)별로. 합 = poseEvals
		uint64_t weaponChanges = 0;				// 입력이 든 무기를 바꿈
		uint64_t animBundleChanges = 0;			// 자세 묶음을 바꿈 — 틱마다 한 번이라 한 틱에 두 번 바꾸면 weaponChanges 보다 적다
		uint64_t animBundleFails = 0;			// 묶음 바꾸기 실패 — 시작할 때 세 묶음을 다 확인하므로 0 이어야 한다
		// 몽타주(9단계 조각 4) — 종류마다 사건 수 = 튼 수 + 버린 사건 몫. 교체 한 번은 빼기 · 꺼내기 둘을 튼다(9-1 기록)
		uint64_t animEvents[(size_t)EAnimEvent::Count] = {};		// 쌓은 사건(EAnimEvent 차례)
		uint64_t animEventsDropped[(size_t)EAnimEvent::Count] = {};	// 틀기 전에 버린 사건 — 같은 틱에 죽었거나 시험용 자리 옮기기 · 칸 넘침
		uint64_t montageStarts[(size_t)MONTAGE_KIND_COUNT] = {};	// 튼 몽타주(EMontageKind 차례)
		uint64_t montageFails = 0;				// 묶음에 그 종류가 없어 못 틈 — 시작할 때 세 묶음을 다 읽으므로 0 이어야 한다
		uint64_t reloadCancels = 0;				// 쏘기가 재장전 칸을 먼저 끔
		uint64_t montageDrops = 0;				// 칸이 차서 버린 몽타주(플랜 montageDropped). 0 이 아니면 칸 수를 다시 본다
		uint64_t stateBytesMax = 0;				// S2C_STATE 의 가장 큰 크기. 합치지 않고 최대를 남긴다(AddRoomStats)

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
		CRoom(const CMapCollision& map, const MoveParams& params, const MatchRules& rules, uint32_t seed, bool allowDebug, IRoomSender& sender, uint32_t roomId, const RewindOptions& rewind = RewindOptions(), const AnimOptions& anim = AnimOptions(),
			const PickupOptions& pickups = PickupOptions());

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
		// 부활 하나 + 입력마다 교체 · 쏘기 · 재장전 · 근접 + 남이 때린 근접 반응(근접 간격이 34 입력 넘게라 한 틱에 한 사람이 둘 못 때린다). 넘치면 버리고 센다
		static constexpr int kAnimEventsPerTick = 1 + 4 * kMaxInputTokens + (wire::kMaxPlayersInRoom - 1);

		struct QueuedInput
		{
			wire::Input input;
			wire::DebugPlace place;
			bool isPlace;
		};

		// 되감기 기록 한 칸. 애니 상태는 패킷과 같은 형식으로 둔다 — 서버 판정과 화면이 같은 수로 자세를 만들게(D32 · D35)
		struct RewindSample
		{
			uint32_t tick = 0;
			Vec3 location;
			float yaw = 0;
			float pitch = 0;
			bool alive = false;
			bool crouched = false;			// 그 틱 이동이 끝낸 앉음(D62 8) — 판정 자세의 앉은 묶음 · 메시 올림
			wire::AnimStateWire anim{};
			uint8_t animWeapon = 0;			// anim 을 만든 자세 묶음(무기 번호) — 판정은 이 묶음으로 푼다
		};

		struct Player
		{
			uint32_t id = 0;
			int64_t session = 0;
			uint8_t team = 0;
			MoveState move;
			WeaponCycles weapons;			// 무기마다 탄 · 순환. 가진 무기인지는 loadout 이 안다
			CLoadout loadout;				// 빠른 칸 · 든 칸(W-2) — 부활 · 입력 · 받침이 이 함수로만 바꾼다
			std::string loggedLoadout;		// 시험 출력 INVEV 가 마지막으로 찍은 가방(--debug 일 때만 쓴다)
			CWeaponFireGate fireGate;
			uint8_t weapon = 0;				// 든 무기 = loadout.Held(). 여기저기서 읽는 값이라 바꿀 때마다 따라 맞춘다
			float hp = 0;
			float yaw = 0, pitch = 0;
			int8_t moveX = 0, moveY = 0;	// 마지막 누름 방향 — 상태 패킷에 실어 보낸다
			JumpAbility jump;				// 점프 능력 — 버튼 · 켜짐. 대시가 누름을 막고 켜는 순간 끊는다(D-2)
			AdsAbility ads;					// 조준 능력(C-4) — 걸음 속도 · 카메라 · 애니 · STATE_ADS
			CAimCamera aimCamera;			// 사격 퍼짐이 읽는 조준 비율의 출처(C-1)
			CMeleeAbility melee;			// 근접 능력(E-3) — 입력 개수로 센다. 부활하면 꺼진 채
			uint16_t meleePushId = 0;		// 이번 근접이 넣은 밀기 힘 번호(0 = 없음) — 대시가 근접을 끊으면 지운다(D-2)
			CDashAbility dash;				// 대시 능력(D-2) — 켜짐 · 쿨다운을 입력 개수로 센다. 부활하면 꺼진 채
			bool dead = false;
			uint32_t respawnTick = 0;		// dead 일 때
			uint32_t immuneUntilTick = 0;
			int moveLockInputs = 0;			// 틱이 아니라 입력 개수로 센다 — 클라가 같은 입력을 돌려 보면 같은 결과가 나오게
			uint16_t kills = 0;
			uint16_t deaths = 0;
			uint16_t assists = 0;			// 처치 도움(A-1, ShooterCore AssistProcessor) — kills · deaths 와 같이 경기 시작에 0
			std::array<uint32_t, wire::kMaxPlayersInRoom> damagedBy{};	// 이 사람에게 0보다 큰 피해를 준 사람 번호(DamageHistory) — 죽을 때 도움을 주고 지운다
			int damagedByCount = 0;
			std::vector<uint8_t> padOverlap;	// 받침마다 지금 겹쳐 있나 — 겹침이 「시작」될 때만 집으므로 앞 상태를 든다. 죽으면 0(충돌이 꺼진다)
			uint32_t lastSeq = 0;
			uint32_t lastQueuedSeq = 0;
			uint32_t lastReceivedSeq = 0;	// 대기열에서 버린 입력도 센다 — 번호를 건너뛰었는지는 받은 번호로 본다
			int starvedInRow = 0;
			int tokens = 1;
			std::array<QueuedInput, kInputQueueSize> queue;
			int queueHead = 0;
			int queueCount = 0;
			std::array<RewindSample, kRewindLimitTicks + 1> rewindHistory;	// 틱 % 크기 자리. 한도 18틱 + 보간용 1칸
			CAnimState anim;
			uint8_t animWeapon = 0;			// anim 이 쓰는 자세 묶음. 무기를 바꾼 틱의 UpdateAnimStates 에서 weapon 을 따라간다
			struct AnimEvent
			{
				EAnimEvent kind;
				uint8_t weapon;				// 몽타주를 찾을 묶음(그 사건의 무기)
			};
			std::array<AnimEvent, kAnimEventsPerTick> animEvents;		// 이번 틱 사건 — 입력 · 부활에서 쌓고 UpdateAnimStates 가 비운다
			int animEventCount = 0;
		};

		// 몸 하나를 어느 시점 자세로 판정하나 — _bodies 와 같은 차례
		struct BodyPoseRef
		{
			const Player* owner;
			uint32_t tick;
			float alpha;
		};

		// 이번 틱에 구해 둔 자세 — 연사가 겹쳐도 같은 대상 · 같은 시점은 한 번만 계산한다
		struct CachedPose
		{
			uint32_t owner = 0;
			uint32_t tick = 0;
			float alpha = 0.f;
			std::vector<BodyPose> bodies;
		};

		struct PendingShot
		{
			uint32_t shooter;
			uint8_t weapon;
			Vec3 start;
			Vec3 aimDir;
			float spreadAngle;
			float spreadMultiplier;
			uint32_t viewTick;
			float viewAlpha;
			uint32_t seq;
		};

		// ALyraWeaponSpawner 하나의 상태 — bIsWeaponAvailable · CoolDownTimerHandle · CheckOverlapsDelayTimerHandle
		struct PadState
		{
			bool available = true;
			uint32_t readyTick = 0;		// 꺼져 있을 때 — 이 틱의 받침 단계에서 켜진다
			bool recheckPending = false;
			uint32_t recheckTick = 0;	// 이 틱의 받침 단계에서 서 있는 사람을 다시 본다
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
		void ApplyInput(Player& player, const wire::Input& input, bool serverMade = false);	// serverMade = 입력이 늦어 서버가 만든 빈 입력
		void ApplyPlace(Player& player, const wire::DebugPlace& place);
		void LogLoadouts();		// 시험 출력(--debug) — 가방(칸 · 든 칸 · 가진 무기 탄)이 바뀐 사람마다 INVEV 한 줄. Lyra 기록기 MOB_CPROBE_INV 와 같은 꼴
		bool IsStartOccupied(const StartSlot& slot) const;
		int ChooseStart(const Player& player);
		void SpawnPlayer(Player& player);
		void KillPlayer(Player& target, Player* attacker, uint8_t weapon, bool weakSpot);
		static void NoteDamage(Player& target, uint32_t attackerId);
		bool OverlapsPad(const MoveState& move, const PickupPad& pad) const;
		void UpdatePadOverlaps(Player& player);
		bool TryPickup(size_t padIndex, Player& player, bool fromTimer);
		bool TryWeaponPickup(size_t padIndex, Player& player, bool fromTimer);	// TryPickup 의 무기 받침 갈래(W-2)
		void TakePad(size_t padIndex, bool fromTimer);		// 집힌 받침을 끈다 — 체력 · 무기 공통(대기 틱 규칙)
		void TickPads();
		void ResetPads();
		// give · weapon = 무기 받침을 집었을 때 줍기 결과(EGiveResult 숫자)와 무기 — 칸 · 탄은 picker 가방에서 읽는다(W-3)
		void SendPad(size_t padIndex, wire::EPickupReason reason, const Player* picker, float heal, const Player* only, uint8_t give = wire::kPickupGiveNone, uint8_t weapon = 0);
		size_t PadCount() const { return _pickups.pads ? _pickups.pads->size() : 0; }
		bool IsImmune(const Player& player) const;
		Vec3 ResolveAimStart(const Player& player, const Vec3& locationBeforeMove, const Vec3& aim);
		bool TryMelee(Player& player, const wire::Input& input);	// 부르기 전에 melee.NextInput — 대시가 끊을지를 이번 입력 값으로 보려고 ApplyInput 이 앞에서 센다
		void CountDash(EDashResult result);
		void StartDash(Player& player, const DashStart& dash, uint32_t seq);	// seq = 켠 입력 번호(S2C_DASH 에 싣는다)
		bool MeleeHandOf(const Player& player, Vec3& out);
		void RecordRewindSamples();
		bool GetRewoundBody(const Player& player, uint32_t viewTick, float viewAlpha, BodyPlacement& out, float& outAlpha) const;
		void UpdateAnimStates();
		void PushAnimEvent(Player& player, EAnimEvent kind, uint8_t weapon);
		void DropAnimEvents(Player& player);
		void PlayAnimEvents(Player& player);
		void MakeMoveInput(const Player& player, AnimMoveInput& out) const;
		void PrepareBodyPoses(std::vector<BodyPlacement>& bodies, const std::vector<BodyPoseRef>& refs, const Vec3& start, const Vec3& end, double sweepRadius);
		const BodyPose* GetPoseAt(const Player& owner, uint32_t tick, float alpha, bool countStats = true);
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
		const RewindOptions _rewind;
		const AnimOptions _anim;
		const PickupOptions _pickups;
		std::vector<PadState> _padStates;	// _pickups.pads 와 같은 차례
		CMoveSim _moveSim;
		CSpreadRng _spawnRng;		// 시작 자리 고르기 전용. 탄 방향은 입력 번호로 정해져(SpreadSeed) 서버에 탄 난수 상태가 없다
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
		std::vector<BodyPlacement> _bodiesNow;	// --rewindShadow 일 때만 — 같은 탄을 현재 자리로 계산
		std::vector<Player*> _bodyOwners;
		std::vector<MeleeTarget> _meleeTargets;	// 근접 판정 때마다 다시 채운다
		std::vector<PawnCapsule> _pawnScratch;	// 사람끼리 막기 — 입력마다 남의 캡슐을 다시 채운다(할당을 안 하려고 멤버)
		std::vector<Player*> _meleeOwners;
		bool _animReady = false;
		std::array<CAnimPoseEvaluator, (size_t)EWeapon::Count> _poseEvaluators;	// 무기 번호 차례 — 묶음마다 하나
		std::array<CAnimState, (size_t)EWeapon::Count> _animCodecs;			// 받은 상태를 자세 입력으로 푸는 데만 쓴다 — 재생 상태는 안 쓴다
		std::array<int, (size_t)EWeapon::Count> _animBundleIndex{};			// 무기 번호 → 클립 파일의 묶음 차례(몽타주를 그 묶음에서 찾는다)
		std::vector<BodyPoseRef> _bodyPoseRefs;
		std::vector<BodyPoseRef> _bodyNowRefs;
		std::vector<CachedPose> _poseCache;
		size_t _poseCacheUsed = 0;
		std::vector<BoneTransform> _localFrom;
		std::vector<BoneTransform> _localTo;
		std::vector<BoneTransform> _localBlend;
		RoomResult _result;
		RoomStats _stats;
	};
}
