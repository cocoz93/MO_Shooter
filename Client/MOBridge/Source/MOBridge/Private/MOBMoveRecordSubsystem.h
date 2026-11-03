#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBMoveRecordSubsystem.generated.h"

class ACharacter;
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
	FRandomStream Rng;

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
