#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayTagContainer.h"

#include "MOBContentProbeSubsystem.generated.h"

class ACharacter;
class ALyraWeaponSpawner;
struct FLyraAbilitySimpleFailureMessage;
struct FLyraVerbMessage;
struct FAbilityEndedData;
class UGameplayAbility;
class UAbilitySystemComponent;
struct FGameplayEffectSpec;
struct FActiveGameplayEffect;
struct FActiveGameplayEffectHandle;
class AController;
class APawn;

/**
 * 컨텐츠 확인 기록 — 조준 · 앉기 · 근접 · 체력 받침의 Lyra 실행값을 정해진 각본으로 찍는다. 서버 없이 돈다(content-add-plan.md 0-3).
 * 에셋에서 읽은 값(content-add-progress.md 0-3 표)이 실제 실행에서도 같은지 보는 것이 목적이다. 판정 대조용 정밀 기록은 각 단계의 기록 도구가 맡는다.
 * 명령줄 -MOBContentProbe=<tag> 일 때만 켜진다. -MOBCPMode=gym(기본, 맵 L_ShooterGym?NumBots=1) | pads(맵 L_Expanse?NumBots=0)
 * | bridge(내 서버에 붙은 지금 클라 — run_game.ps1 의 LyraArgs 로 넘긴다. 0-2 「지금 키를 누르면 무슨 일이 나나」)
 * | weapons(맵 L_Expanse?NumBots=0 — 무기 받침 · 가방 · 재장전 태그, weapon-pickup-plan.md W-0)
 * | dash(맵 L_ShooterGym?NumBots=1 — 대시 방향 · 힘 · 막기 · 끊기 · 쿨다운, dash-plan.md D-0)
 * | grenade(맵 L_ShooterGym?NumBots=1 — 수류탄 방향 · 날아감 · 튕김 · 터짐 · 폭발 판정 · 쿨다운, grenade-plan.md G-0)
 * 산출물 = Saved/MOB/cprobe_<tag>.csv(매 프레임 한 줄) · 로그의 MOB_CPROBE 줄(사건마다 요약)
 */
UCLASS()
class UMOBContentProbeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

	// 각본 한 칸이 하는 일. 칸이 시작할 때 한 번 한다(Melee 는 칸 안 0.3초에 누른다, MeleeSpam 은 칸 내내 누른다)
	enum class EAct : uint8 { None, Home, AdsPress, AdsRelease, CrouchToggle, Equip, FoeSetup, FoeSameTeam, MeleeAt, MeleeAtBack, MeleeAtSide, MeleeHere, MeleeSpam, PadsDump, PadOn, Away, SetHealth, Watch, MeleeJumpAt, BumpStillAt, BumpAt, BumpStillSideAt, BumpSideAt, FoeCrouchToggle,
		WeaponPadOn, SelectSlot, SetAmmo, ReloadPress, SelfDestruct,
		// 대시 판(D-0) — 대시는 칸 안 DASH_PRESS_AT 에 한 프레임 누른다(DashSpam 은 매 프레임, DashJumpApex 는 떨어지기 시작한 프레임, MeleeDash 는 근접이 켜진 프레임)
		// DashTurn(D-2) = 앞으로 걷다가 누르는 프레임에 이동 입력을 Arg 각으로 바꾼다 — 대시가 「앞 입력」을 쓰나 「이번 입력」을 쓰나
		DashMove, DashNoInput, DashPitch, DashJumpApex, DashSpam, DashThen, HoldThenDash, ReloadDash, MeleeDash, DashWall, DashCrouchSame, DashTurn, StandUp, FoeShow, FoeAway,
		// 수류탄 판(G-0) — Arg = GREN_SHOTS 차례(자리 · 피치 · 누르는 때). Q 는 칸 안에서 한 프레임 누른다(DuringGrenade)
		GrenThrow,
		// G-0-b2 죽음 · 부활 길 — 상대 · 내가 다시 생길 때까지 기다림(생기면 칸을 바로 끝낸다, 칸 길이 = 한도) · 상대가 나를 죽임(상대 ASC 로 피해 효과)
		FoeRespawnWait, MeRespawnWait, FoeKillsMe,
		// G-0-b3 경기 끝 — 목표 점수를 지금 두 팀 점수 중 큰 쪽 + 1 로 낮춘다(다음 처치가 경기를 끝낸다)
		LowerTarget,
		// L-1-c 같은 프레임 누름(대시 판, -MOBCPDashSet=l1c) — Arg = DASH_ACTIONS 차례. ActDashSame = 그 능력 → 대시, DashActSame = 대시 → 그 능력을 한 프레임 안에 그 차례로 누른다
		ActDashSame, DashActSame,
		// L-1-d ① 상대가 던진다(수류탄 판, -MOBCPGrenSet=l1d) — Arg 안 씀(상대 자리 FT_FOE_BACK · FT_FOE_SIDE · 내 자리 FT_ME_FROM_HOME). FoeThrowLeave 는 수류탄이 생기고 FT_LEAVE_AFTER 프레임 뒤 상대가 나간다
		FoeThrow, FoeThrowLeave };

private:
	bool WaitReady(float DeltaTime);
	bool Prepare();
	void BeginStep();
	void DuringStep();
	void EndStep();
	void WriteRow();
	void Finish(bool bOk);

	ACharacter* Me() const;
	// 19 F-2 원인 찾기 — 부딪힘 각본에서 이동이 막힐 때마다 엔진이 준 맞은 시각 · 법선 · 쓸기 시작과 끝을 로그에 그대로 적는다(MOB_CPROBE_HIT)
	UFUNCTION()
	void OnPawnHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);
	bool bHitsBound = false;
	void GoHome() const;
	void PlaceFoe(double Distance, bool bFacingMe, double Side = 0.0) const;
	void LogScores(const TCHAR* When) const;
	int32 ScriptIdx() const { return bBridge ? 2 : (bPads ? 1 : (bBump ? 3 : (bWeapons ? 4 : (bDash ? (bDashL1C ? 9 : 5) : (bGrenade ? (bGrenL1D ? 11 : (bGrenL1C ? 10 : (bGrenL1B ? 8 : (bGrenL1A ? 7 : 6)))) : 0))))); }
	// 수류탄 판(G-0, grenade-plan.md) — 생김 · 부딪힘 · 튕김 · 멈춤은 엔진 알림을 받은 그 자리에서 적고(MOB_CPROBE_GSPAWN · _GHIT · _GBOUNCE · _GSTOP),
	// 터짐(메시 숨김)은 틱에서 보고 폭발 판정을 같은 식으로 다시 해 본다(MOB_CPROBE_GBOOM — 겹친 성분은 comps 칸 · _GTRACE). 칸 끝 요약 = MOB_CPROBE_GREN
	void OnActorSpawned(AActor* A);
	UFUNCTION()
	void OnGrenadeHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);
	// 튕김 · 멈춤 알림엔 수류탄이 없다 — 맞은 자리에 가장 가까운 살아 있는 수류탄으로 고른다
	UFUNCTION()
	void OnGrenadeBounce(const FHitResult& ImpactResult, const FVector& ImpactVelocity);
	UFUNCTION()
	void OnGrenadeStop(const FHitResult& ImpactResult);
	const AActor* NearestGrenade(const FVector& Where, double& OutDist) const;
	void BeginGrenade();
	void DuringGrenade();
	void EndGrenade();
	void WatchGrenades();
	void LogBoom(AActor* G, int32 SpawnFrame) const;
	// 능력 GA_Grenade 의 방향 고르기를 그대로 해 본다(카메라 + 방향 × 450 ~ × 3150 무기 채널 선, 나 빼고 · 맞은 점 또는 끝점을 놓을 자리에서 보는 회전)
	void LogAimTrace(const TCHAR* Which, const FVector& CamLoc, const FRotator& CamRot, const FVector& SpawnLoc) const;
	float GrenadeCooldownLeft() const;		// GE_Grenade_Cooldown 효과의 남은 초, 없으면 0
	int32 NearestWallDir() const;			// 집에서 가장 가까운 서 있는 벽의 축 방향(ClearAround, 0 · 2 · 4 · 6) · 없으면 -1
	bool FindSurface(bool bCorner, FVector& OutPoint, FVector& OutStand, FString& OutWhat) const;	// 집 둘레 비스듬한 면 · 두 벽이 만나는 구석(과 그 구석을 대각으로 볼 던질 자리)을 찾는다
	bool StandAt(double X, double Y, FVector& OutStand, FString& OutWhat) const;	// L-1-a 정한 자리 — 그 XY 바닥(위에서 내려 쏨) 위에 캡슐이 안 겹치게 설 자리
	void AimAt(const FVector& Point) const;	// 카메라에서 그 점을 보게 조종 회전 · 몸 요를 돌린다(카메라 선이 그 점을 지나게)
	// 피해 칸(G-0-b) — 멈춘 수류탄 둘레에 상대를 세운다(Pass 0 = 세움 · 1 = 재서 고침 · 2 = 재기만). 벽 칸은 수류탄도 정한 자리로 옮긴다. 줄 = MOB_CPROBE_GBLAST
	void BlastPass(AActor* G, int32 Pass);
	// 대시 판(D-0) — 칸 안에서 누르기 · 이동 입력 · 켜짐 가장자리를 맡고(DuringDash), 칸 끝에 요약 한 줄(EndDash, MOB_CPROBE_DASH)
	static bool IsDashAct(EAct A) { return A == EAct::DashMove || A == EAct::DashNoInput || A == EAct::DashPitch || A == EAct::DashJumpApex || A == EAct::DashSpam
		|| A == EAct::DashThen || A == EAct::HoldThenDash || A == EAct::ReloadDash || A == EAct::MeleeDash || A == EAct::DashWall || A == EAct::DashCrouchSame || A == EAct::DashTurn
		|| A == EAct::ActDashSame || A == EAct::DashActSame; }
	void DuringDash();
	void EndDash();
	float DashCooldownLeft() const;		// 쿨다운 효과(GameplayCue.Character.Dash.Cooldown 을 주는 효과)의 남은 초, 없으면 0
	int32 ActiveMagazine() const;		// 든 무기의 탄창(없으면 -1)
	bool ClearAround(const FVector& Center, bool bLog);	// 여덟 방향이 DASH_CLEAR_NEED 만큼 비고 바닥이 안 끊기나(ClearDist 를 채운다, bLog = MOB_CPROBE_CLEAR 줄)
	void PickDashHome();				// 연습장 집 둘레에서 ClearAround 를 지나는 첫 자리를 대시 판 집으로(MOB_CPROBE_DASHHOME)
	void SetFoeActive(bool bOn) const;	// 상대 숨김 + 충돌 끄기(대시 길을 막지 않게) / 다시 켜기(근접 칸)
	void FaceTo(double Pitch, double Yaw) const;	// 몸 요 · 조종 회전(피치 + = 위)
	// 무기 판(W-0) — 매 프레임 가방 · 무기 받침 · 재장전 태그를 살펴 바뀐 때만 로그(MOB_CPROBE_INV · _WPAD · _RELOAD). 캐릭터가 없어도(죽음 ~ 부활) 돈다
	void WatchWeapons();
	FString InventorySig() const;
	void OnFailMsg(FGameplayTag Channel, const FLyraAbilitySimpleFailureMessage& Msg);
	static bool IsMeleeOnce(EAct A) { return A == EAct::MeleeAt || A == EAct::MeleeAtBack || A == EAct::MeleeAtSide || A == EAct::MeleeHere || A == EAct::MeleeJumpAt; }
	// F-1 부딪힘 칸 — 순간이동으로 시작해 나는 +X 로 걷는다. FoeWalks 면 상대도 -X 로 걷는다(마주 걷기)
	static bool IsBump(EAct A) { return A == EAct::BumpStillAt || A == EAct::BumpAt || A == EAct::BumpStillSideAt || A == EAct::BumpSideAt; }
	static bool FoeWalks(EAct A) { return A == EAct::BumpAt || A == EAct::BumpSideAt; }
	void PressTag(const TCHAR* TagName, bool bPress) const;
	bool IsAbilityActive(const TCHAR* ClassNamePart) const;
	float HealthOf(const AActor* A) const;
	void SetHealthOf(AActor* A, float Max, float Value) const;
	ALyraWeaponSpawner* PadNear(const FVector& Where) const;
	void StandOn(const FVector& Where) const;
	static bool PadAvailable(const ALyraWeaponSpawner* Pad, float& OutPercent);
	void LogMeleeTrace(const TCHAR* Which, const FVector& Hand) const;
	void LogFoeParts(const TCHAR* Which, const TArray<FTransform>& Parts, const TArray<FName>& Names, const FVector& Loc, double Yaw) const;
	// 몸 맞추기(-MOBCPQuinn, 17-d) — 나 · 상대를 서버 클립 · 애니 비교 기록과 같은 Quinn 몸으로 바꾸고(D33) 권총을 다시 쥐여
	// 여성형 무기 층이 붙게 한다(무기 층은 쥘 때 몸 표식으로 고른다). 다 맞고 자리 잡으면 true
	bool QuinnReady(float DeltaTime);
	bool EquipWeapon(ACharacter* C, int32 Weapon, UClass*& OutDef) const;
	void LogBodies(const TCHAR* When) const;		// MOB_CPROBE_BODY — 나 · 상대의 몸 메시와 붙은 애니 층

	FString Tag;
	bool bPads = false;
	bool bBridge = false;
	bool bBump = false;		// F-1 부딪힘 각본(연습장, -MOBCPMode=bump)
	bool bWeapons = false;	// W-0 무기 받침 각본(L_Expanse, -MOBCPMode=weapons)
	bool bDash = false;		// D-0 대시 각본(연습장, -MOBCPMode=dash)
	bool bGrenade = false;	// G-0 수류탄 각본(연습장, -MOBCPMode=grenade)
	bool bGrenL1A = false;	// L-1-a 수류탄 장면 각본(-MOBCPMode=grenade -MOBCPGrenSet=l1a — 구석 · 높은 데서 아래로 · 벽 스침 · 턱 틈 + 옮겨 놓기 10칸. G-0 각본은 그대로)
	bool bGrenL1B = false;	// L-1-b 수류탄 expanse 장면 각본(-MOBCPGrenSet=l1b, 맵 L_Expanse — 이동은 지나고 수류탄은 막는 창틀 띠 셋 + 무기 광선은 지나고 폭발 선은 막는 문틀 뒤 · 막힘 없는 대조 = 다섯 칸)
	bool bGrenL1D = false;	// L-1-d 상대가 던진 뒤 나감 각본(-MOBCPGrenSet=l1d — 나가지 않는 대조 칸 · 나가는 칸)
	bool bGrenL1C = false;	// L-1-c 수류탄 같은 프레임 누름 각본(-MOBCPGrenSet=l1c — 근접 → Q · Q → 근접 · 재장전 → Q · Q → 재장전, 한 프레임 안)
	bool bDashL1C = false;	// L-1-c 대시 같은 프레임 누름 각본(-MOBCPMode=dash -MOBCPDashSet=l1c — 근접 → 대시 · 대시 → 근접, 한 프레임 안)
	bool bReady = false;
	bool bDone = false;
	TWeakObjectPtr<APawn> ClearPawn;
	float ClearSeconds = 0.f;
	double WaitSeconds = 0;
	double StartRealAt = 0;
	bool bPrepared = false;
	bool bJumpPending = false;		// 뛰며 근접 칸 — 칸 첫 프레임 입력으로 점프를 누른다
	int32 JumpPressFrame = -1;		// 뛰며 근접 칸 — 점프를 누른 프레임(다음 프레임에 뗀다)
	bool bForceQuinn = false;
	int32 BodyTries = 0;
	double BodySeconds = 0;
	double NextBodyTryAt = 0;
	double BodyEquippedAt = -1;

	int32 Step = -1;
	float StepTime = 0.f;
	int32 Frame = 0;
	FString FrameEvents;
	FString Csv;

	FVector Home = FVector::ZeroVector;		// 판마다 내 첫 자리(받침 판) · 고정 평지(연습 판)
	TWeakObjectPtr<ACharacter> Foe;

	// 근접 한 번의 요약 — 누른 칸 안에서 모은다
	bool bMeleePressed = false;
	int32 MeleePressFrame = -1;
	float FoeHpAtPress = 0.f;
	float FoeHpMin = 0.f;
	int32 FoeHitFrame = -1;
	FVector MeAtPress = FVector::ZeroVector;
	double LungeMax = 0;
	bool bMeleeWasActive = false;
	int32 MeleeActiveFrames = 0;
	FString FoeMontageSeen;
	FString MyMontageSeen;
	FVector MeAtStepBegin = FVector::ZeroVector;
	float NextWatchLogAt = 0.f;
	TArray<double> MeleeStarts;		// 연타 칸 — 능력이 켜진 시각들
	// 앞 프레임 끝의 내 hand_r(E-1) — 근접 능력은 입력 처리 때 켜져 그 프레임 자세를 평가하기 전 손을 읽는다(짐작, 밀기 속도로 가린다)
	FVector PrevHand = FVector::ZeroVector;
	bool bPrevHandValid = false;
	// 앞 프레임 끝의 상대 몸체(물리 몸 요소마다 세상 자리) · 자리 · 요 — 능력의 쓸기는 상대 메시 몸체에도 닿는다(e1g1, 대상 종류 Pawn 에 메시도 든다)
	TArray<FTransform> PrevFoeParts;
	TArray<FName> PrevFoePartNames;
	FVector PrevFoeLoc = FVector::ZeroVector;
	double PrevFoeYaw = 0.0;

	// 받침 판 — 체력 · 받침 상태가 바뀐 때를 적는다
	float LastHp = -1.f;
	int32 LastPadAvail[2] = { -1, -1 };

	// 무기 판 — 앞 프레임에 본 값(바뀐 때만 적으려고)
	FString LastInvSig;
	TMap<FString, int32> LastWeaponPadAvail;	// 받침 이름 → 켜짐
	int32 LastReloadTag = -1;
	FString LastReloadAbilities;
	int32 ReloadPressFrame = -1;		// 재장전 누른 프레임(다음 프레임에 뗀다)
	FGameplayMessageListenerHandle FailMsgHandle;
	int32 FailMsgs = 0;
	// 화면 사진(UI 포함) — 가방이 바뀐 뒤 · 실패 알림 뒤. 빠른 칸 그림 · 실패 문구가 화면에 뜨는지 본다. 한 프레임에 한 장
	TArray<TPair<int32, FString>> PendingShots;	// 찍을 프레임 → 이름
	int32 ShotCount = 0;

	// 대시 판(D-0) — 칸마다 다시 채운다(BeginStep). 프레임 번호는 기록 표 frame 칸과 같다
	FVector DashIn = FVector::ZeroVector;		// 이 칸의 이동 입력 — 키보드처럼 축마다 ±1(대각은 앞 · 옆 두 입력, 길이 √2)
	FVector FrameIn = FVector::ZeroVector;		// 이 프레임에 넣은 이동 입력(기록 inX · inY — 다음 프레임 이동이 가져간다)
	bool bDashWasActive = false;
	int32 DashPressFrame = -1;		// 대시를 누른 프레임(매 프레임 누름 칸은 첫 프레임)
	int32 DashStartFrame = -1;
	int32 DashEndFrame = -1;
	TArray<int32> DashStarts;			// 칸 안에서 대시가 켜진 프레임들(매 프레임 누름 칸의 간격)
	FString DashMontageSeen;
	float DashCooldownAtPress = -1.f;
	int32 DashCrouchedAtPress = -1;
	FVector DashAtPress = FVector::ZeroVector;	// 누른 프레임의 내 자리(요약 줄 moved = 칸 끝 자리 − 이것)
	int32 DashMagAtBegin = -1;
	double DashMaxSpeed2d = 0, DashMaxVz = 0, DashMinVz = 0;
	int32 DashJumpPressFrame = -1;		// 점프 꼭대기 칸 — 점프 능력 입력을 누른 프레임
	int32 ActionPressFrame = -1;		// 대시 중 누름 칸 — 그 입력을 누른 프레임 · 그 능력이 켜진 첫 프레임
	int32 ActionStartFrame = -1;
	int32 ActionEndFrame = -1;		// 켜졌다 처음 꺼진 프레임 · 다시 켜진 프레임 · 대시와 함께 켜져 있던 프레임 수(0 = 대시 중엔 안 켜짐)
	int32 ActionRestartFrame = -1;
	int32 ActionDuringDash = 0;
	int32 ActionAfterDash = -1;		// 대시가 꺼진 뒤 그 능력이 처음 켜진 프레임(쥔 사격이 언제 다시 쏘나)
	bool bActionHeld = false;
	int32 DashMeleePressFrame = -1;		// 근접 뒤 대시 칸 — 근접을 누른 · 켜진 프레임
	int32 DashMeleeStartFrame = -1;
	float DashFoeHpAtStep = 0.f;
	double ClearNormalZ[8] = {};		// ClearAround — 그 방향에서 맞은 면의 법선 Z(안 맞으면 1). 벽 칸은 서 있는 면만 벽으로 본다
	double ClearDist[8] = {};			// ClearAround — 방향 i × 45도로 캡슐이 벽에 닿기까지(cm). 대시 판은 고른 집 자리의 값(벽 칸이 쓴다)

	// 수류탄 판(G-0) — 생긴 수류탄마다(터짐 · 사라짐을 틱에서 본다). 프레임 번호는 기록 표 frame 칸과 같다
	struct FGrenRec
	{
		TWeakObjectPtr<AActor> A;
		FString Name;
		int32 SpawnFrame = -1;
		int32 BoomFrame = -1;
		bool bGone = false;
	};
	TArray<FGrenRec> Grens;
	FDelegateHandle SpawnedHandle;
	// 칸마다 다시 채운다(BeginGrenade)
	int32 GrenPressFrame = -1;
	int32 GrenSpawnFrame = -1;		// 이 칸에서 처음 생긴 수류탄 · 그 수류탄의 첫 부딪힘 · 터짐 프레임
	int32 GrenPlacedFrame = -1;		// L-1-a 옮겨 놓기 칸 — 옮긴 프레임(−1 = 아직)
	int32 GrenFirstHitFrame = -1;
	int32 GrenBoomFrame = -1;
	int32 GrenHits = 0;
	int32 GrenSpawns = 0;
	int32 GrenJumpFrame = -1;		// 점프 칸 — 점프를 누른 프레임
	float GrenCdAtPress = -1.f;
	float GrenMyHpAtBegin = 0.f;
	float GrenFoeHpAtBegin = 0.f;
	FString GrenFirstHit;
	FString GrenWhere;				// 자리 잡기 요약(벽 · 면 · 상대 거리)
	FVector GrenAimPoint = FVector::ZeroVector;	// 겨눌 점(면 · 구석 · 상대) — Q 를 누를 때까지 매 프레임 겨눈다(AimAt)
	bool bGrenAim = false;
	// 한 줄 앞 기록(WriteRow)의 카메라 · 내 자리 — 대조군. WriteRow 는 그 프레임 카메라 갱신 전에 돌아서 N 줄 카메라 = N−1 프레임 끝 갱신 값 = N 프레임 입력 처리(능력)가
	// 읽는 값이다(g0a1 · g0a3 · g0a4 생김 20/20 — 짐작 4). 이 값(N−1 줄)은 갱신 하나 앞이라 카메라가 움직이는 칸에서 능력 값과 다르다. 생김 줄에 지금 값과 같이 적는다
	FVector PrevCamLoc = FVector::ZeroVector;
	FRotator PrevCamRot = FRotator::ZeroRotator;
	FVector PrevMeLoc = FVector::ZeroVector;
	float PrevMyHp = 0.f;			// 앞 프레임 끝 체력 — 터짐 줄에 지금 값과 같이(피해는 Detonate 안에서 바로 들어간다)
	float PrevFoeHp = 0.f;
	int32 GrenWallDir = -1;			// 벽 칸 — 고른 벽 방향(NearestWallDir)
	int32 GrenAdjusts = 0;			// 벽 카메라 칸 — 몸 앞뒤 맞춤 횟수
	double GrenBlastS = 0.0;		// 피해 칸 — 수류탄에서 상대 몸 가운데까지 수평 거리(GrenBlastDir 쪽) · 상대 몸 가운데 높이
	double GrenBlastZ = 0.0;
	FVector GrenBlastDir = FVector::ZeroVector;
	FString GrenCsv;				// 수류탄마다 프레임마다 한 줄 → Saved/MOB/cprobe_<tag>_gren.csv

	// G-0-b2 죽음 · 부활 길 — 상대 컨트롤러(봇은 다시 생겨도 같은 컨트롤러 · 새 몸) · 기다림 칸이 일찍 끝남 · 생사 줄(바뀐 때만) · 소식 셋(처치 · 피해 · 도움)
	TWeakObjectPtr<AController> FoeCtl;
	TWeakObjectPtr<APawn> WaitFromPawn;		// 기다림 칸 — 이 몸이 아닌 산 몸이 생기면 끝
	bool bStepDone = false;
	int32 GrenDieFrame = -1;				// 던진 뒤 죽는 칸 — 스스로 죽은 프레임 · 상대에게 죽은 프레임(G-0-b3 FoeKillAfter)
	FString LastLifeSig[2];
	TArray<FGameplayMessageListenerHandle> VerbHandles;
	void OnVerbMsg(FGameplayTag Channel, const FLyraVerbMessage& Msg);
	void WatchLife();
	void FlushShots(const TCHAR* StepName, double Now);
	class UAbilitySystemComponent* MyPSASC() const;	// 내 선수 상태의 ASC — 몸이 없을 때(죽음 ~ 부활)도 쿨다운을 읽는다
	void FoeKillMe(const TCHAR* StepName);

	// L-1-d ① 상대가 던진 뒤 나감 — 칸마다 누른 · 생긴 · 나간 · 상대 선수 상태가 사라진 · 내가 죽은 프레임(-1 = 아직) · 능력이 켜졌나. 대조 칸(FoeThrow)은 나감만 없다
	int32 FTPressFrame = -1, FTSpawnFrame = -1, FTLeaveFrame = -1, FTPsGoneFrame = -1, FTMyDeathFrame = -1, FTAbilOk = -1;
	float FTMyHpBegin = 0.f;
	FString FTGrenName;
	TWeakObjectPtr<AActor> FTFoePS;
	void BeginFoeThrow();
	void DuringFoeThrow();
	void EndFoeThrow();
	// L-1-d ② 효과 알림 — 나(0) · 상대(1) 선수 상태 ASC 의 지속 효과 걸림 · 풀림 · 무적 표식 수 바뀜(MOB_CPROBE_GEFX). 다시 생겨도 ASC 는 선수 상태에 있어 그대로다
	TWeakObjectPtr<UAbilitySystemComponent> GEAsc[2];
	FDelegateHandle GEAddH[2], GERemH[2], GETagH[2];
	void WatchEffects(UAbilitySystemComponent* A, int32 Who);
	void OnGEAdded(UAbilitySystemComponent* Target, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle, int32 Who);
	void OnGERemoved(const FActiveGameplayEffect& E, int32 Who);
	void OnImmuneTag(const FGameplayTag ChangedTag, int32 Count, int32 Who);

	// G-0-b3 경기 끝(묶음 20) — 단계 줄(바뀐 때만) · 끝 단계가 처음 보인 프레임 · 내 능력 입력이 막혔나(막힌 프레임에 판을 끝낸다 — 다음 틱에 Lyra 가 맵을 다시 연다)
	FString LastPhaseSig;
	int32 PostGameFrame = -1;
	bool bPostBlocked = false;
	void WatchPhase();
	void LowerTargetScore(const TCHAR* StepName);
	class UActorComponent* FindScoring() const;		// 게임 상태의 B_TeamDeathMatchScoring 부품(목표 점수 · 남은 시간 변수)
	int32 TeamScore(int32 TeamId) const;			// 팀 처치 점수(ShooterGame.Score.Eliminations) — 없으면 −1

	// G-0-c 섞어 누름 칸(묶음 13~16) — 다른 능력 누름 · 켜짐 · 꺼짐 프레임, Q 누름 수, 탄창 바뀜, 내 ASC 능력 알림(켜짐 · 꺼짐 · 못 켬). 칸마다 다시 채운다(BeginGrenade)
	int32 GrenStepFrame = -1;				// 이 칸 시작 프레임(이 칸에서 생긴 수류탄 고르기)
	int32 ComboOtherPress = -1;
	int32 ComboOtherStart = -1;
	int32 ComboOtherEnd = -1;
	int32 ComboOtherCancelled = -1;			// 다른 능력의 첫 꺼짐 알림 — 1 = 취소로 꺼짐 · 0 = 제대로 끝남 · −1 = 알림 없음
	int32 ComboQPresses = 0;
	int32 ComboMagBegin = -1;
	int32 ComboMagPrev = -1;
	bool bComboOtherWas = false;
	bool bComboQReleased = false;
	bool bComboWatch = false;				// 능력 알림을 적는 칸인가
	FString ComboMagChanges;				// 「프레임:앞>뒤」
	FString ComboOns;						// 켜진 능력 「프레임:이름」
	TMap<FString, FIntVector> ComboFails;	// 못 켬 「능력 까닭태그」 → (수, 첫 프레임, 끝 프레임)
	int32 AbilSeqFrame = -1;
	int32 AbilSeq = 0;
	TWeakObjectPtr<class UAbilitySystemComponent> ComboAsc;
	FDelegateHandle AbilOnHandle, AbilEndHandle, AbilFailHandle;
	void DuringGrenCombo();
	void WatchAbilities();					// 내 선수 상태 ASC 에 알림 셋을 건다(이미 걸었으면 그대로)
	int32 NextAbilSeq();
	void OnAbilOn(UGameplayAbility* Ability);
	void OnAbilEnd(const FAbilityEndedData& Data);
	void OnAbilFail(const UGameplayAbility* Ability, const FGameplayTagContainer& Tags);

	// G-0-c2 수류탄끼리 · 멈춘 수류탄 밟기(묶음 17 · 짐작 7) — 첫 수류탄이 멈춘 프레임 · 둘째 누름 · 둘째를 옮긴 프레임 · 걷기 시작(순간이동) · 집으로 돌아간 프레임.
	// 걷기 시작 자리는 수류탄 없는 대조 칸(c2_walk_bare)이 그대로 쓴다(칸마다 안 비움 — 걷기 칸 시작에만 비운다)
	int32 TouchStopFrame = -1;
	int32 TouchPress2 = -1;
	int32 TouchDropFrame = -1;
	int32 TouchT = -1;
	int32 TouchHome = -1;
	int32 TouchCdRemoved = -1;				// 칸 시작에 지운 쿨다운 효과 수 · 둘째 누름 앞에 지운 수(−1 = 안 지운 칸 · ASC 없음)
	int32 TouchCdRemoved2 = -1;
	int32 TouchPawnHits = 0;				// 이 칸 동안 내 몸 막힘 알림(MOB_CPROBE_HIT) 수 · 그중 상대가 수류탄인 수
	int32 TouchPawnGrenHits = 0;
	int32 TouchOnGren = 0;					// 걸음 줄 중 바닥 액터가 수류탄인 수(올라섬)
	bool bTouchWatch = false;
	TWeakObjectPtr<AActor> TouchG1;
	TWeakObjectPtr<AActor> TouchG2;
	TWeakObjectPtr<ACharacter> TouchHitPawn;	// 막힘 알림을 건 몸(칸 끝에 뗀다)
	int32 TouchG2Spawn = -1;
	int32 TouchAliveAtWalk = -1;			// 걷기 시작 때 살아 있는 수류탄 수(대조 칸은 0 이어야 한다)
	FString TouchG1Name;
	FString TouchG2Name;
	FVector TouchG1StopLoc = FVector::ZeroVector;
	FVector TouchWalkStart = FVector::ZeroVector;
	void DuringGrenTouch();
	void StartTouchWalk();
	void LogGrenColl(const AActor* G, const TCHAR* Which) const;	// 수류탄 성분마다 충돌 설정 한 줄(MOB_CPROBE_GCOLL)
	int32 ClearGrenadeCooldown();			// GE_Grenade_Cooldown 효과를 지운다 — 지운 수(ASC 없으면 −1)
	static bool IsGrenadeActor(const AActor* A);
};
