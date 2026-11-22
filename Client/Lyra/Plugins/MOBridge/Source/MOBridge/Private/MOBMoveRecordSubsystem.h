#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBMoveRecordSubsystem.generated.h"

class ACharacter;
class UPrimitiveComponent;
class ULyraCharacterMovementComponent;

/**
 * 조각 1-c — 정해 둔 입력을 Lyra 이동에 재생하며 프레임별 결과를 적는다. Shared/Movement 의 정답지다.
 * 입력은 ControlledCharacterMove 바로 앞에서 끼워 넣어 키보드·능력 시스템을 거치지 않는다.
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBMoveRecord=<이름>   결과 파일 Saved/MOB/move_<이름>.csv
 *   -MOBMoveSeed=<수>       입력 난수 시드(기본 20261102)
 *   -MOBMoveSegs=<수>       구간 수(기본 40). 구간마다 임의 바닥 위로 옮겨 처음부터 떨어뜨린다
 *   -MOBMoveSegSec=<초>     구간 길이(기본 10)
 *   -MOBMoveStress          평소 입력이 안 지나가는 갈래를 일부러 지나간다(짝수 구간은 바닥에 10cm 묻혀 시작, 홀수 구간은 임의 발사)
 *   -MOBMoveAds             조준을 0.05~1.2초마다 무작위로 누르고 뗀다(C 조준 — 걸음 속도 ×0.5). 조준은 따로 쓰는 난수라 이동 입력은 이 옵션이 없을 때와 같다.
 *                           조준은 능력 시스템 입력이라 다음 프레임 컨트롤러 틱에서 먹는다 — 줄 끝 칸 ads 는 이번 프레임에 누른 상태
 *   -MOBMoveCrouch          앉기를 0.2~2.5초마다 누른다(D 앉기). 키와 같은 길 ALyraCharacter::ToggleCrouch — 땅 위에서만 앉고, 앉았으면 일어선다.
 *                           점프는 Lyra 점프 능력처럼 일어서기를 먼저 부른다(LyraGameplayAbility_Jump.cpp:55). 앉기도 따로 쓰는 난수라 이동 입력은 그대로.
 *                           셋째 구간마다 천장이 낮은 자리(앉은 캡슐은 들어가고 선 캡슐은 안 들어가는 곳)에서 앉은 채 시작해 일어서기 막힘을 지나간다.
 *                           줄 끝 칸 = cev(0 없음 · 1 앉기 누름 · 2 점프가 부른 일어서기) · 앞 wants · 앞 crouched · 앞 half · walk(MaxWalkSpeed) · 뒤 wants · 뒤 crouched · 뒤 half
 *   -MOBMovePads            점프대 정답지(jumppad-plan j-2). 구간마다 점프대 하나(구간 차례로 돌림) 앞 바닥에 놓고 그쪽으로 걷는다 — 평소 무작위 입력 · 점프는 끔.
 *                           넷째 구간 묶음마다 갈래를 바꾼다: 0 점프대를 보고 걸음 · 1 아무 쪽이나 보고 걸음 · 2 아무 쪽이나 보고 점프해 들어감 · 3 반 걸음.
 *                           점프대 몫은 Lyra 그래프 그대로 돈다(겹침 → LaunchCharacter → 다음 프레임 줄의 lx · ly · lz).
 *                           줄 끝 칸 = yaw(이동 뒤 몸 방향 — 그래프가 읽는 앞방향) · pads(이동 뒤 몸 캡슐과 겹친 점프대 구 칸 비트, 차례 = 머리 「#pad」 줄) · cue(살아 있는 효과 배우 수)
 * 판정에 쓰는 줄은 「MOB_MOVE」 로 시작한다. 파일 형식은 Tools/MoveCheck 머리 주석이 정본이다.
 */
UCLASS()
class UMOBMoveRecordSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	static void PreMoveThunk(ULyraCharacterMovementComponent* M, FVector& Input, float Dt);
	static void PostMoveThunk(ULyraCharacterMovementComponent* M, float Dt);
	void OnPreMove(ULyraCharacterMovementComponent* M, FVector& Input, float Dt);
	void OnPostMove(ULyraCharacterMovementComponent* M, float Dt);

	void BeginRecording(ACharacter* Ch);
	void DumpParams(ACharacter* Ch);
	bool StartSegment(ULyraCharacterMovementComponent* M, ACharacter* Ch);
	void Finish();

	FString Tag;
	int32 Seed = 20261102;
	int32 NumSegs = 40;
	float SegSeconds = 10.f;
	bool bStress = false;
	bool bAds = false;
	bool bAdsHeld = false;
	double NextAdsAt = 0;
	FRandomStream Rng;
	FRandomStream AdsRng;

	// 앉기(-MOBMoveCrouch, D-1)
	void FindLowSpots(ACharacter* Ch);
	bool bCrouch = false;
	double NextCrouchAt = 0;
	FRandomStream CrouchRng;
	TArray<FVector> LowSpots;		// 앉은 캡슐 중심 자리
	int32 LowSegs = 0;
	int32 CrouchEv = 0;
	bool bWantsBefore = false, bCrouchedBefore = false;
	float HalfBefore = 0.f, WalkBefore = 0.f;
	int32 CrouchPresses = 0, JumpUncrouches = 0, CrouchedFrames = 0, BlockedFrames = 0, FallCrouchedFrames = 0, CrouchChanges = 0;

	// 점프대(-MOBMovePads, 점프대 j-2)
	struct FPadSpot
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<UPrimitiveComponent> Sphere;
		FString Name;
	};
	void FindPads();
	bool PlaceForPad(ULyraCharacterMovementComponent* M, ACharacter* Ch);
	int32 CountCues() const;
	bool bPads = false;
	TArray<FPadSpot> Pads;			// 이름 차례(make_launchers.py 와 같은 차례)
	FRandomStream PadRng;
	int32 PadVariant = 0;
	float PadJumpAt = 0.f;
	bool bPadJumped = false;
	int32 PadPlaced = 0, PadMiss = 0, PadLaunchRows = 0, PadOverlapRows = 0, PadCueRows = 0, PadCueSpawns = 0;
	FDelegateHandle PadSpawnHandle;

	double PawnReadyAt = -1;
	double HookedRealAt = 0;
	bool bHooked = false;
	bool bDone = false;

	FBox PlayArea = FBox(ForceInit);
	TWeakObjectPtr<ULyraCharacterMovementComponent> LastComp;
	int32 SegIndex = -1;
	int32 Frame = 0;
	int32 ExtCount = 0;
	int32 StartMissCount = 0;
	float SegTime = 0.f;
	bool bRowOpen = false;
	bool bPrevValid = false;
	FVector PrevAfterLocation = FVector::ZeroVector;

	double NextDirAt = 0;
	FVector CurrentInput = FVector::ZeroVector;
	int32 JumpFramesLeft = 0;

	FString Csv;
	FString Row;
};
