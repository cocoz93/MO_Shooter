#include "MOBLocalMoveSubsystem.h"

#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "Character/LyraCharacterMovementComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBLocalMoveSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBLocal, Log, All);

namespace
{
	// 파일럿 값 — 맨바닥 걷기만. 정식 이동은 1-c 에서 Shared/ 로 새로 짠다
	constexpr float MAX_SPEED = 600.f;
	constexpr float MAX_ACCEL = 1200.f;
	constexpr float BRAKE = 1400.f;
	constexpr float BRAKE_FRICTION = 3.f;
	constexpr float GROUND_FRICTION = 8.f;

	constexpr double SCRIPT_START = 6.0;   // 생성 효과가 끝난 뒤 시작
	constexpr double PRESS_SEC = 1.2;
	constexpr double RELEASE_SEC = 1.2;
	constexpr int32 NUM_LEGS = 4;

	// 엔진을 모르는 계산 — 공용 코드가 될 자리
	void Step(float& Vx, float& Vy, float Ax, float Ay, float Dt)
	{
		const float Speed = FMath::Sqrt(Vx * Vx + Vy * Vy);
		const float ALen = FMath::Sqrt(Ax * Ax + Ay * Ay);
		if (ALen > 0.f)
		{
			const float K = FMath::Min(Dt * GROUND_FRICTION, 1.f);
			Vx -= (Vx - Ax / ALen * Speed) * K;
			Vy -= (Vy - Ay / ALen * Speed) * K;
			Vx += Ax * Dt;
			Vy += Ay * Dt;
			const float S2 = FMath::Sqrt(Vx * Vx + Vy * Vy);
			if (S2 > MAX_SPEED) { Vx *= MAX_SPEED / S2; Vy *= MAX_SPEED / S2; }
		}
		else if (Speed > 0.f)
		{
			const float NewSpeed = FMath::Max(Speed - (BRAKE + BRAKE_FRICTION * Speed) * Dt, 0.f);
			Vx *= NewSpeed / Speed;
			Vy *= NewSpeed / Speed;
		}
	}

	void MoveHook(ULyraCharacterMovementComponent* M, float Dt)
	{
		const FVector A = M->GetCurrentAcceleration();
		float Vx = M->Velocity.X, Vy = M->Velocity.Y;
		Step(Vx, Vy, A.X, A.Y, Dt);
		M->Velocity = FVector(Vx, Vy, 0.f);
		FHitResult Hit;
		M->SafeMoveUpdatedComponent(M->Velocity * Dt, M->UpdatedComponent->GetComponentQuat(), true, Hit);
	}
}

bool UMOBLocalMoveSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	FString Dummy;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBLocalDrive="), Dummy);
}

void UMOBLocalMoveSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FString S;
	if (FParse::Value(FCommandLine::Get(), TEXT("MOBLocalDrive="), S) && S.Len() > 0) Drive = FChar::ToUpper(S[0]);
	FParse::Value(FCommandLine::Get(), TEXT("MOBTag="), Tag);
	Csv = TEXT("t,x,y,z,speed,accel,mode,onGround,falling,leg,loco\n");
	UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_INIT world=%s drive=%c"), *GetNameSafe(GetWorld()), Drive);
}

void UMOBLocalMoveSubsystem::Deinitialize()
{
	ULyraCharacterMovementComponent::MOBMoveHook = nullptr;
	Super::Deinitialize();
}

TStatId UMOBLocalMoveSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBLocalMoveSubsystem, STATGROUP_Tickables);
}

void UMOBLocalMoveSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch) return;

	if (ReadyAt < 0)
	{
		ReadyAt = FPlatformTime::Seconds();
		Ch->TeleportTo(FVector(-600.f, -1070.f, Ch->GetActorLocation().Z), FRotator(0, 0, 0));
		PC->SetControlRotation(FRotator(0, 0, 0));
		UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_READY pawn=%s moveClass=%s"), *GetNameSafe(Ch), *GetNameSafe(Ch->GetCharacterMovement()->GetClass()));
		return;
	}

	const double T = FPlatformTime::Seconds() - ReadyAt;
	UCharacterMovementComponent* M = Ch->GetCharacterMovement();

	if (T >= SCRIPT_START - 1.0 && !ULyraCharacterMovementComponent::MOBMoveHook && Drive != TEXT('N'))
	{
		ULyraCharacterMovementComponent::MOBMoveHook = &MoveHook;
		if (Drive == TEXT('C')) M->SetMovementMode(MOVE_Custom, 0);
		UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_HOOK t=%.3f mode=%d onGround=%d"), T, (int32)M->MovementMode, M->IsMovingOnGround() ? 1 : 0);
	}

	// 입력 — 시선은 그대로 두고 앞(+X) · 왼(-Y) · 뒤(-X) · 오른(+Y). 사람이 WASD 를 누르는 것과 같은 꼴
	static const FVector Dirs[NUM_LEGS] = { FVector(1, 0, 0), FVector(0, -1, 0), FVector(-1, 0, 0), FVector(0, 1, 0) };
	const double ST = T - SCRIPT_START;
	const double Period = PRESS_SEC + RELEASE_SEC;
	int32 Leg = -1;
	if (ST >= 0)
	{
		const int32 N = (int32)(ST / Period);
		if (N < NUM_LEGS && ST - N * Period < PRESS_SEC) Leg = N;
		if (N >= NUM_LEGS + 1) { Measure(Ch, T); Finish(); return; }
	}
	if (Leg >= 0) Ch->AddMovementInput(Dirs[Leg], 1.f);
	if (Leg != LastLeg)
	{
		UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_INPUT t=%.3f leg=%d pos=(%.1f,%.1f,%.1f)"), T, Leg, Ch->GetActorLocation().X, Ch->GetActorLocation().Y, Ch->GetActorLocation().Z);
		LastLeg = Leg;
	}

	Measure(Ch, T);
}

void UMOBLocalMoveSubsystem::Measure(ACharacter* Ch, double T)
{
	UAnimInstance* AI = Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
	FName Loco = NAME_None;
	if (AI)
	{
		if (const IAnimClassInterface* C = IAnimClassInterface::GetFromClass(AI->GetClass()))
		{
			// 번호로 찾는 GetStateMachineInstance 는 첫 실행에서 아무것도 못 읽음(횟수 0) — 이름으로 찾는다. 파일럿 MeasureRemote 와 같은 방식
			const TArray<FBakedAnimationStateMachine>& Baked = C->GetBakedStateMachines();
			if (!bLoggedMachines)
			{
				bLoggedMachines = true;
				FString Names;
				for (const FBakedAnimationStateMachine& B : Baked) Names += B.MachineName.ToString() + TEXT(" ");
				UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_MACHINES anim=%s num=%d names=[%s]"), *GetNameSafe(AI->GetClass()), Baked.Num(), *Names);
			}
			MachineState.SetNum(Baked.Num());
			for (int32 i = 0; i < Baked.Num(); ++i)
			{
				const FAnimNode_StateMachine* SM = AI->GetStateMachineInstanceFromName(Baked[i].MachineName);
				const FName State = SM ? SM->GetCurrentStateName() : NAME_None;
				if (Baked[i].MachineName == TEXT("LocomotionSM")) Loco = State;
				if (State != MachineState[i])
				{
					// 스크립트 전 대기 구간은 횟수에 넣지 않는다
					if (T >= SCRIPT_START)
					{
						Enter.FindOrAdd(FName(*FString::Printf(TEXT("%s.%s"), *Baked[i].MachineName.ToString(), *State.ToString())))++;
					}
					AnimLog.Add(FString::Printf(TEXT("%.3f %s %s->%s"), T, *Baked[i].MachineName.ToString(), *MachineState[i].ToString(), *State.ToString()));
					MachineState[i] = State;
				}
			}
		}
	}

	const UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const FVector L = Ch->GetActorLocation();
	Csv += FString::Printf(TEXT("%.4f,%.2f,%.2f,%.2f,%.1f,%.1f,%d,%d,%d,%d,%s\n"), T, L.X, L.Y, L.Z,
		M->Velocity.Size2D(), M->GetCurrentAcceleration().Size2D(), (int32)M->MovementMode,
		M->IsMovingOnGround() ? 1 : 0, M->IsFalling() ? 1 : 0, LastLeg, *Loco.ToString());
}

void UMOBLocalMoveSubsystem::Finish()
{
	bDone = true;
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("local_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path);
	Enter.KeySort(FNameLexicalLess());
	FString Sum;
	for (const TPair<FName, int32>& It : Enter) Sum += FString::Printf(TEXT("%s=%d "), *It.Key.ToString(), It.Value);
	for (const FString& L : AnimLog) UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_ANIM %s"), *L);
	UE_LOG(LogMOBLocal, Log, TEXT("MOB_LOCAL_SUMMARY drive=%c enter[%s] csv=%d"), Drive, *Sum, bSaved ? 1 : 0);
	ULyraCharacterMovementComponent::MOBMoveHook = nullptr;
	FPlatformMisc::RequestExit(false);
}
