#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBridgeSubsystem.generated.h"

class FSocket;
class FFrameGrabber;
class UAbilitySystemComponent;
class AAIController;
class ACharacter;
class APawn;
class APlayerState;
class ULyraEquipmentInstance;
class ALyraWeaponSpawner;
struct FMOBCore;
struct FGameplayAbilityActorInfo;

namespace mo { namespace wire { struct PlayerState; struct AnimStateWire; struct Damage; struct Kill; struct Match; struct Shot; struct Pickup; struct Melee; } struct Vec3; }

// 남의 캐릭터 — 서버 상태를 틱 번호와 함께 쌓아 0.1초 뒤 시점을 보여 준다
struct FMOBSnapshot
{
	uint32 Tick = 0;
	FVector Pos = FVector::ZeroVector;
	FVector Vel = FVector::ZeroVector;
	float Yaw = 0, Pitch = 0;
	int8 MoveX = 0, MoveY = 0;
	bool bFalling = false;
	bool bCrouched = false;		// STATE_CROUCHED — 앉은 자세 풀기 · 캡슐 · 메시 올림(D-5-c)
};

struct FMOBRemote
{
	static constexpr int32 kSnapshots = 64;

	uint32 Id = 0;
	uint8 ServerTeam = 0;
	TWeakObjectPtr<AAIController> Ctrl;
	TWeakObjectPtr<ACharacter> Pawn;
	FMOBSnapshot Snaps[kSnapshots];
	uint32 SnapCount = 0;
	bool bSpawnFailed = false;
	bool bHasSet = false;
	FVector LastSet = FVector::ZeroVector;
	float LastSetYaw = 0;

	// 죽음 — 처치 패킷이 켜고, 상태 패킷이 살아났다고 하면 그 틱이 0.1초 뒤 시점에 닿을 때 새 캐릭터로 바꾼다
	bool bKilled = false;
	bool bRespawnPending = false;
	uint32 RespawnTick = 0;

	uint32 ShownNewestTick = 0;	// 지난 프레임에 이 상대를 놓을 때 받아 둔 가장 새 상태의 틱(-MOBViewLog 진단)

	double NextBodyTryAt = 0;		// 서버 애니(-MOBServerAnim) — 몸을 Quinn 으로 맞추는 재시도 시각
	int32 BodyTries = 0;

	uint8 Weapon = 0;
	uint8 ShownWeapon = 0xFF;
	TWeakObjectPtr<ULyraEquipmentInstance> Equipped;
};

// 내 근접 화면 판정 하나(E-3 17-c) — 서버 근접 소식과 입력 번호로 견준다
struct FMOBMeleeGuess
{
	uint8 Kind = 0;			// wire::EMeleeResult 차례(없음 · 같은 팀 · 벽 · 맞음)
	uint32 Target = 0;
	float ForceX = 0.f, ForceY = 0.f;	// 예측에 넣은 밀기 힘(맞음이 아니면 0)
	float Distance = 0.f;
};

/**
 * Lyra 와 매치 서버를 잇는다 — 입력을 서버로 보내고 서버 결과를 받는다. 판정은 전부 서버 몫.
 * 와이어 형식 = MO_Shooter/Shared/GameProtocol.h(봇과 같은 한 벌).
 * 플랜 = MyGit/MyDocs/claude-plans/mo-shooter/authoritative-server-plan.md
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함 — 평소 Lyra 그대로).
 *   -MOBServer=127.0.0.1:7791   서버 접속(접속하면 막기 스위치 둘을 켠다 — 끄려면 -MOBBlockDamage=0)
 *   -MOBAutoInput               사람 없이 정해진 입력을 넣는다(걷기·점프·사격·재장전·무기 바꾸기)
 *   -MOBAutoAim                 자동 입력이 보이는 적 중 가장 가까운 적을 겨눠 쏜다(맞은 표시·내 처치를 보려고)
 *   -MOBKeepPickups             맵의 무기·체력 줍기 받침을 Lyra 그대로 둔다(기본 = 무기 받침은 숨기고, 체력 받침은 서버 받침 소식 S2C_PICKUP 으로 그린다 — B-3)
 *   -MOBBreakAt=N               준비된 뒤 N초에 Lyra 체력·팀 점수·탄·내 도움(점수판 +2 · 센 값 +1)을 일부러 틀어 비교 코드가 잡는지 본다
 *   -MOBEventShots              적이 보일 때·맞혔을 때·처치·내 죽음에 사진(Saved/MOB/<tag>_<사건>_<N>.png)
 *   -MOBShotAt=10,15            준비된 뒤 N초에 캡처(Saved/MOB/<tag>_<N>.png)
 *   -MOBRecord=start,dur,fps    프레임 연속 캡처(사람 확인 영상용)
 *   -MOBQuitAt=N                준비된 뒤 N초에 끝냄(서버 경기 결과가 먼저 오면 그쪽)
 *   -MOBMaxFPS=N                시험 옵션. 화면 프레임 상한을 매 프레임 건다(Lyra 설정이 t.MaxFPS 를 덮어쓴다). 낮추면 한 프레임에 서버 틱이 여럿 지나 멈칫한 프레임을 흉내 낸다
 *   -MOBAbilityOwnTimer         시험 옵션. 내 사격 능력이 Lyra 자기 타이머 · 원본 난수로 쏜다(고치기 전 동작과 견줄 때만). 평소에는 예측이 쏜 입력에서 다리가 능력을 켜서 화면 탄이 서버 탄과 같은 번호 · 방향이다(D53)
 *   -MOBNoViewClamp             시험 옵션. 화면 시점 묶기를 끈다(고치기 전 동작과 견줄 때만). 평소에는 상태가 늦게 와 상대를 마지막 자리에 세워 둔 프레임에 그 상태의 틱을 화면 시점으로 보고한다(candidate1-pilot-plan.md 파일럿 2)
 *   -MOBTag=name                산출물 파일 이름 앞부분
 *   -MOBBreakPredictAt=N        일부러 틀리기 — 준비된 뒤 N초에 내 예측 위치를 1m 민다(보정이 잡는지 보려고)
 *   -MOBSelfBodyDelay=N         시험 옵션. 내 몸 Quinn 바꾸기(D85)를 캐릭터를 처음 본 뒤 N초 미룬다 — 총을 쥔 뒤 몸이 바뀌는 경우를 만든다(층이 빠진 채 남는지 보려고)
 *   -MOBSimpleAnim              화면 시험 — 상대를 Lyra 애니 대신 소총 클립 단순 재생으로 움직인다(MOBSimpleAnimInstance.h)
 *   -MOBStandAt=x,y,z           촬영 전용 — 내 입력을 보내지 않고 서버에 이 자리에 서 있겠다고만 알린다. 서버 --debug 필요(MOBRemoteShotSubsystem.h)
 *   -MOBStandLyra               -MOBStandAt 시험 전용 — 내 Lyra 몸도 그 자리에 둔다(평소엔 서버 몸만 옮겨지고 Lyra 몸은 시작 자리에 남는다). 점프대 · 순간이동 겹침을 내 몸으로 세려고(D91)
 *   -MOBStandHp=N               -MOBStandAt 의 부활 뒤 첫 자리 옮김 체력(받침 시험 — 다친 채 받침 위에 선다). 준 때만 그 뒤 20초마다 알림은 서버가 본 지금 체력으로 보낸다(조용히 체력을 바꾸지 않게)
 *   -MOBAutoMelee               자동 입력에 근접을 더한다 — 적이 4m 안이면 누르고 있고, 아니면 6초마다 0.1초 누른다. -MOBAutoAim 과 같이 켜면 적이 10m 안에 보여도 다가간다(6초 중 4초). 없으면 기존 자동 입력 그대로
 *   -MOBBreakMeleeBlock         일부러 틀리기 — Lyra 근접 능력의 입력 표식을 안 떼어 Lyra 가 키로 혼자 근접을 켠다(MOB_MELEE lyraSelf 가 늘어야 한다)
 *   -MOBBreakPadsAt=N           일부러 틀리기 — 준비된 뒤 N초에 묶인 받침 하나의 Lyra 켜짐을 뒤집고 · 다음 회복 효과를 건너뛰고 · 다음 받침 소식 하나를 버린다(check_pads_bridge.py 가 잡는지 보려고)
 *   -MOBBreakDevices            일부러 틀리기 — 점프대 · 순간이동의 겹침을 안 끈다(MOB_DEVICES overlap 이 늘어야 한다 — 끈 판의 0 이 「못 셌다」가 아님을 보려고)
 *   -MOBHumanCheck              사람 확인 자동화(MyDocs human-check-auto-plan.md) — 자동 입력 앞에 각본(샷건 조준 · 허리 사격, 앉기 · 앉은 채 점프, 무기별 근접 누른 채)을 돌리고
 *                               카메라 · 발 높이 · 앉은 채 점프 차례 · 근접 시작 간격 · 받침 대기 표시 · 받침 효과 줄(MOB_HC_*)을 남긴다. -MOBEventShots 면 그 순간마다 사진, -MOBQuitAt 이면 끝 3초 전 점수판을 연다
 * 판정에 쓰는 줄은 전부 「MOB_」 로 시작한다.
 */
UCLASS()
class UMOBridgeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

	// Lyra 능력이 탄을 쏠 때 — 서버와 같은 식으로 방향을 구한다(D15). 입력 번호 · 조준 · 퍼짐은 다리가 켠 사격의 것이다(D53)
	bool ComputeBulletDirection(const FVector& StartTrace, const FVector& AimDir, float SpreadAngle, float SpreadMultiplier, float SpreadExponent, int32 BulletIndex, FVector& OutDir);
	// 내 사격 능력의 문지기 — 다리가 켜는 중일 때만 연다(D53). 탄창이 비었으면 원래 검사로 넘긴다(빈총 동작)
	bool AllowAbilityActivation();

private:
	void ParseCommandLine();
	bool TryBecomeReady();
	void Connect();
	void PumpSocket();
	void FlushSend();
	void HandlePacket(const uint8* Data, int32 Size);
	void ApplySelfState(const mo::wire::PlayerState& P);
	void AddRemoteSnapshot(const mo::wire::PlayerState& P, uint32 ServerTick, const mo::wire::AnimStateWire* Anim);
	void ApplyServerAnim(FMOBRemote& R, ACharacter* Ch, const FMOBSnapshot& From, const FMOBSnapshot* To, float Alpha);
	void EnsureRemoteBody(FMOBRemote& R, ACharacter* Ch);
	void EnsureSelfBody(ACharacter* Ch);
	void LogViewParts(uint32 InSeq, uint32 Target, const ACharacter& Shown);
	void CheckEngineRays(uint32 InSeq, uint32 Target, const ACharacter& Shown);
	void SpawnRemote(FMOBRemote& R);
	void TickSelfVisual(double Now, float DeltaTime);
	void TickRemotes(double Now);
	void MeasureRemoteAnim(ACharacter* Ch, uint32 Id, const FVector& Vel, int8 MoveX, int8 MoveY, double Now);
	void ApplyServerHp(uint32 TargetId, uint32 AttackerId, float HpAfter);
	void HandleKill(const mo::wire::Kill& M);
	void HandleShot(const mo::wire::Shot& M);
	void ApplyMatch(const mo::wire::Match& M);
	void SyncTeamScores();
	void SyncSelfWeapon(const mo::wire::PlayerState& P);
	void SyncRemoteWeapon(FMOBRemote& R, ACharacter* Ch);
	void CompareHp(const AActor* Actor, float ServerValue);
	void LoadWeaponClasses();
	void ForceCountDown(int32 Seconds, int32 TargetScore);
	int32 LyraTeamOf(uint8 ServerTeam) const;
	APawn* FindPlayerPawn(uint32 Id) const;
	APlayerState* FindPlayerState(uint32 Id) const;
	void QueuePacket(const void* Pkt, int32 Size);
	void TickInput(double Now);
	void TickAbilityFire();
	void TickAbilityMelee();
	FMOBMeleeGuess JudgeMelee(mo::Vec3& OutPush);
	void TickAutoInput(double T);
	bool TickHumanScript(double T);		// -MOBHumanCheck 각본 — 이 프레임 입력을 맡았으면 true(끝났으면 false = 자동 입력으로)
	void SampleHumanCheck(double T);	// 매 프레임 — 각본 재는 칸의 카메라 · 발 높이, 앉은 채 점프 차례, 근접 뒤 내 몽타주, 끝 점수판
	void LogHumanCheck();				// 요약 — MOB_HC_* 줄
	void TickCapture(double T);
	void TickPickups(double Now);
	void FindDevices();		// 1초마다(Tick — 몸을 놓기 전) — 새로 올라온 점프대 · 순간이동을 찾아 겹침을 끈다(월드 파티션이 늦게 올리는 것까지)
	void TickDevices(double Now);		// 매 프레임 — 몸이 장치 겹침 칸에 든 프레임 · 그중 Lyra 가 겹침으로 본 프레임을 센다
	void HandlePickup(const mo::wire::Pickup& M);
	void HandleMelee(const mo::wire::Melee& M);
	ALyraWeaponSpawner* BindPad(int32 Index);
	void ApplyPadLook(ALyraWeaponSpawner& Actor, bool bAvailable, bool bEffects);
	float ApplyPadHeal(const mo::wire::Pickup& M, ALyraWeaponSpawner* Actor);
	void ApplyWeaponPickup(const mo::wire::Pickup& M, ALyraWeaponSpawner* Actor);	// 무기 받침 집음(W-4) — 내 몸이면 받침 그래프 GiveWeapon, 남의 몸이면 줍기 신호만
	FString DescribeLyraBag(int32* OutActive) const;	// Lyra 빠른 칸 셋의 무기 번호(빈 칸 -) — 로그 · 1초 대조용
	bool LyraBagMatches(uint8 PackedSlots, uint8 HeldWeapon) const;	// Lyra 빠른 칸 셋 · 든 칸이 서버 빠른 칸(와이어 quickSlots) · 든 무기와 같은가
	void SetLyraItemAmmo(uint8 WeaponIndex, int32 Magazine, int32 Spare);	// 빠른 칸의 그 무기 아이템 탄 — 없으면 안 함
	void TickWeaponScript(double T);		// -MOBWeaponScript 각본(W-4) — 이 프레임의 고른 칸 · 재장전 · 보낼 시험 자리 옮기기를 정한다
	void RequestEventShot(const TCHAR* Name, double Delay, int32 MaxCount);
	void TickRecord(double T);
	void DrainGrabber();
	void FinishAndQuit(const TCHAR* Why);

	UAbilitySystemComponent* FindASC(AActor* Actor) const;

	// 설정(명령줄)
	FString ServerAddr;
	TArray<float> ShotAt;
	float RecStart = -1, RecDur = 0, RecFps = 30;
	float QuitAt = -1;
	float MaxFpsTest = 0;		// -MOBMaxFPS
	FString Tag = TEXT("mob");
	bool bBlockDamage = true;
	bool bAutoInput = false;
	bool bStandOnly = false;
	bool bStandLyra = false;		// -MOBStandLyra
	FVector StandAt = FVector::ZeroVector;
	double NextStandAt = 0;
	bool bAutoAim = false;
	bool bEventShots = false;
	bool bKeepPickups = false;
	float StandHp = -1.f;			// -MOBStandHp — 0 이하 = 안 줌(늘 100)
	bool bStandPlaced = false;
	float BreakPadsAt = -1;
	bool bBreakPadHeal = false;		// 일부러 틀림 — 다음 회복 효과를 건너뜀
	bool bBreakPadDrop = false;		// 일부러 틀림 — 다음 받침 소식을 버림
	int32 BreakBag = 0;				// -MOBBreakBag=N 일부러 틀림(비트, W-4) — 1 = 다음 내 줍기에 받침과 다른 무기로 GiveWeapon · 2 = 다음 내 줍기를 예측에 안 넘김 · 4 = 다음 1초 대조 직전 든 칸 아이템을 뺌
	// 무기 각본(-MOBWeaponScript, W-4) — 서버 프로브 무기 각본(ServerProbe RunWeapons)과 같은 차례 · 길이를 프로브가 안 쓰는 받침 넷에서 돈다.
	// 자리 · 탄 · 죽음은 시험 입력(C2S_DEBUG_PLACE)으로, 칸 고르기 · 재장전은 보통 입력(키 1 · 2 · 3 · R 과 같은 길)으로
	bool bWeaponScript = false;
	int32 WsStep = -1;
	double WsStepStart = 0;
	int32 WsSlot = -1;				// 각본이 고른 빠른 칸 — -1 = 안 고름
	bool bWsPlacePending = false;	// 이 프레임 첫 입력 뒤에 보낼 자리 옮기기
	FVector WsPlaceAt = FVector::ZeroVector;
	uint8 WsPlaceFlags = 0, WsAmmoWeapon = 0xFF;
	int16 WsAmmoMag = 0, WsAmmoSpare = 0;
	int64 WsPlaces = 0;
	int32 WsPadIndex[4] = { INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE };	// 각본 받침 넷의 서버 받침 번호
	int64 WsNoPad = 0;				// 못 찾은 각본 받침 — 0 이어야 각본이 다 돈 것
	bool bWsReloadPending = false;	// 재장전 단추를 실은 입력이 아직 안 나갔다
	int64 WsReloadMark = 0;			// 누를 때의 InputsReload
	int32 AutoSlot = -1;			// 자동 입력이 고른 빠른 칸(-1 = 무기 번호 AutoWeapon 그대로 — -MOBAutoWeapon 고정)
	bool bSimpleAnim = false;
	bool bServerAnim = false;		// -MOBServerAnim — 남의 캐릭터 자세를 서버가 보낸 상태로 정한다(후보 1, D32)
	bool bAnimBonesChecked = false;
	bool bBreakMeleeBlock = false;	// -MOBBreakMeleeBlock — 일부러 틀림: Lyra 근접 능력의 입력 표식을 안 뗀다(Lyra 가 혼자 켜 lyraSelf 가 는다)
	bool bAutoMeleeOn = false;		// -MOBAutoMelee — 자동 입력에 근접을 더한다
	bool bBreakCrouchBlock = false;	// -MOBBreakCrouchBlock — 일부러 틀림: 앉기 막기 갈고리가 어긋남을 세기만 하고 안 되돌린다. Lyra 가 혼자 앉으며 캡슐을 옮겨 내 캐릭터 밀림(selfDriftOver1cm)이 늘어야 한다(D-5-c)
	bool bAbilityOwnTimer = false;	// -MOBAbilityOwnTimer — 내 사격 능력을 Lyra 자기 타이머 · 원본 난수로 둔다(고치기 전 동작과 견줄 때만, D53)
	bool bRayCheck = false;			// -MOBRayCheck — 같은 자세에 엔진 광선과 TraceBody 를 쏴 견준다(5단계)
	FString RayBreak;				// -MOBRayBreak=yaw|nothick — 이 대조가 틀림을 잡는지 볼 때만
	FRandomStream RayRng{ 7 };
	float BreakAt = -1;
	float BreakPredictAt = -1;
	bool bHumanCheck = false;		// -MOBHumanCheck — 사람 확인 자동화 각본 · 기록 줄

	// -MOBHumanCheck 상태(human-check-auto-plan.md). 각본 칸 표 = .cpp kHumanSteps
	int32 HcStep = 0;				// 지금 칸. 칸 수와 같으면 각본 끝 — 자동 입력으로 넘어간다
	double HcStepClock = 0;			// 이 칸에서 산 채 지난 초(죽은 동안은 안 센다)
	double HcLastT = -1;
	bool bHcStepBegun = false;
	bool bHcDead = false;
	int32 HcStepShots = 0;			// 이 칸에서 누른 사격 수
	double HcNextShotAt = 0;		// 이 칸 시계로 다음 사격 누름
	double HcFireUntil = -1;		// 이 칸 시계로 사격을 누르고 있을 끝
	double HcCrouchUntil = -1;		// 이 칸 시계로 앉기 단추를 누르고 있을 끝(입력은 고정 간격으로 나가 한 프레임 누름은 빠질 수 있다 — 자동 입력과 같은 0.1초)
	float HcHipYawOffset = 0.f;		// 허리 사격 칸만 벽 요에서 비켜 겨누는 각 — 조준 자국 무리와 안 겹치게(±8°, 벽 거리가 가운데와 가까운 쪽)
	// 다가가기 칸 — 길찾기 경로(0.5초마다 다시) · 경로를 얻은 수 · 적까지 거리(끝 줄 MOB_HC_APPROACH) · 칸 시작 때 남 캡슐 닿음 수(늘면 끝)
	uint64 HcApproachHits0 = 0;
	double HcNextPathAt = 0;
	TArray<FVector> HcPath;
	int32 HcNavPaths = 0;
	double HcApproachDist = -1;
	float HcYaw = 0.f;				// 벽을 보는 방향 — 구역 첫 칸에서 고른다
	double HcWallDist = -1;
	int32 HcRestarts = 0;			// 죽어서 구역을 다시 한 수
	struct FHcRange
	{
		int64 N = 0;
		double Sum = 0, Min = 0, Max = 0;
		void Add(double V) { Min = N ? FMath::Min(Min, V) : V; Max = N ? FMath::Max(Max, V) : V; Sum += V; ++N; }
	};
	FHcRange HcCam[2], HcFoot[2];	// [0] 선 채 · [1] 앉은 채 — 재는 칸에서 땅 위 · 앉음이 바뀐 지 0.5초 넘은 프레임의 (카메라 · 낮은 발) 높이 − 바닥
	int64 HcFootMissing = 0;		// 발 뼈(ball_l · ball_r)를 못 찾은 프레임
	bool bHcLastCrouched = false;
	double HcCrouchChangedAt = 0;
	uint64 HcCJumpPressFrame = 0;	// 앉은 채 점프를 누른 프레임(0 = 지켜보는 중 아님)
	double HcCJumpPressT = 0;
	int32 HcCJumps = 0;
	bool bHcCJumpCrouched = false;	// 누를 때 앉아 있었나
	int32 HcCJumpUncrouch = -1, HcCJumpLift = -1;	// 누른 뒤 몇 프레임에 일어섰나 · 떠올랐나(위 속도 > 0)
	TArray<uint32> HcMeleeSeqs[3];	// 각본 근접 칸에서 예측이 근접을 켠 입력 번호 — 무기(EWeapon)별
	int64 HcMeleeDriven[3] = {};	// 같은 칸에서 내 Lyra 근접 능력을 켠 수
	TMap<FString, int32> HcMyMontages;	// 내 근접 능력을 켜고 두 프레임 뒤 내 메시에서 돈 몽타주 이름별 수
	uint64 HcMontageReadFrame = 0;
	int64 AnimHitReactTicks = 0;		// 남의 상태 중 맞은 반응 몽타주 칸이 든 틱 수(mo::HasHitReactMontage)
	int64 PadPickupEffects = 0, PadRespawnEffects = 0;	// 받침 집음 · 다시 생김 효과를 튼 수(ApplyPadLook)
	double HcNextPadLogAt = 0;
	int32 HcMyPad = -1;				// 내가 마지막으로 집은 받침(다시 생김 사진용)
	uint64 HcLastPawnHits = 0;		// 예측이 남 캡슐에 닿은 수(Sim pawnHits) — 늘면 부딪힘 사진
	TMap<uint32, FString> HcRemoteMontageNow;
	TMap<uint32, float> HcRemoteMontagePos;	// 지난 프레임 몽타주 위치 — 같은 이름이 처음부터 다시 돌면(위치가 줄면) 새로 시작으로 센다
	uint32 HcMeleeTarget = 0;		// 내 근접이 맞힌 상대(서버 번호) — 1초 안에 그 상대 몽타주가 맞은 반응으로 새로 돌면 센다
	int32 HcMeleeHits = 0, HcMeleeReacts = 0;	// 남마다 지난 프레임 메시에서 돌던 몽타주(기본 모드의 맞은 반응은 Lyra 맞음 신호 GameplayCue.Character.DamageTaken 이 튼다)
	TMap<FString, int32> HcRemoteMontageStarts;	// 남 메시에서 새로 돌기 시작한 몽타주 이름별 수
	bool bHcBoardPressed = false;

	// 상태
	double ReadyAt = -1;
	double ResultAt = -1;
	double NextInputAt = 0;
	double NextLogAt = 0;
	bool bQuitting = false;
	int32 ShotIndex = 0;

	FSocket* Socket = nullptr;
	TArray<uint8> RecvBuf;
	TArray<uint8> SendBuf;		// 소켓이 다 못 받은 나머지 — 패킷 중간이 잘리면 서버가 스트림을 잃는다

	uint32 PlayerId = 0;
	bool bWelcomed = false;
	bool bHasSpawn = false;
	uint8 Team = 0;

	uint32 Seq = 0;
	uint32 AckSeq = 0;
	FMOBCore* Core = nullptr;	// 맵·이동 계산·내 예측 — 만들고 지우는 곳 = Connect / Deinitialize
	uint8 Weapon = 0;
	bool bReloadWasDown = false;
	bool bCrouchWasDown = false;

	// 내 캐릭터 화면 — 예측은 60Hz 로 끊겨 있어 직전·지금 사이를 메우고, 보정으로 생긴 차이는 서서히 없앤다
	FVector SelfPrev = FVector::ZeroVector;
	FVector SelfCur = FVector::ZeroVector;
	FVector VisualError = FVector::ZeroVector;
	FVector SelfLastSet = FVector::ZeroVector;
	bool bSelfSet = false;
	FName LastPawnName;

	// 내 몸도 Quinn(D85 — D33 연장). 캐릭터마다 한 번 닫는다: 몸이 Quinn 이고 무기 층이 여성형이면 끝
	TWeakObjectPtr<ACharacter> SelfBodyChecking;	// 지금 보고 있는 캐릭터 — 바뀌면 시도 수를 새로 센다
	bool bSelfBodyDone = false;
	double SelfBodyNextTryAt = 0;
	int32 SelfBodyTries = 0, SelfReequipTries = 0;
	int64 SelfBodyDonePawns = 0, SelfBodySets = 0, SelfReequips = 0, SelfBodyFails = 0;
	float SelfBodyDelay = 0;			// -MOBSelfBodyDelay (시험)
	double SelfEmptyLogAt = 0;			// 층이 빈 채 기다리는 줄 — 1초에 한 줄
	int32 SelfEmptyLogs = 0;			// 캐릭터마다 그 줄 수(상한 20)
	int64 SelfBodyEmptyFrames = 0;		// Quinn 몸인데 층이 빈 채 기다린 프레임 합
	double SelfEmptySince = 0;			// 층이 비기 시작한 때(0 = 비어 있지 않음) — 쥔 칸이 있는데 1초 넘으면 다시 쥔다(D88)

	TMap<uint32, FMOBRemote> Remotes;
	double TickClockOffset = 0;		// 서버 틱 - 내 시계 * 60
	bool bHasTickClock = false;

	// 자동 입력이 고른 값 — 키보드 대신
	bool bAutoFire = false;
	bool bAutoAds = false;
	bool bAutoSeesEnemy = false;
	bool bNamedSelf = false;
	double NextPickupCheckAt = 0;
	int32 PickupsHidden = 0;
	// 점프대(B_Launcher_*) · 순간이동(B_Teleport) — 서버에 안 넣고 Lyra 동작만 끈다(사용자 결정 11-18, design-decisions.md D91).
	// 장치 그래프는 겹침 시작에서만 돈다(LaunchCharacter · K2_TeleportTo · 효과 큐) → 장치 쪽 겹침을 끄면 안 돈다. 바닥 · 불빛 · 막는 충돌은 그대로
	struct FMOBDevice
	{
		TWeakObjectPtr<AActor> Actor;
		bool bTeleport = false;
		TArray<TWeakObjectPtr<class UPrimitiveComponent>> Shapes;		// 겹침을 켜고 있던 칸(구 · 상자) — 몸이 든 프레임을 셀 자리
	};
	TArray<FMOBDevice> Devices;
	bool bBreakDevices = false;		// -MOBBreakDevices — 일부러 틀림: 겹침을 안 끈다
	double NextDeviceCheckAt = 0;
	int32 DeviceShapesOff = 0;		// 겹침을 끈 칸 수
	int32 DeviceLightsOff = 0;		// 점프대 바닥 조명(PointLight)을 끈 수
	int32 DeviceGlowOff = 0;		// 점프대 발판 빛 재질(Intensity 칸이 있는 재질)을 0 으로 둔 수
	int32 DeviceSounds = 0;			// 점프대에 붙은 소리 · 효과 부품 수 — 원본 청사진엔 없다(0 이어야 한다). 있으면 끄고 센다
	int32 DeviceFx = 0;
	int64 DeviceInsideSelf = 0;		// 내 몸 캡슐이 장치 칸 상자에 닿은 프레임(지나간 양)
	int64 DeviceInsideRemote = 0;	// 남의 몸 — 같은 셈(몸 하나 × 프레임)
	int64 DeviceOverlapSelf = 0;	// 그중 Lyra 가 겹침으로 본 프레임 — 끈 판은 0 이어야 한다
	int64 DeviceOverlapRemote = 0;
	// 체력 받침(B-3) — 칸 = 서버 받침 번호. 상태는 서버 소식 그대로, 액터는 자리로 찾아 묶는다
	struct FMOBPad
	{
		bool bKnown = false;			// 서버 소식을 한 번이라도 받음
		bool bAvailable = true;
		uint32 ReadyTick = 0;
		uint32 CooldownTicks = 0;
		FVector Location = FVector::ZeroVector;
		TWeakObjectPtr<ALyraWeaponSpawner> Actor;
		uint8 Kind = 0;					// 소식 kind = EPadKind 숫자(W-4) — 체력이면 회복, 무기면 가방
	};
	TArray<FMOBPad> Pads;
	// msgs = 받은 받침 소식(takes · readies · syncs = 까닭별), badIndex = 번호가 칸 수 밖, noActor = 받을 때 맵 받침을 못 찾은 소식,
	// bound = 맵 받침을 서버 받침에 묶은 수(Lyra 가 받침을 새로 놓아 다시 묶은 것 포함), heals = 받침 회복 효과(GiveWeapon)를 건 수,
	// healBeforeFix = 회복 전 Lyra 체력이 서버 값(회복 뒤 - 회복량)과 달라 먼저 맞춘 수, healMismatch = 회복 효과 뒤 체력이 서버 회복 뒤 체력과 다른 수(그때 서버 값으로 맞춤),
	// healNoPawn = 집은 사람 몸이 없거나 죽어 있던 수, stateChecks · stateDiff = 1초마다 묶인 받침의 Lyra 켜짐 · 구슬 보임 · 충돌 꺼짐을 서버 상태와 견준 수 · 다른 수(그때 서버 상태로 다시 그림)
	int64 PadMsgs = 0, PadTakes = 0, PadReadies = 0, PadSyncs = 0, PadBadIndex = 0, PadNoActor = 0, PadBound = 0;
	int64 PadHeals = 0, PadHealBeforeFix = 0, PadHealMismatch = 0, PadHealNoPawn = 0, PadStateChecks = 0, PadStateDiff = 0;
	int32 PadDiffLinesLeft = 10;
	// 무기 받침(W-4) — takesMine · takesOther = 무기 받침 집음 소식(내 것 · 남의 것), gives = 내 몸에 받침 그래프 GiveWeapon 을 부른 수,
	// giveDiff = 그 뒤 Lyra 빠른 칸 · 든 칸 · 그 무기 탄이 서버 소식과 다른 수(탄은 그 자리에서, 칸은 그 틱 상태의 가방 맞추기가 서버 값으로 고친다),
	// cues = 남의 몸에 줍기 신호를 튼 수, noPawn = 집은 사람 몸 · 가방 · 맵 받침이 없던 수, bad = 줍기 칸이 빈 집음 소식(무기 번호 밖)
	int64 PadWeaponTakesMine = 0, PadWeaponTakesOther = 0, PadWeaponGives = 0, PadWeaponGiveDiff = 0, PadWeaponCues = 0, PadWeaponNoPawn = 0, PadWeaponBad = 0;
	int32 WeaponGiveLinesLeft = 30;
	// 내 가방 맞추기(W-4) — states = 맞춰 본 상태, diffStates = 맞추기 전 Lyra 빠른 칸(무기 아이템 · 칸 밖 무기 아이템)이 서버 빠른 칸과 달랐던 상태,
	// adds · removes = 칸에 넣고 뺀 아이템, strays = 빠른 칸 밖에 남은 무기 아이템을 지운 수, check1s · diff1s = 1초마다 Lyra 빠른 칸 · 든 칸을 마지막 서버 상태와 견준 수 · 다른 수
	int64 BagStates = 0, BagDiffStates = 0, BagAdds = 0, BagRemoves = 0, BagStrays = 0, BagCheck1s = 0, BagDiff1s = 0;
	int32 BagDiffLinesLeft = 30;
	uint8 LastServerQuickSlots = 0;
	uint8 LastServerHeld = 0xFF;	// 마지막 살아 있는 상태의 든 무기 — 0xFF = 아직 없음 · 죽음
	double LastSelfSpawnAt = -1.0;	// 내 부활 소식을 받은 시각(FPlatformTime) — 가방 다름 줄이 부활 뒤 몇 초인지 찍는다
	// 예측 ↔ 서버 가방 · 탄(W-4, 봇 CompareSelf 와 같은 식) — invCompared = 견준 상태, invMulti = 그중 서버가 무기 둘 이상 든 줄,
	// invDiff = 빠른 칸 다름, ammoDiff = 든 무기 · 탄창 · 예비 다름(경기 끝 단계 전만 — 그 단계는 서버가 사격 입력을 지운다)
	int64 InvCompared = 0, InvMulti = 0, InvDiff = 0, AmmoCompared = 0, AmmoDiff = 0;
	int32 InvDiffLinesLeft = 20;
	uint32 LastStateTick = 0;		// 마지막으로 받은 상태의 틱 — 받침 소식은 그 틱의 상태보다 먼저 나가므로, 이 틱까지의 서버 사건은 다 받았어야 한다
	int32 HpDiffLinesLeft = 20;
	int64 HpFixes = 0;
	double InputRawMax = 0, InputSentMax = 0;
	int64 InputsDiagonal = 0;
	int64 BulletsLyra = 0, BulletsServer = 0, BulletsLyraImmune = 0, BulletsServerImmune = 0, BulletsLyraOther = 0, BulletsServerOther = 0;
	int32 LastSyncMag = -1;

	// 내 사격 능력 구동(D53) — 예측이 쏜 입력을 쌓아 두고, 능력이 쉬고 있으면 쌓인 것 중 가장 오래된 하나로 켠다
	struct FMOBPendingFire
	{
		uint32 Seq = 0;
		uint8 Weapon = 0;
		float SpreadAngle = 0, SpreadMultiplier = 0;
		FVector AimDir = FVector::ZeroVector;	// 입력의 요 · 피치로 구한 조준(서버와 같은 식)
		FVector AimFrom = FVector::ZeroVector;	// 입력에 실은 조준 시작점 = 서버 탄의 시작점
		uint64 Frame = 0;						// 쌓은 화면 프레임(GFrameCounter)
	};
	TArray<FMOBPendingFire> PendingFires;
	FMOBPendingFire ActiveFire;					// 마지막으로 켠 사격 — 탄 방향 훅이 읽는다
	bool bActiveFireValid = false;
	bool bAbilityActivating = false;			// 다리가 능력을 켜는 중(이때만 문지기가 연다)
	// queued = 예측이 쏜 발, driven = 그 발로 능력이 켜진 수, activateFail = 켜기 실패, noSpec = 사격 능력이 없어 버린 발, dropped = 너무 밀려 버린 발,
	// waitFrames = 능력이 아직 켜져 있어 기다린 프레임, weaponMismatch = 켤 때 Lyra 무기(서버 상태로 맞춘 무기)가 그 발의 무기와 다른 수, gateBlocks = 문지기가 막은 수(Lyra 입력이 프레임마다 켜 본다),
	// gateDryFire = 탄창이 비어 문지기가 원래 검사로 넘긴 수(탄 부족으로 막히며 빈총 몽타주 · 문구가 난다),
	// bullets = 훅이 방향을 정한 탄, bulletsOutside = 그중 다리가 켜는 자리 밖에서 쏜 탄, lag = 쌓은 프레임에서 켠 프레임까지 0 · 1 · 2 · 3 이상
	int64 FireQueued = 0, FireDriven = 0, FireActivateFail = 0, FireNoSpec = 0, FireDropped = 0, FireWaitFrames = 0, FireWeaponMismatch = 0, FireGateBlocks = 0, FireGateDryFire = 0, FireBullets = 0, FireBulletsOutside = 0;
	int64 FireLagFrames[4] = {};
	double FireLyraAimMaxDeg = 0, FireStartDiffMax = 0;	// 능력이 잰 조준 · 시작점과 그 발 입력의 차이 최대(그림에는 입력 쪽을 쓴다)
	uint8 LastSyncWeapon = 0xFF;
	int32 AimFarLinesLeft = 8;
	int64 AimFarLocal = 0;
	double AimDistMax = 0, AimHorizMax = 0;
	// 시작점 거리 훑기(C-5, 서버 허용 거리 다시 재기) — 안 쏜 입력도 「이 입력에 쐈다면」의 시작점으로 센다. [0] = 조준 안 누름 · [1] = 누름, 칸 = 카메라 시선 높낮이 10도씩(−90~90)
	bool bAutoPitchSweep = false;
	int32 InputsSinceSpawn = 0;
	int64 AimSweepSkipped = 0;
	// 칸 번호 = 조준(0 · 1) + 앉음이면 2(D-6-a — 앉으면 카메라가 내려가 시작점 거리가 바뀐다)
	int64 AimSweepInputs[4][18] = {};
	double AimSweepDistMax[4][18] = {}, AimSweepHorizMax[4][18] = {};
	// 앉거나 선 직후 0.25초를 따로 — Lyra 카메라는 앉는 순간 받침점을 그대로 두고(LyraCameraMode.cpp GetPivotLocation, 캡슐 반높이 차를 더한다)
	// 앉은 눈높이로 0.2초에 걸쳐 섞어 내린다(LyraCameraMode_ThirdPerson.cpp UpdateCrouchOffset, 배수 5). 그동안 시작점이 캡슐 중심보다 서 있을 때보다 최대 25 높다(D-6-a). [0] = 섬 · [1] = 앉음
	bool bSweepCrouched = false;
	double SweepCrouchChangedAt = -1.0;
	int64 AimSweepFreshInputs[2] = {};
	double AimSweepFreshMax[2] = {}, AimSweepSteadyMax[2] = {};
	double AimSweepPitchMin = 0, AimSweepPitchMax = 0;
	// 조준 키 → Lyra 최대 속도(C-5 사람 판, D61 미검증 닫기) — 키가 바뀐 프레임부터 Lyra 이동 최대 속도가 따라 바뀌기까지 몇 프레임인가.
	// [0] = 뗌 · [1] = 누름, 칸 = 0 · 1 · 2 프레임 늦음 · 3 = 3프레임 안에 안 바뀜(죽음 · 능력 막힘 등)
	bool bPrevAdsKey = false;
	bool bAdsKeyWantOn = false;
	int32 AdsKeyWatchFrame = -1;		// -1 = 안 지켜봄
	float AdsKeyBaseSpeed = 0.f, PrevMaxSpeed = 0.f;
	int64 AdsKeyLag[2][4] = {};
	bool bAutoReloadPressed = false;
	bool bJustReloaded = false;
	int32 MagDiffMax = 0;
	int64 MyShots[3] = { 0, 0, 0 };
	int64 ReloadStarts[3] = { 0, 0, 0 };
	int64 ReloadDones[3] = { 0, 0, 0 };
	bool bWasReloading = false;
	uint8 ReloadWeapon = 0;
	int32 ReloadMagBefore = 0;
	double NextSeeShotAt = 0;
	TMap<FString, int32> EventShotCounts;
	TArray<TPair<double, FString>> PendingEventShots;
	bool bAutoJump = false;
	bool bAutoReload = false;
	bool bAutoCrouch = false;
	bool bAutoMelee = false;
	uint8 AutoWeapon = 0;
	int32 AutoWeaponFixed = -1;		// -MOBAutoWeapon=N — 자동 시험의 무기를 고정한다(-1 = 10초마다 바꿈). 무기별 대조에 쓴다

	// 관측값
	int64 InputsSent = 0, InputsSkipped = 0;
	int64 InputsFire = 0, InputsJump = 0, InputsReload = 0, InputsWeaponChange = 0;
	// 근접(E-3 17-c) — 단추를 실은 입력 · 화면 판정 · 화면 판정 결과(없음 · 같은 팀 · 벽 · 맞음) · 서버가 보낸 내 근접 결과(같은 넷) · 그중 화면과 결과가 다름 ·
	// 결과는 같은데 대상이 다름 · 소식에 맞는 화면 판정이 없음 · 남의 근접 소식. 힘 차이 최대 = 화면 판정 밀기 ↔ 서버 밀기
	int64 InputsMelee = 0, MeleeJudged = 0, MeleeScreen[4] = {}, MeleeMine[4] = {}, MeleeKindDiff = 0, MeleeTargetDiff = 0, MeleeNoGuess = 0, MeleeOthers = 0;
	double MeleeForceDiffMax = 0;
	// Lyra 근접 능력 — 예측이 켠 근접(켤 차례로 둠) · 켬 · 켜기 실패 · 능력 칸 없음 · 기다리다 버림 · 기다린 프레임 · 다리가 안 켰는데 켜짐(Lyra 혼자) · 입력 표식을 뗀 칸
	int64 MeleeQueued = 0, MeleeDriven = 0, MeleeActivateFail = 0, MeleeNoSpec = 0, MeleeDropped = 0, MeleeWaitFrames = 0, LyraMeleeSelf = 0, LyraMeleeStripped = 0;
	bool bMeleePending = false;
	uint32 MeleePendingSeq = 0;
	uint64 MeleePendingFrame = 0;
	bool bLyraMeleeWasActive = false;
	float MeleeYaw = 0.f;		// 서버 player.yaw = 마지막으로 적용한 입력의 요(부활 직후는 시작 자리 요) — 다음 근접의 몸 앞(봇 _meleeYaw 와 같다)
	int32 MeleeLinesLeft = 60;	// 근접 하나씩 줄(MOB_MELEE_ONE) 남은 수
	// 보정 하나씩 줄(MOB_CORR) — 보정이 무엇 뒤에 났나 가르려고: 남이 나를 친 근접 · 내가 친 근접 · 남 캡슐 닿음(사람 확인 자동화 ha2 의 보정 최대 6.00cm)
	int32 CorrLinesLeft = 80;
	double LastMeleeOnMeAt = -1, LastMyMeleeHitAt = -1;
	uint64 CorrLastPawnHits = 0;
	int32 DriftLinesLeft = 20;	// 내 밀림 1cm 넘는 프레임 줄(MOB_SELF_DRIFT) 남은 수
	TMap<uint32, FMOBMeleeGuess> MeleeGuesses;	// 입력 번호 → 화면 판정
	int64 InputsCrouch = 0, CrouchComparedOn = 0, CrouchFlagDiff = 0;	// 앉기를 누른 입력 · 서버가 앉음으로 보낸 비교 줄 · 그중 내 예측과 어긋남(D-5-c)
	int64 SelfCrouchChanges = 0, RemoteCrouchChanges = 0, RemoteLiftFrames = 0;	// 내 · 남의 캐릭터 앉음을 바꾼 수 · 남의 메시를 올린 프레임 수(D-5-c)
	int64 InputsAds = 0, AdsComparedOn = 0, AdsFlagDiff = 0;	// 조준을 누른 입력 · 서버가 조준 켜짐으로 보낸 비교 줄 · 그중 내 예측과 어긋남(C-4)
	int64 StatesRecv = 0, StatesWithoutMe = 0, ShotsRecv = 0, DamagesRecv = 0, MatchesRecv = 0, KillsRecv = 0, SpawnsRecv = 0;
	int64 UnknownPackets = 0, SendShort = 0;
	int32 MaxAckLag = 0;
	double PosDiffSum = 0, PosDiffMax = 0, VelDiffMax = 0;
	int64 PosDiffCount = 0, PosDiffNonZero = 0;
	int64 Corrections = 0, VisualSnaps = 0, PawnChanges = 0;
	double CorrectionSum = 0, CorrectionMax = 0;
	double SelfDriftMax = 0;
	int64 SelfDriftOver1cm = 0;
	double MaxFrameGap = 0;
	int32 RemoteSpawned = 0, RemoteSpawnFail = 0;
	int64 RemoteFrames = 0, RemoteUnderrun = 0, RemoteTeleports = 0, RemoteDriftOver1cm = 0;
	int64 AnimTailPlayers = 0, AnimTailBad = 0, AnimNoState = 0, AnimPoseEvals = 0, AnimPoseFrames = 0, AnimPoseFail = 0;
	int64 AnimPoseEvalsByWeapon[3] = {};	// 무기 번호(EWeapon) 차례 권총 · 소총 · 샷건 — 무기 수와 같은지 .cpp 에서 정적 검사
	int64 AnimSetChanges = 0, AnimSetMisses = 0, AnimBadWeapon = 0;
	int64 AnimAdsLayers = 0, AnimBadLayers = 0;	// 언리얼 경로에서 조준 전환 층을 허리 전환으로 바꿔 튼 수 · 칸 밖이라 버린 층 수(C-5)
	int64 AnimCrouchLayers = 0, AnimStanceLayers = 0;	// 같은 경로에서 앉은 전환 층을 허리 전환으로 바꿔 튼 수 · 서기 안 전환 층을 걷기 · 서기 자리로 돌린 수(D-3)
	int64 AnimMontageCellsIn = 0, AnimMontageCells = 0, AnimMontageMisses = 0, AnimMontageFrames = 0, AnimSlotPasses = 0, AnimMontageNoSlot = 0, AnimMaskMissing = 0, AnimWeaponNotifies = 0, AnimWeaponPlaying = 0, AnimSoundNotifies = 0, AnimMontageRestarts = 0, AnimStepStates = 0;	// 몽타주(9단계 5-a)
	struct FMOBRayStats { int64 Rays = 0, Map = 0, EngHit = 0, Same = 0, EngOnly = 0, MineOnly = 0, Both = 0, BoneSame = 0, EngWeak = 0, WeakLost = 0, WeakGain = 0; };
	FMOBRayStats RayStats[2][2];		// [선 · 구 5.5][몸 쪽 · 머리 쪽 겨눔]
	int64 RayNoBody = 0, RayDiffLogged = 0;
	// -MOBAnimLog 진단 — 남의 캐릭터가 움직이는 프레임에 애니가 무슨 상태였나
	bool bViewLog = false;		// -MOBViewLog 쏠 때마다 상대 화면 자리를 찍는다. 서버 REWIND 줄과 대조
	double RemotePitchWantMax = 0;		// -MOBViewLog — 상대에게 넣은 위아래 시선의 최대
	double RemotePitchSeenMax = 0;		// -MOBViewLog — 다음 프레임에 애니가 읽는 시선으로 남아 있던 값의 최대
	double ShownRenderTick = -1.0;	// 지난 프레임에 상대를 놓은 시점(틱). 입력의 viewTick·viewAlpha 로 나간다
	double ShownWallTick = -1.0;	// 같은 프레임에 시계로 계산한 시점. 묶기가 걸리지 않은 프레임에는 ShownRenderTick 과 같다
	bool bViewClamp = true;			// 기본 켜짐. -MOBNoViewClamp 로 끈다
	int64 ViewClampFrames = 0;		// 받은 상태가 시계 시점보다 늦어 시점을 당긴 프레임 수
	bool bAnimLog = false;
	double NextAnimLogAt = 0;
	int64 AnimMoving = 0, AnimMovingRendered = 0, AnimMovingNoInput = 0, AnimMovingNoAccel = 0;
	TMap<FName, int64> AnimStates;
	double RemoteDriftMax = 0, RemoteYawDiffMax = 0, RemoteLagSum = 0, RemoteBehindSum = 0;
	float ServerHp = -1.f;
	int16 ServerMagazine = -1, ServerSpare = -1;
	uint8 ServerFlags = 0, ServerWeapon = 0, MatchPhase = 0;
	uint16 ScoreA = 0, ScoreB = 0;
	double LastServerPosX = 0, LastServerPosY = 0, LastServerPosZ = 0;
	int64 DamageApplied = 0, DamageMismatch = 0, DamageNoTarget = 0, HpCompared = 0, HpDiffCount = 0;
	double HpDiffMax = 0;
	int64 SelfRespawns = 0, RemoteRespawns = 0, ScoreFixes = 0, AmmoFixes = 0, WeaponSwaps = 0, RemoteEquips = 0, PhaseChanges = 0, ShotCues = 0, HitMarkers = 0, CountDownSets = 0;
	int32 LastCountDownShown = -1;
	// 도움(A-3) — 처치 패킷의 도움 번호로 센 사람별 도움 수. Lyra AssistProcessor 가 혼자 센 점수판 값을 처치마다 이 값으로 덮는다
	TMap<uint32, int32> ServerAssists;
	int64 AssistsRecv = 0, AssistFixes = 0, AssistResultPlayers = 0, AssistResultLyraDiff = 0, AssistResultCountDiff = 0;
	int64 AssistResultLyraSeen = 0, AssistResultLyraSum = 0;	// 결과 대조에서 Lyra 사람 정보를 찾은 수 · 그 점수판 도움 합 — diff 0 이 「못 읽음」이 아님을 보이는 지나간 양
	void SyncAssists();
	bool bLoggedScoringMissing = false;
	bool bPostGamePrepared = false;
	int32 LyraScoreA = -1, LyraScoreB = -1;

	// Lyra 무기 — 인덱스는 mo::EWeapon 순서
	UPROPERTY()
	TArray<TObjectPtr<UClass>> WeaponItemDefs;
	UPROPERTY()
	TArray<TObjectPtr<UClass>> WeaponEquipDefs;
	uint8 HitMarkerId = 0;

	// 영상 — 엔진 비동기 프레임 수집기(시퀀서 녹화가 쓰는 것). 매 프레임 PNG 캡처는 초당 6~11프레임으로 게임을 끌어내렸다
	FFrameGrabber* Grabber = nullptr;	// 만들고 지우는 곳 = TickRecord
	FArchive* RawWriter = nullptr;
	int32 GrabbedFrames = 0;
	double GrabFirstAt = 0, GrabLastAt = 0;
	FIntPoint GrabSize = FIntPoint::ZeroValue;
	bool bGrabDone = false;
};
