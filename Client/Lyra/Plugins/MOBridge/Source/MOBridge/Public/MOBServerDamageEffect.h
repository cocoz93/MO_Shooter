#pragma once

#include "GameplayEffect.h"
#include "MOBServerDamageEffect.generated.h"

/**
 * 서버가 정한 피해를 Lyra 체력 경로에 그대로 넣는 효과.
 *
 * Lyra 의 체력바·사망 신호는 효과 적용 뒤 처리(LyraHealthSet::PostGameplayEffectExecute)에서만 나간다.
 * 체력 값을 직접 바꾸면 신호가 안 나가므로(같은 파일 PostAttributeChange 엔 신호 없음) 이 효과를 거친다.
 * Lyra 피해 계산(LyraDamageExecution)은 붙이지 않는다 — 서버 값이 그대로 Damage 속성에 들어간다.
 * 에셋으로 만들면 에디터를 열어야 해서 C++ 로 둔다.
 */
UCLASS()
class UMOBServerDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UMOBServerDamageEffect(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
