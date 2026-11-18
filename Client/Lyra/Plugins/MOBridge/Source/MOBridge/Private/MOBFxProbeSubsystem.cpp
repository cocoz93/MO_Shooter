#include "MOBFxProbeSubsystem.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UnrealClient.h"

#include "AbilitySystem/LyraAbilitySystemComponent.h"
#include "Equipment/LyraQuickBarComponent.h"
#include "Inventory/LyraInventoryItemDefinition.h"
#include "Inventory/LyraInventoryItemInstance.h"
#include "Inventory/LyraInventoryManagerComponent.h"
#include "Physics/LyraCollisionChannels.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBFxProbeSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogMOBFx, Log, All);

namespace
{
	const TCHAR* const WEAPON_NAMES[3] = { TEXT("pistol"), TEXT("rifle"), TEXT("shotgun") };
	const TCHAR* const ITEM_DEFS[3] = {
		TEXT("/ShooterCore/Weapons/Pistol/ID_Pistol.ID_Pistol_C"),
		TEXT("/ShooterCore/Weapons/Rifle/ID_Rifle.ID_Rifle_C"),
		TEXT("/ShooterCore/Weapons/Shotgun/ID_Shotgun.ID_Shotgun_C"),
	};

	constexpr float SETTLE_SECONDS = 3.f;			// 내용 확인 도구(MOBContentProbeSubsystem)와 같은 기준 — 생성 무적이 풀린 채 3초
	constexpr double WAIT_LIMIT_SECONDS = 120.0;
	constexpr double REAL_LIMIT_SECONDS = 300.0;

	// ① 자국 칸 — 무기를 쥐고 90프레임(1.5초, 꺼내기 몽타주가 끝나고 퍼짐이 가라앉을 때까지 — 제안값) 뒤 사진 한 장, 2프레임 뒤 한 프레임 누름.
	// 사진은 누른 프레임 기준 아래 프레임마다(사람이 본 「약 0.5초」 = 30프레임 앞뒤를 촘촘히). 무기마다 벽에서 요를 비켜 자국 무리가 안 겹치게.
	// fx2 판에서 90프레임째에도 자국이 아직 진해지는 중이라 240프레임(4초)까지 늘렸다
	constexpr int32 DECAL_EQUIP_WAIT = 90;
	constexpr int32 DECAL_PRESS_AT = DECAL_EQUIP_WAIT + 2;
	constexpr int32 DECAL_END_AT = DECAL_PRESS_AT + 250;
	const int32 DECAL_SHOT_OFFSETS[] = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 18, 21, 24, 27, 30, 36, 42, 48, 60, 75, 90, 105, 120, 150, 180, 240 };
	const double DECAL_YAW_OFFSET[3] = { 0.0, 7.0, -7.0 };
	constexpr double WALL_SIDE_DEG = 7.0;			// 벽 고르기 — 가운데 · 좌우 이 각도 세 줄이 모두 같은 벽에 닿아야 한다

	// ② 바꾸기 칸 — From 을 쥔 채 75프레임 뒤 To 로 바꾸고(같은 틱), K 프레임 뒤 한 프레임 누른다. K = -1 은 안 누름(바꾸기만으로 탄창이 줄지 않나 — 대조군),
	// From == To 는 바꾸지 않고 누름(이 칸에서 사격을 잡아내는지 — 대조군). 바꾼 뒤 50프레임 동안 To 탄창 · 내 몽타주를 본다
	struct FSwapTrial { int32 From; int32 To; int32 K; };
	const FSwapTrial SWAP_TRIALS[] = {
		{ 0, 0, 0 },
		{ 0, 1, -1 },
		{ 0, 1, 0 }, { 0, 1, 1 }, { 0, 1, 2 }, { 0, 1, 3 }, { 0, 1, 5 }, { 0, 1, 8 }, { 0, 1, 12 }, { 0, 1, 15 }, { 0, 1, 20 },
		{ 1, 2, 0 }, { 1, 2, 2 }, { 1, 2, 5 },
		{ 2, 0, 0 }, { 2, 0, 2 },
	};
	constexpr int32 SWAP_AT = 75;
	constexpr int32 SWAP_WATCH = 50;

	UAbilitySystemComponent* FindASC(const AActor* A)
	{
		return A ? UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A) : nullptr;
	}
}

bool UMOBFxProbeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* W = Cast<UWorld>(Outer);
	if (!W || !W->IsGameWorld()) return false;
	FString T;
	return FParse::Value(FCommandLine::Get(), TEXT("MOBFxProbe="), T);
}

void UMOBFxProbeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	FParse::Value(FCommandLine::Get(), TEXT("MOBFxProbe="), Tag);
	StartRealAt = FPlatformTime::Seconds();
	GAreScreenMessagesEnabled = false;
	UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_INIT world=%s tag=%s"), *GetNameSafe(GetWorld()), *Tag);
}

TStatId UMOBFxProbeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UMOBFxProbeSubsystem, STATGROUP_Tickables);
}

ACharacter* UMOBFxProbeSubsystem::Me() const
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
}

ULyraQuickBarComponent* UMOBFxProbeSubsystem::Bar() const
{
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	return PC ? PC->FindComponentByClass<ULyraQuickBarComponent>() : nullptr;
}

void UMOBFxProbeSubsystem::Tick(float DeltaTime)
{
	if (bDone) return;
	if (FPlatformTime::Seconds() - StartRealAt > REAL_LIMIT_SECONDS)
	{
		UE_LOG(LogMOBFx, Error, TEXT("MOB_FX_FAIL realLimit decal=%d swap=%d"), DecalWeapon, SwapTrial);
		Finish(false);
		return;
	}
	if (!bReady)
	{
		if (!WaitReady(DeltaTime))
		{
			WaitSeconds += DeltaTime;
			if (WaitSeconds > WAIT_LIMIT_SECONDS)
			{
				UE_LOG(LogMOBFx, Error, TEXT("MOB_FX_FAIL notReady"));
				Finish(false);
			}
			return;
		}
		if (!Setup())
		{
			Finish(false);
			return;
		}
		bReady = true;
	}
	// 도중에 내 캐릭터가 바뀌면(경기 시작 초기화 · 죽음) 탄창 · 몽타주 값이 다른 캐릭터 것이다 — 멈춘다
	if (Me() != ClearPawn.Get())
	{
		UE_LOG(LogMOBFx, Error, TEXT("MOB_FX_FAIL pawnChanged decal=%d swap=%d"), DecalWeapon, SwapTrial);
		Finish(false);
		return;
	}
	if (DecalWeapon < 3) TickDecal();
	else if (SwapTrial < (int32)UE_ARRAY_COUNT(SWAP_TRIALS)) TickSwap();
	else
	{
		Finish(true);
		return;
	}
	++Frame;
}

bool UMOBFxProbeSubsystem::WaitReady(float DeltaTime)
{
	ACharacter* Ch = Me();
	const UAbilitySystemComponent* ASC = FindASC(Ch);
	static const FGameplayTag Immune = FGameplayTag::RequestGameplayTag(TEXT("Gameplay.DamageImmunity"));
	if (!ASC || ASC->HasMatchingGameplayTag(Immune) || Ch != ClearPawn.Get() || !Bar())
	{
		ClearPawn = Ch;
		ClearSeconds = 0.f;
		return false;
	}
	ClearSeconds += DeltaTime;
	return ClearSeconds > SETTLE_SECONDS;
}

bool UMOBFxProbeSubsystem::Setup()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	ULyraQuickBarComponent* B = Bar();
	ULyraInventoryManagerComponent* Inv = PC ? PC->FindComponentByClass<ULyraInventoryManagerComponent>() : nullptr;
	if (!B || !Inv) return false;
	// 빠른 칸에 세 무기 — 다리가 내 판에서 넣는 것과 같은 길(MOBridgeSubsystem SyncSelfWeapon: 없으면 아이템을 만들어 빈 칸에)
	for (int32 W = 0; W < 3; ++W)
	{
		UClass* Def = LoadClass<ULyraInventoryItemDefinition>(nullptr, ITEM_DEFS[W]);
		ItemDefs.Add(Def);
		if (!Def)
		{
			UE_LOG(LogMOBFx, Error, TEXT("MOB_FX_FAIL itemDef=%s"), ITEM_DEFS[W]);
			return false;
		}
		if (SlotOf(W) != INDEX_NONE) continue;
		const int32 Free = B->GetNextFreeItemSlot();
		if (Free == INDEX_NONE) return false;
		if (ULyraInventoryItemInstance* Item = Inv->AddItemDefinition(Def, 1)) B->AddItemToSlot(Free, Item);
	}
	for (int32 W = 0; W < 3; ++W)
	{
		UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_SLOT weapon=%s slot=%d mag=%d"), WEAPON_NAMES[W], SlotOf(W), MagOf(W));
		if (SlotOf(W) == INDEX_NONE) return false;
	}
	if (!PickWall()) return false;
	UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_READY pawn=%s at=%s wallYaw=%.1f wallDist=%.0f"), *GetNameSafe(Me()), *Me()->GetActorLocation().ToString(), WallYaw, WallDist);
	return true;
}

// 눈높이에서 수평으로 10도마다 쏴 보고, 가운데 · 좌우 7도 세 줄이 모두 서 있는 벽(법선이 거의 수평)에 비슷한 거리로 닿는 방향 중 450cm 에 가장 가까운 것을 고른다.
// fx2 판은 800cm 기준이라 카메라에서 약 11m — 자국이 몇 화소뿐이라 진하기를 재기 어려워 당겼다
bool UMOBFxProbeSubsystem::PickWall()
{
	ACharacter* Ch = Me();
	if (!Ch) return false;
	const FVector Eye = Ch->GetPawnViewLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(MOBFxWall), true, Ch);
	const auto Cast1 = [&](double Yaw, FHitResult& Hit)
	{
		const FVector Dir = FRotator(0.0, Yaw, 0.0).Vector();
		return GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Dir * 3000.0, Lyra_TraceChannel_Weapon, Q)
			&& FMath::Abs(Hit.ImpactNormal.Z) < 0.25 && !Cast<APawn>(Hit.GetActor());
	};
	double BestScore = 1e9;
	int32 Candidates = 0;
	for (int32 i = 0; i < 36; ++i)
	{
		const double Yaw = i * 10.0;
		FHitResult H0, HL, HR;
		if (!Cast1(Yaw, H0) || !Cast1(Yaw - WALL_SIDE_DEG, HL) || !Cast1(Yaw + WALL_SIDE_DEG, HR)) continue;
		const double D = H0.Distance;
		if (D < 250.0 || D > 1800.0) continue;
		if (FMath::Abs(HL.Distance - D) > D * 0.25 || FMath::Abs(HR.Distance - D) > D * 0.25) continue;
		++Candidates;
		const double Score = FMath::Abs(D - 450.0);
		if (Score < BestScore)
		{
			BestScore = Score;
			WallYaw = Yaw;
			WallDist = D;
		}
	}
	UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_WALL candidates=%d yaw=%.1f dist=%.0f eye=%s"), Candidates, WallYaw, WallDist, *Eye.ToString());
	return Candidates > 0;
}

void UMOBFxProbeSubsystem::Face(double YawOffset) const
{
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController()) PC->SetControlRotation(FRotator(0.0, WallYaw + YawOffset, 0.0));
}

int32 UMOBFxProbeSubsystem::SlotOf(int32 Weapon) const
{
	const ULyraQuickBarComponent* B = Bar();
	if (!B || !ItemDefs.IsValidIndex(Weapon) || !ItemDefs[Weapon]) return INDEX_NONE;
	const TArray<ULyraInventoryItemInstance*> Slots = B->GetSlots();
	for (int32 S = 0; S < Slots.Num(); ++S)
	{
		if (Slots[S] && Slots[S]->GetItemDef() == ItemDefs[Weapon]) return S;
	}
	return INDEX_NONE;
}

int32 UMOBFxProbeSubsystem::MagOf(int32 Weapon) const
{
	static const FGameplayTag MagTag = FGameplayTag::RequestGameplayTag(TEXT("Lyra.ShooterGame.Weapon.MagazineAmmo"));
	const ULyraQuickBarComponent* B = Bar();
	const int32 S = SlotOf(Weapon);
	if (!B || S == INDEX_NONE) return -1;
	const ULyraInventoryItemInstance* Item = B->GetSlots()[S];
	return Item ? Item->GetStatTagStackCount(MagTag) : -1;
}

FString UMOBFxProbeSubsystem::MyMontage() const
{
	const ACharacter* Ch = Me();
	const UAnimInstance* A = Ch && Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
	const UAnimMontage* M = A ? A->GetCurrentActiveMontage() : nullptr;
	return M ? M->GetName() : FString(TEXT("-"));
}

// Lyra 사격 키와 같은 길 — 입력 표식을 능력 시스템에 누른다(내용 확인 도구 PressTag 와 같다).
// 권총 능력은 Fire, 소총 · 샷건 능력은 FireAuto 에 묶여 있어 둘 다 누른다(MOBridgeSubsystem.cpp 사격 표식 · 애니 대조 도구와 같은 두 표식). fx1 판은 Fire 만 눌러 소총 · 샷건이 안 나갔다
void UMOBFxProbeSubsystem::PressFire(bool bPress) const
{
	ULyraAbilitySystemComponent* A = Cast<ULyraAbilitySystemComponent>(FindASC(Me()));
	if (!A) return;
	static const FGameplayTag Fire = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.Fire"));
	static const FGameplayTag FireAuto = FGameplayTag::RequestGameplayTag(TEXT("InputTag.Weapon.FireAuto"));
	if (bPress)
	{
		A->AbilityInputTagPressed(Fire);
		A->AbilityInputTagPressed(FireAuto);
	}
	else
	{
		A->AbilityInputTagReleased(Fire);
		A->AbilityInputTagReleased(FireAuto);
	}
}

// 화면 그대로(UI 빼고) 한 장 — 한 프레임에 한 장만 요청한다(요청이 겹치면 앞의 것이 덮인다)
void UMOBFxProbeSubsystem::Shot(const FString& Name)
{
	const FString Path = FPaths::ProjectSavedDir() / TEXT("MOB") / FString::Printf(TEXT("fx_%s_%s.png"), *Tag, *Name);
	FScreenshotRequest::RequestScreenshot(Path, false, false);
	UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_SHOT frame=%d name=%s path=%s"), Frame, *Name, *Path);
}

void UMOBFxProbeSubsystem::TickDecal()
{
	const int32 W = DecalWeapon;
	const int32 F = DecalFrame;
	if (F == 0)
	{
		if (ULyraQuickBarComponent* B = Bar()) B->SetActiveSlotIndex(SlotOf(W));
		Face(DECAL_YAW_OFFSET[W]);
		DecalPressFrame = -1;
		DecalFiredAt = -1;
		DecalShotIdx = 0;
	}
	else if (F == DECAL_EQUIP_WAIT)
	{
		// 맞을 자리 — 카메라에서 바라보는 방향 그대로(Lyra 사격 기준점이 카메라 쪽이라 퍼짐 없는 탄의 끝) · 화면 자리
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const APlayerCameraManager* Cam = PC ? PC->PlayerCameraManager : nullptr;
		if (Cam)
		{
			const FVector From = Cam->GetCameraLocation();
			const FVector To = From + Cam->GetCameraRotation().Vector() * 5000.0;
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(MOBFxAim), true, Me());
			Q.bReturnPhysicalMaterial = true;
			const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, From, To, Lyra_TraceChannel_Weapon, Q);
			FVector2D Screen(-1, -1);
			if (bHit) PC->ProjectWorldLocationToScreen(Hit.ImpactPoint, Screen);
			int32 VX = 0, VY = 0;
			PC->GetViewportSize(VX, VY);
			UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_AIM weapon=%s hit=%d dist=%.0f point=%s normal=%s actor=%s physMat=%s screen=%.1f,%.1f viewport=%d,%d"),
				WEAPON_NAMES[W], bHit ? 1 : 0, Hit.Distance, *Hit.ImpactPoint.ToString(), *Hit.ImpactNormal.ToString(), *GetNameSafe(Hit.GetActor()),
				*GetNameSafe(Hit.PhysMaterial.Get()), Screen.X, Screen.Y, VX, VY);
		}
		Shot(FString::Printf(TEXT("%s_before"), WEAPON_NAMES[W]));
	}
	else if (F == DECAL_PRESS_AT)
	{
		DecalMagBefore = MagOf(W);
		DecalPressFrame = Frame;
		PressFire(true);
	}
	else if (F == DECAL_PRESS_AT + 1)
	{
		PressFire(false);
	}
	if (F > DECAL_PRESS_AT)
	{
		if (DecalFiredAt < 0 && MagOf(W) < DecalMagBefore) DecalFiredAt = Frame - DecalPressFrame;
		if (DecalShotIdx < (int32)UE_ARRAY_COUNT(DECAL_SHOT_OFFSETS) && F == DECAL_PRESS_AT + DECAL_SHOT_OFFSETS[DecalShotIdx])
		{
			Shot(FString::Printf(TEXT("%s_%03d"), WEAPON_NAMES[W], DECAL_SHOT_OFFSETS[DecalShotIdx]));
			++DecalShotIdx;
		}
	}
	if (F == DECAL_END_AT)
	{
		// fired = 누른 프레임에서 탄창이 줄어든 것을 본 프레임까지(이 틱의 읽기 기준). 사진 offset 도 같은 누른 프레임 기준이다
		UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_DECAL weapon=%s pressFrame=%d fired=%d magBefore=%d magAfter=%d shots=%d"),
			WEAPON_NAMES[W], DecalPressFrame, DecalFiredAt, DecalMagBefore, MagOf(W), DecalShotIdx);
		++DecalWeapon;
		DecalFrame = 0;
		return;
	}
	++DecalFrame;
}

void UMOBFxProbeSubsystem::TickSwap()
{
	const FSwapTrial& T = SWAP_TRIALS[SwapTrial];
	const int32 F = SwapFrame;
	ULyraQuickBarComponent* B = Bar();
	if (!B) return;
	if (F == 0)
	{
		B->SetActiveSlotIndex(SlotOf(T.From));
		Face(0.0);
		SwapFiredAt = -1;
		SwapFireActiveFrames = 0;
		SwapMontages.Reset();
		bSwapStarted = false;
	}
	else if (F == SWAP_AT)
	{
		SwapMagBefore = MagOf(T.To);
		if (T.To != T.From) B->SetActiveSlotIndex(SlotOf(T.To));
		bSwapStarted = true;
		SwapLastMontage = MyMontage();
		SwapMontages = FString::Printf(TEXT("0:%s"), *SwapLastMontage);
	}
	if (T.K >= 0 && F == SWAP_AT + T.K) PressFire(true);
	if (T.K >= 0 && F == SWAP_AT + T.K + 1) PressFire(false);
	if (bSwapStarted && F > SWAP_AT)
	{
		const int32 Off = F - SWAP_AT;
		if (SwapFiredAt < 0 && MagOf(T.To) < SwapMagBefore) SwapFiredAt = Off;
		const FString Now = MyMontage();
		if (Now != SwapLastMontage)
		{
			SwapMontages += FString::Printf(TEXT(" %d:%s"), Off, *Now);
			SwapLastMontage = Now;
		}
		const UAbilitySystemComponent* ASC = FindASC(Me());
		if (ASC)
		{
			for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
			{
				if (Spec.Ability && Spec.IsActive() && Spec.Ability->GetClass()->GetName().Contains(TEXT("GA_Weapon_Fire"))) { ++SwapFireActiveFrames; break; }
			}
		}
	}
	if (F == SWAP_AT + SWAP_WATCH)
	{
		// fired = 바꾼 프레임(0)에서 To 탄창이 줄어든 것을 본 프레임까지 · press = 누른 프레임(같은 기준, -1 = 안 누름)
		UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_SWAP from=%s to=%s press=%d fired=%d magBefore=%d magAfter=%d fireActiveFrames=%d montages=[%s]"),
			WEAPON_NAMES[T.From], WEAPON_NAMES[T.To], T.K, SwapFiredAt, SwapMagBefore, MagOf(T.To), SwapFireActiveFrames, *SwapMontages);
		++SwapTrial;
		SwapFrame = 0;
		return;
	}
	++SwapFrame;
}

void UMOBFxProbeSubsystem::Finish(bool bOk)
{
	bDone = true;
	UE_LOG(LogMOBFx, Log, TEXT("MOB_FX_DONE ok=%d frames=%d decal=%d swap=%d realSec=%.1f"), bOk ? 1 : 0, Frame, DecalWeapon, SwapTrial, FPlatformTime::Seconds() - StartRealAt);
	FPlatformMisc::RequestExit(false, TEXT("MOBFxProbe"));
}
