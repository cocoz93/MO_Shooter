#include "MOBNavDumpSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Math/RandomStream.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "WorldPartition/WorldPartition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBNavDumpSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBNav, Log, All);

namespace
{
	// 길찾기 자료를 언리얼이 아직 만드는 중이면 이만큼까지 기다린다
	constexpr double MAX_BUILD_WAIT_SEC = 240.0;
	// 위아래로 겹쳤다고 보는 높이 차 — 사람 키(캡슐 180cm)보다 커야 서로 다른 층이다
	constexpr double STACKED_MIN_DZ = 250.0;
	constexpr double STACKED_CELL = 300.0;
	// 언리얼 탐색 노드 번호가 16비트라 이 안에서 가장 크게 잡는다. 기본 2048이면 먼 길이 「길 없음」으로 나온다
	constexpr uint32 PATH_SEARCH_NODES = 65000;

	struct FNavNode
	{
		NavNodeRef Ref = 0;
		uint32 Kind = 0;
		uint32 Area = 0;
		FVector Center = FVector::ZeroVector;
		TArray<FVector> Verts;
		TArray<uint32> Neighbors;
	};

	void AppendU32(TArray<uint8>& Out, uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(V)); }
	void AppendF(TArray<uint8>& Out, float V) { Out.Append(reinterpret_cast<const uint8*>(&V), sizeof(V)); }
	void AppendVec(TArray<uint8>& Out, const FVector& V)
	{
		AppendF(Out, (float)V.X); AppendF(Out, (float)V.Y); AppendF(Out, (float)V.Z);
	}
}

bool UMOBNavDumpSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !(World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE)) return false;
	FString Dummy;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBDumpNav="), Dummy);
}

void UMOBNavDumpSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBDumpNav="), OutPath);
	FParse::Value(FCommandLine::Get(), TEXT("MOBDumpDelay="), DelaySec);
	FParse::Value(FCommandLine::Get(), TEXT("MOBNavPairs="), PairCount);
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_INIT world=%s out=%s delay=%.1f pairs=%d"), *GetNameSafe(GetWorld()), *OutPath, DelaySec, PairCount);
}

TStatId UMOBNavDumpSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBNavDumpSubsystem, STATGROUP_Tickables);
}

void UMOBNavDumpSubsystem::Tick(float DeltaTime)
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
			UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_PAWN_READY pawn=%s at=%s"), *GetNameSafe(PC->GetPawn()), *PC->GetPawn()->GetActorLocation().ToString());
		}
		return;
	}
	const double Elapsed = FPlatformTime::Seconds() - PawnReadyAt;
	if (Elapsed < DelaySec) return;

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	if (NavSys && NavSys->IsNavigationBuildInProgress() && Elapsed < MAX_BUILD_WAIT_SEC) return;

	bDone = true;
	DumpAndQuit();
}

void UMOBNavDumpSubsystem::DumpAndQuit()
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	const APlayerController* PC = World->GetFirstPlayerController();

	// 「자료가 없다」와 「못 읽었다」를 가르려고, 찾기 전에 있는 것부터 센다
	int32 Actors = 0, NavDatas = 0, RecastActors = 0, ChunkActors = 0;
	ARecastNavMesh* Nav = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Actors;
		const FString ClassName = It->GetClass()->GetName();
		if (ClassName.Contains(TEXT("NavigationDataChunk"))) ++ChunkActors;
		if (Cast<ANavigationData>(*It)) ++NavDatas;
		if (ARecastNavMesh* Candidate = Cast<ARecastNavMesh>(*It))
		{
			++RecastActors;
			UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_DATA name=%s tiles=%d radius=%.1f height=%.1f maxSlope=%.1f generation=%d"),
				*Candidate->GetName(), Candidate->GetNavMeshTilesCount(), Candidate->AgentRadius, Candidate->AgentHeight, Candidate->AgentMaxSlope, (int32)Candidate->GetRuntimeGenerationMode());
			if (!Nav) Nav = Candidate;
		}
	}
	ARecastNavMesh* DefaultNav = NavSys ? Cast<ARecastNavMesh>(NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate)) : nullptr;
	if (DefaultNav) Nav = DefaultNav;

	const UWorldPartition* WP = World->GetWorldPartition();
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_WORLD partitioned=%d streaming=%d levels=%d actors=%d navSystem=%d navData=%d recast=%d chunks=%d defaultNav=%s building=%d"),
		WP ? 1 : 0, (WP && WP->IsStreamingEnabled()) ? 1 : 0, World->GetStreamingLevels().Num(), Actors, NavSys ? 1 : 0, NavDatas, RecastActors, ChunkActors,
		*GetNameSafe(DefaultNav), (NavSys && NavSys->IsNavigationBuildInProgress()) ? 1 : 0);

	if (!Nav || !NavSys)
	{
		UE_LOG(LogMOBNav, Warning, TEXT("MOB_NAV_NONE 길찾기 자료가 세계에 없다"));
		FPlatformMisc::RequestExit(false);
		return;
	}

	TArray<FNavNode> Nodes;
	TMap<NavNodeRef, uint32> IndexOf;
	const int32 TileSlots = Nav->GetNavMeshTilesCount();
	int32 ActiveTiles = 0;
	TArray<FNavTileRef> TileRefs;
	Nav->GetAllNavMeshTiles(TileRefs);

	// 내 순회와 무관한 언리얼의 답 — 내 캐릭터 발밑이 바닥 위인지 묻는다
	FNavLocation Projected;
	const bool bPawnOnNav = PC && PC->GetPawn() && NavSys->ProjectPointToNavigation(PC->GetPawn()->GetActorLocation(), Projected, FVector(200, 200, 300), Nav);
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_ENGINE valid=%d numActiveTiles=%d tileRefs=%d pawnOnNav=%d navBounds=%s"),
		Nav->HasValidNavmesh() ? 1 : 0, Nav->GetNumActiveTiles(), TileRefs.Num(), bPawnOnNav ? 1 : 0, *Nav->GetNavMeshBounds().ToString());

	for (const FNavTileRef& Tile : TileRefs)
	{
		TArray<FNavPoly> Polys;
		if (!Nav->GetPolysInTile(Tile, Polys)) continue;
		++ActiveTiles;
		for (const FNavPoly& Poly : Polys)
		{
			IndexOf.Add(Poly.Ref, (uint32)Nodes.Num());
			FNavNode& Node = Nodes.AddDefaulted_GetRef();
			Node.Ref = Poly.Ref;
			Node.Center = Poly.Center;
		}
	}
	const int32 GroundCount = Nodes.Num();

	// 연결을 따라가다 바닥 목록에 없는 번호가 나오면 바닥 사이 연결선이다. 그 연결선도 노드로 넣어 양끝을 잇는다
	int32 Links = 0, NullLinks = 0, VertFail = 0;
	for (int32 I = 0; I < Nodes.Num(); ++I)
	{
		const NavNodeRef Ref = Nodes[I].Ref;
		TArray<FVector> Verts;
		if (!Nav->GetPolyVerts(Ref, Verts)) ++VertFail;
		const uint32 Area = Nav->GetPolyAreaID(Ref);

		TArray<NavNodeRef> Neighbors;
		Nav->GetPolyNeighbors(Ref, Neighbors);
		TArray<uint32> NeighborIndices;
		for (const NavNodeRef Neighbor : Neighbors)
		{
			if (Neighbor == 0) { ++NullLinks; continue; }
			uint32* Found = IndexOf.Find(Neighbor);
			uint32 Index = Found ? *Found : (uint32)Nodes.Num();
			if (!Found)
			{
				IndexOf.Add(Neighbor, Index);
				FNavNode& Link = Nodes.AddDefaulted_GetRef();
				Link.Ref = Neighbor;
				Link.Kind = 1;
				Nav->GetPolyCenter(Neighbor, Link.Center);
			}
			NeighborIndices.Add(Index);
			++Links;
		}
		// 위에서 Nodes 가 늘면 참조가 무효가 되므로 다 모은 뒤에 넣는다
		Nodes[I].Verts = MoveTemp(Verts);
		Nodes[I].Area = Area;
		Nodes[I].Neighbors = MoveTemp(NeighborIndices);
	}

	FBox Bounds(ForceInit);
	TMap<uint32, int32> ByArea;
	for (int32 I = 0; I < GroundCount; ++I)
	{
		for (const FVector& V : Nodes[I].Verts) Bounds += V;
		ByArea.FindOrAdd(Nodes[I].Area)++;
	}
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_COUNT tileSlots=%d activeTiles=%d ground=%d offMesh=%d links=%d nullLinks=%d vertFail=%d"),
		TileSlots, ActiveTiles, GroundCount, Nodes.Num() - GroundCount, Links, NullLinks, VertFail);
	for (const auto& Pair : ByArea) UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_AREA area=%u count=%d"), Pair.Key, Pair.Value);
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_BOUNDS min=%s max=%s"), *Bounds.Min.ToString(), *Bounds.Max.ToString());

	// 뽑은 바닥이 실제 충돌 바닥 위에 있는가 — 다각형 가운데에서 아래로 쏴 거리를 나눠 센다
	int32 FloorBins[4] = {}, FloorMiss = 0;
	FString FloorCsv = TEXT("node,distance\n");
	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(MOBNavFloor), false);
	if (PC && PC->GetPawn()) TraceParams.AddIgnoredActor(PC->GetPawn());
	for (int32 I = 0; I < GroundCount; ++I)
	{
		const FVector C = Nodes[I].Center;
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, C + FVector(0, 0, 60), C - FVector(0, 0, 120), ECC_Pawn, TraceParams))
		{
			++FloorMiss;
			FloorCsv += FString::Printf(TEXT("%d,-1\n"), I);
			continue;
		}
		const double Distance = FMath::Abs(Hit.ImpactPoint.Z - C.Z);
		++FloorBins[Distance <= 5 ? 0 : Distance <= 15 ? 1 : Distance <= 35 ? 2 : 3];
		FloorCsv += FString::Printf(TEXT("%d,%.1f\n"), I, Distance);
	}
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_FLOOR within5=%d within15=%d within35=%d farther=%d miss=%d"), FloorBins[0], FloorBins[1], FloorBins[2], FloorBins[3], FloorMiss);

	// 언리얼의 경로 탐색에 쌍을 물어 둔다 — 뽑은 연결이 언리얼의 답과 같은지 파일만으로 대조하려는 것
	struct FPairAsk { int32 A, B; const TCHAR* Kind; };
	TArray<FPairAsk> Asks;
	FRandomStream Rng(20261111);
	if (GroundCount >= 2)
	{
		for (int32 I = 0; I < PairCount; ++I)
		{
			Asks.Add({ Rng.RandRange(0, GroundCount - 1), Rng.RandRange(0, GroundCount - 1), TEXT("random") });
		}

		TMap<FIntPoint, TArray<int32>> Cells;
		for (int32 I = 0; I < GroundCount; ++I)
		{
			Cells.FindOrAdd(FIntPoint(FMath::FloorToInt(Nodes[I].Center.X / STACKED_CELL), FMath::FloorToInt(Nodes[I].Center.Y / STACKED_CELL))).Add(I);
		}
		TArray<TPair<int32, int32>> Stacked;
		for (const auto& Cell : Cells)
		{
			for (int32 X = 0; X < Cell.Value.Num(); ++X)
			{
				for (int32 Y = X + 1; Y < Cell.Value.Num(); ++Y)
				{
					if (FMath::Abs(Nodes[Cell.Value[X]].Center.Z - Nodes[Cell.Value[Y]].Center.Z) >= STACKED_MIN_DZ) Stacked.Add({ Cell.Value[X], Cell.Value[Y] });
				}
			}
		}
		UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_STACKED candidates=%d"), Stacked.Num());
		for (int32 I = Stacked.Num() - 1; I > 0; --I) Stacked.Swap(I, Rng.RandRange(0, I));
		for (int32 I = 0; I < FMath::Min(PairCount, Stacked.Num()); ++I) Asks.Add({ Stacked[I].Key, Stacked[I].Value, TEXT("stacked") });
	}

	FSharedNavQueryFilter Filter = Nav->GetDefaultQueryFilter()->GetCopy();
	Filter->SetMaxSearchNodes(PATH_SEARCH_NODES);
	FString PairsCsv = TEXT("kind,a,b,result,points,length\n");
	int32 Found = 0, NotFound = 0;
	for (const FPairAsk& Ask : Asks)
	{
		FPathFindingQuery Query(this, *Nav, Nodes[Ask.A].Center, Nodes[Ask.B].Center, Filter);
		Query.SetAllowPartialPaths(false);
		const FPathFindingResult Result = NavSys->FindPathSync(Query, EPathFindingMode::Regular);
		const bool bOk = Result.IsSuccessful() && Result.Path.IsValid();
		bOk ? ++Found : ++NotFound;
		PairsCsv += FString::Printf(TEXT("%s,%d,%d,%d,%d,%.1f\n"), Ask.Kind, Ask.A, Ask.B, (int32)Result.Result,
			bOk ? Result.Path->GetPathPoints().Num() : 0, bOk ? Result.Path->GetLength() : 0.0);
	}
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_PAIRS asked=%d found=%d notFound=%d"), Asks.Num(), Found, NotFound);

	TArray<uint8> File;
	File.Append(reinterpret_cast<const uint8*>("MONV"), 4);
	AppendU32(File, 1);
	AppendU32(File, (uint32)Nodes.Num());
	AppendU32(File, (uint32)Links);
	AppendVec(File, Bounds.IsValid ? Bounds.Min : FVector::ZeroVector);
	AppendVec(File, Bounds.IsValid ? Bounds.Max : FVector::ZeroVector);
	AppendF(File, Nav->AgentRadius);
	AppendF(File, Nav->AgentHeight);
	for (const FNavNode& Node : Nodes)
	{
		AppendU32(File, Node.Kind);
		AppendU32(File, Node.Area);
		AppendVec(File, Node.Center);
		AppendU32(File, (uint32)Node.Verts.Num());
		for (const FVector& V : Node.Verts) AppendVec(File, V);
		AppendU32(File, (uint32)Node.Neighbors.Num());
		for (const uint32 Neighbor : Node.Neighbors) AppendU32(File, Neighbor);
	}

	const bool bSaved = FFileHelper::SaveArrayToFile(File, *OutPath);
	const bool bPairsSaved = FFileHelper::SaveStringToFile(PairsCsv, *(OutPath + TEXT(".pairs.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const bool bFloorSaved = FFileHelper::SaveStringToFile(FloorCsv, *(OutPath + TEXT(".floor.csv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBNav, Log, TEXT("MOB_NAV_DONE file=%s bytes=%d saved=%d pairsSaved=%d floorSaved=%d"), *OutPath, File.Num(), bSaved ? 1 : 0, bPairsSaved ? 1 : 0, bFloorSaved ? 1 : 0);

	FPlatformMisc::RequestExit(false);
}
