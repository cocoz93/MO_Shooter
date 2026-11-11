#include "MOBDediBaselineSubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "MOBAnimCompareSubsystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBDediBaselineSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBDedi, Log, All);

namespace
{
	constexpr float CLIENT_MAX_FPS = 60.f;		// 화면 쪽은 60프레임으로 잰다(파일럿 영상 비교와 같은 값). Lyra 설정이 t.MaxFPS 를 덮어써 첫 판이 128프레임으로 돌았으므로 매 프레임 건다
	constexpr int32 FLUSH_EVERY_FRAMES = 30;	// 서버는 경로가 끝나면 바로 프로세스를 끝낸다. 마지막 저장을 못 해도 잃는 양이 0.5초 안이 되는 간격
}

bool UMOBDediBaselineSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBDediBaseline="), T);
}

void UMOBDediBaselineSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBDediBaseline="), Tag);
	PhysWeak = LoadObject<UPhysicalMaterial>(nullptr, TEXT("/Game/Characters/Heroes/PhysMat_Player_WeakSpot.PhysMat_Player_WeakSpot"));
	BodyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Quinn_Invis.SKM_Quinn_Invis"));
	RifleLayer = LoadClass<UAnimInstance>(nullptr, TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/ABP_RifleAnimLayers_Feminine.ABP_RifleAnimLayers_Feminine_C"));
	UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI assets body=%s layer=%s"), *GetNameSafe(BodyMesh), *GetNameSafe(RifleLayer.Get()));
}

void UMOBDediBaselineSubsystem::Deinitialize()
{
	const bool bClient = GetWorld() && GetWorld()->GetNetMode() == NM_Client;
	if (Writer)
	{
		Writer->Close();
		delete Writer;
		Writer = nullptr;
		UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI done tag=%s frames=%d boneRows=%d"), *Tag, Frame, BoneRows);
	}
	if (bClient && bHadFoe) FPlatformMisc::RequestExit(false, TEXT("MOBDediBaseline"));	// 서버가 끝나 끊겼다
	Super::Deinitialize();
}

TStatId UMOBDediBaselineSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBDediBaselineSubsystem, STATGROUP_Tickables);
}

ACharacter* UMOBDediBaselineSubsystem::FindClientFoe() const
{
	for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
	{
		if (It->GetLocalRole() == ROLE_SimulatedProxy && It->GetMesh() && It->GetMesh()->GetPhysicsAsset()) return *It;
	}
	return nullptr;
}

// 양쪽 몸을 같게 맞춘다 — 후보 1의 상대 몸이 전원 Quinn 이라(D33) Quinn 으로 잰다. 매 프레임 확인한다. Lyra 가 무기를 바꿀 때 층을 다시 잇기 때문이다
void UMOBDediBaselineSubsystem::ForceSameBody(ACharacter* Target)
{
	USkeletalMeshComponent* Mesh = Target->GetMesh();
	if (!Mesh || !BodyMesh || !RifleLayer) return;
	if (Mesh->GetSkeletalMeshAsset() != BodyMesh) Mesh->SetSkeletalMesh(BodyMesh, true);
	if (!Mesh->GetLinkedAnimLayerInstanceByClass(RifleLayer)) Mesh->LinkAnimClassLayers(RifleLayer);
}

void UMOBDediBaselineSubsystem::OpenWriter(const TCHAR* Side)
{
	if (Writer) return;
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("dedi_%s_%s.csv"), *Tag, Side);
	Writer = IFileManager::Get().CreateFileWriter(*Path);
	UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI open side=%s ok=%d path=%s"), Side, Writer ? 1 : 0, *Path);
}

void UMOBDediBaselineSubsystem::LogMeshIfChanged(const ACharacter* Target, const TCHAR* Side)
{
	const USkeletalMeshComponent* Mesh = Target->GetMesh();
	FString Anims;
	const_cast<USkeletalMeshComponent*>(Mesh)->ForEachAnimInstance([&Anims](UAnimInstance* Inst) { Anims += GetNameSafe(Inst ? Inst->GetClass() : nullptr) + TEXT(" "); });
	const FString Now = FString::Printf(TEXT("%s/%s anims=[%s]"), *GetNameSafe(Mesh->GetSkeletalMeshAsset()), *GetNameSafe(Mesh->GetPhysicsAsset()), *Anims);
	if (Now == LastMesh) return;
	LastMesh = Now;
	UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI body side=%s frame=%d mesh=%s"), Side, Frame, *Now);
}

// 받거나 보낸 총량 — 판단에 쓰지 않고 참고로 한 줄 남긴다(플랜 0단계 트래픽)
void UMOBDediBaselineSubsystem::LogTraffic(const TCHAR* When) const
{
	const UNetDriver* Driver = GetWorld()->GetNetDriver();
	const UNetConnection* Conn = Driver ? (Driver->ServerConnection ? Driver->ServerConnection.Get() : (Driver->ClientConnections.Num() > 0 ? Driver->ClientConnections[0].Get() : nullptr)) : nullptr;
	if (!Conn) return;
	const int32 Bytes = Driver->ServerConnection ? Conn->InTotalBytes : Conn->OutTotalBytes;
	const double Seconds = GetWorld()->GetRealTimeSeconds() - TrafficStartSeconds;
	UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI traffic when=%s bytes=%d seconds=%.1f bytesPerSecond=%.0f avgLagMs=%.1f"), When, Bytes - TrafficStartBytes, Seconds, Seconds > 0.0 ? (Bytes - TrafficStartBytes) / Seconds : 0.0, Conn->AvgLag * 1000.f);
}

void UMOBDediBaselineSubsystem::WriteFrame(ACharacter* Target, int32 Pass, const TCHAR* Segment, float SegmentTime)
{
	USkeletalMeshComponent* Mesh = Target->GetMesh();
	const UPhysicsAsset* PA = Mesh ? Mesh->GetPhysicsAsset() : nullptr;
	if (!Writer || !PA) return;

	const UWorld* World = GetWorld();
	const AGameStateBase* GS = World->GetGameState();
	const double Time = World->GetNetMode() == NM_Client ? (GS ? GS->GetServerWorldTimeSeconds() : -1.0) : World->GetTimeSeconds();

	// 화면에 그려진 몸의 자리와 방향. 메시의 기본 치우침을 빼면 서버에서는 캡슐 자리와 같다
	const FTransform MeshW = Mesh->GetComponentTransform();
	const float BodyYaw = FRotator::NormalizeAxis(MeshW.Rotator().Yaw - Target->GetBaseRotationOffsetRotator().Yaw);
	const FQuat BodyQ = FRotator(0.f, BodyYaw, 0.f).Quaternion();
	const FQuat BodyInv = BodyQ.Inverse();
	const FVector Loc = MeshW.GetLocation() - BodyQ.RotateVector(Target->GetBaseTranslationOffset());
	const FVector V = Target->GetVelocity();

	Buffer += FString::Printf(TEXT("FRAME,%d,%.5f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%d,%s,%.4f\n"), Frame, Time, Loc.X, Loc.Y, Loc.Z, BodyYaw, V.X, V.Y, V.Z, Pass, Segment, SegmentTime);

	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B) continue;
		const int32 Bone = Mesh->GetBoneIndex(B->BoneName);
		if (Bone == INDEX_NONE) continue;
		FTransform BoneW = Mesh->GetBoneTransform(Bone);
		BoneW.SetScale3D(FVector::OneVector);	// 크기는 물리 몸체처럼 뺀다(WriteShapes 와 같음)
		const int32 Weak = B->GetPhysMaterial() == PhysWeak ? 1 : 0;
		const auto Emit = [&](const TCHAR* Kind, const FTransform& ElemLocal, double A, double C, double D)
		{
			const FTransform W = ElemLocal * BoneW;
			const FVector P = BodyInv.RotateVector(W.GetLocation() - Loc);
			const FVector X = BodyInv.RotateVector(W.GetUnitAxis(EAxis::X));
			const FVector Y = BodyInv.RotateVector(W.GetUnitAxis(EAxis::Y));
			const FVector Z = BodyInv.RotateVector(W.GetUnitAxis(EAxis::Z));
			Buffer += FString::Printf(TEXT("POSEB,0,%d,%s,%s,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f\n"),
				Frame, *B->BoneName.ToString(), Kind, Weak, P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, A, C, D);
			++BoneRows;
		};
		for (const FKSphereElem& E : B->AggGeom.SphereElems) Emit(TEXT("sphere"), FTransform(E.Center), E.Radius, 0, 0);
		for (const FKSphylElem& E : B->AggGeom.SphylElems) Emit(TEXT("sphyl"), FTransform(E.Rotation, E.Center), E.Radius, E.Length, 0);
		for (const FKBoxElem& E : B->AggGeom.BoxElems) Emit(TEXT("box"), FTransform(E.Rotation, E.Center), E.X, E.Y, E.Z);
	}

	const FTCHARToUTF8 Bytes(*Buffer);
	Writer->Serialize(const_cast<ANSICHAR*>(Bytes.Get()), Bytes.Length());
	Buffer.Reset();
	if (++Frame % FLUSH_EVERY_FRAMES == 0) Writer->Flush();
}

void UMOBDediBaselineSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	const ENetMode Mode = World->GetNetMode();
	if (Mode != NM_DedicatedServer && Mode != NM_Client) return;
	const bool bServer = Mode == NM_DedicatedServer;
	const TCHAR* const Side = bServer ? TEXT("server") : TEXT("client");

	int32 Pass = -1;
	const TCHAR* Segment = TEXT("-");
	float SegmentTime = 0.f;
	ACharacter* Target = nullptr;
	if (bServer)
	{
		UMOBAnimCompareSubsystem* Compare = World->GetSubsystem<UMOBAnimCompareSubsystem>();
		Target = Compare ? Compare->GetFoe() : nullptr;
		if (!Target) return;
		// 경로를 돌기 전 대기 구간부터 뼈 갱신을 켜 둔다 — 애니가 자리 잡은 뒤의 자세를 잰다
		if (Target->GetMesh()) Target->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		ForceSameBody(Target);
		if (!Compare->GetRunPosition(Pass, Segment, SegmentTime)) return;
	}
	else
	{
		Target = Foe.Get();
		if (!Target) Target = FindClientFoe();
		if (!Target) return;
		// 화면 밖에 있어도 뼈를 갱신한다. 실제로 쏠 때는 화면 안이므로 같은 경로다
		Target->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		ForceSameBody(Target);
		if (GEngine && GEngine->GetMaxFPS() != CLIENT_MAX_FPS) GEngine->SetMaxFPS(CLIENT_MAX_FPS);
	}

	if (!bHadFoe)
	{
		bHadFoe = true;
		OpenWriter(Side);
		const UNetDriver* Driver = World->GetNetDriver();
		const UNetConnection* Conn = Driver ? (Driver->ServerConnection ? Driver->ServerConnection.Get() : (Driver->ClientConnections.Num() > 0 ? Driver->ClientConnections[0].Get() : nullptr)) : nullptr;
		TrafficStartBytes = Conn ? (Driver->ServerConnection ? Conn->InTotalBytes : Conn->OutTotalBytes) : 0;
		TrafficStartSeconds = World->GetRealTimeSeconds();
		UE_LOG(LogMOBDedi, Log, TEXT("MOB_DEDI start side=%s foe=%s role=%d"), Side, *GetNameSafe(Target), (int32)Target->GetLocalRole());
	}
	Foe = Target;
	LogMeshIfChanged(Target, Side);
	WriteFrame(Target, Pass, Segment, SegmentTime);
	if (Frame % 600 == 0) LogTraffic(TEXT("run"));
}
