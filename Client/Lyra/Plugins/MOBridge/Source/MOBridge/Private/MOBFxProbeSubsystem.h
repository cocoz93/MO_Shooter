#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBFxProbeSubsystem.generated.h"

class ACharacter;
class ULyraInventoryItemInstance;
class ULyraQuickBarComponent;

/**
 * 원본 확인 — 서버 없이 Lyra 혼자 돌려 사람 판(11-18)에서 나온 물음 둘을 Lyra 원본 동작으로 찍는다.
 *   ① 내 총알 벽 자국이 쏜 뒤 언제 생겨 얼마 동안 진해지나 — 무기마다 한 발, 누른 프레임 기준으로 정해 둔 프레임마다 사진
 *   ② 무기를 바꾼 뒤 몇 프레임째 누른 사격부터 실제로 나가나 — 새 무기 탄창 수가 줄어든 프레임 · 내 메시 몽타주 이름 바뀜
 * 명령줄 -MOBFxProbe=<tag> 일 때만 켜진다(다리 -MOBServer 와 같이 쓰지 않는다 — 다리가 꺼져 있어야 사격 막기 장치도 안 걸린다).
 * 맵 = L_Expanse?NumBots=0(사람 판과 같은 벽). 산출물 = Saved/MOB/fx_<tag>_*.png · 로그 MOB_FX_* 줄. 띄우기 = Tools/LyraPilot/record_fx_probe.ps1
 */
UCLASS()
class UMOBFxProbeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	ACharacter* Me() const;
	ULyraQuickBarComponent* Bar() const;
	bool WaitReady(float DeltaTime);
	bool Setup();
	bool PickWall();
	void Face(double YawOffset) const;
	int32 SlotOf(int32 Weapon) const;
	int32 MagOf(int32 Weapon) const;
	FString MyMontage() const;
	void PressFire(bool bPress) const;
	void Shot(const FString& Name);
	void TickDecal();
	void TickSwap();
	void Finish(bool bOk);

	FString Tag;
	bool bDone = false;
	bool bReady = false;
	TWeakObjectPtr<APawn> ClearPawn;
	float ClearSeconds = 0.f;
	double WaitSeconds = 0;
	double StartRealAt = 0;
	int32 Frame = 0;

	TArray<UClass*> ItemDefs;	// 권총 · 소총 · 샷건 ID_*
	double WallYaw = 0;
	double WallDist = 0;

	// ① 자국 칸 — 무기 하나씩
	int32 DecalWeapon = 0;
	int32 DecalFrame = 0;
	int32 DecalPressFrame = -1;
	int32 DecalMagBefore = -1;
	int32 DecalFiredAt = -1;
	int32 DecalShotIdx = 0;
	FString DecalLog;

	// ② 바꾸기 칸 — 시도 하나씩
	int32 SwapTrial = 0;
	int32 SwapFrame = 0;
	int32 SwapMagBefore = -1;
	int32 SwapFiredAt = -1;
	FString SwapLastMontage;
	FString SwapMontages;
	int32 SwapFireActiveFrames = 0;
	bool bSwapStarted = false;
};
