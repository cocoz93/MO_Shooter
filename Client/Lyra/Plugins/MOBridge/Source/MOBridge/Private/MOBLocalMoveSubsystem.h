#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBLocalMoveSubsystem.generated.h"

/**
 * 파일럿 1-a-2 — 내 캐릭터를 공용 이동 코드로 움직여도 Lyra 애니가 유지되나 확인한다.
 * 같은 입력(앞·왼·뒤·오른 네 번 눌렀다 떼기)을 세 방식으로 돌려 애니 상태 전환 횟수를 비교한다.
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBLocalDrive=N   언리얼 기본 이동(대조군)
 *   -MOBLocalDrive=W   걷기 상태를 유지하고 PhysWalking 자리에서 공용 코드 실행
 *   -MOBLocalDrive=C   MOVE_Custom 으로 바꾸고 PhysCustom 자리에서 공용 코드 실행
 *   -MOBTag=<이름>     결과 파일 이름. Saved/MOB/local_<이름>.csv
 * 판정에 쓰는 줄은 「MOB_LOCAL」 로 시작한다.
 */
UCLASS()
class UMOBLocalMoveSubsystem : public UTickableWorldSubsystem
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
	void Measure(class ACharacter* Ch, double T);
	void Finish();

	TCHAR Drive = TEXT('N');
	FString Tag = TEXT("local");
	double ReadyAt = -1;
	bool bDone = false;
	bool bLoggedMachines = false;
	int32 LastLeg = -1;
	TArray<FName> MachineState;
	TMap<FName, int32> Enter;
	TArray<FString> AnimLog;
	FString Csv;
};
