#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBWeaponRecordSubsystem.generated.h"

class ACharacter;
class APawn;
class UAbilitySystemComponent;
class ULyraAbilitySystemComponent;
class ULyraInventoryItemInstance;
class ULyraRangedWeaponInstance;
class UPhysicalMaterial;
class UGameplayEffect;

/**
 * 조각 1-d — 무기의 실행값과 실제 사격 결과를 적는다. Shared/Weapon 의 정답지다. Tools/WeaponCheck 가 읽는다.
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함). 봇 둘이 필요하다(?NumBots=2). 적 하나 + 같은 팀 하나.
 *   -MOBWeaponRecord=<이름>  결과 파일 Saved/MOB/weaponrec_<이름>.csv
 *   -MOBWeaponSeed=<수>      각본 난수 시드(기본 20261102)
 *   -MOBWeaponSegs=<수>      무기당 각본 구간 수(기본 4)
 *   -MOBWeaponSegSec=<초>    각본 구간 길이(기본 30)
 *   -MOBWeaponRaySec=<초>    몸 판정 상태당 시간(기본 20)
 *
 * 차례: 표 + 피해 표본(TABLE·DMG) → 각본 사격(FRAME) → 실제 맞힘(HIT·HPDELTA) → 몸 모양(SHAPE) + 광선(RAY·BOT).
 * 판정에 쓰는 줄은 「MOB_WREC」 로 시작한다. 파일 형식은 Tools/WeaponCheck 머리 주석이 정본이다.
 */
UCLASS()
class UMOBWeaponRecordSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	enum class EPhase : uint8 { WaitPawns, Equip, Table, ScriptStart, Script, HitStart, Hit, RayStart, Ray, PoseEquip, Done };

	bool IsMatchPlaying(double Real);
	bool PrepareActors();
	bool AddWeapons();
	void Equip(int32 WeaponIndex);
	ULyraRangedWeaponInstance* CurrentWeapon() const;
	ULyraInventoryItemInstance* CurrentItem() const;
	float ReadHeat(ULyraRangedWeaponInstance* W) const;
	int32 ReadStack(const TCHAR* TagName) const;
	void RefillAmmo();

	void WriteTable(int32 WeaponIndex);
	void WriteCurve(int32 WeaponIndex, ULyraRangedWeaponInstance* W, const TCHAR* PropName);
	void WriteDamageSamples(int32 WeaponIndex);
	float ApplyDamageSample(int32 WeaponIndex, APawn* Target, double Distance, UPhysicalMaterial* PM);

	void TickScript(float Dt);
	void TickHit(float Dt);
	void WriteShapes(const TCHAR* Prefix, bool bLog = true);
	void PreparePose();
	void EquipFoe(int32 WeaponIndex);
	int32 StateTag() const { return bPoseOnly ? PoseWeapon * 10 + RayState : RayState; }
	void TickRays(float Dt);
	void Finish();

	void LogAbilityState(const TCHAR* Where);
	void OnAbilityFailed(const class UGameplayAbility* Ability, const struct FGameplayTagContainer& Tags);
	void OnAbilityActivated(class UGameplayAbility* Ability);
	void OnAbilityEnded(class UGameplayAbility* Ability);
	void SetFire(bool bHeld);
	void PulseReload();
	void SetHugeHealth(APawn* P);
	float ReadHealth(APawn* P) const;
	void OnEnemyEffect(UAbilitySystemComponent* Source, const struct FGameplayEffectSpec& Spec, struct FActiveGameplayEffectHandle Handle);

	FString Tag;
	int32 Seed = 20261102;
	int32 SegsPerWeapon = 4;
	float SegSeconds = 30.f;
	float RaySeconds = 20.f;
	FRandomStream Rng;

	EPhase Phase = EPhase::WaitPawns;
	double PhaseAt = 0;
	double StartedAt = -1;
	double ClearSince = 0;
	TWeakObjectPtr<APawn> ClearPawn;
	int32 WeaponIdx = 0;
	int32 SegIdx = 0;
	int32 Frame = 0;

	TWeakObjectPtr<ACharacter> Mine;
	TWeakObjectPtr<ACharacter> Enemy;
	TWeakObjectPtr<ACharacter> Ally;
	TArray<TWeakObjectPtr<ULyraInventoryItemInstance>> Items;
	TObjectPtr<UPhysicalMaterial> PhysBody;
	TObjectPtr<UPhysicalMaterial> PhysWeak;
	TArray<TSubclassOf<UGameplayEffect>> DamageEffects;
	TArray<UClass*> FireAbilities;

	bool bFireHeld = false;
	bool bReloadPulse = false;
	double NextActionAt = 0;
	FVector MoveDir = FVector::ZeroVector;
	bool bJumpWanted = false;
	bool bReloadWanted = false;

	float LastEnemyHealth = 0.f;
	FDelegateHandle EffectHandle;
	int32 HitShots = 0;
	double NextShotAt = 0;
	int32 HitStage = 0;
	FVector HitDir = FVector::ForwardVector;
	FVector MineHome = FVector::ZeroVector;

	// -MOBPoseOnly — 상대 무기 × 동작마다 몸 자리·광선만 기록한다(동작별 자세 표를 넣을지 정하는 자료). 상태 번호 = 무기*10 + 동작
	bool bPoseOnly = false;
	int32 PoseWeapon = 0;
	float PoseYaw = 0.f;
	int32 RayState = 0;
	FVector EnemyHome = FVector::ZeroVector;
	double NextEnemyActionAt = 0;
	FVector EnemyMoveDir = FVector::ZeroVector;
	int32 RayCount = 0;

	FString Csv;
};
