#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayTagContainer.h"

#include "MOBContentProbeSubsystem.generated.h"

class ACharacter;
class ALyraWeaponSpawner;
struct FLyraAbilitySimpleFailureMessage;

/**
 * 컨텐츠 확인 기록 — 조준 · 앉기 · 근접 · 체력 받침의 Lyra 실행값을 정해진 각본으로 찍는다. 서버 없이 돈다(content-add-plan.md 0-3).
 * 에셋에서 읽은 값(content-add-progress.md 0-3 표)이 실제 실행에서도 같은지 보는 것이 목적이다. 판정 대조용 정밀 기록은 각 단계의 기록 도구가 맡는다.
 * 명령줄 -MOBContentProbe=<tag> 일 때만 켜진다. -MOBCPMode=gym(기본, 맵 L_ShooterGym?NumBots=1) | pads(맵 L_Expanse?NumBots=0)
 * | bridge(내 서버에 붙은 지금 클라 — run_game.ps1 의 LyraArgs 로 넘긴다. 0-2 「지금 키를 누르면 무슨 일이 나나」)
 * | weapons(맵 L_Expanse?NumBots=0 — 무기 받침 · 가방 · 재장전 태그, weapon-pickup-plan.md W-0)
 * | dash(맵 L_ShooterGym?NumBots=1 — 대시 방향 · 힘 · 막기 · 끊기 · 쿨다운, dash-plan.md D-0)
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
		DashMove, DashNoInput, DashPitch, DashJumpApex, DashSpam, DashThen, HoldThenDash, ReloadDash, MeleeDash, DashWall, DashCrouchSame, DashTurn, StandUp, FoeShow, FoeAway };

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
	int32 ScriptIdx() const { return bBridge ? 2 : (bPads ? 1 : (bBump ? 3 : (bWeapons ? 4 : (bDash ? 5 : 0)))); }
	// 대시 판(D-0) — 칸 안에서 누르기 · 이동 입력 · 켜짐 가장자리를 맡고(DuringDash), 칸 끝에 요약 한 줄(EndDash, MOB_CPROBE_DASH)
	static bool IsDashAct(EAct A) { return A == EAct::DashMove || A == EAct::DashNoInput || A == EAct::DashPitch || A == EAct::DashJumpApex || A == EAct::DashSpam
		|| A == EAct::DashThen || A == EAct::HoldThenDash || A == EAct::ReloadDash || A == EAct::MeleeDash || A == EAct::DashWall || A == EAct::DashCrouchSame || A == EAct::DashTurn; }
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
};
