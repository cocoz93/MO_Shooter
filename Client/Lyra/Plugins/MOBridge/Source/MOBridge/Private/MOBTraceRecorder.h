#pragma once

#include "CoreMinimal.h"

class ACharacter;
class UWorld;

/**
 * 같은 광선·캡슐을 Lyra 물리로 쏴서 결과를 CSV 로 남긴다 — 서버 충돌 코드의 정답지.
 * - 왜: 서버 판정이 Lyra 와 같다는 근거는 실제 값 대조뿐이다. 맵을 뽑는 실행 안에서 함께 돌려 시점 차이를 없앤다.
 * 무기는 LyraGameplayAbility_RangedWeapon.cpp 의 WeaponTrace 와 같은 채널·복잡 충돌, 캡슐은 내 캐릭터 캡슐의 응답 설정을 쓴다.
 *
 * CSV 첫 칸 = R 공중 임의 광선 · E 눈높이 수평 광선 · D 내리꽂는 광선 · C 걷는 높이 수평 캡슐 · F 낙하 캡슐 · A 공중 임의 캡슐
 */
void MOBRecordTraces(UWorld* World, const ACharacter* Hero,const FBox& Bounds, const FString& OutPath, int32 RayCount, int32 SweepCount, double& OutRaySeconds, double& OutSweepSeconds);
