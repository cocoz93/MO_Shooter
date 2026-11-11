#include "MOBMoveRecordSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PhysicsVolume.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "Character/LyraCharacterMovementComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBMoveRecordSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBMove, Log, All);

namespace
{
	constexpr double START_DELAY = 8.0;			// 폰이 생기고 생성 효과가 끝날 때까지(월드 시간)
	constexpr double MAX_REAL_SECONDS = 500.0;	// 넘으면 모은 만큼만 쓰고 끝낸다
	constexpr double PLAY_AREA_PAD = 1000.0;
	constexpr double START_ABOVE_FLOOR = 3.0;
	constexpr int32 START_TRIES = 200;
	constexpr double STRESS_EMBED = 10.0;		// 시작 겹침 갈래를 지나가게 바닥에 묻는 깊이
	constexpr float STRESS_LAUNCH_EVERY = 2.f;	// 평균 몇 초에 한 번 발사

	UMOBMoveRecordSubsystem* GInstance = nullptr;
}

bool UMOBMoveRecordSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	FString Dummy;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBMoveRecord="), Dummy);
}

void UMOBMoveRecordSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("MOBMoveRecord="), Tag);
	FParse::Value(Cmd, TEXT("MOBMoveSeed="), Seed);
	FParse::Value(Cmd, TEXT("MOBMoveSegs="), NumSegs);
	FParse::Value(Cmd, TEXT("MOBMoveSegSec="), SegSeconds);
	bStress = FParse::Param(Cmd, TEXT("MOBMoveStress"));
	Rng.Initialize(Seed);
	GInstance = this;
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_INIT world=%s tag=%s seed=%d segs=%d segSec=%.1f"), *GetNameSafe(GetWorld()), *Tag, Seed, NumSegs, SegSeconds);
}

void UMOBMoveRecordSubsystem::Deinitialize()
{
	ULyraCharacterMovementComponent::MOBPreMoveHook = nullptr;
	ULyraCharacterMovementComponent::MOBPostMoveHook = nullptr;
	GInstance = nullptr;
	Super::Deinitialize();
}

TStatId UMOBMoveRecordSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBMoveRecordSubsystem, STATGROUP_Tickables);
}

void UMOBMoveRecordSubsystem::PreMoveThunk(ULyraCharacterMovementComponent* M, FVector& Input, float Dt)
{
	if (GInstance) GInstance->OnPreMove(M, Input, Dt);
}

void UMOBMoveRecordSubsystem::PostMoveThunk(ULyraCharacterMovementComponent* M, float Dt)
{
	if (GInstance) GInstance->OnPostMove(M, Dt);
}

void UMOBMoveRecordSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;

	if (!bHooked)
	{
		if (!Ch) return;
		if (PawnReadyAt < 0) PawnReadyAt = World->GetTimeSeconds();
		if (World->GetTimeSeconds() - PawnReadyAt >= START_DELAY) BeginRecording(Ch);
		return;
	}

	if (SegIndex >= NumSegs || FPlatformTime::Seconds() - HookedRealAt > MAX_REAL_SECONDS)
	{
		Finish();
	}
}

void UMOBMoveRecordSubsystem::BeginRecording(ACharacter* Ch)
{
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		PlayArea += It->GetActorLocation();
	}
	if (!PlayArea.IsValid)
	{
		PlayArea += Ch->GetActorLocation();
	}
	PlayArea = PlayArea.ExpandBy(FVector(PLAY_AREA_PAD));

	DumpParams(Ch);
	Csv += TEXT("frame,seg,ev,dt,ix,iy,iz,px,py,pz,vx,vy,vz,mode,ext,lx,ly,lz,pressed,was,hold,force,count,countPre,maxSpeed,")
		TEXT("ax,ay,az,avx,avy,avz,amode,fBlock,fWalk,fLine,fDist,fLineDist,aCount\n");

	ULyraCharacterMovementComponent::MOBPreMoveHook = &UMOBMoveRecordSubsystem::PreMoveThunk;
	ULyraCharacterMovementComponent::MOBPostMoveHook = &UMOBMoveRecordSubsystem::PostMoveThunk;
	bHooked = true;
	HookedRealAt = FPlatformTime::Seconds();
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_READY pawn=%s playArea=%s"), *GetNameSafe(Ch), *PlayArea.ToString());
}

void UMOBMoveRecordSubsystem::DumpParams(ACharacter* Ch)
{
	UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	const auto F = [this](const TCHAR* Name, double Value) { Csv += FString::Printf(TEXT("#param %s %.17g\n"), Name, Value); };

	F(TEXT("MaxWalkSpeed"), M->MaxWalkSpeed);
	F(TEXT("MaxAcceleration"), M->MaxAcceleration);
	F(TEXT("GroundFriction"), M->GroundFriction);
	F(TEXT("BrakingDecelerationWalking"), M->BrakingDecelerationWalking);
	F(TEXT("BrakingDecelerationFalling"), M->BrakingDecelerationFalling);
	F(TEXT("BrakingFriction"), M->BrakingFriction);
	F(TEXT("BrakingFrictionFactor"), M->BrakingFrictionFactor);
	F(TEXT("bUseSeparateBrakingFriction"), M->bUseSeparateBrakingFriction ? 1 : 0);
	F(TEXT("BrakingSubStepTime"), M->BrakingSubStepTime);
	F(TEXT("GravityZ"), M->GetGravityZ());
	F(TEXT("GravityScale"), M->GravityScale);
	F(TEXT("JumpZVelocity"), M->JumpZVelocity);
	F(TEXT("AirControl"), M->AirControl);
	F(TEXT("AirControlBoostMultiplier"), M->AirControlBoostMultiplier);
	F(TEXT("AirControlBoostVelocityThreshold"), M->AirControlBoostVelocityThreshold);
	F(TEXT("FallingLateralFriction"), M->FallingLateralFriction);
	F(TEXT("MaxStepHeight"), M->MaxStepHeight);
	F(TEXT("WalkableFloorZ"), M->GetWalkableFloorZ());
	F(TEXT("PerchRadiusThreshold"), M->GetPerchRadiusThreshold());
	F(TEXT("PerchAdditionalHeight"), M->PerchAdditionalHeight);
	F(TEXT("bMaintainHorizontalGroundVelocity"), M->bMaintainHorizontalGroundVelocity ? 1 : 0);
	F(TEXT("bAlwaysCheckFloor"), M->bAlwaysCheckFloor ? 1 : 0);
	F(TEXT("MaxSimulationTimeStep"), M->MaxSimulationTimeStep);
	F(TEXT("MaxSimulationIterations"), M->MaxSimulationIterations);
	F(TEXT("MaxJumpApexAttemptsPerSimulation"), M->MaxJumpApexAttemptsPerSimulation);
	F(TEXT("bApplyGravityWhileJumping"), M->bApplyGravityWhileJumping ? 1 : 0);
	F(TEXT("bDontFallBelowJumpZVelocityDuringJump"), M->bDontFallBelowJumpZVelocityDuringJump ? 1 : 0);
	F(TEXT("MaxDepenetrationWithGeometry"), M->MaxDepenetrationWithGeometry);
	F(TEXT("TerminalVelocity"), M->GetPhysicsVolume() ? M->GetPhysicsVolume()->TerminalVelocity : 0.f);
	F(TEXT("JumpMaxHoldTime"), Ch->JumpMaxHoldTime);
	F(TEXT("JumpMaxCount"), Ch->JumpMaxCount);
	F(TEXT("bForceMaxAccel"), M->bForceMaxAccel ? 1 : 0);
	F(TEXT("bUseFlatBaseForFloorChecks"), M->bUseFlatBaseForFloorChecks ? 1 : 0);
	F(TEXT("MinAnalogWalkSpeed"), M->MinAnalogWalkSpeed);
	F(TEXT("bCanWalkOffLedges"), M->bCanWalkOffLedges ? 1 : 0);
	F(TEXT("LedgeCheckThreshold"), M->LedgeCheckThreshold);
	F(TEXT("CapsuleRadius"), Ch->GetCapsuleComponent()->GetScaledCapsuleRadius());
	F(TEXT("CapsuleHalfHeight"), Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	F(TEXT("Seed"), Seed);
}

bool UMOBMoveRecordSubsystem::StartSegment(ULyraCharacterMovementComponent* M, ACharacter* Ch)
{
	++SegIndex;
	SegTime = 0.f;
	bPrevValid = false;
	NextDirAt = 0;
	JumpFramesLeft = 0;
	CurrentInput = FVector::ZeroVector;
	if (SegIndex >= NumSegs) return false;

	UWorld* World = GetWorld();
	UCapsuleComponent* Capsule = Ch->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBMoveStart), false, Ch);
	const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());

	bool bPlaced = false;
	for (int32 Try = 0; Try < START_TRIES && !bPlaced; ++Try)
	{
		const double X = Rng.FRandRange(PlayArea.Min.X, PlayArea.Max.X);
		const double Y = Rng.FRandRange(PlayArea.Min.Y, PlayArea.Max.Y);
		const FVector A(X, Y, PlayArea.Max.Z + 3000.0);
		const FVector B(X, Y, PlayArea.Min.Z - 3000.0);
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, A, B, Capsule->GetCollisionObjectType(), Params, Response)) continue;
		if (Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.7) continue;

		const bool bEmbed = bStress && (SegIndex % 2) == 0;
		const FVector Spot = Hit.ImpactPoint + FVector(0, 0, HalfHeight + (bEmbed ? -STRESS_EMBED : START_ABOVE_FLOOR));
		if (!bEmbed && World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response)) continue;

		M->UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		bPlaced = true;
	}
	if (!bPlaced) ++StartMissCount;

	Ch->StopJumping();
	M->Velocity = FVector::ZeroVector;
	M->PendingLaunchVelocity = FVector::ZeroVector;
	M->SetMovementMode(MOVE_Falling);
	Ch->JumpCurrentCount = 0;
	Ch->JumpCurrentCountPreJump = 0;
	return true;
}

void UMOBMoveRecordSubsystem::OnPreMove(ULyraCharacterMovementComponent* M, FVector& Input, float Dt)
{
	bRowOpen = false;
	if (bDone || SegIndex >= NumSegs) return;

	ACharacter* Ch = M->GetCharacterOwner();
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!Ch || !PC || PC->GetPawn() != Ch) return;

	// 죽어 이동이 꺼진 폰은 적지 않는다. 다시 태어나면 컴포넌트가 바뀌어 새 구간이 된다
	if (M->MovementMode != MOVE_Walking && M->MovementMode != MOVE_Falling) return;

	if (LastComp.Get() != M || SegTime >= SegSeconds)
	{
		LastComp = M;
		if (!StartSegment(M, Ch)) return;
	}

	// 입력 — 방향은 0.2~3초마다, 크기는 {1,1,1,0.5,0} 중에서
	if (SegTime >= NextDirAt)
	{
		const double Angle = Rng.FRandRange(0.0, UE_DOUBLE_TWO_PI);
		static const double Mags[] = { 1.0, 1.0, 1.0, 0.5, 0.0 };
		const double Mag = Mags[Rng.RandRange(0, 4)];
		CurrentInput = FVector(FMath::Cos(Angle) * Mag, FMath::Sin(Angle) * Mag, 0.0);
		NextDirAt = SegTime + Rng.FRandRange(0.2, 3.0);
	}
	Input = CurrentInput;

	// 점프 — 평균 1.5초에 한 번, 1~20프레임 누른다
	int32 Ev = 0;
	if (JumpFramesLeft > 0)
	{
		if (--JumpFramesLeft == 0)
		{
			Ch->StopJumping();
			Ev = 2;
		}
	}
	else if (Rng.FRand() < Dt / 1.5f)
	{
		Ch->Jump();
		Ev = 1;
		JumpFramesLeft = Rng.RandRange(1, 20);
	}

	if (bStress && (SegIndex % 2) == 1 && Rng.FRand() < Dt / STRESS_LAUNCH_EVERY)
	{
		const FVector Launch(Rng.FRandRange(-600.0, 600.0), Rng.FRandRange(-600.0, 600.0), Rng.FRandRange(300.0, 900.0));
		Ch->LaunchCharacter(Launch, true, true);
	}

	const FVector P = M->UpdatedComponent->GetComponentLocation();
	const FVector V = M->Velocity;
	const FVector L = M->PendingLaunchVelocity;
	const bool bExt = bPrevValid && P != PrevAfterLocation;
	if (bExt) ++ExtCount;

	Row = FString::Printf(TEXT("%d,%d,%d,%.9g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%.17g,%.17g,%.17g,%d,%d,%.9g,%.9g,%d,%d,%.9g,"),
		Frame, SegIndex, Ev, Dt, Input.X, Input.Y, Input.Z, P.X, P.Y, P.Z, V.X, V.Y, V.Z, (int32)M->MovementMode, bExt ? 1 : 0, L.X, L.Y, L.Z,
		Ch->bPressedJump ? 1 : 0, Ch->bWasJumping ? 1 : 0, Ch->JumpKeyHoldTime, Ch->JumpForceTimeRemaining, Ch->JumpCurrentCount, Ch->JumpCurrentCountPreJump, M->GetMaxSpeed());
	bRowOpen = true;
}

void UMOBMoveRecordSubsystem::OnPostMove(ULyraCharacterMovementComponent* M, float Dt)
{
	if (!bRowOpen) return;
	bRowOpen = false;

	ACharacter* Ch = M->GetCharacterOwner();
	const FVector P = M->UpdatedComponent->GetComponentLocation();
	const FVector V = M->Velocity;
	const FFindFloorResult& Floor = M->CurrentFloor;
	Csv += Row;
	Csv += FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%d,%d,%.9g,%.9g,%d\n"),
		P.X, P.Y, P.Z, V.X, V.Y, V.Z, (int32)M->MovementMode, Floor.bBlockingHit ? 1 : 0, Floor.bWalkableFloor ? 1 : 0, Floor.bLineTrace ? 1 : 0,
		Floor.FloorDist, Floor.LineDist, Ch ? Ch->JumpCurrentCount : 0);

	PrevAfterLocation = P;
	bPrevValid = true;
	++Frame;
	SegTime += Dt;
}

void UMOBMoveRecordSubsystem::Finish()
{
	bDone = true;
	ULyraCharacterMovementComponent::MOBPreMoveHook = nullptr;
	ULyraCharacterMovementComponent::MOBPostMoveHook = nullptr;

	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("move_%s.csv"), *Tag);
	const bool bSaved = FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_DONE saved=%d path=%s frames=%d segs=%d ext=%d startMiss=%d realSec=%.1f"),
		bSaved ? 1 : 0, *Path, Frame, FMath::Min(SegIndex + 1, NumSegs), ExtCount, StartMissCount, FPlatformTime::Seconds() - HookedRealAt);
	FPlatformMisc::RequestExit(false);
}
