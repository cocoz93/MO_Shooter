#include "MOBWeaponRecordSubsystem.h"

#include "AIController.h"
#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "AbilitySystem/LyraAbilitySourceInterface.h"
#include "AbilitySystem/LyraGameplayEffectContext.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "BrainComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Animation/AnimMontage.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Equipment/LyraEquipmentDefinition.h"
#include "Equipment/LyraEquipmentInstance.h"
#include "Equipment/LyraEquipmentManagerComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"
#include "HAL/PlatformMisc.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/LyraCollisionChannels.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Teams/LyraTeamSubsystem.h"
#include "UObject/UnrealType.h"
#include "Weapons/LyraRangedWeaponInstance.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBWeaponRecordSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBWeaponRec, Log, All);

namespace
{
	constexpr int32 NUM_WEAPONS = 3;
	const TCHAR* const WEAPON_NAMES[NUM_WEAPONS] = { TEXT("pistol"), TEXT("rifle"), TEXT("shotgun") };
	const TCHAR* const ITEM_PATHS[NUM_WEAPONS] = {
		TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle.ID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/ID_Shotgun.ID_Shotgun_C"),
	};
	const TCHAR* const EFFECT_PATHS[NUM_WEAPONS] = {
		TEXT("/Game/Weapons/Pistol/GE_Damage_Pistol.GE_Damage_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GE_Damage_RifleAuto.GE_Damage_RifleAuto_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GE_Damage_Shotgun.GE_Damage_Shotgun_C"),
	};
	const TCHAR* const FIRE_PATHS[NUM_WEAPONS] = {
		TEXT("/ShooterCore/Weapons/Pistol/GA_Weapon_Fire_Pistol.GA_Weapon_Fire_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Fire_Rifle_Auto.GA_Weapon_Fire_Rifle_Auto_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Fire_Shotgun.GA_Weapon_Fire_Shotgun_C"),
	};

	constexpr double MATCH_SETTLE = 3.0;
	constexpr double EQUIP_SETTLE = 1.5;
	constexpr float HUGE_HEALTH = 20000.f;
	constexpr double ENEMY_DISTANCE_NEAR = 450.0;
	constexpr int32 SHOTS_PER_STAGE = 8;
	constexpr double SHOT_GAP = 0.7;
	constexpr double RAY_LENGTH_MIN = 100.0;
	constexpr double RAY_LENGTH_MAX = 3000.0;
	constexpr int32 RAYS_PER_FRAME = 4;
	constexpr int32 POSE_LAST_STATE = 5;			// 0 서기 · 1 걷기 · 2 점프 · 3 제자리 회전 · 4 위아래 조준(실제 게임과 같은 설정) · 5 위아래 조준(AI 컨트롤러 멈춤 — 시선이 몸에 먹었을 때의 값). Lyra 에 달리기 입력 자산이 없어(IA_Sprint 없음, IA_AutoRun 은 자동 전진) 걷기가 곧 달리기다
	constexpr int32 POSE_SAMPLE_FRAMES = 6;		// 60프레임 기준 초당 10번 — 걸음 한 주기에 여러 번 찍히면서 기록이 수 MB 로 끝나는 간격
	constexpr double POSE_SETTLE_SECONDS = 1.0;	// 동작이 바뀐 직후 애니 섞임 구간은 빼고 잰다. 섞임이 1초 안쪽이라는 가정 — 더 길면 앞 동작이 섞여 들어온다
	constexpr double POSE_PITCH_DEGREES = 60.0;	// 위아래 조준 폭. 평소 교전보다 넓게 — 몸이 가장 벌어질 때를 보려고

	// 곡선 경계 앞뒤를 일부러 넣은 거리 표본
	const double DISTANCES[] = { 0, 1, 50, 100, 500, 599, 600, 601, 620, 639, 640, 641, 1000, 1999, 2000, 2000.5, 2001, 2002, 2500, 2799, 2800, 2800.5, 2801, 2802, 5000, 10000, 24999, 25000, 30000 };

	const FGameplayTag FireTag() { return FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire")); }
	const FGameplayTag FireAutoTag() { return FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.FireAuto")); }
	const FGameplayTag ReloadTag() { return FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Reload")); }

	UAbilitySystemComponent* FindASC(AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}

	float ReadFloatProp(UObject* O, const TCHAR* Name)
	{
		const FProperty* P = O->GetClass()->FindPropertyByName(Name);
		if (const FFloatProperty* F = CastField<FFloatProperty>(P)) return F->GetPropertyValue_InContainer(O);
		if (const FDoubleProperty* D = CastField<FDoubleProperty>(P)) return (float)D->GetPropertyValue_InContainer(O);
		return -1.f;
	}

	int32 ReadIntProp(UObject* O, const TCHAR* Name)
	{
		const FProperty* P = O->GetClass()->FindPropertyByName(Name);
		if (const FIntProperty* I = CastField<FIntProperty>(P)) return I->GetPropertyValue_InContainer(O);
		return -1;
	}

	bool ReadBoolProp(UObject* O, const TCHAR* Name)
	{
		const FBoolProperty* B = CastField<FBoolProperty>(O->GetClass()->FindPropertyByName(Name));
		return B ? B->GetPropertyValue_InContainer(O) : false;
	}
}

bool UMOBWeaponRecordSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBWeaponRecord="), T);
}

void UMOBWeaponRecordSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const TCHAR* Cmd = FCommandLine::Get();
	FParse::Value(Cmd, TEXT("MOBWeaponRecord="), Tag);
	FParse::Value(Cmd, TEXT("MOBWeaponSeed="), Seed);
	FParse::Value(Cmd, TEXT("MOBWeaponSegs="), SegsPerWeapon);
	FParse::Value(Cmd, TEXT("MOBWeaponSegSec="), SegSeconds);
	FParse::Value(Cmd, TEXT("MOBWeaponRaySec="), RaySeconds);
	bPoseOnly = FParse::Param(Cmd, TEXT("MOBPoseOnly"));
	Rng.Initialize(Seed);
}

TStatId UMOBWeaponRecordSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBWeaponRecordSubsystem, STATGROUP_Tickables);
}

float UMOBWeaponRecordSubsystem::ReadHealth(APawn* P) const
{
	const UAbilitySystemComponent* A = FindASC(P);
	return A ? A->GetNumericAttribute(ULyraHealthSet::GetHealthAttribute()) : -1.f;
}

void UMOBWeaponRecordSubsystem::SetHugeHealth(APawn* P)
{
	if (UAbilitySystemComponent* A = FindASC(P))
	{
		A->SetNumericAttributeBase(ULyraHealthSet::GetMaxHealthAttribute(), HUGE_HEALTH);
		A->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), HUGE_HEALTH);
	}
}

bool UMOBWeaponRecordSubsystem::IsMatchPlaying(double Real)
{
	// 조건: 내 폰이 있고 면역 태그가 없는 상태가 정해진 시간 이어진다(재시작으로 폰이 바뀌면 처음부터)
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	APawn* Me = PC ? PC->GetPawn() : nullptr;
	const UAbilitySystemComponent* A = FindASC(Me);
	const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	if (!A || A->HasMatchingGameplayTag(Immune) || Me != ClearPawn.Get())
	{
		ClearPawn = Me;
		ClearSince = Real;
		return false;
	}
	return Real - ClearSince > MATCH_SETTLE;
}

bool UMOBWeaponRecordSubsystem::PrepareActors()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World->GetFirstPlayerController();
	ACharacter* Me = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Me || !FindASC(Me)) return false;

	ULyraTeamSubsystem* Teams = World->GetSubsystem<ULyraTeamSubsystem>();
	ACharacter* Foe = nullptr;
	ACharacter* Mate = nullptr;
	for (TActorIterator<ACharacter> It(World); It; ++It)
	{
		ACharacter* C = *It;
		if (C == Me || !FindASC(C)) continue;
		const bool bCanHurt = Teams ? Teams->CanCauseDamage(Me, C) : true;
		if (bCanHurt && !Foe) Foe = C;
		if (!bCanHurt && !Mate) Mate = C;
	}
	if (!Foe) return false;

	Mine = Me;
	Enemy = Foe;
	Ally = Mate;

	// 봇 머리를 끈다 — 몸만 빌린다
	for (ACharacter* C : { Foe, Mate })
	{
		if (!C) continue;
		if (AAIController* AI = Cast<AAIController>(C->GetController()))
		{
			if (AI->BrainComponent) AI->BrainComponent->StopLogic(TEXT("MOBWeaponRecord"));
			AI->StopMovement();
			AI->ClearFocus(EAIFocusPriority::Gameplay);
		}
		SetHugeHealth(C);
	}
	SetHugeHealth(Me);
	MineHome = Me->GetActorLocation();

	// 적을 내 앞 빈 자리로 — 8 방향 중 800cm 까지 막힘 없는 쪽
	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBWeaponPlace), true, Me);
	Params.AddIgnoredActor(Foe);
	if (Mate) Params.AddIgnoredActor(Mate);
	bool bFound = false;
	for (int32 I = 0; I < 8 && !bFound; ++I)
	{
		const FVector Dir = FRotator(0, 45.f * I, 0).Vector();
		FHitResult H;
		if (!World->LineTraceSingleByChannel(H, MineHome, MineHome + Dir * 800.0, Lyra_TraceChannel_Weapon, Params))
		{
			HitDir = Dir;
			bFound = true;
		}
	}
	UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC actors mine=%s enemy=%s ally=%s clearDir=%d"), *Me->GetName(), *Foe->GetName(), *GetNameSafe(Mate), bFound ? 1 : 0);
	EnemyHome = MineHome + HitDir * ENEMY_DISTANCE_NEAR;
	Foe->TeleportTo(EnemyHome, FRotator(0, (HitDir * -1.0).Rotation().Yaw, 0), false, true);
	if (Mate) Mate->TeleportTo(MineHome - HitDir * 300.0, FRotator::ZeroRotator, false, true);

	// 체력 확인
	if (UAbilitySystemComponent* FoeASC = FindASC(Foe))
	{
		EffectHandle = FoeASC->OnGameplayEffectAppliedDelegateToSelf.AddUObject(this, &ThisClass::OnEnemyEffect);
	}
	if (UAbilitySystemComponent* MyASC = FindASC(Me))
	{
		MyASC->AbilityFailedCallbacks.AddUObject(this, &ThisClass::OnAbilityFailed);
		MyASC->AbilityActivatedCallbacks.AddUObject(this, &ThisClass::OnAbilityActivated);
		MyASC->AbilityEndedCallbacks.AddUObject(this, &ThisClass::OnAbilityEnded);
	}

	PhysBody = LoadObject<UPhysicalMaterial>(nullptr, TEXT("/Game/Characters/Heroes/PhysMat_Player.PhysMat_Player"));
	PhysWeak = LoadObject<UPhysicalMaterial>(nullptr, TEXT("/Game/Characters/Heroes/PhysMat_Player_WeakSpot.PhysMat_Player_WeakSpot"));
	for (int32 I = 0; I < NUM_WEAPONS; ++I)
	{
		DamageEffects.Add(LoadClass<UGameplayEffect>(nullptr, EFFECT_PATHS[I]));
		FireAbilities.Add(LoadObject<UClass>(nullptr, FIRE_PATHS[I]));
	}
	return AddWeapons();
}

bool UMOBWeaponRecordSubsystem::AddWeapons()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	ULyraQuickBarComponent* QB = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!Inv || !QB)
	{
		UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC no_inventory inv=%d qb=%d"), Inv ? 1 : 0, QB ? 1 : 0);
		return false;
	}
	for (int32 I = 0; I < NUM_WEAPONS; ++I)
	{
		UClass* ItemClass = LoadClass<ULyraInventoryItemDefinition>(nullptr, ITEM_PATHS[I]);
		ULyraInventoryItemInstance* Item = ItemClass ? Inv->AddItemDefinition(ItemClass, 1) : nullptr;
		if (!Item)
		{
			UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC item_failed %s"), ITEM_PATHS[I]);
			return false;
		}
		Items.Add(Item);
	}
	// 기존 슬롯을 비워 무기 셋을 0·1·2 번에 넣는다
	for (int32 I = 0; I < NUM_WEAPONS; ++I)
	{
		QB->RemoveItemFromSlot(I);
		QB->AddItemToSlot(I, Items[I].Get());
	}
	return true;
}

void UMOBWeaponRecordSubsystem::Equip(int32 WeaponIndex)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* QB = PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
	if (!QB) return;
	// 같은 슬롯 재선택은 무시되므로 비었던 슬롯을 거쳐 다시 장착한다
	QB->SetActiveSlotIndex(QB->GetActiveSlotIndex() == WeaponIndex ? (WeaponIndex + 1) % NUM_WEAPONS : WeaponIndex);
	QB->SetActiveSlotIndex(WeaponIndex);
	Csv += FString::Printf(TEXT("EQUIP,%s,%d,%.6f\n"), WEAPON_NAMES[WeaponIndex], SegIdx, GetWorld()->GetTimeSeconds());
}

ULyraRangedWeaponInstance* UMOBWeaponRecordSubsystem::CurrentWeapon() const
{
	if (ACharacter* Me = Mine.Get())
	{
		if (ULyraEquipmentManagerComponent* E = Me->FindComponentByClass<ULyraEquipmentManagerComponent>())
		{
			return Cast<ULyraRangedWeaponInstance>(E->GetFirstInstanceOfType(ULyraRangedWeaponInstance::StaticClass()));
		}
	}
	return nullptr;
}

ULyraInventoryItemInstance* UMOBWeaponRecordSubsystem::CurrentItem() const
{
	return Items.IsValidIndex(WeaponIdx) ? Items[WeaponIdx].Get() : nullptr;
}

float UMOBWeaponRecordSubsystem::ReadHeat(ULyraRangedWeaponInstance* W) const
{
	return W ? ReadFloatProp(W, TEXT("Debug_CurrentHeat")) : -1.f;
}

int32 UMOBWeaponRecordSubsystem::ReadStack(const TCHAR* TagName) const
{
	ULyraInventoryItemInstance* I = CurrentItem();
	return I ? I->GetStatTagStackCount(FGameplayTag::RequestGameplayTag(TagName)) : -1;
}

void UMOBWeaponRecordSubsystem::RefillAmmo()
{
	ULyraInventoryItemInstance* I = CurrentItem();
	if (!I) return;
	const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	const FGameplayTag SpareTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));
	const FGameplayTag SizeTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineSize"));
	const int32 Size = I->GetStatTagStackCount(SizeTag);
	I->RemoveStatTagStack(MagTag, I->GetStatTagStackCount(MagTag));
	I->AddStatTagStack(MagTag, Size);
	const int32 SpareTarget = (Phase == EPhase::ScriptStart && SegIdx == SegsPerWeapon - 1) ? Size / 2 + 1 : Size * 4;
	I->RemoveStatTagStack(SpareTag, I->GetStatTagStackCount(SpareTag));
	I->AddStatTagStack(SpareTag, SpareTarget);
}

void UMOBWeaponRecordSubsystem::WriteCurve(int32 WeaponIndex, ULyraRangedWeaponInstance* W, const TCHAR* PropName)
{
	const FStructProperty* P = CastField<FStructProperty>(W->GetClass()->FindPropertyByName(PropName));
	const FRuntimeFloatCurve* C = P ? P->ContainerPtrToValuePtr<FRuntimeFloatCurve>(W) : nullptr;
	const FRichCurve* R = C ? C->GetRichCurveConst() : nullptr;
	if (!R)
	{
		Csv += FString::Printf(TEXT("TABLE,%s,%s.missing,1\n"), WEAPON_NAMES[WeaponIndex], PropName);
		return;
	}
	Csv += FString::Printf(TEXT("TABLE,%s,%s.count,%d\n"), WEAPON_NAMES[WeaponIndex], PropName, R->Keys.Num());
	Csv += FString::Printf(TEXT("TABLE,%s,%s.pre,%d\n"), WEAPON_NAMES[WeaponIndex], PropName, (int32)R->PreInfinityExtrap.GetValue());
	Csv += FString::Printf(TEXT("TABLE,%s,%s.post,%d\n"), WEAPON_NAMES[WeaponIndex], PropName, (int32)R->PostInfinityExtrap.GetValue());
	for (int32 K = 0; K < R->Keys.Num(); ++K)
	{
		Csv += FString::Printf(TEXT("TABLE,%s,%s.%d.time,%.6f\n"), WEAPON_NAMES[WeaponIndex], PropName, K, R->Keys[K].Time);
		Csv += FString::Printf(TEXT("TABLE,%s,%s.%d.value,%.6f\n"), WEAPON_NAMES[WeaponIndex], PropName, K, R->Keys[K].Value);
		Csv += FString::Printf(TEXT("TABLE,%s,%s.%d.interp,%d\n"), WEAPON_NAMES[WeaponIndex], PropName, K, (int32)R->Keys[K].InterpMode.GetValue());
	}
	// 곡선을 직접 대조하기 위한 표본 — 엔진 Eval 결과
	const float Span = R->Keys.Num() > 0 ? R->Keys.Last().Time : 1.f;
	for (int32 S = 0; S <= 64; ++S)
	{
		const float T = -1.f + (Span + 2.f) * (float)S / 64.f;
		Csv += FString::Printf(TEXT("CURVE,%s,%s,%.6f,%.6f\n"), WEAPON_NAMES[WeaponIndex], PropName, T, R->Eval(T));
	}
}

void UMOBWeaponRecordSubsystem::WriteTable(int32 WeaponIndex)
{
	ULyraRangedWeaponInstance* W = CurrentWeapon();
	if (!W)
	{
		UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC no_weapon %s"), WEAPON_NAMES[WeaponIndex]);
		return;
	}
	const TCHAR* N = WEAPON_NAMES[WeaponIndex];
	const auto F = [&](const TCHAR* Name) { Csv += FString::Printf(TEXT("TABLE,%s,%s,%.6f\n"), N, Name, ReadFloatProp(W, Name)); };
	Csv += FString::Printf(TEXT("TABLE,%s,InstanceClass,%s\n"), N, *W->GetClass()->GetName());
	Csv += FString::Printf(TEXT("TABLE,%s,BulletsPerCartridge,%d\n"), N, ReadIntProp(W, TEXT("BulletsPerCartridge")));
	Csv += FString::Printf(TEXT("TABLE,%s,bAllowFirstShotAccuracy,%d\n"), N, ReadBoolProp(W, TEXT("bAllowFirstShotAccuracy")) ? 1 : 0);
	for (const TCHAR* Name : { TEXT("SpreadExponent"), TEXT("SpreadRecoveryCooldownDelay"), TEXT("SpreadAngleMultiplier_Aiming"), TEXT("SpreadAngleMultiplier_StandingStill"), TEXT("TransitionRate_StandingStill"), TEXT("StandingStillSpeedThreshold"), TEXT("StandingStillToMovingSpeedRange"), TEXT("SpreadAngleMultiplier_Crouching"), TEXT("TransitionRate_Crouching"), TEXT("SpreadAngleMultiplier_JumpingOrFalling"), TEXT("TransitionRate_JumpingOrFalling"), TEXT("MaxDamageRange"), TEXT("BulletTraceSweepRadius") })
	{
		F(Name);
	}
	for (const TCHAR* Name : { TEXT("HeatToSpreadCurve"), TEXT("HeatToHeatPerShotCurve"), TEXT("HeatToCoolDownPerSecondCurve"), TEXT("DistanceDamageFalloff") })
	{
		WriteCurve(WeaponIndex, W, Name);
	}

	// 약점 배수 — 약점 재질의 태그를 무기 표에 물어 본 값
	const ILyraAbilitySourceInterface* Src = W;
	Csv += FString::Printf(TEXT("TABLE,%s,WeakSpotMultiplier,%.6f\n"), N, Src->GetPhysicalMaterialAttenuation(PhysWeak, nullptr, nullptr));
	Csv += FString::Printf(TEXT("TABLE,%s,BodyMultiplier,%.6f\n"), N, Src->GetPhysicalMaterialAttenuation(PhysBody, nullptr, nullptr));

	// 기본 피해 — 효과 안에 박힌 값
	float Base = -1.f;
	if (const UGameplayEffect* GE = DamageEffects[WeaponIndex] ? DamageEffects[WeaponIndex]->GetDefaultObject<UGameplayEffect>() : nullptr)
	{
		if (GE->Executions.Num() > 0 && GE->Executions[0].CalculationModifiers.Num() > 0)
		{
			GE->Executions[0].CalculationModifiers[0].ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, Base);
		}
	}
	Csv += FString::Printf(TEXT("TABLE,%s,BaseDamage,%.6f\n"), N, Base);

	// 연사 — 능력 블루프린트 변수
	if (UClass* FA = FireAbilities[WeaponIndex])
	{
		UObject* CDO = FA->GetDefaultObject();
		Csv += FString::Printf(TEXT("TABLE,%s,FireDelayTimeSecs,%.6f\n"), N, ReadFloatProp(CDO, TEXT("FireDelayTimeSecs")));
		Csv += FString::Printf(TEXT("TABLE,%s,AutoRate,%.6f\n"), N, ReadFloatProp(CDO, TEXT("AutoRate")));
	}

	// 몽타주 — 발사 능력이 언제 끝나는지, 재장전 탄이 언제 들어오는지의 근거
	const auto DumpMontage = [&](const TCHAR* Label, UObject* Owner, const TCHAR* PropName, const TCHAR* RateName)
	{
		const FObjectPropertyBase* OP = Owner ? CastField<FObjectPropertyBase>(Owner->GetClass()->FindPropertyByName(PropName)) : nullptr;
		const UAnimMontage* M = OP ? Cast<UAnimMontage>(OP->GetObjectPropertyValue_InContainer(Owner)) : nullptr;
		if (!M)
		{
			Csv += FString::Printf(TEXT("INFO,%s,%s.missing,1\n"), N, Label);
			return;
		}
		Csv += FString::Printf(TEXT("INFO,%s,%s.name,%s\n"), N, Label, *M->GetName());
		Csv += FString::Printf(TEXT("INFO,%s,%s.length,%.6f\n"), N, Label, M->GetPlayLength());
		Csv += FString::Printf(TEXT("INFO,%s,%s.rateScale,%.6f\n"), N, Label, M->RateScale);
		Csv += FString::Printf(TEXT("INFO,%s,%s.blendIn,%.6f\n"), N, Label, M->BlendIn.GetBlendTime());
		Csv += FString::Printf(TEXT("INFO,%s,%s.blendOut,%.6f\n"), N, Label, M->BlendOut.GetBlendTime());
		Csv += FString::Printf(TEXT("INFO,%s,%s.playRate,%.6f\n"), N, Label, ReadFloatProp(Owner, RateName));
		for (const FAnimNotifyEvent& E : M->Notifies)
		{
			Csv += FString::Printf(TEXT("INFO,%s,%s.notify,%s@%.6f\n"), N, Label, *E.NotifyName.ToString(), E.GetTriggerTime());
		}
	};
	if (UClass* FA = FireAbilities[WeaponIndex]) DumpMontage(TEXT("fireMontage"), FA->GetDefaultObject(), TEXT("CharacterFireMontage"), TEXT("AutoRate"));
	static const TCHAR* const RELOAD_PATHS[NUM_WEAPONS] = {
		TEXT("/Game/Weapons/Pistol/GA_Weapon_Reload_Pistol.GA_Weapon_Reload_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/GA_Weapon_Reload_Rifle.GA_Weapon_Reload_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/GA_Weapon_Reload_Shotgun.GA_Weapon_Reload_Shotgun_C"),
	};
	if (UClass* RA = LoadObject<UClass>(nullptr, RELOAD_PATHS[WeaponIndex])) DumpMontage(TEXT("reloadMontage"), RA->GetDefaultObject(), TEXT("ReloadMontage"), TEXT("Playrate"));

	// 탄창 — 처음 받은 값
	Csv += FString::Printf(TEXT("TABLE,%s,MagazineSize,%d\n"), N, ReadStack(TEXT("Lyra.ShooterGame.Weapon.MagazineSize")));
	Csv += FString::Printf(TEXT("TABLE,%s,MagazineAmmo,%d\n"), N, ReadStack(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo")));
	Csv += FString::Printf(TEXT("TABLE,%s,SpareAmmo,%d\n"), N, ReadStack(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo")));
}

float UMOBWeaponRecordSubsystem::ApplyDamageSample(int32 WeaponIndex, APawn* Target, double Distance, UPhysicalMaterial* PM)
{
	UAbilitySystemComponent* Src = FindASC(Mine.Get());
	UAbilitySystemComponent* Dst = FindASC(Target);
	ULyraRangedWeaponInstance* W = CurrentWeapon();
	if (!Src || !Dst || !W || !DamageEffects[WeaponIndex]) return -1.f;

	FGameplayEffectContextHandle Ctx = Src->MakeEffectContext();
	FLyraGameplayEffectContext* L = FLyraGameplayEffectContext::ExtractEffectContext(Ctx);
	if (!L) return -2.f;
	L->SetAbilitySource(static_cast<const ILyraAbilitySourceInterface*>(W), 1.f);
	L->AddSourceObject(W);

	const FVector Start(0, 0, 0);
	const FVector Impact(Distance, 0, 0);
	FHitResult Hit(Target, nullptr, Impact, FVector::UpVector);
	Hit.TraceStart = Start;
	Hit.TraceEnd = Impact;
	Hit.ImpactPoint = Impact;
	Hit.Location = Impact;
	Hit.bBlockingHit = true;
	Hit.PhysMaterial = PM;
	Ctx.AddHitResult(Hit, true);

	FGameplayEffectSpecHandle Spec = Src->MakeOutgoingSpec(DamageEffects[WeaponIndex], 1.f, Ctx);
	if (!Spec.IsValid()) return -3.f;

	const float Before = Dst->GetNumericAttribute(ULyraHealthSet::GetHealthAttribute());
	Src->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Dst);
	const float After = Dst->GetNumericAttribute(ULyraHealthSet::GetHealthAttribute());
	Dst->SetNumericAttributeBase(ULyraHealthSet::GetHealthAttribute(), HUGE_HEALTH);
	return Before - After;
}

void UMOBWeaponRecordSubsystem::WriteDamageSamples(int32 WeaponIndex)
{
	const TCHAR* N = WEAPON_NAMES[WeaponIndex];
	int32 Count = 0;
	float Max = 0.f;
	for (const double D : DISTANCES)
	{
		const float Enemy0 = ApplyDamageSample(WeaponIndex, Enemy.Get(), D, nullptr);
		const float Enemy1 = ApplyDamageSample(WeaponIndex, Enemy.Get(), D, PhysBody);
		const float Enemy2 = ApplyDamageSample(WeaponIndex, Enemy.Get(), D, PhysWeak);
		Csv += FString::Printf(TEXT("DMG,%s,%.3f,none,enemy,%.6f\nDMG,%s,%.3f,body,enemy,%.6f\nDMG,%s,%.3f,weak,enemy,%.6f\n"), N, D, Enemy0, N, D, Enemy1, N, D, Enemy2);
		Count += 3;
		Max = FMath::Max3(Max, Enemy1, Enemy2);
		if (Ally.IsValid())
		{
			const float Mate = ApplyDamageSample(WeaponIndex, Ally.Get(), D, PhysWeak);
			Csv += FString::Printf(TEXT("DMG,%s,%.3f,weak,ally,%.6f\n"), N, D, Mate);
			++Count;
		}
	}
	UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC damage_samples weapon=%s count=%d max=%.3f"), N, Count, Max);
}

void UMOBWeaponRecordSubsystem::OnAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& Tags)
{
	if (Phase != EPhase::Script) return;
	Csv += FString::Printf(TEXT("ABILITY,fail,%s,%d,%d,%s,%s\n"), WEAPON_NAMES[WeaponIdx], SegIdx, Frame, *GetNameSafe(Ability), *Tags.ToStringSimple().Replace(TEXT(","), TEXT(";")));
}

void UMOBWeaponRecordSubsystem::OnAbilityActivated(UGameplayAbility* Ability)
{
	if (Phase != EPhase::Script) return;
	Csv += FString::Printf(TEXT("ABILITY,start,%s,%d,%d,%s,\n"), WEAPON_NAMES[WeaponIdx], SegIdx, Frame, *GetNameSafe(Ability));
}

void UMOBWeaponRecordSubsystem::OnAbilityEnded(UGameplayAbility* Ability)
{
	if (Phase != EPhase::Script) return;
	Csv += FString::Printf(TEXT("ABILITY,end,%s,%d,%d,%s,\n"), WEAPON_NAMES[WeaponIdx], SegIdx, Frame, *GetNameSafe(Ability));
}

void UMOBWeaponRecordSubsystem::LogAbilityState(const TCHAR* Where)
{
	UAbilitySystemComponent* A = FindASC(Mine.Get());
	if (!A) return;
	const FGameplayTag NoFiring = FGameplayTag::RequestGameplayTag(TEXT("Ability.Weapon.NoFiring"));
	UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC state %s weapon=%s frame=%d noFiring=%d held=%d inst=%s"), Where, WEAPON_NAMES[WeaponIdx], Frame, A->GetGameplayTagCount(NoFiring), bFireHeld ? 1 : 0, *GetNameSafe(CurrentWeapon()));
	for (const FGameplayAbilitySpec& Spec : A->GetActivatableAbilities())
	{
		if (!Spec.Ability || !(Spec.GetDynamicSpecSourceTags().HasTagExact(FireTag()) || Spec.GetDynamicSpecSourceTags().HasTagExact(FireAutoTag()))) continue;
		UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC  fire_ability %s active=%d src=%s"), *Spec.Ability->GetName(), Spec.IsActive() ? 1 : 0, *GetNameSafe(Spec.SourceObject.Get()));
	}
}

void UMOBWeaponRecordSubsystem::SetFire(bool bHeld)
{
	ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Mine.Get()));
	if (!A || bHeld == bFireHeld) return;
	bFireHeld = bHeld;
	if (bHeld)
	{
		A->AbilityInputTagPressed(FireTag());
		A->AbilityInputTagPressed(FireAutoTag());
	}
	else
	{
		A->AbilityInputTagReleased(FireTag());
		A->AbilityInputTagReleased(FireAutoTag());
	}
}

void UMOBWeaponRecordSubsystem::PulseReload()
{
	if (ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Mine.Get())))
	{
		A->AbilityInputTagPressed(ReloadTag());
		A->AbilityInputTagReleased(ReloadTag());
	}
}

void UMOBWeaponRecordSubsystem::TickScript(float Dt)
{
	ACharacter* Me = Mine.Get();
	ULyraRangedWeaponInstance* W = CurrentWeapon();
	UWorld* World = GetWorld();
	if (!Me || !W) return;
	const double Now = World->GetTimeSeconds();
	UCharacterMovementComponent* M = Me->GetCharacterMovement();

	// 이 프레임의 결과를 먼저 적고, 다음 프레임 입력을 정한다
	const float Mult = W->GetCalculatedSpreadAngleMultiplier();
	const float Speed = (float)Me->GetVelocity().Size();
	const int32 Mag = ReadStack(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	const int32 Spare = ReadStack(TEXT("Lyra.ShooterGame.Weapon.SpareAmmo"));

	// 각본 — 짧게 끊은 동작을 무작위로
	bool bPulseThisFrame = false;
	bool bJumpThisFrame = false;
	if (Now >= NextActionAt)
	{
		NextActionAt = Now + Rng.FRandRange(0.2f, 1.6f);
		const float Roll = Rng.FRand();
		const bool bHold = Roll < 0.65f;
		SetFire(bHold);
		const float MoveRoll = Rng.FRand();
		MoveDir = MoveRoll < 0.4f ? FVector::ZeroVector : FRotator(0, Rng.FRandRange(0.f, 360.f), 0).Vector();
		bJumpWanted = Rng.FRand() < 0.2f;
		bReloadWanted = Rng.FRand() < 0.12f;
	}
	if (bJumpWanted) { Me->Jump(); bJumpThisFrame = true; bJumpWanted = false; }
	else Me->StopJumping();
	if (bReloadWanted) { PulseReload(); bPulseThisFrame = true; bReloadWanted = false; }
	if (!MoveDir.IsNearlyZero())
	{
		// 맵 밖으로 나가지 않게 집 쪽으로 돌려 보낸다
		if (FVector::Dist2D(Me->GetActorLocation(), MineHome) > 500.0)
		{
			MoveDir = (MineHome - Me->GetActorLocation()).GetSafeNormal2D();
		}
		Me->AddMovementInput(MoveDir, 1.f);
	}

	int32 FireActive = 0;
	int32 ReloadActive = 0;
	if (UAbilitySystemComponent* A = FindASC(Me))
	{
		ReloadActive = A->GetGameplayTagCount(FGameplayTag::RequestGameplayTag(TEXT("Event.Movement.Reload")));
		for (const FGameplayAbilitySpec& Spec : A->GetActivatableAbilities())
		{
			if (Spec.IsActive() && (Spec.GetDynamicSpecSourceTags().HasTagExact(FireTag()) || Spec.GetDynamicSpecSourceTags().HasTagExact(FireAutoTag()))) FireActive = 1;
		}
	}
	Csv += FString::Printf(TEXT("FRAME,%s,%d,%d,%.6f,%.6f,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%d,%d,%d,%d\n"), WEAPON_NAMES[WeaponIdx], SegIdx, Frame, Now, Dt, bFireHeld ? 1 : 0, bPulseThisFrame ? 1 : 0, bJumpThisFrame ? 1 : 0, Mag, Spare, ReadHeat(W), W->GetCalculatedSpreadAngle(), Mult, Speed, M && M->IsFalling() ? 1 : 0, W->HasFirstShotAccuracy() ? 1 : 0, FireActive, ReloadActive);
	++Frame;
}

void UMOBWeaponRecordSubsystem::OnEnemyEffect(UAbilitySystemComponent* Source, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
{
	if (Phase != EPhase::Hit) return;
	const FGameplayEffectContext* C = Spec.GetContext().Get();
	const FHitResult* H = C ? C->GetHitResult() : nullptr;
	if (!H) return;
	const FVector S = H->TraceStart;
	const FVector I = H->ImpactPoint;
	const FVector Me = Mine.IsValid() ? Mine->GetActorLocation() : FVector::ZeroVector;
	Csv += FString::Printf(TEXT("HIT,%s,%d,%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n"), WEAPON_NAMES[WeaponIdx], Frame, *H->BoneName.ToString(), *GetNameSafe(H->PhysMaterial.Get()), S.X, S.Y, S.Z, I.X, I.Y, I.Z, (S - I).Size(), Me.X, Me.Y, Me.Z);
}

void UMOBWeaponRecordSubsystem::TickHit(float Dt)
{
	UWorld* World = GetWorld();
	ACharacter* Me = Mine.Get();
	ACharacter* Foe = Enemy.Get();
	APlayerController* PC = World->GetFirstPlayerController();
	if (!Me || !Foe || !PC) return;
	const double Now = World->GetTimeSeconds();

	// 체력 변화를 프레임 단위로 적는다 — HIT 합계와 대조
	const float H = ReadHealth(Foe);
	if (LastEnemyHealth != H)
	{
		Csv += FString::Printf(TEXT("HPDELTA,%s,%d,%.6f\n"), WEAPON_NAMES[WeaponIdx], Frame, LastEnemyHealth - H);
		LastEnemyHealth = H;
	}

	// 몸통 또는 머리를 겨눈다
	const bool bHead = (HitShots % 2) == 1;
	const FVector Aim = Foe->GetActorLocation() + FVector(0, 0, bHead ? 65.0 : 15.0);
	FVector ViewLoc;
	FRotator ViewRot;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	PC->SetControlRotation((Aim - ViewLoc).Rotation());

	if (Now >= NextShotAt)
	{
		SetFire(true);
		NextShotAt = Now + SHOT_GAP;
		++HitShots;
	}
	else if (Now >= NextShotAt - SHOT_GAP + 0.05)
	{
		SetFire(false);
	}
	++Frame;
}

// 실제 게임의 상대와 같게 맞춘다 — 시선은 내가 정하고(MOBridgeSubsystem::SpawnRemote), 무기는 장비 칸을 바로 바꾼다(SyncRemoteWeapon)
void UMOBWeaponRecordSubsystem::PreparePose()
{
	ACharacter* Foe = Enemy.Get();
	if (!Foe) return;
	PoseYaw = Foe->GetActorRotation().Yaw;
	if (AAIController* AI = Cast<AAIController>(Foe->GetController())) AI->bSetControlRotationFromPawnOrientation = false;
}

void UMOBWeaponRecordSubsystem::EquipFoe(int32 WeaponIndex)
{
	ACharacter* Foe = Enemy.Get();
	ULyraEquipmentManagerComponent* EM = Foe ? Foe->FindComponentByClass<ULyraEquipmentManagerComponent>() : nullptr;
	if (!EM)
	{
		UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC foe_no_equipment"));
		return;
	}
	static const TCHAR* const Names[NUM_WEAPONS] = { TEXT("Pistol"), TEXT("Rifle"), TEXT("Shotgun") };
	const FString Path = FString::Printf(TEXT("/ShooterCore/Weapons/%s/WID_%s.WID_%s_C"), Names[WeaponIndex], Names[WeaponIndex], Names[WeaponIndex]);
	UClass* Def = LoadClass<ULyraEquipmentDefinition>(nullptr, *Path);
	const TArray<ULyraEquipmentInstance*> Old = EM->GetEquipmentInstancesOfType(ULyraEquipmentInstance::StaticClass());
	for (ULyraEquipmentInstance* I : Old) EM->UnequipItem(I);
	ULyraEquipmentInstance* Now = Def ? EM->EquipItem(Def) : nullptr;
	UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC foe_weapon=%s removed=%d instance=%s"), WEAPON_NAMES[WeaponIndex], Old.Num(), *GetNameSafe(Now));
	Csv += FString::Printf(TEXT("POSEWEAPON,%d,%s,%d\n"), WeaponIndex, WEAPON_NAMES[WeaponIndex], Now ? 1 : 0);

	// 몸 종류 기록 — Lyra 는 캐릭터마다 몸을 무작위로 고른다(B_PickRandomCharacter). 몸이 다르면 뼈 길이가 달라
	// 고정 자세와의 어긋남이 그만큼 커지므로, 어느 몸으로 잰 표인지 기록에 남긴다
	USkeletalMeshComponent* FoeMesh = Foe->GetMesh();
	Csv += FString::Printf(TEXT("POSEMESH,%d,pawn,%s\n"), WeaponIndex, FoeMesh ? *GetNameSafe(FoeMesh->GetSkeletalMeshAsset()) : TEXT("none"));
	TArray<USkeletalMeshComponent*> MeshParts;
	Foe->GetComponents<USkeletalMeshComponent>(MeshParts, true);
	for (const USkeletalMeshComponent* Part : MeshParts)
	{
		if (Part && Part != FoeMesh) Csv += FString::Printf(TEXT("POSEMESH,%d,part,%s\n"), WeaponIndex, *GetNameSafe(Part->GetSkeletalMeshAsset()));
	}
}

void UMOBWeaponRecordSubsystem::WriteShapes(const TCHAR* Prefix, bool bLog)
{
	ACharacter* Foe = Enemy.Get();
	USkeletalMeshComponent* Sk = Foe ? Foe->GetMesh() : nullptr;
	UPhysicsAsset* PA = Sk ? Sk->GetPhysicsAsset() : nullptr;
	if (!PA) return;

	const FVector Loc = Foe->GetActorLocation();
	const FQuat ActorInv = FRotator(0, Foe->GetActorRotation().Yaw, 0).Quaternion().Inverse();
	Csv += FString::Printf(TEXT("SHAPEBASE,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"), Loc.X, Loc.Y, Loc.Z, Foe->GetActorRotation().Yaw, Foe->GetCapsuleComponent()->GetScaledCapsuleRadius(), Foe->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	int32 Count = 0;
	for (const USkeletalBodySetup* B : PA->SkeletalBodySetups)
	{
		if (!B) continue;
		const FBodyInstance* Inst = Sk->GetBodyInstance(B->BoneName);
		if (!Inst) continue;
		const FTransform BodyW = Inst->GetUnrealWorldTransform();
		const int32 Weak = B->GetPhysMaterial() == PhysWeak ? 1 : 0;
		const auto Emit = [&](const TCHAR* Kind, const FTransform& ElemLocal, double A, double C, double D, double E)
		{
			const FTransform W = ElemLocal * BodyW;
			const FVector P = ActorInv.RotateVector(W.GetLocation() - Loc);
			const FVector X = ActorInv.RotateVector(W.GetUnitAxis(EAxis::X));
			const FVector Y = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Y));
			const FVector Z = ActorInv.RotateVector(W.GetUnitAxis(EAxis::Z));
			Csv += FString::Printf(TEXT("%s,%s,%s,%d,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f\n"), Prefix, *B->BoneName.ToString(), Kind, Weak, P.X, P.Y, P.Z, X.X, X.Y, X.Z, Y.X, Y.Y, Y.Z, Z.X, Z.Y, Z.Z, A, C, D, E);
			++Count;
		};
		for (const FKSphereElem& E : B->AggGeom.SphereElems) Emit(TEXT("sphere"), FTransform(E.Center), E.Radius, 0, 0, 0);
		for (const FKSphylElem& E : B->AggGeom.SphylElems) Emit(TEXT("sphyl"), FTransform(E.Rotation, E.Center), E.Radius, E.Length, 0, 0);
		for (const FKBoxElem& E : B->AggGeom.BoxElems) Emit(TEXT("box"), FTransform(E.Rotation, E.Center), E.X, E.Y, E.Z, 0);
	}
	if (bLog) UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC shapes=%d"), Count);
}

void UMOBWeaponRecordSubsystem::TickRays(float Dt)
{
	UWorld* World = GetWorld();
	ACharacter* Foe = Enemy.Get();
	if (!Foe) return;
	const double Now = World->GetTimeSeconds();
	UCharacterMovementComponent* M = Foe->GetCharacterMovement();

	const FVector Loc = Foe->GetActorLocation();
	const FVector V = Foe->GetVelocity();
	const int32 Falling = M && M->IsFalling() ? 1 : 0;
	const bool bSettling = bPoseOnly && Now - PhaseAt < POSE_SETTLE_SECONDS;
	if (bPoseOnly && !bSettling && Frame % POSE_SAMPLE_FRAMES == 0) WriteShapes(*FString::Printf(TEXT("POSE,%d,%d"), StateTag(), Frame), false);
	if (!bSettling) Csv += FString::Printf(TEXT("BOT,%d,%d,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%d\n"), StateTag(), Frame, Loc.X, Loc.Y, Loc.Z, Foe->GetActorRotation().Yaw, V.X, V.Y, V.Z, Falling);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MOBWeaponRay), /*bTraceComplex=*/ true);
	Params.bReturnPhysicalMaterial = true;
	Params.AddIgnoredActor(Mine.Get());
	if (Ally.IsValid()) Params.AddIgnoredActor(Ally.Get());
	static const float Radii[] = { 0.f, 0.5f, 5.5f, 6.f };

	for (int32 I = 0; I < (bSettling ? 0 : RAYS_PER_FRAME); ++I)
	{
		const float R = Radii[RayCount % 4];
		++RayCount;

		// 몸 부근의 한 점을 겨눠 그 점을 지나는 광선
		const FVector Target = Loc + FVector(Rng.FRandRange(-50.f, 50.f), Rng.FRandRange(-50.f, 50.f), Rng.FRandRange(-95.f, 95.f));
		const FVector Dir = Rng.GetUnitVector() * FVector(1.0, 1.0, 0.5);
		const FVector DirN = Dir.GetSafeNormal();
		const double Len = Rng.FRandRange((float)RAY_LENGTH_MIN, (float)RAY_LENGTH_MAX);
		const FVector A = Target - DirN * Len;
		const FVector B = Target + DirN * 200.0;

		TArray<FHitResult> Hits;
		if (R > 0.f) World->SweepMultiByChannel(Hits, A, B, FQuat::Identity, Lyra_TraceChannel_Weapon, FCollisionShape::MakeSphere(R), Params);
		else World->LineTraceMultiByChannel(Hits, A, B, Lyra_TraceChannel_Weapon, Params);

		// 맞은 차례대로 앞 3개까지 — 0=없음 1=맵 2=적 몸
		int32 Kind = 0;
		FString Bone = TEXT("-");
		int32 Weak = 0;
		double Dist = 0;
		int32 Blocking = 0;
		if (Hits.Num() > 0)
		{
			const FHitResult& H = Hits[0];
			Kind = H.GetActor() == Foe ? 2 : 1;
			Bone = H.BoneName.ToString();
			Weak = H.PhysMaterial.Get() == PhysWeak ? 1 : 0;
			Dist = (H.Location - A).Size();
			Blocking = H.bBlockingHit ? 1 : 0;
		}
		Csv += FString::Printf(TEXT("RAY,%d,%d,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%s,%d,%.3f,%d\n"), StateTag(), Frame, R, A.X, A.Y, A.Z, B.X, B.Y, B.Z, Hits.Num(), Kind, *Bone, Weak, Dist, Blocking);
	}

	// 움직임: 상태 0=서 있기, 1=걷기, 2=점프 섞어 걷기, 3=제자리 방향 바꾸기
	if (RayState == 3)
	{
		if (Now >= NextEnemyActionAt)
		{
			NextEnemyActionAt = Now + 1.0;
			PoseYaw = Rng.FRandRange(-180.f, 180.f);
			Foe->TeleportTo(EnemyHome, FRotator(0, PoseYaw, 0), false, true);
		}
	}
	else if (RayState == 1 || RayState == 2)
	{
		if (Now >= NextEnemyActionAt)
		{
			NextEnemyActionAt = Now + Rng.FRandRange(0.5f, 1.5f);
			EnemyMoveDir = FRotator(0, Rng.FRandRange(0.f, 360.f), 0).Vector();
			if (RayState == 2) Foe->Jump();
		}
		if (FVector::Dist2D(Foe->GetActorLocation(), EnemyHome) > 400.0)
		{
			EnemyMoveDir = (EnemyHome - Foe->GetActorLocation()).GetSafeNormal2D();
		}
		Foe->AddMovementInput(EnemyMoveDir, 1.f);
		if (bPoseOnly && RayState == 2) Foe->Jump();	// 자세만 잴 때는 내려오자마자 다시 뛰어 공중 프레임을 늘린다
	}
	if (bPoseOnly)
	{
		const double Pitch = RayState >= 4 ? POSE_PITCH_DEGREES * FMath::Sin(2.0 * UE_DOUBLE_PI * (Now - PhaseAt) / 4.0) : 0.0;
		AController* FoeCtrl = Foe->GetController();
		if (FoeCtrl)
		{
			// 값이 몸에 닿는지 — 내가 지난 프레임에 넣은 값이 애니가 읽는 시선으로 남아 있나
			if (RayState >= 4 && Frame % POSE_SAMPLE_FRAMES == 0) Csv += FString::Printf(TEXT("POSEAIM,%d,%d,%.3f,%.3f\n"), StateTag(), Frame, Pitch, FRotator::NormalizeAxis(Foe->GetBaseAimRotation().Pitch));
			FoeCtrl->SetActorTickEnabled(RayState != 5);
			FoeCtrl->SetControlRotation(FRotator(Pitch, PoseYaw, 0.0));
		}
	}
	++Frame;
}

void UMOBWeaponRecordSubsystem::Tick(float DeltaTime)
{
	if (Phase == EPhase::Done) return;
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay()) return;
	const double Real = World->GetRealTimeSeconds();
	if (StartedAt < 0) StartedAt = Real;

	switch (Phase)
	{
	case EPhase::WaitPawns:
		if (!IsMatchPlaying(Real))
		{
			if (Real - StartedAt > 120.0)
			{
				UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC match_timeout"));
				Finish();
			}
		}
		else if (PrepareActors())
		{
			Csv += FString::Printf(TEXT("META,seed,%d\n"), Seed);
			Csv += FString::Printf(TEXT("META,ally,%d\n"), Ally.IsValid() ? 1 : 0);
			if (bPoseOnly)
			{
				PreparePose();
				PoseWeapon = 0;
				EquipFoe(PoseWeapon);
				PhaseAt = Real;
				Phase = EPhase::PoseEquip;
				break;
			}
			Phase = EPhase::Equip;
			WeaponIdx = 0;
			Equip(WeaponIdx);
			PhaseAt = Real;
		}
		else if (Real - StartedAt > 120.0)
		{
			UE_LOG(LogMOBWeaponRec, Error, TEXT("MOB_WREC pawns_timeout"));
			Finish();
		}
		break;

	case EPhase::Equip:
		if (Real - PhaseAt > EQUIP_SETTLE) Phase = EPhase::Table;
		break;

	case EPhase::Table:
		WriteTable(WeaponIdx);
		WriteDamageSamples(WeaponIdx);
		++WeaponIdx;
		if (WeaponIdx < NUM_WEAPONS)
		{
			Equip(WeaponIdx);
			PhaseAt = Real;
			Phase = EPhase::Equip;
		}
		else
		{
			WeaponIdx = 0;
			SegIdx = 0;
			Phase = EPhase::ScriptStart;
			Equip(WeaponIdx);
			PhaseAt = Real;
			UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC table_done"));
		}
		break;

	case EPhase::ScriptStart:
		if (Real - PhaseAt > EQUIP_SETTLE)
		{
			RefillAmmo();
			NextActionAt = 0;
			Frame = 0;
			PhaseAt = World->GetTimeSeconds();
			Phase = EPhase::Script;
		}
		break;

	case EPhase::Script:
		if (Frame == 0 || Frame == 90) LogAbilityState(TEXT("script"));
		TickScript(DeltaTime);
		if (World->GetTimeSeconds() - PhaseAt > SegSeconds)
		{
			SetFire(false);
			MoveDir = FVector::ZeroVector;
			++SegIdx;
			if (SegIdx >= SegsPerWeapon)
			{
				SegIdx = 0;
				++WeaponIdx;
			}
			if (WeaponIdx < NUM_WEAPONS)
			{
				Equip(WeaponIdx);
				PhaseAt = Real;
				Phase = EPhase::ScriptStart;
			}
			else
			{
				UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC script_done"));
				WeaponIdx = 0;
				HitStage = 0;
				if (ACharacter* Me = Mine.Get()) Me->TeleportTo(MineHome, Me->GetActorRotation(), false, true);
				Equip(WeaponIdx);
				PhaseAt = Real;
				Phase = EPhase::HitStart;
			}
		}
		break;

	case EPhase::HitStart:
		if (Real - PhaseAt > EQUIP_SETTLE)
		{
			RefillAmmo();
			if (ACharacter* Foe = Enemy.Get())
			{
				static const double Far[] = { ENEMY_DISTANCE_NEAR, 750.0 };
				Foe->TeleportTo(MineHome + HitDir * Far[HitStage], Foe->GetActorRotation(), false, true);
				SetHugeHealth(Foe);
				LastEnemyHealth = ReadHealth(Foe);
			}
			HitShots = 0;
			NextShotAt = World->GetTimeSeconds() + 0.5;
			Frame = 0;
			Phase = EPhase::Hit;
		}
		break;

	case EPhase::Hit:
		TickHit(DeltaTime);
		if (HitShots > SHOTS_PER_STAGE)
		{
			SetFire(false);
			++HitStage;
			if (HitStage >= 2)
			{
				HitStage = 0;
				++WeaponIdx;
			}
			if (WeaponIdx < NUM_WEAPONS)
			{
				Equip(WeaponIdx);
				PhaseAt = Real;
				Phase = EPhase::HitStart;
			}
			else
			{
				UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC hit_done"));
				if (ACharacter* Foe = Enemy.Get()) Foe->TeleportTo(EnemyHome, Foe->GetActorRotation(), false, true);
				PhaseAt = Real;
				Phase = EPhase::RayStart;
			}
		}
		break;

	case EPhase::RayStart:
		if (Real - PhaseAt > 2.0)
		{
			WriteShapes(TEXT("SHAPE"));
			Frame = 0;
			RayState = 0;
			PhaseAt = World->GetTimeSeconds();
			Phase = EPhase::Ray;
		}
		break;

	case EPhase::Ray:
		TickRays(DeltaTime);
		if (World->GetTimeSeconds() - PhaseAt > RaySeconds)
		{
			WriteShapes(*FString::Printf(TEXT("SHAPE_END%d"), StateTag()));
			PhaseAt = World->GetTimeSeconds();
			NextEnemyActionAt = 0;
			++RayState;
			if (RayState > (bPoseOnly ? POSE_LAST_STATE : 3))
			{
				if (bPoseOnly && PoseWeapon + 1 < NUM_WEAPONS)
				{
					++PoseWeapon;
					RayState = 0;
					if (ACharacter* PoseFoe = Enemy.Get()) PoseFoe->TeleportTo(EnemyHome, FRotator(0, PoseYaw, 0), false, true);
					EquipFoe(PoseWeapon);
					PhaseAt = Real;
					Phase = EPhase::PoseEquip;
				}
				else
				{
					UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC rays=%d"), RayCount);
					Finish();
				}
			}
		}
		break;

	case EPhase::PoseEquip:
		if (Real - PhaseAt > 2.0)
		{
			WriteShapes(*FString::Printf(TEXT("POSESHAPE%d"), PoseWeapon), false);
			Frame = 0;
			RayState = 0;
			NextEnemyActionAt = 0;
			PhaseAt = World->GetTimeSeconds();
			Phase = EPhase::Ray;
		}
		break;

	default:
		break;
	}
}

void UMOBWeaponRecordSubsystem::Finish()
{
	Phase = EPhase::Done;
	const FString File = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("weaponrec_%s.csv"), *Tag);
	FFileHelper::SaveStringToFile(Csv, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(LogMOBWeaponRec, Log, TEXT("MOB_WREC done chars=%d"), Csv.Len());
	FPlatformMisc::RequestExit(false);
}
