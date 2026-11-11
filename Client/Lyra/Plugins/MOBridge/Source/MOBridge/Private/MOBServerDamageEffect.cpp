#include "MOBServerDamageEffect.h"

#include "AbilitySystem/Attributes/LyraHealthSet.h"
#include "LyraGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MOBServerDamageEffect)

UMOBServerDamageEffect::UMOBServerDamageEffect(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// Damage 속성(체력에서 빠질 양) += 서버가 SetByCaller 로 넣은 값
	FSetByCallerFloat ByCaller;
	ByCaller.DataTag = LyraGameplayTags::SetByCaller_Damage;

	FGameplayModifierInfo Mod;
	Mod.Attribute = ULyraHealthSet::GetDamageAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(ByCaller);
	Modifiers.Add(Mod);
}
