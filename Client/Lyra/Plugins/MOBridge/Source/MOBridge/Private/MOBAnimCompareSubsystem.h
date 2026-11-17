#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBAnimCompareSubsystem.generated.h"

class ACameraActor;
class ACharacter;
class APawn;
class FFrameGrabber;
class UAnimInstance;
class UPhysicalMaterial;

/**
 * 화면 시험 촬영 — 상대 캐릭터 하나를 정해진 경로로 달리게 하고 고정 카메라로 찍는다. 서버 없이 돈다.
 * Lyra 원본과 단순 재생(-MOBSimpleAnim)을 같은 경로로 찍어 나란히 비교하려고 넣었다.
 * 명령줄 -MOBAnimCompare=<tag> 일 때만 켜진다.
 *   -MOBSimpleAnim      상대를 단순 재생으로 바꾼다(없으면 Lyra 원본)
 *   -MOBACNoVideo       영상 없이 머리 위치만 남긴다(원본끼리의 기준선용)
 *   -MOBACShapes        프레임마다 부위별 판정 모양도 남긴다 → animcmp_<tag>_shapes.csv(오탈 계산의 입력). POSE = 물리 몸체 자리, POSEB = 뼈 자리
 *   -MOBACDumpClips     단순 재생의 클립 자료를 뽑는다 → animcmp_<tag>_clips60.csv · _clips120.csv(언리얼 밖 자세 계산의 입력, candidate1-pilot-plan.md)
 *   -MOBACProbe         프레임마다 애니 쪽 속사정을 남긴다 → animcmp_<tag>_probe.csv. 애니 블루프린트 변수 · 상태 기계의 지금 상태 · 곡선 값 · 몸 기준 뼈 자리(손 · 발 · 머리).
 *                       Lyra 원본이 언제 무엇을 하는지 실행값으로 보고 단순 계산을 맞추려고 넣었다
 *   -MOBACScript=aimjump  달리기 경로 대신 위아래 시선과 점프를 섞은 경로로 찍는다(2차 화면 시험). 조각마다 「MOB_ACOMP seg」 줄로 실제 시선 각 · 뜬 높이 · 단순 재생이 받은 값을 남긴다
 *   -MOBACScript=startstop 네 방향 출발 · 멈춤 · 돌아서기를 한 번씩 찍는다(3차 화면 시험, start-stop-pivot-test.md)
 *   -MOBACScript=crouch  앉기 · 일어서기를 경로 안에서 누른다 — 서서 앉고 일어섬 · 앉은 걸음 네 방향 출발 · 멈춤 · 돌아서기 · 앉아 쏘기 · 앉아 조준 · 앉은 데서 점프 · 달리기 · 출발 · 멈춤 · 돌아서기 도중 앉음 바꿈(content-add-progress.md D-3)
 *   -MOBACScript=events  쏘기 · 재장전 · 재장전 중 쏘기 · 무기 바꾸기 · 부활 효과를 경로 안에서 일으킨다(9단계 Lyra 원본 기록, candidate1-step9-plan.md 9-1).
 *                       -MOBACProbe 와 같이 주면 probe 에 사건 · 재생 중 몽타주 전부 칸이 붙고, _montages.csv(처음 본 몽타주 자산 값) · _blendprofiles.csv(골격의 뼈별 비중 표)를 남긴다
 *   -MOBACWeapon=pistol|rifle|shotgun  상대에게 쥐여 줄 무기(기본 rifle)
 *   -MOBACEventDelay=<초>  사건 경로의 사건을 모두 늦춘다(도구 확인용)
 * 산출물 = Saved/MOB/animcmp_<tag>.csv(프레임별 머리·골반 자리) · rec_animcmp_<tag>.bgra(원시 영상, 바깥에서 ffmpeg 로 변환)
 * 판정에 쓰는 줄은 전부 「MOB_ACOMP」 로 시작한다.
 * 데디 서버(-server)에 주면 서버의 봇을 같은 경로로 움직인다. 카메라와 촬영은 건너뛴다(candidate1-impl-plan.md 0단계)
 */
enum class EMOBACScript : uint8 { Run, AimJump, StartStop, Events, Ads, Crouch, Melee };

UCLASS()
class UMOBAnimCompareSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

	// 데디 서버에서 돌 때 기준값 기록(UMOBDediBaselineSubsystem)이 읽는다
	ACharacter* GetFoe() const { return Foe.Get(); }
	bool GetRunPosition(int32& OutPass, const TCHAR*& OutSegment, float& OutSegmentTime) const;		// 경로를 도는 중이면 true

private:
	enum class EStage : uint8 { WaitActors, Settle, Run, Stopping, Done };

	bool IsMatchPlaying(float DeltaTime);
	bool Prepare();
	void LogBody() const;
	void BeginPass();
	void ForceLyraIdleBreak(float DeltaTime);
	void DriveFoe(float DeltaTime);
	void UpdateCamera();
	void WriteRow(float SegTime);
	void WriteShapes();
	void WriteClipDump(int32 SampleHz) const;
	void ResetSegmentStats();
	void TrackSegment();
	void LogSegment() const;
	void BuildProbe();
	void WriteProbe(float SegTime);
	void TickCapture();
	void DrainCapture();
	void Finish();
	// -MOBACScript=events
	bool EquipFoeWeapon(int32 WeaponIndex);
	void RunEvents(float DeltaTime);
	void SetFoeFire(bool bHeld);
	void PulseFoeReload();
	bool ActivateFoeSpawnEffect();
	void NoteMontageAssets();
	void WriteBlendProfiles();

	FString Tag;
	bool bSimple = false;
	bool bVideo = true;
	bool bShapes = false;
	bool bDumpClips = false;
	EMOBACScript Script = EMOBACScript::Run;
	bool bAimJump = false;		// 시선을 바깥에서 정하는 경로(aimjump · startstop)
	bool bProbe = false;

	// -MOBACProbe — 읽을 변수와 상태 기계 목록(BuildProbe 가 첫 촬영 프레임에 만든다)
	// 무기를 바꾸면 무기 층 인스턴스가 새것으로 갈린다 — 옛것이 사라지면 같은 클래스의 새 인스턴스를 다시 찾아 붙이고, 없으면 칸 수만큼 비워 줄이 밀리지 않게 한다
	struct FProbeVar
	{
		TWeakObjectPtr<UAnimInstance> Instance;
		const FProperty* Prop = nullptr;
		TWeakObjectPtr<UClass> Class;
		int32 Columns = 1;	// 제목에 낸 칸 수(1 또는 3)
	};
	struct FProbeMachine
	{
		TWeakObjectPtr<UAnimInstance> Instance;
		FName Name;		// GetCurrentStateName(번호) 의 번호는 상태 기계 번호가 아니라 노드 번호라 이름으로 찾는다
		TWeakObjectPtr<UClass> Class;
	};
	TArray<FProbeVar> ProbeVars;
	TArray<FProbeMachine> ProbeMachines;
	bool bProbeBuilt = false;
	int32 ProbeRebinds = 0;		// 사라진 인스턴스를 같은 클래스의 새것으로 다시 붙인 수
	int32 ProbeBlankRows = 0;	// 붙일 인스턴스가 없어 칸을 비운 줄 수
	FString ProbeCsv;

	EStage Stage = EStage::WaitActors;
	double WaitSeconds = 0;
	TWeakObjectPtr<APawn> ClearPawn;
	float ClearSeconds = 0.f;
	float SettleLeft = 0.f;
	float IdleBreakForceLeft = 0.f;		// Lyra 쪽 — 단순 재생이 대기 몸짓을 시작하기까지 남은 시간
	bool bIdleBreakForcing = false;
	int32 Pass = 0;
	int32 Segment = 0;
	float SegmentTime = 0.f;
	float BodyYaw = 0.f;
	float StartYaw = 0.f;
	float MoveAngle = 0.f;
	int32 Frame = 0;
	FVector Home = FVector::ZeroVector;
	FVector CameraSide = FVector::ZeroVector;

	TWeakObjectPtr<ACharacter> Foe;
	TWeakObjectPtr<ACameraActor> Camera;
	TWeakObjectPtr<APawn> AimMark;	// -MOBACScript=aimjump 일 때 상대 봇이 바라보는 보이지 않는 표적
	float AimPitch = 0.f;

	// 조각 하나 동안 실제로 들어간 값(LogSegment 가 남기고 비운다)
	float SegPitchMin = 0.f;
	float SegPitchMax = 0.f;
	float SegSimplePitchMin = 0.f;
	float SegSimplePitchMax = 0.f;
	double SegRiseMax = 0.0;
	int32 SegFallFrames = 0;
	float SegJumpWeightMax = 0.f;
	int32 SegJumpClips = 0;

	FString Csv;
	FString ShapeCsv;
	int32 ShapeRows = 0;
	int32 BoneShapeRows = 0;
	UPROPERTY()
	TObjectPtr<UPhysicalMaterial> PhysWeak;

	// -MOBACWeapon · -MOBACScript=events. 무기 번호 = 0 권총 · 1 소총 · 2 샷건(Shared/Weapon 의 EWeapon 과 같은 차례)
	int32 WeaponIndex = 1;
	int32 HeldWeapon = 1;			// 지금 쥐여 준 무기(바꾸기 사건 뒤 달라진다)
	bool bFoeFireHeld = false;
	float FoeFireReleaseLeft = -1.f;	// 0 이하로 떨어지면 방아쇠를 놓는다. 음수 = 시간으로 놓지 않음
	int32 VideoPass = -1;			// -MOBACVideoPass=<판 번호> — 그 판만 영상으로 찍는다(0 = 가까이). 음수 = 전부
	float EventDelay = 0.f;			// -MOBACEventDelay=<초> — 사건을 모두 그만큼 늦춘다(도구 확인: 몽타주 칸이 같은 만큼 밀려야 한다)
	FString FrameEvents;			// 이 프레임에 일으킨 사건(probe 의 events 칸)
	// 사건 종류마다 일으킨 수 · 실패 수 — 세는 자리를 종류마다 둔다(종류 하나만 세면 나머지의 0 이 거짓말이 된다)
	int32 EvFire = 0, EvHold = 0, EvRelease = 0, EvReload = 0, EvSwap = 0, EvSpawn = 0;
	int32 EvSwapFail = 0, EvSpawnFail = 0, EvNoAsc = 0;
	// 조준(-MOBACScript=ads). 누르는 동안 매 프레임 「눌림」을 보낸다 — 사람 입력과 같다(MOBMoveRecordSubsystem 조준 주석)
	bool bFoeAdsHeld = false;
	void SetFoeAdsReleased();		// 누른 채면 뗀다(이미 뗐으면 아무것도 안 한다)
	int32 EvAdsOn = 0, EvAdsOff = 0;
	// 앉기(-MOBACScript=crouch). 사람 입력과 같은 ALyraCharacter::ToggleCrouch 를 부른다 — 이미 그 상태면 안 부르고 Skip, 공중이라 안 바뀌면 Fail 로 센다
	void SetFoeCrouch(bool bWant);
	int32 EvCrouchOn = 0, EvCrouchOff = 0, EvCrouchSkip = 0, EvCrouchFail = 0;
	// 근접(-MOBACScript=melee). 근접 = 사람 입력과 같은 InputTag.Ability.Melee 누름(다음 프레임에 뗌) · 반응 = 피해 연출 GCNL_Character_DamageTaken 과 같은 PlayAnimMontage(배율 1)
	bool bFoeMeleeReleasePending = false;
	bool PlayFoeHitReact(bool bBack);
	int32 EvMelee = 0, EvHitBack = 0, EvHitFront = 0, EvHitFail = 0;
	int32 MaxLiveMontages = 0;		// 한 프레임에 살아 있던 몽타주 최대 수(패킷 칸 수를 정한다)
	TSet<FName> SeenMontages;
	FString MontageCsv;
	FString BlendProfileCsv;

	FFrameGrabber* Grabber = nullptr;	// 만드는 곳 = TickCapture, 지우는 곳 = Tick 의 Stopping
	FArchive* RawWriter = nullptr;
	FIntPoint GrabSize = FIntPoint::ZeroValue;
	int32 GrabbedFrames = 0;
	int32 CaptureCalls = 0;
};
