#include "MOBMoveRecordSubsystem.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
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

#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Character/LyraCharacter.h"
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
	constexpr int32 LOW_SPOT_COLUMNS = 200000;	// 낮은 천장 찾기 — 세로 줄 수. 한 줄에 위에서부터 바닥을 여러 겹 본다(지붕 밑 바닥까지)
	constexpr int32 LOW_SPOT_LAYERS = 6;
	constexpr int32 LOW_SPOT_MAX = 64;
	constexpr double LOW_SPOT_APART = 150.0;	// 같은 자리를 여러 번 담지 않게

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
	bAds = FParse::Param(Cmd, TEXT("MOBMoveAds"));
	bCrouch = FParse::Param(Cmd, TEXT("MOBMoveCrouch"));
	bPads = FParse::Param(Cmd, TEXT("MOBMovePads"));
	bTele = FParse::Param(Cmd, TEXT("MOBMoveTeleports")) && !bPads;	// 둘 다면 점프대 기록(record_moves.ps1 은 둘을 같이 못 넘긴다)
	Rng.Initialize(Seed);
	AdsRng.Initialize(Seed + 1);
	CrouchRng.Initialize(Seed + 2);
	PadRng.Initialize(Seed + 4);	// +3 은 낮은 천장 찾기(FindLowSpots)
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
	if (bPads || bTele) FindPads();
	if (bTele) FindTeles();
	Csv += TEXT("frame,seg,ev,dt,ix,iy,iz,px,py,pz,vx,vy,vz,mode,ext,lx,ly,lz,pressed,was,hold,force,count,countPre,maxSpeed,")
		TEXT("ax,ay,az,avx,avy,avz,amode,fBlock,fWalk,fLine,fDist,fLineDist,aCount");
	if (bAds) Csv += TEXT(",ads");
	if (bCrouch) Csv += TEXT(",cev,cwant,crouched,half,walk,cwantA,crouchedA,halfA");
	if (bPads || bTele) Csv += TEXT(",yaw,pads,cue");
	if (bTele) Csv += TEXT(",tele,tdev,tcue,cyaw");
	Csv += TEXT("\n");
	if (bCrouch) FindLowSpots(Ch);

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
	F(TEXT("MaxWalkSpeedCrouched"), M->MaxWalkSpeedCrouched);
	F(TEXT("CrouchedHalfHeight"), M->GetCrouchedHalfHeight());
	F(TEXT("bCanWalkOffLedgesWhenCrouching"), M->bCanWalkOffLedgesWhenCrouching ? 1 : 0);
	F(TEXT("BaseEyeHeight"), Ch->BaseEyeHeight);
	F(TEXT("CrouchedEyeHeight"), Ch->CrouchedEyeHeight);
	F(TEXT("Seed"), Seed);
}

void UMOBMoveRecordSubsystem::FindLowSpots(ACharacter* Ch)
{
	// 앉은 캡슐은 들어가고 선 캡슐은 안 들어가는 바닥 — 반지름이 같으니 막는 것은 위쪽 50cm 안의 천장이다.
	// 하늘에서 내린 광선은 지붕에 먼저 닿으므로, 닿은 점 바로 밑에서 다시 내려 지붕 밑 바닥까지 겹겹이 본다(메시 뒷면은 안 닿는다)
	UWorld* World = GetWorld();
	UCharacterMovementComponent* M = Ch->GetCharacterMovement();
	UCapsuleComponent* Capsule = Ch->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Stand = Capsule->GetScaledCapsuleHalfHeight();
	const float Low = M->GetCrouchedHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBMoveLow), false, Ch);
	const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
	const ECollisionChannel Channel = Capsule->GetCollisionObjectType();
	FRandomStream LowRng(Seed + 3);
	int32 Floors = 0;
	const double T0 = FPlatformTime::Seconds();
	for (int32 C = 0; C < LOW_SPOT_COLUMNS && LowSpots.Num() < LOW_SPOT_MAX; ++C)
	{
		const double X = LowRng.FRandRange(PlayArea.Min.X, PlayArea.Max.X);
		const double Y = LowRng.FRandRange(PlayArea.Min.Y, PlayArea.Max.Y);
		double TopZ = PlayArea.Max.Z + 3000.0;
		for (int32 L = 0; L < LOW_SPOT_LAYERS; ++L)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, FVector(X, Y, TopZ), FVector(X, Y, PlayArea.Min.Z - 3000.0), Channel, Params, Response)) break;
			TopZ = Hit.ImpactPoint.Z - 1.0;
			if (Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.7) continue;
			++Floors;
			const FVector LowSpot = Hit.ImpactPoint + FVector(0, 0, Low + START_ABOVE_FLOOR);
			const FVector StandSpot = Hit.ImpactPoint + FVector(0, 0, Stand + START_ABOVE_FLOOR);
			if (World->OverlapBlockingTestByChannel(LowSpot, FQuat::Identity, Channel, FCollisionShape::MakeCapsule(Radius, Low), Params, Response)) continue;
			if (!World->OverlapBlockingTestByChannel(StandSpot, FQuat::Identity, Channel, FCollisionShape::MakeCapsule(Radius, Stand), Params, Response)) continue;
			bool bNear = false;
			for (const FVector& S : LowSpots) bNear |= FVector::Dist(S, LowSpot) < LOW_SPOT_APART;
			if (!bNear) LowSpots.Add(LowSpot);
		}
	}
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_LOWSPOTS found=%d floors=%d sec=%.2f"), LowSpots.Num(), Floors, FPlatformTime::Seconds() - T0);
	for (int32 I = 0; I < FMath::Min(LowSpots.Num(), 8); ++I)
	{
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_LOWSPOT %d %s"), I, *LowSpots[I].ToString());
	}
}

// 점프대 j-2 — 점프대 배우(B_Launcher_C 자식)와 겹침 구 칸(Sphere)을 이름 차례로 모은다. 차례 = make_launchers.py 줄 차례(이름 정렬)
void UMOBMoveRecordSubsystem::FindPads()
{
	static const FName LauncherName(TEXT("B_Launcher_C"));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		bool bLauncher = false;
		for (const UClass* C = It->GetClass(); C && !bLauncher; C = C->GetSuperClass()) bLauncher = C->GetFName() == LauncherName;
		if (!bLauncher) continue;
		TInlineComponentArray<UPrimitiveComponent*> Prims(*It);
		for (UPrimitiveComponent* P : Prims)
		{
			if (P->GetFName() != TEXT("Sphere") || !P->GetGenerateOverlapEvents()) continue;
			FPadSpot& S = Pads.AddDefaulted_GetRef();
			S.Actor = *It;
			S.Sphere = P;
			S.Name = It->GetName();
		}
	}
	Pads.Sort([](const FPadSpot& A, const FPadSpot& B) { return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0; });	// 파이썬 sorted 와 같은 대소문자 구별 차례
	for (int32 I = 0; I < Pads.Num(); ++I)
	{
		const FVector C = Pads[I].Sphere->GetComponentLocation();
		Csv += FString::Printf(TEXT("#pad %d %s %.4f %.4f %.4f\n"), I, *Pads[I].Name, C.X, C.Y, C.Z);
	}
	// 효과 배우가 새로 생긴 수 — 효과 배우는 다시 쓰는 통에 들어가므로 처음 한 번만 생긴다. 프레임 칸 cue 는 숨지 않은 효과 배우 수
	PadSpawnHandle = GetWorld()->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* A)
	{
		if (A && A->GetClass()->GetName().Contains(TEXT("GCNL_Launcher"))) ++PadCueSpawns;
	}));
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_PADS found=%d"), Pads.Num());
}

int32 UMOBMoveRecordSubsystem::CountCues() const
{
	int32 N = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->IsHidden() && It->GetClass()->GetName().Contains(TEXT("GCNL_Launcher"))) ++N;
	}
	return N;
}

// 구간 차례로 점프대 하나 — 구 칸 바깥 150~350cm 바닥(점프대 바닥 높이 ±150 안)에 놓고, 몸 방향 · 입력(점프대 쪽 ±15°)을 정한다
bool UMOBMoveRecordSubsystem::PlaceForPad(ULyraCharacterMovementComponent* M, ACharacter* Ch)
{
	if (Pads.Num() == 0) return false;
	const FPadSpot& Pad = Pads[SegIndex % Pads.Num()];
	const UPrimitiveComponent* Sphere = Pad.Sphere.Get();
	const AActor* Actor = Pad.Actor.Get();
	if (!Sphere || !Actor) return false;
	PadVariant = (SegIndex / Pads.Num()) % 4;
	bPadJumped = false;
	PadJumpAt = PadRng.FRandRange(0.0, 0.3);

	UWorld* World = GetWorld();
	UCapsuleComponent* Capsule = Ch->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBMovePad), false, Ch);
	const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
	const FVector Center = Sphere->GetComponentLocation();
	const double PadFloorZ = Actor->GetActorLocation().Z;
	const double SphereR = Sphere->Bounds.SphereRadius;
	for (int32 Try = 0; Try < START_TRIES; ++Try)
	{
		const double Angle = PadRng.FRandRange(0.0, UE_DOUBLE_TWO_PI);
		const double Dist = SphereR + PadRng.FRandRange(150.0, 350.0);
		const double X = Center.X + FMath::Cos(Angle) * Dist;
		const double Y = Center.Y + FMath::Sin(Angle) * Dist;
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, FVector(X, Y, PadFloorZ + 400.0), FVector(X, Y, PadFloorZ - 600.0), Capsule->GetCollisionObjectType(), Params, Response)) continue;
		if (Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.7 || FMath::Abs(Hit.ImpactPoint.Z - PadFloorZ) > 150.0) continue;
		const FVector Spot = Hit.ImpactPoint + FVector(0, 0, HalfHeight + START_ABOVE_FLOOR);
		if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response)) continue;

		M->UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		const double ToPad = FMath::Atan2(Center.Y - Spot.Y, Center.X - Spot.X);
		const double InputAngle = ToPad + FMath::DegreesToRadians(PadRng.FRandRange(-15.0, 15.0));
		const double Mag = PadVariant == 3 ? 0.5 : 1.0;
		CurrentInput = FVector(FMath::Cos(InputAngle) * Mag, FMath::Sin(InputAngle) * Mag, 0.0);
		// 몸 방향 — 0 은 점프대 쪽, 나머지는 아무 쪽. 밀기 점프대는 이 방향으로 날린다(그래프 Target Method = 몸 앞방향)
		const double Yaw = PadVariant == 0 ? FMath::RadiansToDegrees(ToPad) : PadRng.FRandRange(-180.0, 180.0);
		if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		Ch->SetActorRotation(FRotator(0.0, Yaw, 0.0));
		++PadPlaced;
		return true;
	}
	++PadMiss;
	return false;
}

// 순간이동 t-2 — 순간이동 배우(B_Teleport_C 자식)와 겹침 상자(Box)를 이름 차례로 모은다. 차례 = make_teleports.py 줄 차례(이름 정렬)
void UMOBMoveRecordSubsystem::FindTeles()
{
	static const FName TeleportName(TEXT("B_Teleport_C"));
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		bool bTeleport = false;
		for (const UClass* C = It->GetClass(); C && !bTeleport; C = C->GetSuperClass()) bTeleport = C->GetFName() == TeleportName;
		if (!bTeleport) continue;
		TInlineComponentArray<UPrimitiveComponent*> Prims(*It);
		for (UPrimitiveComponent* P : Prims)
		{
			if (P->GetFName() != TEXT("Box") || !P->GetGenerateOverlapEvents()) continue;
			FPadSpot& S = Teles.AddDefaulted_GetRef();
			S.Actor = *It;
			S.Sphere = P;
			S.Name = It->GetName();
		}
	}
	Teles.Sort([](const FPadSpot& A, const FPadSpot& B) { return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0; });
	TeleByDevice.Init(0, Teles.Num());
	for (int32 I = 0; I < Teles.Num(); ++I)
	{
		const FVector C = Teles[I].Sphere->GetComponentLocation();
		Csv += FString::Printf(TEXT("#tele %d %s %.4f %.4f %.4f\n"), I, *Teles[I].Name, C.X, C.Y, C.Z);
	}
	TeleSpawnHandle = GetWorld()->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* A)
	{
		if (A && A->GetClass()->GetName().Contains(TEXT("GCNL_Teleporter"))) ++TeleCueSpawns;
	}));
	UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_TELES found=%d"), Teles.Num());
}

int32 UMOBMoveRecordSubsystem::CountTeleCues() const
{
	int32 N = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (!It->IsHidden() && It->GetClass()->GetName().Contains(TEXT("GCNL_Teleporter"))) ++N;
	}
	return N;
}

// 구간 차례로 순간이동 하나 — 상자 앞쪽(배우 앞방향 ±60°) 바깥 150~350cm 바닥(배우 높이 ±150 안)에 놓는다. 입력 · 몸 방향은 점프대 모드와 같은 규칙.
// 앞쪽만 — 점프대 기록(move_expanse_pads.csv 프레임 10002)의 문 포털도 앞에서 들어갔다. 옆 · 뒤로 들어갈 수 있는지는 안 봤다. 거리 기준은 상자 수평 반대각선(놓은 자리가 상자와 겹쳐 바로 옮겨지지 않게)
bool UMOBMoveRecordSubsystem::PlaceForTele(ULyraCharacterMovementComponent* M, ACharacter* Ch)
{
	if (Teles.Num() == 0) return false;
	const FPadSpot& Tele = Teles[SegIndex % Teles.Num()];
	const UBoxComponent* Box = Cast<UBoxComponent>(Tele.Sphere.Get());
	const AActor* Actor = Tele.Actor.Get();
	if (!Box || !Actor) return false;
	PadVariant = (SegIndex / Teles.Num()) % 4;
	bPadJumped = false;
	PadJumpAt = PadRng.FRandRange(0.0, 0.3);

	UWorld* World = GetWorld();
	UCapsuleComponent* Capsule = Ch->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBMoveTele), false, Ch);
	const FCollisionResponseParams Response(Capsule->GetCollisionResponseToChannels());
	const FVector Center = Box->GetComponentLocation();
	const FVector Ext = Box->GetScaledBoxExtent();
	const double FloorZ = Actor->GetActorLocation().Z;
	const double HalfDiag = FMath::Sqrt(Ext.X * Ext.X + Ext.Y * Ext.Y);
	const double Forward = FMath::Atan2(Actor->GetActorForwardVector().Y, Actor->GetActorForwardVector().X);
	for (int32 Try = 0; Try < START_TRIES; ++Try)
	{
		const double Angle = Forward + FMath::DegreesToRadians(PadRng.FRandRange(-60.0, 60.0));
		const double Dist = HalfDiag + PadRng.FRandRange(150.0, 350.0);
		const double X = Center.X + FMath::Cos(Angle) * Dist;
		const double Y = Center.Y + FMath::Sin(Angle) * Dist;
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, FVector(X, Y, FloorZ + 400.0), FVector(X, Y, FloorZ - 600.0), Capsule->GetCollisionObjectType(), Params, Response)) continue;
		if (Hit.bStartPenetrating || Hit.ImpactNormal.Z < 0.7 || FMath::Abs(Hit.ImpactPoint.Z - FloorZ) > 150.0) continue;
		const FVector Spot = Hit.ImpactPoint + FVector(0, 0, HalfHeight + START_ABOVE_FLOOR);
		if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response)) continue;

		M->UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		const double ToTele = FMath::Atan2(Center.Y - Spot.Y, Center.X - Spot.X);
		const double InputAngle = ToTele + FMath::DegreesToRadians(PadRng.FRandRange(-15.0, 15.0));
		const double Mag = PadVariant == 3 ? 0.5 : 1.0;
		CurrentInput = FVector(FMath::Cos(InputAngle) * Mag, FMath::Sin(InputAngle) * Mag, 0.0);
		const double Yaw = PadVariant == 0 ? FMath::RadiansToDegrees(ToTele) : PadRng.FRandRange(-180.0, 180.0);
		if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		Ch->SetActorRotation(FRotator(0.0, Yaw, 0.0));
		++TelePlaced;
		return true;
	}
	// 앞에 바닥이 없으면 상자 위 50cm 에서 떨어뜨린다 — L_Expanse 위층 순간이동 둘은 걸어서 못 들어간다(바로 아래 바닥 z 10 이 600cm 밑 ·
	// 가장 가까운 z 305 바닥이 640cm 떨어짐, nav_expanse.monv). 받는 자리로만 쓰이지만 들어가는 셈의 정답지가 있어야 해서 떨어뜨려 넣는다.
	// 자리 = 상자 바닥 넓이 안 무작위(캡슐 반지름만큼 안쪽) · 갈래 0 은 입력 없음, 나머지는 앞방향 ±15° 쪽(공중이라 거의 안 밀린다)
	for (int32 Try = 0; Try < START_TRIES; ++Try)
	{
		const FVector Fwd = Box->GetForwardVector(), Right = Box->GetRightVector();
		const double F = PadRng.FRandRange(-1.0, 1.0) * FMath::Max(0.0, Ext.X - Radius);
		const double R = PadRng.FRandRange(-1.0, 1.0) * FMath::Max(0.0, Ext.Y - Radius);
		const FVector Spot = Center + Fwd * F + Right * R + FVector(0, 0, Ext.Z + HalfHeight + 50.0);
		if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response)) continue;

		M->UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		const double InputAngle = Forward + FMath::DegreesToRadians(PadRng.FRandRange(-15.0, 15.0));
		const double Mag = PadVariant == 0 ? 0.0 : PadVariant == 3 ? 0.5 : 1.0;
		CurrentInput = FVector(FMath::Cos(InputAngle) * Mag, FMath::Sin(InputAngle) * Mag, 0.0);
		const double Yaw = PadVariant == 0 ? FMath::RadiansToDegrees(Forward) : PadRng.FRandRange(-180.0, 180.0);
		if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		Ch->SetActorRotation(FRotator(0.0, Yaw, 0.0));
		++TeleDropped;
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_TELE_DROP seg=%d dev=%d kind=above at=%s"), SegIndex, SegIndex % Teles.Num(), *Spot.ToString());
		return true;
	}
	{
		// 위가 막혔다 — 무엇이 막는지 한 번 적는다
		const FVector Above = Center + FVector(0, 0, Ext.Z + HalfHeight + 50.0);
		TArray<FOverlapResult> Hits;
		World->OverlapMultiByChannel(Hits, Above, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response);
		FString Names;
		for (const FOverlapResult& H : Hits) if (H.bBlockingHit) Names += FString::Printf(TEXT("%s.%s "), *GetNameSafe(H.GetActor()), *GetNameSafe(H.GetComponent()));
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_TELE_ABOVE_BLOCKED seg=%d dev=%d at=%s by=%s"), SegIndex, SegIndex % Teles.Num(), *Above.ToString(), *Names);
	}
	// 그것도 막히면 상자 앞면 5cm 앞 공중(캡슐 꼭대기 = 상자 꼭대기)에 놓고 상자 쪽으로 민다 — 공중 조종으로 앞면을 넘어 들어간다. 갈래 3 은 반 입력, 나머지는 온 입력
	for (int32 Try = 0; Try < START_TRIES; ++Try)
	{
		const FVector Fwd = Box->GetForwardVector(), Right = Box->GetRightVector();
		const double R = PadRng.FRandRange(-1.0, 1.0) * FMath::Max(0.0, Ext.Y - Radius);
		const FVector Spot = Center + Fwd * (Ext.X + Radius + 5.0) + Right * R + FVector(0, 0, Ext.Z - HalfHeight);
		if (World->OverlapBlockingTestByChannel(Spot, FQuat::Identity, Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, Response)) continue;

		M->UpdatedComponent->SetWorldLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
		const double InputAngle = Forward + UE_DOUBLE_PI + FMath::DegreesToRadians(PadRng.FRandRange(-15.0, 15.0));
		const double Mag = PadVariant == 3 ? 0.5 : 1.0;
		CurrentInput = FVector(FMath::Cos(InputAngle) * Mag, FMath::Sin(InputAngle) * Mag, 0.0);
		const double Yaw = PadVariant == 0 ? FMath::RadiansToDegrees(Forward) + 180.0 : PadRng.FRandRange(-180.0, 180.0);
		if (APlayerController* PC = Cast<APlayerController>(Ch->GetController())) PC->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		Ch->SetActorRotation(FRotator(0.0, Yaw, 0.0));
		++TeleDropped;
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_TELE_DROP seg=%d dev=%d kind=front at=%s"), SegIndex, SegIndex % Teles.Num(), *Spot.ToString());
		return true;
	}
	++TeleMiss;
	return false;
}

bool UMOBMoveRecordSubsystem::StartSegment(ULyraCharacterMovementComponent* M, ACharacter* Ch)
{
	++SegIndex;
	SegTime = 0.f;
	bPrevValid = false;
	NextDirAt = 0;
	NextAdsAt = 0;	// 구간 시간 기준이라 구간마다 처음부터. 누른 상태는 이어 간다
	NextCrouchAt = bCrouch ? CrouchRng.FRandRange(0.2, 2.5) : 0;	// 앉은 상태도 이어 간다(순간 이동한 같은 몸)
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
	if (bPads || bTele)
	{
		bPlaced = bPads ? PlaceForPad(M, Ch) : PlaceForTele(M, Ch);
		if (!bPlaced) ++StartMissCount;
	}
	// 앉기 기록의 셋째 구간마다 — 낮은 천장 밑에 앉은 채 둔다. 낙하 모드에서 앉혀야 캡슐이 중심 그대로 줄어든다(걷기 모드면 바닥 쪽으로 쓸어 내린다)
	if (bCrouch && LowSpots.Num() > 0 && (SegIndex % 3) == 2)
	{
		M->SetMovementMode(MOVE_Falling);
		Ch->Crouch();
		M->Crouch(false);
		M->UpdatedComponent->SetWorldLocation(LowSpots[LowSegs++ % LowSpots.Num()], false, nullptr, ETeleportType::TeleportPhysics);
		bPlaced = true;
	}
	for (int32 Try = 0; Try < START_TRIES && !bPlaced && !bPads && !bTele; ++Try)
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

	// 입력 — 방향은 0.2~3초마다, 크기는 {1,1,1,0.5,0} 중에서. 점프대 기록은 구간 내내 놓을 때 정한 입력
	if (!bPads && !bTele && SegTime >= NextDirAt)
	{
		const double Angle = Rng.FRandRange(0.0, UE_DOUBLE_TWO_PI);
		static const double Mags[] = { 1.0, 1.0, 1.0, 0.5, 0.0 };
		const double Mag = Mags[Rng.RandRange(0, 4)];
		CurrentInput = FVector(FMath::Cos(Angle) * Mag, FMath::Sin(Angle) * Mag, 0.0);
		NextDirAt = SegTime + Rng.FRandRange(0.2, 3.0);
	}
	Input = CurrentInput;

	// 앉기 — 이 프레임 누름 · 점프 전의 상태(한 프레임 대조가 여기서 시작한다)
	if (bCrouch)
	{
		bWantsBefore = M->bWantsToCrouch;
		bCrouchedBefore = Ch->bIsCrouched;
		HalfBefore = Ch->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
		WalkBefore = M->MaxWalkSpeed;
		CrouchEv = 0;
	}

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
	else if (bPads || bTele)
	{
		// 점프대 갈래 2 — 구간 처음 0~0.3초에 한 번, 10프레임 눌러 떨어지며 들어간다
		if (PadVariant == 2 && !bPadJumped && SegTime >= PadJumpAt)
		{
			bPadJumped = true;
			Ch->Jump();
			Ev = 1;
			JumpFramesLeft = 10;
		}
	}
	else if (Rng.FRand() < Dt / 1.5f)
	{
		// Lyra 점프 능력은 점프 전에 일어서기를 부른다(LyraGameplayAbility_Jump.cpp:55) — 앉기 기록에서만 같은 순서로
		if (bCrouch && (Ch->bIsCrouched || M->bWantsToCrouch))
		{
			Ch->UnCrouch();
			CrouchEv = 2;
			++JumpUncrouches;
		}
		Ch->Jump();
		Ev = 1;
		JumpFramesLeft = Rng.RandRange(1, 20);
	}

	if (bStress && (SegIndex % 2) == 1 && Rng.FRand() < Dt / STRESS_LAUNCH_EVERY)
	{
		const FVector Launch(Rng.FRandRange(-600.0, 600.0), Rng.FRandRange(-600.0, 600.0), Rng.FRandRange(300.0, 900.0));
		Ch->LaunchCharacter(Launch, true, true);
	}

	// 앉기 누름 — 키와 같은 길. 점프 누른 프레임은 건너뛰고 다음 프레임에 누른다(한 프레임에 두 사건을 겹치지 않게)
	if (bCrouch && Ev != 1 && SegTime >= NextCrouchAt)
	{
		NextCrouchAt = SegTime + CrouchRng.FRandRange(0.2, 2.5);
		if (ALyraCharacter* LCh = Cast<ALyraCharacter>(Ch))
		{
			LCh->ToggleCrouch();
			CrouchEv = 1;
			++CrouchPresses;
		}
	}

	// 조준 — 이번 프레임 최대 속도(maxSpeed 칸)는 앞 프레임까지 누른 것으로 정해져 있다. 여기서 바꾼 누름은 다음 프레임에 먹는다
	if (bAds)
	{
		static const FGameplayTag AdsInput = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.ADS"));
		ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Ch));
		if (SegTime >= NextAdsAt)
		{
			NextAdsAt = SegTime + AdsRng.FRandRange(0.05, 1.2);
			const bool bWant = AdsRng.FRand() < 0.5f;
			if (A && bWant != bAdsHeld)
			{
				bAdsHeld = bWant;
				if (!bWant) A->AbilityInputTagReleased(AdsInput);
			}
		}
		// 사람 입력과 같게 누르고 있는 동안 매 프레임 「눌림」을 보낸다 — IA_ADS 매핑엔 트리거가 없어
		// 눌린 동안 매 프레임 Triggered(EnhancedPlayerInput.cpp SetStateForNoTriggers), Lyra는 Triggered마다 Pressed(LyraInputComponent.h BindAbilityActions).
		// 한 번만 보내면 다시 태어난 몸의 첫 프레임처럼 능력이 아직 안 붙었을 때 누름이 버려지고, 다음 누름까지 600으로 남는다(11-15 gym_ads 구간 5·39)
		if (A && bAdsHeld) A->AbilityInputTagPressed(AdsInput);
	}

	const FVector P = M->UpdatedComponent->GetComponentLocation();
	const FVector V = M->Velocity;
	const FVector L = M->PendingLaunchVelocity;
	const bool bExt = bPrevValid && P != PrevAfterLocation;
	if (bExt) ++ExtCount;
	if ((bPads || bTele) && !L.IsZero()) ++PadLaunchRows;
	if (bTele)
	{
		if (bPrevTeleMove && !L.IsZero()) ++TeleLaunchNext;	// 옮긴 다음 이동 머리에 발사가 차 있나(그래프 LaunchCharacter)
		PreMoveLocation = P;
	}

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
	Csv += FString::Printf(TEXT("%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%d,%d,%.9g,%.9g,%d"),
		P.X, P.Y, P.Z, V.X, V.Y, V.Z, (int32)M->MovementMode, Floor.bBlockingHit ? 1 : 0, Floor.bWalkableFloor ? 1 : 0, Floor.bLineTrace ? 1 : 0,
		Floor.FloorDist, Floor.LineDist, Ch ? Ch->JumpCurrentCount : 0);
	if (bAds) Csv += FString::Printf(TEXT(",%d"), bAdsHeld ? 1 : 0);
	if (bCrouch)
	{
		const bool bWantsA = M->bWantsToCrouch;
		const bool bCrouchedA = Ch && Ch->bIsCrouched;
		const float HalfA = Ch ? Ch->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 0.f;
		Csv += FString::Printf(TEXT(",%d,%d,%d,%.9g,%.9g,%d,%d,%.9g"), CrouchEv, bWantsBefore ? 1 : 0, bCrouchedBefore ? 1 : 0, HalfBefore, WalkBefore,
			bWantsA ? 1 : 0, bCrouchedA ? 1 : 0, HalfA);
		CrouchedFrames += bCrouchedA ? 1 : 0;
		BlockedFrames += (bCrouchedA && !bWantsA) ? 1 : 0;	// 일어서려는데 못 일어선 프레임(엔진은 매 프레임 다시 시도한다)
		FallCrouchedFrames += (bCrouchedA && M->MovementMode == MOVE_Falling) ? 1 : 0;
		CrouchChanges += (bCrouchedA != bCrouchedBefore) ? 1 : 0;
	}
	if (bPads || bTele)
	{
		// 이동 뒤 — 겹침 시작은 이동 묶음(FScopedCapsuleMovementUpdate)이 닫힐 때 이미 났다. 그래프가 읽은 앞방향 · 속도가 이 줄의 yaw · avx~avz
		uint32 Mask = 0;
		for (int32 I = 0; I < Pads.Num() && I < 32; ++I)
		{
			const UPrimitiveComponent* Sphere = Pads[I].Sphere.Get();
			if (Sphere && Ch && Sphere->IsOverlappingActor(Ch)) Mask |= 1u << I;
		}
		const int32 Cues = CountCues();
		PadOverlapRows += Mask != 0 ? 1 : 0;
		PadCueRows += Cues > 0 ? 1 : 0;
		Csv += FString::Printf(TEXT(",%.9g,%u,%d"), Ch ? Ch->GetActorRotation().Yaw : 0.0, Mask, Cues);
	}
	if (bTele)
	{
		// 옮김은 이동 안에서 끝났다 — 이동 뒤 상자 비트는 보통 0(옮긴 자리는 상자 밖). 옮겼는지는 이동 앞뒤 거리로(한 이동 500cm 넘게 = 순간이동, MoveCheck IsTeleportMove 와 같은 기준)
		uint32 Mask = 0;
		for (int32 I = 0; I < Teles.Num() && I < 32; ++I)
		{
			const UPrimitiveComponent* Box = Teles[I].Sphere.Get();
			if (Box && Ch && Box->IsOverlappingActor(Ch)) Mask |= 1u << I;
		}
		int32 Dev = -1;
		if (FVector::Dist(P, PreMoveLocation) > 500.0)
		{
			double Best = TNumericLimits<double>::Max();
			for (int32 I = 0; I < Teles.Num(); ++I)
			{
				const UPrimitiveComponent* Box = Teles[I].Sphere.Get();
				const double D = Box ? FVector::Dist(Box->GetComponentLocation(), PreMoveLocation) : Best;
				if (D < Best) { Best = D; Dev = I; }
			}
			++TeleMoves;
			if (TeleByDevice.IsValidIndex(Dev)) ++TeleByDevice[Dev];
		}
		bPrevTeleMove = Dev >= 0;
		const int32 Cues = CountTeleCues();
		TeleOverlapRows += Mask != 0 ? 1 : 0;
		TeleCueRows += Cues > 0 ? 1 : 0;
		const AController* C = Ch ? Ch->GetController() : nullptr;
		Csv += FString::Printf(TEXT(",%u,%d,%d,%.9g"), Mask, Dev, Cues, C ? C->GetControlRotation().Yaw : 0.0);
	}
	Csv += TEXT("\n");

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
	if (bCrouch)
	{
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_CROUCH presses=%d jumpUncrouch=%d changes=%d crouchedFrames=%d blockedFrames=%d fallCrouchedFrames=%d lowSpots=%d lowSegs=%d"),
			CrouchPresses, JumpUncrouches, CrouchChanges, CrouchedFrames, BlockedFrames, FallCrouchedFrames, LowSpots.Num(), LowSegs);
	}
	if (bPads || bTele)
	{
		GetWorld()->RemoveOnActorSpawnedHandler(PadSpawnHandle);
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_PADS_DONE pads=%d placed=%d miss=%d launchRows=%d overlapRows=%d cueRows=%d cueSpawns=%d"),
			Pads.Num(), PadPlaced, PadMiss, PadLaunchRows, PadOverlapRows, PadCueRows, PadCueSpawns);
	}
	if (bTele)
	{
		GetWorld()->RemoveOnActorSpawnedHandler(TeleSpawnHandle);
		FString ByDevice;
		for (int32 I = 0; I < TeleByDevice.Num(); ++I) ByDevice += FString::Printf(TEXT("%s%d"), I ? TEXT("/") : TEXT(""), TeleByDevice[I]);
		UE_LOG(LogMOBMove, Log, TEXT("MOB_MOVE_TELES_DONE teles=%d placed=%d dropped=%d miss=%d moves=%d launchNext=%d byDevice=%s overlapRows=%d cueRows=%d cueSpawns=%d"),
			Teles.Num(), TelePlaced, TeleDropped, TeleMiss, TeleMoves, TeleLaunchNext, *ByDevice, TeleOverlapRows, TeleCueRows, TeleCueSpawns);
	}
	FPlatformMisc::RequestExit(false);
}
