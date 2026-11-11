#include "MOBTraceRecorder.h"

#include "Components/CapsuleComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Physics/LyraCollisionChannels.h"

DEFINE_LOG_CATEGORY_STATIC(LogMOBTrace, Log, All);

namespace
{
	constexpr int32 RandomSeed = 20261102;
	constexpr double RayLength = 20000.0;
	constexpr double EyeHeight = 150.0;

	struct FTraceWriter
	{
		FArchive* Ar = nullptr;

		void Line(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			Ar->Serialize((void*)Utf8.Get(), Utf8.Length());
		}

		void Row(TCHAR Kind, const FVector& A, const FVector& B, bool bHit, const FHitResult& Hit, double Distance)
		{
			Line(FString::Printf(TEXT("%c,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.5f,%.5f,%.5f,%s\n"), Kind,
				A.X, A.Y, A.Z, B.X, B.Y, B.Z, bHit ? 1 : 0, (bHit && Hit.bStartPenetrating) ? 1 : 0,
				bHit ? Distance : 0.0, bHit ? Hit.ImpactNormal.X : 0.0, bHit ? Hit.ImpactNormal.Y : 0.0, bHit ? Hit.ImpactNormal.Z : 0.0,
				(bHit && Hit.GetActor()) ? *Hit.GetActor()->GetName() : TEXT("-")));
		}
	};
}

void MOBRecordTraces(UWorld* World, const ACharacter* Hero,const FBox& Bounds, const FString& OutPath, int32 RayCount, int32 SweepCount, double& OutRaySeconds, double& OutSweepSeconds)
{
	OutRaySeconds = OutSweepSeconds = 0;
	TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*OutPath));
	if (!Ar || !World || !Hero)
	{
		UE_LOG(LogMOBTrace, Error, TEXT("MOB_TRACE_FAIL path=%s"), *OutPath);
		return;
	}

	const UCapsuleComponent* Capsule = Hero->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();

	FTraceWriter W{ Ar.Get() };
	W.Line(FString::Printf(TEXT("# capsule r=%.3f hh=%.3f\n"), Radius, HalfHeight));
	W.Line(TEXT("kind,ax,ay,az,bx,by,bz,hit,penetrating,distance,nx,ny,nz,actor\n"));

	FRandomStream Rng(RandomSeed);

	// 무기: WeaponTrace 와 같은 조건(복잡 충돌 · 내 캐릭터 제외)
	FCollisionQueryParams RayParams(SCENE_QUERY_STAT(MOBTraceRecord), /*bTraceComplex=*/ true, Hero);

	// 이동: 캡슐 컴포넌트가 움직일 때 쓰는 응답 설정 그대로(단순 충돌)
	FCollisionQueryParams SweepParams(SCENE_QUERY_STAT(MOBTraceRecordSweep), /*bTraceComplex=*/ false, Hero);
	FCollisionResponseParams SweepResponse;
	Capsule->InitSweepCollisionParams(SweepParams, SweepResponse);
	SweepParams.bTraceComplex = false;
	SweepParams.AddIgnoredActor(Hero);
	const ECollisionChannel SweepChannel = Capsule->GetCollisionObjectType();
	const FCollisionShape SweepShape = FCollisionShape::MakeCapsule(Radius, HalfHeight);

	const FVector2D RangeX(Bounds.Min.X, Bounds.Max.X);
	const FVector2D RangeY(Bounds.Min.Y, Bounds.Max.Y);
	const FVector2D RangeZ(-500.0, FMath::Min(1400.0, Bounds.Max.Z));

	const auto RandomInAir = [&]()
	{
		return FVector(Rng.FRandRange(RangeX.X, RangeX.Y), Rng.FRandRange(RangeY.X, RangeY.Y), Rng.FRandRange(RangeZ.X, RangeZ.Y));
	};
	const auto RandomDir = [&]()
	{
		return Rng.GetUnitVector();
	};
	const auto Ray = [&](const FVector& A, const FVector& B, FHitResult& Hit)
	{
		return World->LineTraceSingleByChannel(Hit, A, B, Lyra_TraceChannel_Weapon, RayParams);
	};

	// 바닥 찾기(광선 D 로도 기록). 위를 보는 면에 맞았으면 true
	const auto FindFloor = [&](const FVector2D& XY, FVector& OutFloor, bool bRecord)
	{
		const FVector A(XY.X, XY.Y, Bounds.Max.Z + 100.0);
		const FVector B(XY.X, XY.Y, Bounds.Min.Z - 100.0);
		FHitResult Hit;
		const bool bHit = Ray(A, B, Hit);
		if (bRecord) W.Row(TEXT('D'), A, B, bHit, Hit, bHit ? (Hit.Location - A).Size() : 0.0);
		if (!bHit || Hit.ImpactNormal.Z < 0.7) return false;
		OutFloor = Hit.ImpactPoint;
		return true;
	};

	// 기록 비용을 뺀 Lyra 쪽 판정 시간은 묶음 끝에 재 본다(아래 시간 재기)
	const int32 PerRayGroup = RayCount / 3;
	for (int32 I = 0; I < PerRayGroup; ++I)
	{
		const FVector A = RandomInAir();
		const FVector B = A + RandomDir() * RayLength;
		FHitResult Hit;
		const bool bHit = Ray(A, B, Hit);
		W.Row(TEXT('R'), A, B, bHit, Hit, bHit ? (Hit.Location - A).Size() : 0.0);
	}

	TArray<FVector> Floors;
	Floors.Reserve(PerRayGroup);
	for (int32 I = 0; I < PerRayGroup; ++I)
	{
		FVector Floor;
		if (FindFloor(FVector2D(Rng.FRandRange(RangeX.X, RangeX.Y), Rng.FRandRange(RangeY.X, RangeY.Y)), Floor, true))
		{
			Floors.Add(Floor);
		}
	}
	UE_LOG(LogMOBTrace, Log, TEXT("MOB_TRACE_FLOORS tried=%d found=%d"), PerRayGroup, Floors.Num());

	if (Floors.Num() == 0)
	{
		UE_LOG(LogMOBTrace, Error, TEXT("MOB_TRACE_FAIL no floors"));
		Ar->Close();
		return;
	}

	for (int32 I = 0; I < PerRayGroup; ++I)
	{
		const FVector A = Floors[Rng.RandHelper(Floors.Num())] + FVector(0, 0, EyeHeight);
		const FRotator Aim(Rng.FRandRange(-15.0, 15.0), Rng.FRandRange(-180.0, 180.0), 0.0);
		const FVector B = A + Aim.Vector() * RayLength;
		FHitResult Hit;
		const bool bHit = Ray(A, B, Hit);
		W.Row(TEXT('E'), A, B, bHit, Hit, bHit ? (Hit.Location - A).Size() : 0.0);
	}

	const auto Sweep = [&](TCHAR Kind, const FVector& A, const FVector& B)
	{
		FHitResult Hit;
		const bool bHit = World->SweepSingleByChannel(Hit, A, B, FQuat::Identity, SweepChannel, SweepShape, SweepParams, SweepResponse);
		W.Row(Kind, A, B, bHit, Hit, bHit ? (Hit.TraceEnd - Hit.TraceStart).Size() * Hit.Time : 0.0);
	};

	const int32 PerSweepGroup = SweepCount / 3;
	for (int32 I = 0; I < PerSweepGroup; ++I)
	{
		// 걷는 높이: 바닥에서 2cm 띄운 캡슐을 수평으로
		const FVector A = Floors[Rng.RandHelper(Floors.Num())] + FVector(0, 0, HalfHeight + 2.0);
		const double Yaw = Rng.FRandRange(-180.0, 180.0);
		const FVector B = A + FRotator(0.0, Yaw, 0.0).Vector() * Rng.FRandRange(50.0, 1500.0);
		Sweep(TEXT('C'), A, B);
	}
	for (int32 I = 0; I < PerSweepGroup; ++I)
	{
		// 낙하: 위에서 바닥을 지나 아래로
		const FVector Floor = Floors[Rng.RandHelper(Floors.Num())];
		const FVector A = Floor + FVector(0, 0, HalfHeight + Rng.FRandRange(20.0, 600.0));
		const FVector B = Floor + FVector(Rng.FRandRange(-150.0, 150.0), Rng.FRandRange(-150.0, 150.0), -100.0);
		Sweep(TEXT('F'), A, B);
	}
	for (int32 I = 0; I < PerSweepGroup; ++I)
	{
		const FVector A = RandomInAir();
		const FVector B = A + RandomDir() * Rng.FRandRange(100.0, 3000.0);
		Sweep(TEXT('A'), A, B);
	}

	Ar->Close();

	// Lyra 쪽 한 번당 시간(참고값): 파일 쓰기를 뺀 판정만 다시 돌린다
	FRandomStream TimeRng(RandomSeed + 1);
	const int32 TimeCount = 20000;
	double Start = FPlatformTime::Seconds();
	for (int32 I = 0; I < TimeCount; ++I)
	{
		const FVector A = Floors[TimeRng.RandHelper(Floors.Num())] + FVector(0, 0, EyeHeight);
		const FVector B = A + FRotator(TimeRng.FRandRange(-15.0, 15.0), TimeRng.FRandRange(-180.0, 180.0), 0.0).Vector() * RayLength;
		FHitResult Hit;
		Ray(A, B, Hit);
	}
	OutRaySeconds = (FPlatformTime::Seconds() - Start) / TimeCount;

	Start = FPlatformTime::Seconds();
	for (int32 I = 0; I < TimeCount; ++I)
	{
		const FVector A = Floors[TimeRng.RandHelper(Floors.Num())] + FVector(0, 0, HalfHeight + 2.0);
		const FVector B = A + FRotator(0.0, TimeRng.FRandRange(-180.0, 180.0), 0.0).Vector() * TimeRng.FRandRange(50.0, 1500.0);
		FHitResult Hit;
		World->SweepSingleByChannel(Hit, A, B, FQuat::Identity, SweepChannel, SweepShape, SweepParams, SweepResponse);
	}
	OutSweepSeconds = (FPlatformTime::Seconds() - Start) / TimeCount;

	UE_LOG(LogMOBTrace, Log, TEXT("MOB_TRACE_DONE file=%s rays=%d sweeps=%d rayUs=%.2f sweepUs=%.2f"), *OutPath, RayCount, SweepCount, OutRaySeconds * 1e6, OutSweepSeconds * 1e6);
}
