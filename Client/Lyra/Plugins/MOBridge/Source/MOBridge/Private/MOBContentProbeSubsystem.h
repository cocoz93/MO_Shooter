#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBContentProbeSubsystem.generated.h"

class ACharacter;
class ALyraWeaponSpawner;

/**
 * 컨텐츠 확인 기록 — 조준 · 앉기 · 근접 · 체력 받침의 Lyra 실행값을 정해진 각본으로 찍는다. 서버 없이 돈다(content-add-plan.md 0-3).
 * 에셋에서 읽은 값(content-add-progress.md 0-3 표)이 실제 실행에서도 같은지 보는 것이 목적이다. 판정 대조용 정밀 기록은 각 단계의 기록 도구가 맡는다.
 * 명령줄 -MOBContentProbe=<tag> 일 때만 켜진다. -MOBCPMode=gym(기본, 맵 L_ShooterGym?NumBots=1) | pads(맵 L_Expanse?NumBots=0)
 * | bridge(내 서버에 붙은 지금 클라 — run_game.ps1 의 LyraArgs 로 넘긴다. 0-2 「지금 키를 누르면 무슨 일이 나나」)
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
	enum class EAct : uint8 { None, Home, AdsPress, AdsRelease, CrouchToggle, Equip, FoeSetup, FoeSameTeam, MeleeAt, MeleeAtBack, MeleeAtSide, MeleeHere, MeleeSpam, PadsDump, PadOn, Away, SetHealth, Watch };

private:
	bool WaitReady(float DeltaTime);
	bool Prepare();
	void BeginStep();
	void DuringStep();
	void EndStep();
	void WriteRow();
	void Finish(bool bOk);

	ACharacter* Me() const;
	void GoHome() const;
	void PlaceFoe(double Distance, bool bFacingMe, double Side = 0.0) const;
	void LogScores(const TCHAR* When) const;
	int32 ScriptIdx() const { return bBridge ? 2 : (bPads ? 1 : 0); }
	static bool IsMeleeOnce(EAct A) { return A == EAct::MeleeAt || A == EAct::MeleeAtBack || A == EAct::MeleeAtSide || A == EAct::MeleeHere; }
	void PressTag(const TCHAR* TagName, bool bPress) const;
	bool IsAbilityActive(const TCHAR* ClassNamePart) const;
	float HealthOf(const AActor* A) const;
	void SetHealthOf(AActor* A, float Max, float Value) const;
	ALyraWeaponSpawner* PadNear(const FVector& Where) const;
	void StandOn(const FVector& Where) const;
	static bool PadAvailable(const ALyraWeaponSpawner* Pad, float& OutPercent);

	FString Tag;
	bool bPads = false;
	bool bBridge = false;
	bool bReady = false;
	bool bDone = false;
	TWeakObjectPtr<APawn> ClearPawn;
	float ClearSeconds = 0.f;
	double WaitSeconds = 0;
	double StartRealAt = 0;

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

	// 받침 판 — 체력 · 받침 상태가 바뀐 때를 적는다
	float LastHp = -1.f;
	int32 LastPadAvail[2] = { -1, -1 };
};
