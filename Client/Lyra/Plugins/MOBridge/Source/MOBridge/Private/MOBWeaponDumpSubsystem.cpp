#include "MOBWeaponDumpSubsystem.h"

#include "Animation/AnimMontage.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Physics/LyraCollisionChannels.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBWeaponDumpSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBWeaponDump, Log, All);

namespace
{
	constexpr double WAIT_PAWNS_SECONDS = 15.0;

	// 값을 뽑을 에셋 — 클래스는 기본 객체(CDO)를 적는다
	const TCHAR* const ASSETS[] = {
		TEXT("/ShooterCore/Weapons/B_WeaponInstance_Base.B_WeaponInstance_Base_C"),
		TEXT("/ShooterCore/Weapons/Pistol/B_WeaponInstance_Pistol.B_WeaponInstance_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/B_WeaponInstance_Rifle.B_WeaponInstance_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/B_WeaponInstance_Shotgun.B_WeaponInstance_Shotgun_C"),
		TEXT("/Game/Weapons/GA_Weapon_Fire.GA_Weapon_Fire_C"),
		TEXT("/ShooterCore/Weapons/Pistol/GA_Weapon_Fire_Pistol.GA_Weapon_Fire_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Fire_Rifle_Auto.GA_Weapon_Fire_Rifle_Auto_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Fire_Shotgun.GA_Weapon_Fire_Shotgun_C"),
		TEXT("/Game/Weapons/GA_Weapon_ReloadMagazine.GA_Weapon_ReloadMagazine_C"),
		TEXT("/Game/Weapons/GA_Weapon_AutoReload.GA_Weapon_AutoReload_C"),
		TEXT("/Game/Weapons/Pistol/GA_Weapon_Reload_Pistol.GA_Weapon_Reload_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Reload_Rifle.GA_Weapon_Reload_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Reload_Shotgun.GA_Weapon_Reload_Shotgun_C"),
		TEXT("/Game/Weapons/Pistol/GE_Damage_Pistol.GE_Damage_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GE_Damage_RifleAuto.GE_Damage_RifleAuto_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GE_Damage_Shotgun.GE_Damage_Shotgun_C"),
		TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle.ID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/ID_Shotgun.ID_Shotgun_C"),
		TEXT("/ShooterCore/Weapons/Pistol/WID_Pistol.WID_Pistol"),
		TEXT("/ShooterCore/Weapons/Rifle/WID_Rifle.WID_Rifle"),
		TEXT("/ShooterCore/Weapons/Shotgun/WID_Shotgun.WID_Shotgun"),
		TEXT("/ShooterCore/Weapons/Pistol/AbilitySet_ShooterPistol.AbilitySet_ShooterPistol"),
		TEXT("/ShooterCore/Weapons/Rifle/AbilitySet_ShooterRifle.AbilitySet_ShooterRifle"),
		TEXT("/ShooterCore/Weapons/Shotgun/AbilitySet_ShooterShotgun.AbilitySet_ShooterShotgun"),
		TEXT("/Game/Characters/Heroes/PhysMat_Player.PhysMat_Player"),
		TEXT("/Game/Characters/Heroes/PhysMat_Player_WeakSpot.PhysMat_Player_WeakSpot"),
		// 조각 2-b — 경기 규칙
		TEXT("/ShooterCore/Elimination/B_TeamDeathMatchScoring.B_TeamDeathMatchScoring_C"),
		TEXT("/ShooterCore/Game/B_ShooterGameScoring_Base.B_ShooterGameScoring_Base_C"),
		TEXT("/ShooterCore/Game/Respawn/GA_AutoRespawn.GA_AutoRespawn_C"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_Warmup.Phase_Warmup_C"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_Playing.Phase_Playing_C"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame_C"),
		TEXT("/ShooterCore/Game/B_TeamSetup_TwoTeams.B_TeamSetup_TwoTeams_C"),
		TEXT("/ShooterCore/Game/Respawn/GE_SpawnIn.GE_SpawnIn_C"),
		TEXT("/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect_C"),
		TEXT("/ShooterCore/Experiences/Phases/GE_PregameLobby.GE_PregameLobby_C"),
		TEXT("/ShooterCore/Experiences/Phases/GE_DamageImmunity_FromGameMode.GE_DamageImmunity_FromGameMode_C"),
		TEXT("/Game/GameplayEffects/GE_BlockAbilityInput.GE_BlockAbilityInput_C"),
		TEXT("/Game/Characters/Heroes/Abilities/GA_Hero_Death.GA_Hero_Death_C"),
	};

	// 흐름을 눈으로 읽을 그래프 — 연사 간격처럼 변수가 아니라 노드 상수에 박힌 값을 찾는다
	const TCHAR* const GRAPHS[] = {
		TEXT("/Game/Weapons/GA_Weapon_Fire.GA_Weapon_Fire"),
		TEXT("/ShooterCore/Weapons/Pistol/GA_Weapon_Fire_Pistol.GA_Weapon_Fire_Pistol"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Fire_Rifle_Auto.GA_Weapon_Fire_Rifle_Auto"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Fire_Shotgun.GA_Weapon_Fire_Shotgun"),
		TEXT("/Game/Weapons/GA_Weapon_ReloadMagazine.GA_Weapon_ReloadMagazine"),
		TEXT("/Game/Weapons/GA_Weapon_AutoReload.GA_Weapon_AutoReload"),
		TEXT("/Game/Weapons/Pistol/GA_Weapon_Reload_Pistol.GA_Weapon_Reload_Pistol"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Reload_Rifle.GA_Weapon_Reload_Rifle"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Reload_Shotgun.GA_Weapon_Reload_Shotgun"),
		// 조각 2-b — 경기 규칙
		TEXT("/ShooterCore/Elimination/B_TeamDeathMatchScoring.B_TeamDeathMatchScoring"),
		TEXT("/ShooterCore/Game/B_ShooterGameScoring_Base.B_ShooterGameScoring_Base"),
		TEXT("/ShooterCore/Game/Respawn/GA_AutoRespawn.GA_AutoRespawn"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_Warmup.Phase_Warmup"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_Playing.Phase_Playing"),
		TEXT("/ShooterCore/Experiences/Phases/Phase_PostGame.Phase_PostGame"),
		TEXT("/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect"),
		TEXT("/Game/Characters/Heroes/Abilities/GA_Hero_Death.GA_Hero_Death"),
	};

	UClass* NativeBase(UClass* C)
	{
		while (C && !C->HasAnyClassFlags(CLASS_Native)) C = C->GetSuperClass();
		return C;
	}
}

bool UMOBWeaponDumpSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBWeaponDump="), T);
}

void UMOBWeaponDumpSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBWeaponDump="), Tag);
}

TStatId UMOBWeaponDumpSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBWeaponDumpSubsystem, STATGROUP_Tickables);
}

void UMOBWeaponDumpSubsystem::DumpObject(UObject* O, const FString& Indent, TSet<UObject*>& Seen)
{
	if (!O || Seen.Contains(O)) return;
	Seen.Add(O);

	UClass* Cls = O->GetClass();
	UObject* NativeDefaults = NativeBase(Cls) ? NativeBase(Cls)->GetDefaultObject() : nullptr;

	for (TFieldIterator<FProperty> It(Cls); It; ++It)
	{
		FProperty* P = *It;
		if (P->GetOwnerUObject() == UObject::StaticClass()) continue;
		if (P->HasAnyPropertyFlags(CPF_Transient) && P->GetFName() != NAME_None && !P->IsA<FFloatProperty>()) continue;

		// 네이티브 기본값과 같은 것은 건너뛴다. 블루프린트 변수는 네이티브에 없으니 항상 적는다
		const bool bInNative = NativeDefaults && Cls->IsChildOf(P->GetOwnerClass()) && NativeDefaults->GetClass()->IsChildOf(P->GetOwnerClass());
		if (bInNative && P->Identical_InContainer(O, NativeDefaults)) continue;

		FString V;
		P->ExportText_InContainer(0, V, O, O, O, PPF_None);
		if (V.Len() > 6000) V = V.Left(6000) + TEXT(" ...(잘림)");
		Out += FString::Printf(TEXT("%s%s (%s) = %s\n"), *Indent, *P->GetName(), *P->GetCPPType(), *V);

		// 안에 품은 객체(능력 비용·조각·효과 구성 요소)는 따로 펼친다
		if (const FObjectProperty* OP = CastField<FObjectProperty>(P))
		{
			if (P->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance | CPF_ContainsInstancedReference))
			{
				if (UObject* Sub = OP->GetObjectPropertyValue_InContainer(O))
				{
					Out += FString::Printf(TEXT("%s  [%s] %s\n"), *Indent, *Sub->GetClass()->GetName(), *Sub->GetName());
					DumpObject(Sub, Indent + TEXT("    "), Seen);
				}
			}
		}
		else if (const FArrayProperty* AP = CastField<FArrayProperty>(P))
		{
			if (const FObjectProperty* Inner = CastField<FObjectProperty>(AP->Inner))
			{
				FScriptArrayHelper_InContainer H(AP, O);
				for (int32 i = 0; i < H.Num(); ++i)
				{
					UObject* Sub = Inner->GetObjectPropertyValue(H.GetRawPtr(i));
					if (Sub && Sub->GetOuter() != GetTransientPackage() && (Inner->HasAnyPropertyFlags(CPF_InstancedReference) || AP->HasAnyPropertyFlags(CPF_ContainsInstancedReference)))
					{
						Out += FString::Printf(TEXT("%s  [%d] [%s] %s\n"), *Indent, i, *Sub->GetClass()->GetName(), *Sub->GetName());
						DumpObject(Sub, Indent + TEXT("    "), Seen);
					}
				}
			}
		}
	}
}

void UMOBWeaponDumpSubsystem::DumpBlueprintGraphs(const FString& Path, bool bDeep)
{
	Out += FString::Printf(TEXT("\n=== GRAPH %s\n"), *Path);
#if WITH_EDITORONLY_DATA
	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Path);
	if (!BP)
	{
		Out += TEXT("  (블루프린트를 못 읽음)\n");
		UE_LOG(LogMOBWeaponDump, Warning, TEXT("MOB_WDUMP graph_missing %s"), *Path);
		return;
	}

	TArray<UEdGraph*> Graphs;
	Graphs.Append(BP->UbergraphPages);
	Graphs.Append(BP->FunctionGraphs);
	Graphs.Append(BP->MacroGraphs);
	// bDeep 이면 노드 안 그래프(상태 기계 · 상태 · 전이 · 층)를 뒤에 붙여 차례로 걷는다 — 그래서 첨자로 돈다
	for (int32 GI = 0; GI < Graphs.Num(); ++GI)
	{
		UEdGraph* G = Graphs[GI];
		if (!G) continue;
		Out += FString::Printf(TEXT("--- %s (노드 %d)\n"), *G->GetName(), G->Nodes.Num());
		for (UEdGraphNode* N : G->Nodes)
		{
			if (!N || N->GetClass()->GetName() == TEXT("EdGraphNode_Comment")) continue;
			Out += FString::Printf(TEXT("  [%s] %s\n"), *N->GetClass()->GetName(), *N->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" / ")));
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (!Pin || Pin->bHidden) continue;
				FString Line = FString::Printf(TEXT("    %s %s"), Pin->Direction == EGPD_Input ? TEXT("<") : TEXT(">"), *Pin->PinName.ToString());
				if (!Pin->DefaultValue.IsEmpty()) Line += FString::Printf(TEXT(" = %s"), *Pin->DefaultValue);
				if (Pin->DefaultObject) Line += FString::Printf(TEXT(" = obj %s"), *Pin->DefaultObject->GetName());
				for (UEdGraphPin* L : Pin->LinkedTo)
				{
					if (L && L->GetOwningNode()) Line += FString::Printf(TEXT(" -> %s.%s"), *L->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\n"), TEXT(" / ")), *L->PinName.ToString());
				}
				const bool bUseful = !Pin->DefaultValue.IsEmpty() || Pin->DefaultObject || Pin->LinkedTo.Num() > 0;
				if (bUseful) Out += Line + TEXT("\n");
			}
#if WITH_EDITOR
			if (bDeep)
			{
				// 애님 노드는 알파 · 입력 연결 · 속성 묶음이 핀이 아니라 노드 속성에 있다
				if (N->GetClass()->GetName().StartsWith(TEXT("AnimGraphNode_")))
				{
					TSet<UObject*> Seen;
					DumpObject(N, TEXT("      "), Seen);
				}
				for (UEdGraph* Sub : N->GetSubGraphs())
				{
					if (Sub && !Graphs.Contains(Sub)) Graphs.Add(Sub);
				}
			}
#endif
		}
	}
#else
	Out += TEXT("  (편집기 데이터 없음)\n");
#endif
}

void UMOBWeaponDumpSubsystem::DumpAssets()
{
	int32 Loaded = 0, Missing = 0;
	for (const TCHAR* Path : ASSETS)
	{
		Out += FString::Printf(TEXT("\n=== ASSET %s\n"), Path);
		UObject* Obj = LoadObject<UObject>(nullptr, Path);
		if (!Obj)
		{
			Out += TEXT("  (못 읽음)\n");
			UE_LOG(LogMOBWeaponDump, Warning, TEXT("MOB_WDUMP asset_missing %s"), Path);
			++Missing;
			continue;
		}
		++Loaded;
		UClass* AsClass = Cast<UClass>(Obj);
		UObject* Target = AsClass ? AsClass->GetDefaultObject() : Obj;
		Out += FString::Printf(TEXT("  class=%s super=%s\n"), *Target->GetClass()->GetName(), Target->GetClass()->GetSuperClass() ? *Target->GetClass()->GetSuperClass()->GetName() : TEXT("-"));
		TSet<UObject*> Seen;
		DumpObject(Target, TEXT("  "), Seen);
	}
	for (const TCHAR* Path : GRAPHS) DumpBlueprintGraphs(Path);
	// L-2-a — 명령줄로 받은 그래프(쌍반점으로 여럿)는 깊게. 옛 GRAPHS 출력은 그대로
	FString Extra;
	if (FParse::Value(FCommandLine::Get(), TEXT("MOBWDGraph="), Extra))
	{
		TArray<FString> Paths;
		Extra.ParseIntoArray(Paths, TEXT(";"));
		for (const FString& P : Paths) DumpBlueprintGraphs(P, true);
	}
	UE_LOG(LogMOBWeaponDump, Log, TEXT("MOB_WDUMP assets loaded=%d missing=%d"), Loaded, Missing);
}

void UMOBWeaponDumpSubsystem::DumpPawnBodies(APawn* Pawn, const TCHAR* Label)
{
	if (!Pawn) return;
	const FVector PawnLoc = Pawn->GetActorLocation();
	Out += FString::Printf(TEXT("\n=== PAWN %s %s class=%s loc=(%.2f %.2f %.2f) yaw=%.2f\n"), Label, *Pawn->GetName(), *Pawn->GetClass()->GetName(), PawnLoc.X, PawnLoc.Y, PawnLoc.Z, Pawn->GetActorRotation().Yaw);

	TArray<AActor*> Actors;
	Actors.Add(Pawn);
	Pawn->GetAttachedActors(Actors, /*bResetArray=*/false, /*bRecursivelyIncludeAttachedActors=*/true);

	int32 BodyCount = 0;
	for (AActor* A : Actors)
	{
		TArray<UPrimitiveComponent*> Prims;
		A->GetComponents<UPrimitiveComponent>(Prims);
		Out += FString::Printf(TEXT("ACTOR %s class=%s prims=%d\n"), *A->GetName(), *A->GetClass()->GetName(), Prims.Num());
		for (UPrimitiveComponent* C : Prims)
		{
			const ECollisionResponse WeaponResp = C->GetCollisionResponseToChannel(Lyra_TraceChannel_Weapon);
			Out += FString::Printf(TEXT(" COMP %s class=%s profile=%s enabled=%d weaponResp=%d complexAsSimple? \n"), *C->GetName(), *C->GetClass()->GetName(), *C->GetCollisionProfileName().ToString(), (int32)C->GetCollisionEnabled(), (int32)WeaponResp);

			USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(C);
			if (!Sk) continue;
			UPhysicsAsset* PA = Sk->GetPhysicsAsset();
			Out += FString::Printf(TEXT("  mesh=%s physicsAsset=%s bodies=%d\n"), *GetNameSafe(Sk->GetSkeletalMeshAsset()), *GetPathNameSafe(PA), PA ? PA->SkeletalBodySetups.Num() : 0);
			if (!PA) continue;
			for (USkeletalBodySetup* B : PA->SkeletalBodySetups)
			{
				if (!B) continue;
				const int32 BoneIndex = Sk->GetBoneIndex(B->BoneName);
				const FTransform BoneW = BoneIndex != INDEX_NONE ? Sk->GetBoneTransform(BoneIndex) : FTransform::Identity;
				const FVector BoneRel = BoneW.GetLocation() - PawnLoc;
				const FRotator BoneRot = BoneW.Rotator();
				const FBodyInstance* Inst = Sk->GetBodyInstance(B->BoneName);
				const UPhysicalMaterial* PM = B->GetPhysMaterial();
				const bool bQuery = Inst ? (Inst->GetCollisionEnabled() == ECollisionEnabled::QueryOnly || Inst->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics) : false;
				const int32 Resp = Inst ? (int32)Inst->GetResponseToChannel(Lyra_TraceChannel_Weapon) : -1;
				Out += FString::Printf(TEXT("  BODY bone=%s collisionResp=%d bodyQuery=%d bodyWeaponResp=%d physMat=%s boneRelPawn=(%.2f %.2f %.2f) boneRot=(%.2f %.2f %.2f)\n"), *B->BoneName.ToString(), (int32)B->DefaultInstance.GetCollisionEnabled(), bQuery ? 1 : 0, Resp, *GetNameSafe(PM), BoneRel.X, BoneRel.Y, BoneRel.Z, BoneRot.Pitch, BoneRot.Yaw, BoneRot.Roll);
				for (const FKSphereElem& E : B->AggGeom.SphereElems)
				{
					Out += FString::Printf(TEXT("    SPHERE c=(%.3f %.3f %.3f) r=%.3f\n"), E.Center.X, E.Center.Y, E.Center.Z, E.Radius);
				}
				for (const FKSphylElem& E : B->AggGeom.SphylElems)
				{
					Out += FString::Printf(TEXT("    SPHYL c=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f) r=%.3f len=%.3f\n"), E.Center.X, E.Center.Y, E.Center.Z, E.Rotation.Pitch, E.Rotation.Yaw, E.Rotation.Roll, E.Radius, E.Length);
				}
				for (const FKBoxElem& E : B->AggGeom.BoxElems)
				{
					Out += FString::Printf(TEXT("    BOX c=(%.3f %.3f %.3f) rot=(%.3f %.3f %.3f) ext=(%.3f %.3f %.3f)\n"), E.Center.X, E.Center.Y, E.Center.Z, E.Rotation.Pitch, E.Rotation.Yaw, E.Rotation.Roll, E.X, E.Y, E.Z);
				}
				if (B->AggGeom.ConvexElems.Num() > 0) Out += FString::Printf(TEXT("    CONVEX count=%d\n"), B->AggGeom.ConvexElems.Num());
				++BodyCount;
			}
		}
	}
	UE_LOG(LogMOBWeaponDump, Log, TEXT("MOB_WDUMP pawn=%s actors=%d bodies=%d"), Label, Actors.Num(), BodyCount);
}

// 조각 2-b — 시작 자리와 실행 중인 경기 규칙 컴포넌트(에셋 기본값을 경험이 덮을 수 있어 실행값을 본다)
void UMOBWeaponDumpSubsystem::DumpMatch()
{
	UWorld* World = GetWorld();
	int32 Starts = 0;
	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		const FVector L = It->GetActorLocation();
		const FRotator R = It->GetActorRotation();
		const UCapsuleComponent* Capsule = It->GetCapsuleComponent();
		Out += FString::Printf(TEXT("MOB_PLAYERSTART %s %s loc=%.3f,%.3f,%.3f yaw=%.3f tag=%s halfHeight=%.3f radius=%.3f\n"),
			*It->GetClass()->GetName(), *It->GetName(), L.X, L.Y, L.Z, R.Yaw, *It->PlayerStartTag.ToString(), Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.f, Capsule ? Capsule->GetScaledCapsuleRadius() : 0.f);
		++Starts;
	}
	Out += FString::Printf(TEXT("MOB_PLAYERSTARTS count=%d\n"), Starts);
	// 부활 무적 길이 = 몽타주 재생 길이(GA_SpawnEffect 그래프)
	if (UClass* SpawnEffect = LoadObject<UClass>(nullptr, TEXT("/ShooterCore/Game/Respawn/GA_SpawnEffect.GA_SpawnEffect_C")))
	{
		UObject* Defaults = SpawnEffect->GetDefaultObject();
		for (TFieldIterator<FObjectProperty> It(SpawnEffect); It; ++It)
		{
			if (UAnimMontage* Montage = Cast<UAnimMontage>(It->GetObjectPropertyValue_InContainer(Defaults)))
			{
				Out += FString::Printf(TEXT("MOB_SPAWNMONTAGE prop=%s name=%s playLength=%.6f rate=%.6f\n"), *It->GetName(), *Montage->GetName(), Montage->GetPlayLength(), Montage->RateScale);
			}
		}
	}
	if (const AWorldSettings* Settings = World->GetWorldSettings())
	{
		Out += FString::Printf(TEXT("MOB_WORLD killZ=%.3f boundsChecks=%d\n"), Settings->KillZ, Settings->bEnableWorldBoundsChecks ? 1 : 0);
	}

	if (AGameStateBase* GS = World->GetGameState())
	{
		Out += FString::Printf(TEXT("MOB_GAMESTATE class=%s\n"), *GS->GetClass()->GetName());
		TArray<UActorComponent*> Components;
		GS->GetComponents(Components);
		for (UActorComponent* C : Components)
		{
			Out += FString::Printf(TEXT("MOB_GSCOMP %s\n"), *C->GetClass()->GetName());
			if (C->GetClass()->GetName().Contains(TEXT("Scoring")) || C->GetClass()->GetName().Contains(TEXT("Team")) || C->GetClass()->GetName().Contains(TEXT("Spawning")))
			{
				TSet<UObject*> Seen;
				DumpObject(C, TEXT("  "), Seen);
			}
		}
	}
}

// 점프대 j-1 — 점프대마다 청사진 변수(UpOverride · 대상 · 배율 같은 발사 값)와 칸 모양 · 월드 변환을 적는다. 실행 중 값이라 짓기 그래프가 채운 값까지 보인다.
// 줄 머리 = MOB_LAUNCHER(배우) · MOB_LAUNCHER_VAR(청사진 변수) · MOB_LAUNCHER_COMP(장면 컴포넌트) — make_launchers.py 가 읽는다
// 순간이동 t-1(teleport-plan.md) — 같은 몫을 B_Teleport_C 자식에 머리 MOB_TELEPORT* 로. 순간이동만 MOB_TELEPORT_REF(배우를 가리키는 변수 → 그 배우 월드 변환)를 더 적는다 — 나갈 자리가 다른 배우일 수 있어서.
// 배우 하나를 가리키는 변수만 잡힌다 — L_Expanse 의 TeleportingTarget 은 배열이라 REF 0 줄이고, 짝은 VAR 값 줄의 경로로 읽는다(make_teleports.py).
// MOB_TELEPORT_COMPX = 컴포넌트 월드 자리 · 세 축 · 상자 반크기를 소수 아홉 · 열두 자리로, MOB_TELEPORT_PAWN = 내 몸 메시가 캡슐 중심에서 얼마 아래인지
void UMOBWeaponDumpSubsystem::DumpLaunchers()
{
	int32 Count = 0, Vars = 0, Shapes = 0;
	DumpDevices(TEXT("B_Launcher_C"), TEXT("MOB_LAUNCHER"), false, Count, Vars, Shapes);
	UE_LOG(LogMOBWeaponDump, Log, TEXT("MOB_WDUMP launchers=%d vars=%d overlapShapes=%d"), Count, Vars, Shapes);
}

void UMOBWeaponDumpSubsystem::DumpTeleports()
{
	int32 Count = 0, Vars = 0, Shapes = 0;
	const int32 Refs = DumpDevices(TEXT("B_Teleport_C"), TEXT("MOB_TELEPORT"), true, Count, Vars, Shapes);
	// 「들어온 자리 그대로」(KeepReleativeEntryUponExit) 는 몸 메시의 월드 자리로 셈한다(그래프 GetOffsetLocation ← Get Mesh) — 메시가 캡슐 중심에서 얼마 아래인지 적는다
	const APlayerController* PC = GetWorld()->GetFirstPlayerController();
	const ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (Ch && Ch->GetMesh() && Ch->GetCapsuleComponent())
	{
		const FVector Rel = Ch->GetMesh()->GetRelativeLocation(), Base = Ch->GetBaseTranslationOffset();
		Out += FString::Printf(TEXT("MOB_TELEPORT_PAWN name=%s meshRel=%.6f,%.6f,%.6f baseOffset=%.6f,%.6f,%.6f capsuleR=%.6f capsuleHH=%.6f meshMinusRoot=%.6f\n"), *Ch->GetName(), Rel.X, Rel.Y, Rel.Z, Base.X, Base.Y, Base.Z,
			Ch->GetCapsuleComponent()->GetScaledCapsuleRadius(), Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Ch->GetMesh()->GetComponentLocation().Z - Ch->GetActorLocation().Z);
	}
	UE_LOG(LogMOBWeaponDump, Log, TEXT("MOB_WDUMP teleports=%d vars=%d overlapShapes=%d refs=%d pawn=%d"), Count, Vars, Shapes, Refs, Ch ? 1 : 0);
}

int32 UMOBWeaponDumpSubsystem::DumpDevices(const TCHAR* ClassName, const TCHAR* Prefix, bool bRefs, int32& Count, int32& Vars, int32& Shapes)
{
	const FName DeviceName(ClassName);
	int32 Refs = 0;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* A = *It;
		bool bDevice = false;
		for (const UClass* C = A->GetClass(); C && !bDevice; C = C->GetSuperClass()) bDevice = C->GetFName() == DeviceName;
		if (!bDevice) continue;
		++Count;
		const FTransform T = A->GetActorTransform();
		const FRotator R = T.Rotator();
		Out += FString::Printf(TEXT("%s name=%s class=%s loc=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f scale=%.4f,%.4f,%.4f\n"), Prefix, *A->GetName(), *A->GetClass()->GetName(),
			T.GetLocation().X, T.GetLocation().Y, T.GetLocation().Z, R.Pitch, R.Yaw, R.Roll, T.GetScale3D().X, T.GetScale3D().Y, T.GetScale3D().Z);
		// 청사진이 더한 변수만 — 네이티브 배우 속성은 장치 동작과 무관하고 양이 많다
		for (TFieldIterator<FProperty> PIt(A->GetClass()); PIt; ++PIt)
		{
			const FProperty* P = *PIt;
			const UClass* Owner = P->GetOwnerClass();
			if (!Owner || Owner->HasAnyClassFlags(CLASS_Native)) continue;
			const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(P);
			if (ObjProp && ObjProp->PropertyClass->IsChildOf(UActorComponent::StaticClass())) continue;	// 컴포넌트는 아래 줄에서
			FString V;
			P->ExportText_InContainer(0, V, A, A, A, PPF_None);
			Out += FString::Printf(TEXT("%s_VAR name=%s owner=%s var=%s type=%s value=%s\n"), Prefix, *A->GetName(), *Owner->GetName(), *P->GetName(), *P->GetCPPType(), *V.Replace(TEXT("\n"), TEXT(" ")));
			++Vars;
			if (!bRefs || !ObjProp) continue;
			const AActor* Ref = Cast<AActor>(ObjProp->GetObjectPropertyValue_InContainer(A));
			if (!Ref) continue;
			const FTransform RT = Ref->GetActorTransform();
			const FRotator RR = RT.Rotator();
			Out += FString::Printf(TEXT("%s_REF name=%s var=%s target=%s class=%s loc=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f scale=%.4f,%.4f,%.4f\n"), Prefix, *A->GetName(), *P->GetName(), *Ref->GetName(), *Ref->GetClass()->GetName(),
				RT.GetLocation().X, RT.GetLocation().Y, RT.GetLocation().Z, RR.Pitch, RR.Yaw, RR.Roll, RT.GetScale3D().X, RT.GetScale3D().Y, RT.GetScale3D().Z);
			++Refs;
		}
		TInlineComponentArray<USceneComponent*> Parts(A);
		for (USceneComponent* S : Parts)
		{
			const FTransform W = S->GetComponentTransform();
			const FRotator WR = W.Rotator();
			FString Shape = TEXT("-");
			if (const USphereComponent* Sp = Cast<USphereComponent>(S)) Shape = FString::Printf(TEXT("sphere r=%.4f scaledR=%.4f"), Sp->GetUnscaledSphereRadius(), Sp->GetScaledSphereRadius());
			else if (const UBoxComponent* Bx = Cast<UBoxComponent>(S)) Shape = FString::Printf(TEXT("box ext=%.4f,%.4f,%.4f scaledExt=%.4f,%.4f,%.4f"), Bx->GetUnscaledBoxExtent().X, Bx->GetUnscaledBoxExtent().Y, Bx->GetUnscaledBoxExtent().Z,
				Bx->GetScaledBoxExtent().X, Bx->GetScaledBoxExtent().Y, Bx->GetScaledBoxExtent().Z);
			else if (const UCapsuleComponent* Cp = Cast<UCapsuleComponent>(S)) Shape = FString::Printf(TEXT("capsule r=%.4f hh=%.4f"), Cp->GetScaledCapsuleRadius(), Cp->GetScaledCapsuleHalfHeight());
			const UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(S);
			if (Prim && Prim->GetGenerateOverlapEvents()) ++Shapes;
			Out += FString::Printf(TEXT("%s_COMP name=%s comp=%s class=%s parent=%s overlap=%d profile=%s collision=%d loc=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f scale=%.4f,%.4f,%.4f shape=%s\n"),
				Prefix, *A->GetName(), *S->GetName(), *S->GetClass()->GetName(), S->GetAttachParent() ? *S->GetAttachParent()->GetName() : TEXT("-"),
				Prim && Prim->GetGenerateOverlapEvents() ? 1 : 0, Prim ? *Prim->GetCollisionProfileName().ToString() : TEXT("-"), Prim ? (int32)Prim->GetCollisionEnabled() : -1,
				W.GetLocation().X, W.GetLocation().Y, W.GetLocation().Z, WR.Pitch, WR.Yaw, WR.Roll, W.GetScale3D().X, W.GetScale3D().Y, W.GetScale3D().Z, *Shape);
			if (!bRefs) continue;
			// 순간이동만 — 위 줄은 소수 넷째 자리라 나가는 방향(Exit Direction 앞방향 × 발사 배율)이 0.002cm/s 쯤 틀어진다. 셈에 쓰는 값은 이 줄에서 읽는다
			const FVector X = W.GetUnitAxis(EAxis::X), Y = W.GetUnitAxis(EAxis::Y), Z = W.GetUnitAxis(EAxis::Z);
			const UBoxComponent* Box = Cast<UBoxComponent>(S);
			const FVector Ext = Box ? Box->GetScaledBoxExtent() : FVector::ZeroVector;
			Out += FString::Printf(TEXT("%s_COMPX name=%s comp=%s loc=%.9f,%.9f,%.9f x=%.12f,%.12f,%.12f y=%.12f,%.12f,%.12f z=%.12f,%.12f,%.12f boxExt=%.9f,%.9f,%.9f\n"),
				Prefix, *A->GetName(), *S->GetName(), W.GetLocation().X, W.GetLocation().Y, W.GetLocation().Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, Ext.X, Ext.Y, Ext.Z);
		}
	}
	return Refs;
}

void UMOBWeaponDumpSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay()) return;
	if (StartAt < 0) StartAt = World->GetRealTimeSeconds();
	const double Elapsed = World->GetRealTimeSeconds() - StartAt;

	// 게임 기능(ShooterCore)이 켜져야 에셋 경로가 살아난다
	if (!bAssetsDone && Elapsed > 5.0)
	{
		bAssetsDone = true;
		DumpAssets();
	}
	if (Elapsed < WAIT_PAWNS_SECONDS) return;

	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Mine = PC ? PC->GetPawn() : nullptr;
	APawn* Other = nullptr;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		if (*It != Mine && !Other) Other = *It;
	}
	DumpPawnBodies(Mine, TEXT("mine"));
	DumpPawnBodies(Other, TEXT("other"));
	DumpMatch();
	if (FParse::Param(FCommandLine::Get(), TEXT("MOBWDLaunchers"))) DumpLaunchers();
	if (FParse::Param(FCommandLine::Get(), TEXT("MOBWDTeleports"))) DumpTeleports();
	Finish();
}

void UMOBWeaponDumpSubsystem::Finish()
{
	bDone = true;
	const FString File = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("weapons_%s.txt"), *Tag);
	FFileHelper::SaveStringToFile(Out, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBWeaponDump, Log, TEXT("MOB_WDUMP done chars=%d"), Out.Len());
	FPlatformMisc::RequestExit(false);
}
