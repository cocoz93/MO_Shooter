#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBBodyShotSubsystem.generated.h"

class ACameraActor;
class ACharacter;
class APawn;
class FFrameGrabber;
class UPhysicalMaterial;

/**
 * 몸 비교 사진 — Lyra 기본 몸 둘(Manny · Quinn)을 나란히 세워 앞 · 옆을 찍고, 판정 모양을 겹친 사진도 같이 찍는다. 서버 없이 돈다.
 * 상대 몸을 하나로 통일하기 전에 두 몸이 어떻게 다른지 눈으로 보려고 넣었다(design-decisions.md D33).
 * 명령줄 -MOBBodyShot=<tag> 일 때만 켜진다. 맵 주소에 NumBots=2 가 있어야 한다(봇 둘의 몸을 빌린다).
 * 산출물 = Saved/MOB/bodyshot_<tag>_<n>_<이름>.bgra(한 장씩 원시 그림, 바깥에서 변환) · 로그의 MOB_BODYSHOT 줄(키 · 머리 판정 크기 · 어깨 폭)
 */
UCLASS()
class UMOBBodyShotSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	enum class EStage : uint8 { WaitActors, Pose, Grab, Done };

	bool IsMatchPlaying(float DeltaTime);
	bool Prepare(bool& bFatal);
	bool SetBody(ACharacter* Foe, const TCHAR* PartClassPath) const;
	static void HideMe(APawn* Me);
	bool KeepView();
	void PlaceFoes(bool bTeleport);
	void DrawShapes() const;
	void LogBodies() const;
	bool TakeFrame();
	void Finish(bool bOk);

	FString Tag;
	EStage Stage = EStage::WaitActors;
	double WaitSeconds = 0;
	TWeakObjectPtr<APawn> ClearPawn;
	float ClearSeconds = 0.f;
	float PoseLeft = 0.f;
	int32 Shot = 0;
	int32 GrabWait = 0;
	int32 Saved = 0;
	bool bLogged = false;

	TWeakObjectPtr<ACharacter> Foes[2];
	TWeakObjectPtr<ACameraActor> Camera;
	TWeakObjectPtr<APawn> MyPawn;	// 숨겨 둔 내 캐릭터 — 바뀌면 새 것도 숨긴다
	int32 ViewFixes = 0;
	UPROPERTY()
	TObjectPtr<UPhysicalMaterial> PhysWeak;

	FFrameGrabber* Grabber = nullptr;	// 만드는 곳 = Prepare, 지우는 곳 = Finish · Deinitialize
	FIntPoint GrabSize = FIntPoint::ZeroValue;
};
