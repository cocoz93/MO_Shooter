#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBRemoteShotSubsystem.generated.h"

class ACameraActor;
class ACharacter;
class APawn;
class FArchive;
class FFrameGrabber;

/**
 * 상대 몸 촬영 — 서버가 움직이는 상대 한 명을 애니 비교 촬영과 같은 카메라로 따라 찍는다.
 * 합격 영상(애니 비교 촬영의 단순 재생)과 나란히 놓아, 서버 구동 몸이 같은 동작에서 같게 보이는지 사람이 본다.
 * 명령줄 -MOBRemoteShot=<tag> 일 때만 켜진다. -MOBServer · -MOBServerAnim · -MOBStandAt 과 BotClient --script=startstop 을 같이 쓴다.
 * 애니 비교 촬영은 시간을 고정해 돌리지만, 여기는 네트워크가 실제 시간으로 돌므로 프레임마다 찍은 시각을 남긴다.
 * 산출물 = Saved/MOB/rec_remoteshot_<tag>.bgra(원시 영상) · remoteshot_<tag>.csv(프레임별 시각 · 자리). 실행 = Tools/LyraPilot/record_remote_shot.ps1
 */
UCLASS()
class UMOBRemoteShotSubsystem : public UTickableWorldSubsystem
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
	enum class EStage : uint8 { WaitTarget, Capture, Drain, Done };

	ACharacter* FindTarget() const;
	bool Prepare(ACharacter* Found);
	void AttachCamera(ACharacter* Found);
	static void HideMe(APawn* Me);
	void KeepView();
	void DrainFrames();
	void ReleaseGrabber();
	void SaveCsv() const;
	void Finish(bool bOk);

	FString Tag;
	EStage Stage = EStage::WaitTarget;
	double WaitSeconds = 0;
	double HomeSeconds = 0;
	double StartedAt = 0;
	double NextGrabAt = 0;
	double MovedAt = 0;

	TWeakObjectPtr<ACharacter> Target;
	TWeakObjectPtr<ACameraActor> Camera;
	TWeakObjectPtr<APawn> MyPawn;	// 숨겨 둔 내 캐릭터 — 바뀌면 새 것도 숨긴다
	int32 ViewFixes = 0;
	int32 TargetChanges = 0;

	FFrameGrabber* Grabber = nullptr;	// 만드는 곳 = Prepare, 지우는 곳 = ReleaseGrabber
	FArchive* RawWriter = nullptr;
	FIntPoint GrabSize = FIntPoint::ZeroValue;
	int32 Requested = 0;
	int32 Written = 0;
	FString Csv;
};
