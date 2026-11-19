#include "MOBMapDumpSubsystem.h"

#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Chaos/CollisionFilterData.h"
#include "Chaos/Convex.h"
#include "Chaos/HeightField.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/ShapeInstance.h"
#include "Chaos/Sphere.h"
#include "Chaos/TriangleMeshImplicitObject.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "WorldPartition/WorldPartition.h"

#include "Physics/LyraCollisionChannels.h"

#include "MOBTraceRecorder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBMapDumpSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBMap, Log, All);

namespace
{
	constexpr uint32 FLAG_BLOCK_MOVE = 1;
	constexpr uint32 FLAG_BLOCK_WEAPON = 2;
	constexpr uint32 FLAG_MOVABLE = 4;
	constexpr uint32 FLAG_CULL_BACKFACE = 8;
	constexpr uint32 FLAG_SOLID = 16;
	constexpr uint32 FLAG_BLOCK_GRENADE = 32;	// 수류탄 몸 — 단순 충돌(G-0-d 셈: 사람만 막는 벽 · 수류탄만 막는 물체가 있다)
	constexpr uint32 FLAG_BLOCK_BLAST = 64;		// 폭발 선(Weapon_Capsule 채널) — 복잡 충돌

	// 세는 자리를 한곳에 모은다 — 물체 수만 보면 「못 읽었다」와 「없었다」를 못 가른다
	struct FDumpStats
	{
		int32 Actors = 0, Components = 0, Bodies = 0, Shapes = 0, Leaves = 0;
		int32 Objects = 0, Triangles = 0, MoveObjects = 0, WeaponObjects = 0, MovableObjects = 0, GrenadeObjects = 0, BlastObjects = 0;
		int32 SkipPawn = 0, SkipNoQuery = 0, SkipNoBody = 0, SkipNotBlocking = 0;
		TMap<FName, int32> LeafByType, SkippedLeafByType;
		TMap<FName, int32> TrianglesByClass;
	};

	void AppendU32(TArray<uint8>& Out, uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(V)); }
	void AppendF(TArray<uint8>& Out, float V) { Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(V)); }
	void AppendVec(TArray<uint8>& Out, const FVector& V)
	{
		AppendF(Out, (float)V.X); AppendF(Out, (float)V.Y); AppendF(Out, (float)V.Z);
	}

	struct FDumpWriter
	{
		TArray<uint8> Body;
		FBox Bounds = FBox(ForceInit);
		FDumpStats Stats;

		void BeginTriangles(uint32 Flags, TArray<FVector>& Tris)
		{
			const uint32 TriCount = (uint32)(Tris.Num() / 3);
			AppendU32(Body, Flags);
			AppendU32(Body, 0);
			AppendU32(Body, TriCount);
			for (const FVector& V : Tris)
			{
				AppendVec(Body, V);
				Bounds += V;
			}
			Stats.Triangles += (int32)TriCount;
		}
	};

	void AddTri(TArray<FVector>& Out, const FVector& A, const FVector& B, const FVector& C, bool bFlip)
	{
		Out.Add(A);
		Out.Add(bFlip ? C : B);
		Out.Add(bFlip ? B : C);
	}

	// 잎 물체 하나 → 월드 좌표 삼각형. 구·캡슐은 삼각형으로 바꾸지 않고 그대로 쓴다
	//   돌려준 값 false = 모르는 모양(세어서 보고한다)
	// 볼록 물체의 삼각형이 모두 바깥을 보게 한다 — 서버가 앞뒤로 「안에서 쐈나」를 가린다
	void OrientOutward(TArray<FVector>& Tris)
	{
		if (Tris.Num() < 3) return;
		FVector Center = FVector::ZeroVector;
		for (const FVector& V : Tris) Center += V;
		Center /= (double)Tris.Num();
		for (int32 I = 0; I + 2 < Tris.Num(); I += 3)
		{
			const FVector Normal = FVector::CrossProduct(Tris[I + 1] - Tris[I], Tris[I + 2] - Tris[I]);
			if (FVector::DotProduct(Normal, Tris[I] - Center) < 0) Swap(Tris[I + 1], Tris[I + 2]);
		}
	}

	bool EmitLeaf(const Chaos::FImplicitObject& Leaf, const FTransform& ToWorld, uint32 Flags, FDumpWriter& W, TArray<FVector>& OutTris, int32& OutPrims, uint32& OutExtraFlags)
	{
		using namespace Chaos;
		const EImplicitObjectType Inner = GetInnerType(Leaf.GetType());
		FVec3 Scale(1);
		FReal Margin = 0;
		const auto ToW = [&](const FVector& Local) { return ToWorld.TransformPosition(Local * FVector(Scale)); };

		if (Inner == ImplicitObjectType::TriangleMesh)
		{
			const FTriangleMeshImplicitObject* Mesh = UnwrapImplicit<FTriangleMeshImplicitObject>(Leaf, Scale, Margin);
			if (!Mesh) return false;
			const bool bMirror = (Scale.X * Scale.Y * Scale.Z) < 0;
			if (Mesh->GetCullsBackFaceRaycast()) OutExtraFlags |= FLAG_CULL_BACKFACE;
			const auto& P = Mesh->Particles();
			const FTrimeshIndexBuffer& E = Mesh->Elements();
			const auto Emit = [&](const auto& Buf)
			{
				for (const auto& T : Buf)
				{
					AddTri(OutTris, ToW(FVector(P.GetX(T[0]))), ToW(FVector(P.GetX(T[1]))), ToW(FVector(P.GetX(T[2]))), bMirror);
				}
			};
			if (E.RequiresLargeIndices()) Emit(E.GetLargeIndexBuffer());
			else Emit(E.GetSmallIndexBuffer());
			return true;
		}
		if (Inner == ImplicitObjectType::Convex)
		{
			const FConvex* Cv = UnwrapImplicit<FConvex>(Leaf, Scale, Margin);
			if (!Cv) return false;
			const bool bMirror = (Scale.X * Scale.Y * Scale.Z) < 0;
			for (int32 Plane = 0; Plane < Cv->NumPlanes(); ++Plane)
			{
				const int32 N = Cv->NumPlaneVertices(Plane);
				if (N < 3) continue;
				const FVector A = ToW(FVector(Cv->GetVertex(Cv->GetPlaneVertex(Plane, 0))));
				for (int32 I = 1; I + 1 < N; ++I)
				{
					AddTri(OutTris, A, ToW(FVector(Cv->GetVertex(Cv->GetPlaneVertex(Plane, I)))), ToW(FVector(Cv->GetVertex(Cv->GetPlaneVertex(Plane, I + 1)))), bMirror);
				}
			}
			OrientOutward(OutTris);
			OutExtraFlags |= FLAG_SOLID;
			return true;
		}
		if (Inner == ImplicitObjectType::Box)
		{
			const TBox<FReal, 3>* Bx = UnwrapImplicit<TBox<FReal, 3>>(Leaf, Scale, Margin);
			if (!Bx) return false;
			const FVector Mn(Bx->Min()), Mx(Bx->Max());
			const FVector C[8] = {
				ToW(FVector(Mn.X, Mn.Y, Mn.Z)), ToW(FVector(Mx.X, Mn.Y, Mn.Z)), ToW(FVector(Mx.X, Mx.Y, Mn.Z)), ToW(FVector(Mn.X, Mx.Y, Mn.Z)),
				ToW(FVector(Mn.X, Mn.Y, Mx.Z)), ToW(FVector(Mx.X, Mn.Y, Mx.Z)), ToW(FVector(Mx.X, Mx.Y, Mx.Z)), ToW(FVector(Mn.X, Mx.Y, Mx.Z)) };
			static const int32 F[6][4] = { {0,3,2,1}, {4,5,6,7}, {0,1,5,4}, {1,2,6,5}, {2,3,7,6}, {3,0,4,7} };
			for (const auto& Q : F)
			{
				AddTri(OutTris, C[Q[0]], C[Q[1]], C[Q[2]], false);
				AddTri(OutTris, C[Q[0]], C[Q[2]], C[Q[3]], false);
			}
			OrientOutward(OutTris);
			OutExtraFlags |= FLAG_SOLID;
			return true;
		}
		if (Inner == ImplicitObjectType::HeightField)
		{
			const FHeightField* Hf = Leaf.template GetObject<FHeightField>();
			if (!Hf) return false;
			const int32 Rows = Hf->GetNumRows(), Cols = Hf->GetNumCols();
			const auto PointAt = [&](int32 Index) { return ToWorld.TransformPosition(FVector(Hf->GeomData.GetPointScaled(Index))); };
			for (int32 Y = 0; Y + 1 < Rows; ++Y)
			{
				for (int32 X = 0; X + 1 < Cols; ++X)
				{
					const int32 I0 = Y * Cols + X, I1 = I0 + 1, I2 = I0 + Cols, I3 = I2 + 1;
					AddTri(OutTris, PointAt(I0), PointAt(I3), PointAt(I1), false);
					AddTri(OutTris, PointAt(I0), PointAt(I2), PointAt(I3), false);
				}
			}
			return true;
		}
		if (Inner == ImplicitObjectType::Sphere)
		{
			const TSphere<FReal, 3>* Sp = UnwrapImplicit<TSphere<FReal, 3>>(Leaf, Scale, Margin);
			if (!Sp) return false;
			AppendU32(W.Body, Flags);
			AppendU32(W.Body, 1);
			const FVector Center = ToWorld.TransformPosition(FVector(Sp->GetCenterf()) * FVector(Scale));
			AppendVec(W.Body, Center);
			const float Radius = (float)(Sp->GetRadiusf() * Scale.GetAbsMax());
			AppendF(W.Body, Radius);
			W.Bounds += FBox::BuildAABB(Center, FVector(Radius));
			++OutPrims;
			return true;
		}
		if (Inner == ImplicitObjectType::Capsule)
		{
			const FCapsule* Cp = UnwrapImplicit<FCapsule>(Leaf, Scale, Margin);
			if (!Cp) return false;
			AppendU32(W.Body, Flags);
			AppendU32(W.Body, 2);
			const FVector A = ToWorld.TransformPosition(FVector(Cp->GetX1f()) * FVector(Scale));
			const FVector B = ToWorld.TransformPosition(FVector(Cp->GetX2f()) * FVector(Scale));
			AppendVec(W.Body, A);
			AppendVec(W.Body, B);
			const float Radius = (float)(Cp->GetRadiusf() * Scale.GetAbsMax());
			AppendF(W.Body, Radius);
			W.Bounds += FBox::BuildAABB(A, FVector(Radius));
			W.Bounds += FBox::BuildAABB(B, FVector(Radius));
			++OutPrims;
			return true;
		}
		return false;
	}

	// 수류탄 몸 틀 — B_Grenade 생성 스크립트의 첫 루트 성분(G-0-c2 실측 루트 = Mesh). 부모 블루프린트까지 올라가며 찾는다
	const UPrimitiveComponent* FindGrenadeBodyTemplate(FString& OutWhere)
	{
		UClass* Cls = LoadClass<AActor>(nullptr, TEXT("/ShooterCore/Weapon/Grenade/B_Grenade.B_Grenade_C"));
		if (!Cls) { OutWhere = TEXT("noClass"); return nullptr; }
		for (UBlueprintGeneratedClass* Bp = Cast<UBlueprintGeneratedClass>(Cls); Bp; Bp = Cast<UBlueprintGeneratedClass>(Bp->GetSuperClass()))
		{
			if (!Bp->SimpleConstructionScript) continue;
			for (USCS_Node* Root : Bp->SimpleConstructionScript->GetRootNodes())
			{
				if (const UPrimitiveComponent* P = Root ? Cast<UPrimitiveComponent>(Root->ComponentTemplate) : nullptr)
				{
					OutWhere = FString::Printf(TEXT("%s.%s"), *Bp->GetName(), *Root->GetVariableName().ToString());
					return P;
				}
			}
		}
		OutWhere = TEXT("noRoot");
		return nullptr;
	}

	// G-0-d(grenade-plan.md 154 · 61줄) — 지금 깃발 둘이 수류탄에도 같은 물체 집합인가. 파일 · 기존 줄은 안 건드리고 따로 한 번 돈다.
	// 이동 = 본 판(DumpAndQuit) bBlocksMove 와 같은 양쪽 막음(단순) · 수류탄 = 같은 양쪽 막음을 수류탄 몸 틀로(단순 — 날아감은 몸 캡슐 쓸기) ·
	// 무기 = 본 판 bBlocksWeapon(복잡) · 폭발 선 = B_Grenade 「Detonate」 LineTraceSingle(TraceTypeQuery5 · bTraceComplex, b_flow.txt)이 막힘(복잡).
	// 잎마다 넷을 세고 한쪽만인 수를 MOB_MAP_GFLAG 로(이동 · 무기 합은 MOB_MAP_COUNT move · weapon 과 같아야 세는 단위가 같다),
	// 한쪽만인 잎이 있는 성분을 MOB_MAP_GDIFF 로(앞 200개 — expanse 54). 본 판이 건너뛰는 「이동 · 무기 둘 다 안 막음」 성분도 센다
	void LogGrenadeFlags(UWorld* World, const UCapsuleComponent* Capsule)
	{
		FString Where;
		const UPrimitiveComponent* Gren = FindGrenadeBodyTemplate(Where);
		FString GrenResp;
		for (int32 C = 0; C < 32; ++C) GrenResp.AppendInt(Gren ? (int32)Gren->GetCollisionResponseToChannel((ECollisionChannel)C) : 9);
		const ECollisionChannel GrenObj = Gren ? Gren->GetCollisionObjectType() : ECC_WorldDynamic;
		const ECollisionChannel BlastCh = UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery5);
		const ECollisionChannel WeaponCh = UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery4);
		UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_GREN body=%s found=%d enabled=%d objType=%d resp=%s blastCh=%d weaponCh=%d weaponMacro=%d capsuleObj=%d"),
			*Where, Gren ? 1 : 0, Gren ? (int32)Gren->GetCollisionEnabled() : -1, (int32)GrenObj, *GrenResp, (int32)BlastCh, (int32)WeaponCh,
			(int32)Lyra_TraceChannel_Weapon, Capsule ? (int32)Capsule->GetCollisionObjectType() : -1);
		if (!Gren || !Capsule) return;

		int32 Comps = 0, Leaves = 0, Move = 0, GrenN = 0, MoveOnly = 0, GrenOnly = 0, Weapon = 0, Blast = 0, WeaponOnly = 0, BlastOnly = 0;
		int32 CompsMoveDiff = 0, CompsWeaponDiff = 0, Printed = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Cast<APawn>(Actor)) continue;
			TInlineComponentArray<UPrimitiveComponent*> Prims(Actor);
			for (UPrimitiveComponent* Comp : Prims)
			{
				if (!Comp || !Comp->IsRegistered() || !Comp->IsQueryCollisionEnabled()) continue;
				const ECollisionChannel Obj = Comp->GetCollisionObjectType();
				const bool bMove = Comp->GetCollisionResponseToChannel(Capsule->GetCollisionObjectType()) == ECR_Block
					&& Capsule->GetCollisionResponseToChannel(Obj) == ECR_Block;
				const bool bGren = Comp->GetCollisionResponseToChannel(GrenObj) == ECR_Block && Gren->GetCollisionResponseToChannel(Obj) == ECR_Block;
				const bool bWeapon = Comp->GetCollisionResponseToChannel(Lyra_TraceChannel_Weapon) == ECR_Block;
				const bool bBlast = Comp->GetCollisionResponseToChannel(BlastCh) == ECR_Block;
				if (!bMove && !bGren && !bWeapon && !bBlast) continue;
				++Comps;

				TArray<FBodyInstance*, TInlineAllocator<1>> Bodies;
				if (const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Comp))
				{
					for (FBodyInstance* B : Ism->GetInstanceBodies()) if (B) Bodies.Add(B);
				}
				else if (FBodyInstance* B = Comp->GetBodyInstance())
				{
					Bodies.Add(B);
				}

				int32 CMoveOnly = 0, CGrenOnly = 0, CWeaponOnly = 0, CBlastOnly = 0, CLeaves = 0;
				for (FBodyInstance* Body : Bodies)
				{
					const FPhysicsActorHandle Handle = Body->GetPhysicsActor();
					if (!Handle) continue;
					for (const auto& Shape : Handle->GetGameThreadAPI().ShapesArray())
					{
						if (!Shape->GetQueryEnabled()) continue;
						const Chaos::Filter::FShapeFilterData Filter = Shape->GetShapeFilterData();
						const bool bSimple = Filter.HasFlag(Chaos::EFilterFlags::SimpleCollision);
						const bool bComplex = Filter.HasFlag(Chaos::EFilterFlags::ComplexCollision);
						const bool M = bMove && bSimple, G = bGren && bSimple, Wp = bWeapon && bComplex, Bl = bBlast && bComplex;
						if (!M && !G && !Wp && !Bl) continue;
						const Chaos::FImplicitObject* Geom = Shape->GetGeometry();
						if (!Geom) continue;
						Geom->VisitLeafObjects([&](const Chaos::FImplicitObject*, const Chaos::FRigidTransform3&, int32, int32, int32)
						{
							++Leaves; ++CLeaves;
							Move += M; GrenN += G; Weapon += Wp; Blast += Bl;
							CMoveOnly += (M && !G); CGrenOnly += (G && !M); CWeaponOnly += (Wp && !Bl); CBlastOnly += (Bl && !Wp);
						});
					}
				}
				MoveOnly += CMoveOnly; GrenOnly += CGrenOnly; WeaponOnly += CWeaponOnly; BlastOnly += CBlastOnly;
				CompsMoveDiff += (CMoveOnly + CGrenOnly) > 0;
				CompsWeaponDiff += (CWeaponOnly + CBlastOnly) > 0;
				if ((CMoveOnly + CGrenOnly + CWeaponOnly + CBlastOnly) > 0 && Printed < 200)
				{
					++Printed;
					const UStaticMeshComponent* Smc = Cast<UStaticMeshComponent>(Comp);
					UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_GDIFF actor=%s class=%s comp=%s mesh=%s profile=%s objType=%d movable=%d leaves=%d moveOnly=%d grenOnly=%d weaponOnly=%d blastOnly=%d respCapsule=%d respGren=%d grenToIt=%d respWeapon=%d respBlast=%d"),
						*Actor->GetName(), *Actor->GetClass()->GetName(), *Comp->GetName(), Smc ? *GetNameSafe(Smc->GetStaticMesh()) : TEXT("-"),
						*Comp->GetCollisionProfileName().ToString(), (int32)Obj, Comp->Mobility == EComponentMobility::Movable ? 1 : 0, CLeaves,
						CMoveOnly, CGrenOnly, CWeaponOnly, CBlastOnly,
						(int32)Comp->GetCollisionResponseToChannel(Capsule->GetCollisionObjectType()), (int32)Comp->GetCollisionResponseToChannel(GrenObj),
						(int32)Gren->GetCollisionResponseToChannel(Obj), (int32)Comp->GetCollisionResponseToChannel(Lyra_TraceChannel_Weapon), (int32)Comp->GetCollisionResponseToChannel(BlastCh));
				}
			}
		}
		UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_GFLAG comps=%d leaves=%d move=%d gren=%d moveOnly=%d grenOnly=%d weapon=%d blast=%d weaponOnly=%d blastOnly=%d compsMoveDiff=%d compsWeaponDiff=%d printed=%d"),
			Comps, Leaves, Move, GrenN, MoveOnly, GrenOnly, Weapon, Blast, WeaponOnly, BlastOnly, CompsMoveDiff, CompsWeaponDiff, Printed);
	}
}

bool UMOBMapDumpSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	FString Dummy;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBDumpMap="), Dummy);
}

void UMOBMapDumpSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBDumpMap="), OutPath);
	FParse::Value(FCommandLine::Get(), TEXT("MOBDumpDelay="), DelaySec);
	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_INIT world=%s out=%s delay=%.1f"), *GetNameSafe(GetWorld()), *OutPath, DelaySec);
}

TStatId UMOBMapDumpSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBMapDumpSubsystem, STATGROUP_Tickables);
}

void UMOBMapDumpSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	UWorld* World = GetWorld();
	if (!World) return;

	if (PawnReadyAt < 0)
	{
		const APlayerController* PC = World->GetFirstPlayerController();
		if (PC && PC->GetPawn())
		{
			PawnReadyAt = FPlatformTime::Seconds();
			UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_PAWN_READY pawn=%s at=%s"), *GetNameSafe(PC->GetPawn()), *PC->GetPawn()->GetActorLocation().ToString());
		}
		return;
	}
	if (FPlatformTime::Seconds() - PawnReadyAt < DelaySec) return;

	bDone = true;
	DumpAndQuit();
}

void UMOBMapDumpSubsystem::DumpAndQuit()
{
	UWorld* World = GetWorld();
	const APlayerController* PC = World->GetFirstPlayerController();
	const ACharacter* Hero = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	const UCapsuleComponent* Capsule = Hero ? Hero->GetCapsuleComponent() : nullptr;

	const UWorldPartition* WP = World->GetWorldPartition();
	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_WORLD partitioned=%d streaming=%d levels=%d capsule=%s r=%.1f hh=%.1f"),
		WP ? 1 : 0, (WP && WP->IsStreamingEnabled()) ? 1 : 0, World->GetStreamingLevels().Num(),
		Capsule ? *Capsule->GetCollisionProfileName().ToString() : TEXT("없음"),
		Capsule ? Capsule->GetUnscaledCapsuleRadius() : -1.f, Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : -1.f);

	LogGrenadeFlags(World, Capsule);	// 수류탄 깃발 셈(G-0-d) — 진단 줄. 파일 깃발은 아래 본 판이 같은 규칙으로 쓴다(G-1-a)

	// 수류탄 몸 틀이 없으면 수류탄 깃발을 못 정한다 — 깃발 없는 파일은 서버에서 수류탄이 벽을 지나가므로 쓰지 않는다
	FString GrenWhere;
	const UPrimitiveComponent* Gren = FindGrenadeBodyTemplate(GrenWhere);
	if (!Gren)
	{
		UE_LOG(LogMOBMap, Error, TEXT("MOB_MAP_FAIL reason=grenadeBodyMissing where=%s"), *GrenWhere);
		FPlatformMisc::RequestExit(false);
		return;
	}
	const ECollisionChannel GrenObj = Gren->GetCollisionObjectType();
	const ECollisionChannel BlastCh = UEngineTypes::ConvertToCollisionChannel(TraceTypeQuery5);	// B_Grenade Detonate 의 LineTraceSingle 채널

	FDumpWriter W;
	FString Csv = TEXT("actor,class,component,flags,triangles,prims,x,y,z\n");

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		++W.Stats.Actors;

		TInlineComponentArray<UPrimitiveComponent*> Prims(Actor);
		for (UPrimitiveComponent* Comp : Prims)
		{
			if (!Comp || !Comp->IsRegistered()) continue;
			++W.Stats.Components;

			if (Cast<APawn>(Actor)) { ++W.Stats.SkipPawn; continue; }
			if (!Comp->IsQueryCollisionEnabled()) { ++W.Stats.SkipNoQuery; continue; }

			// 이동 판정 = 양쪽이 서로 막아야 함 · 총알 판정 = 물체가 무기 채널을 막아야 함
			// 수류탄 몸도 이동처럼 양쪽 · 폭발 선은 총알처럼 물체 쪽만(G-0-d 셈과 같은 규칙)
			const bool bBlocksMove = Capsule
				&& Comp->GetCollisionResponseToChannel(Capsule->GetCollisionObjectType()) == ECR_Block
				&& Capsule->GetCollisionResponseToChannel(Comp->GetCollisionObjectType()) == ECR_Block;
			const bool bBlocksWeapon = Comp->GetCollisionResponseToChannel(Lyra_TraceChannel_Weapon) == ECR_Block;
			const bool bBlocksGrenade = Comp->GetCollisionResponseToChannel(GrenObj) == ECR_Block
				&& Gren->GetCollisionResponseToChannel(Comp->GetCollisionObjectType()) == ECR_Block;
			const bool bBlocksBlast = Comp->GetCollisionResponseToChannel(BlastCh) == ECR_Block;
			if (!bBlocksMove && !bBlocksWeapon && !bBlocksGrenade && !bBlocksBlast) { ++W.Stats.SkipNotBlocking; continue; }

			TArray<FBodyInstance*, TInlineAllocator<1>> Bodies;
			if (const UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Comp))
			{
				for (FBodyInstance* B : Ism->GetInstanceBodies()) if (B) Bodies.Add(B);
			}
			else if (FBodyInstance* B = Comp->GetBodyInstance())
			{
				Bodies.Add(B);
			}

			int32 CompTris = 0, CompPrims = 0;
			uint32 CompFlags = 0;
			bool bAnyBody = false;
			for (FBodyInstance* Body : Bodies)
			{
				const FPhysicsActorHandle Handle = Body->GetPhysicsActor();
				if (!Handle) continue;
				bAnyBody = true;
				++W.Stats.Bodies;
				const Chaos::FRigidBodyHandle_External& Api = Handle->GetGameThreadAPI();
				const FTransform BodyToWorld(FQuat(Api.GetR()), FVector(Api.GetX()));

				for (const auto& Shape : Api.ShapesArray())
				{
					++W.Stats.Shapes;
					if (!Shape->GetQueryEnabled()) continue;
					const Chaos::Filter::FShapeFilterData Filter = Shape->GetShapeFilterData();
					const bool bSimple = Filter.HasFlag(Chaos::EFilterFlags::SimpleCollision);
					const bool bComplex = Filter.HasFlag(Chaos::EFilterFlags::ComplexCollision);

					uint32 Flags = 0;
					if (bBlocksMove && bSimple) Flags |= FLAG_BLOCK_MOVE;
					if (bBlocksWeapon && bComplex) Flags |= FLAG_BLOCK_WEAPON;
					if (bBlocksGrenade && bSimple) Flags |= FLAG_BLOCK_GRENADE;
					if (bBlocksBlast && bComplex) Flags |= FLAG_BLOCK_BLAST;
					if (Flags == 0) continue;
					if (Comp->Mobility == EComponentMobility::Movable) Flags |= FLAG_MOVABLE;

					const Chaos::FImplicitObject* Geom = Shape->GetGeometry();
					if (!Geom) continue;
					Geom->VisitLeafObjects([&](const Chaos::FImplicitObject* Leaf, const Chaos::FRigidTransform3& Local, int32, int32, int32)
					{
						++W.Stats.Leaves;
						const FName TypeName = Leaf->GetTypeName(Chaos::GetInnerType(Leaf->GetType()));
						W.Stats.LeafByType.FindOrAdd(TypeName)++;

						TArray<FVector> Tris;
						int32 PrimsHere = 0;
						const FTransform ToWorld = FTransform(FQuat(Local.GetRotation()), FVector(Local.GetTranslation())) * BodyToWorld;
						uint32 ExtraFlags = 0;
						if (!EmitLeaf(*Leaf, ToWorld, Flags, W, Tris, PrimsHere, ExtraFlags))
						{
							W.Stats.SkippedLeafByType.FindOrAdd(TypeName)++;
							return;
						}
						if (Tris.Num() > 0)
						{
							W.BeginTriangles(Flags | ExtraFlags, Tris);
							++W.Stats.Objects;
							CompTris += Tris.Num() / 3;
						}
						W.Stats.Objects += PrimsHere;
						CompPrims += PrimsHere;
						CompFlags |= Flags;
						if (Flags & FLAG_BLOCK_MOVE) ++W.Stats.MoveObjects;
						if (Flags & FLAG_BLOCK_WEAPON) ++W.Stats.WeaponObjects;
						if (Flags & FLAG_MOVABLE) ++W.Stats.MovableObjects;
						if (Flags & FLAG_BLOCK_GRENADE) ++W.Stats.GrenadeObjects;
						if (Flags & FLAG_BLOCK_BLAST) ++W.Stats.BlastObjects;
					});
				}
			}

			if (!bAnyBody) { ++W.Stats.SkipNoBody; continue; }
			if (CompFlags != 0)
			{
				const FVector Loc = Comp->GetComponentLocation();
				Csv += FString::Printf(TEXT("%s,%s,%s,%u,%d,%d,%.1f,%.1f,%.1f\n"), *Actor->GetName(), *Actor->GetClass()->GetName(), *Comp->GetName(), CompFlags, CompTris, CompPrims, Loc.X, Loc.Y, Loc.Z);
				W.Stats.TrianglesByClass.FindOrAdd(Actor->GetClass()->GetFName()) += CompTris;
			}
		}
	}

	TArray<uint8> File;
	File.Append(reinterpret_cast<const uint8*>("MOCL"), 4);
	AppendU32(File, 3);	// 판 3 = 수류탄 · 폭발 선 깃발(G-1-a)
	AppendU32(File, (uint32)W.Stats.Objects);
	AppendU32(File, (uint32)W.Stats.Triangles);
	AppendVec(File, W.Bounds.IsValid ? W.Bounds.Min : FVector::ZeroVector);
	AppendVec(File, W.Bounds.IsValid ? W.Bounds.Max : FVector::ZeroVector);
	File.Append(W.Body);

	const bool bSaved = FFileHelper::SaveArrayToFile(File, *OutPath);
	const bool bCsvSaved = FFileHelper::SaveStringToFile(Csv, *(OutPath + TEXT(".csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FDumpStats& S = W.Stats;
	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_COUNT actors=%d components=%d bodies=%d shapes=%d leaves=%d objects=%d triangles=%d move=%d weapon=%d movable=%d grenade=%d blast=%d"),
		S.Actors, S.Components, S.Bodies, S.Shapes, S.Leaves, S.Objects, S.Triangles, S.MoveObjects, S.WeaponObjects, S.MovableObjects, S.GrenadeObjects, S.BlastObjects);
	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_SKIP pawn=%d noQuery=%d notBlocking=%d noBody=%d"), S.SkipPawn, S.SkipNoQuery, S.SkipNotBlocking, S.SkipNoBody);
	for (const auto& Pair : S.LeafByType) UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_LEAF type=%s count=%d"), *Pair.Key.ToString(), Pair.Value);
	for (const auto& Pair : S.SkippedLeafByType) UE_LOG(LogMOBMap, Warning, TEXT("MOB_MAP_LEAF_UNREAD type=%s count=%d"), *Pair.Key.ToString(), Pair.Value);

	TArray<TPair<FName, int32>> ByClass;
	for (const auto& Pair : S.TrianglesByClass) ByClass.Add(TPair<FName, int32>(Pair.Key, Pair.Value));
	ByClass.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B) { return A.Value > B.Value; });
	for (int32 I = 0; I < FMath::Min(ByClass.Num(), 15); ++I) UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_CLASS %s triangles=%d"), *ByClass[I].Key.ToString(), ByClass[I].Value);

	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_BOUNDS min=%s max=%s"), *W.Bounds.Min.ToString(), *W.Bounds.Max.ToString());
	UE_LOG(LogMOBMap, Log, TEXT("MOB_MAP_DONE file=%s bytes=%d saved=%d csvSaved=%d"), *OutPath, File.Num(), bSaved ? 1 : 0, bCsvSaved ? 1 : 0);

	FString TracePath;
	if (FParse::Value(FCommandLine::Get(), TEXT("MOBTraceRecord="), TracePath))
	{
		int32 RayCount = 150000, SweepCount = 45000;
		FParse::Value(FCommandLine::Get(), TEXT("MOBTraceRays="), RayCount);
		FParse::Value(FCommandLine::Get(), TEXT("MOBTraceSweeps="), SweepCount);
		double RaySeconds = 0, SweepSeconds = 0;
		MOBRecordTraces(World, Hero, W.Bounds, TracePath, RayCount, SweepCount, RaySeconds, SweepSeconds);
	}

	FPlatformMisc::RequestExit(false);
}
