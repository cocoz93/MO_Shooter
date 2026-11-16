#pragma once

#include "Camera/LyraCameraMode.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagContainer.h"
#include "UObject/UnrealType.h"

// Lyra 카메라 모드 스택을 기록 도구가 읽는다(사격 기록 · 컨텐츠 기록).
// - 왜 리플렉션: ULyraCameraComponent 와 GetBlendInfo 는 Lyra 모듈 밖으로 안 나간다
// - 사격이 읽는 조준 비율(LyraRangedWeaponInstance.cpp:196-203)은 스택 맨 끝(GetBlendInfo 의 Last) 모드가 조준 카메라면 그 비중, 아니면 0 이다.
//   맨 끝은 늘 비중 1 이라(LyraCameraMode.cpp:321) 값은 0 또는 1 로 뛴다(0-3 실행 기록)
namespace MOBLyraCamera
{
	struct FRead
	{
		int32 Count = 0;			// 스택에 든 모드 수
		bool bTopAds = false;		// 맨 앞(새로 들어온 쪽, 인덱스 0)이 조준 카메라인가
		float TopWeight = 1.f;		// 맨 앞 모드의 섞임 비중
		bool bBottomAds = false;	// 맨 끝(GetBlendInfo 가 보는 쪽)이 조준 카메라인가
		float BottomWeight = 1.f;
		float AimingAlpha = 0.f;	// 사격이 쓰는 값 = bBottomAds ? BottomWeight : 0
	};

	inline bool Read(const APawn* Pawn, FRead& Out)
	{
		Out = FRead();
		if (!Pawn) return false;
		const UActorComponent* CamComp = nullptr;
		for (const UActorComponent* C : Pawn->GetComponents())
		{
			if (C && C->GetClass()->GetName() == TEXT("LyraCameraComponent")) { CamComp = C; break; }
		}
		if (!CamComp) return false;
		const FObjectProperty* StackProp = CastField<FObjectProperty>(CamComp->GetClass()->FindPropertyByName(TEXT("CameraModeStack")));
		const UObject* Stack = StackProp ? StackProp->GetObjectPropertyValue_InContainer(CamComp) : nullptr;
		const FArrayProperty* ArrProp = Stack ? CastField<FArrayProperty>(Stack->GetClass()->FindPropertyByName(TEXT("CameraModeStack"))) : nullptr;
		const FObjectProperty* Inner = ArrProp ? CastField<FObjectProperty>(ArrProp->Inner) : nullptr;
		if (!Inner) return false;
		FScriptArrayHelper H(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Stack));
		Out.Count = H.Num();
		if (H.Num() == 0) return true;	// GetBlendInfo 도 비중 1 · 빈 표식을 준다
		static const FGameplayTag Steady = FGameplayTag::RequestGameplayTag(TEXT("Lyra.Weapon.SteadyAimingCamera"));
		const ULyraCameraMode* Top = Cast<ULyraCameraMode>(Inner->GetObjectPropertyValue(H.GetRawPtr(0)));
		const ULyraCameraMode* Bottom = Cast<ULyraCameraMode>(Inner->GetObjectPropertyValue(H.GetRawPtr(H.Num() - 1)));
		if (!Top || !Bottom) return false;
		Out.bTopAds = Top->GetCameraTypeTag().MatchesTagExact(Steady);
		Out.TopWeight = Top->GetBlendWeight();
		Out.bBottomAds = Bottom->GetCameraTypeTag().MatchesTagExact(Steady);
		Out.BottomWeight = Bottom->GetBlendWeight();
		Out.AimingAlpha = Out.bBottomAds ? Out.BottomWeight : 0.f;
		return true;
	}
}
