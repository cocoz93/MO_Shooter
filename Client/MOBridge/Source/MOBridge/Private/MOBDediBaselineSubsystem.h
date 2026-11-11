#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "MOBDediBaselineSubsystem.generated.h"

class ACharacter;
class FArchive;
class UAnimInstance;
class UPhysicalMaterial;
class USkeletalMesh;
class UMOBAnimCompareSubsystem;

/**
 * 언리얼 서버-클라 기준값 기록 (candidate1-impl-plan.md 0단계).
 * 데디 서버와 클라가 같은 봇의 22부위를 같은 줄 형식으로 남긴다. 바깥의 check_dedi_baseline.py 가 위치로 짝지어 차이를 내고 오탈로 환산한다.
 * 명령줄 -MOBDediBaseline=<tag> 일 때만 켜진다.
 *   서버 = 비교 스크립트(-MOBAnimCompare)가 경로를 도는 동안만 기록한다. 데디 서버는 기본값이 「자세만 돌고 뼈를 안 고친다」라 뼈 갱신을 켠다(Character.cpp AlwaysTickPose)
 *   클라 = 봇이 보이는 동안 프레임마다 기록한다. 서버가 끝나 끊기면 저장하고 스스로 끝낸다
 * 산출물 = Saved/MOB/dedi_<tag>_server.csv · dedi_<tag>_client.csv
 *   FRAME,프레임,서버 시각,x,y,z,몸 방향,속도 xyz,판,조각,조각 안 시각
 *   POSEB,0,프레임,부위,종류,약점,중심 xyz,X축 xyz,Y축 xyz,Z축 xyz,크기 a,b,c  (MOBAnimCompareSubsystem::WriteShapes 와 같은 형식)
 * 자리와 방향은 화면에 그려진 몸(메시) 기준이다. 클라의 메시는 캡슐을 부드럽게 따라가므로 캡슐 자리를 쓰면 위치 차이가 자세 차이로 섞인다
 * 몸은 양쪽 다 Quinn 으로 맞춘다(ForceSameBody). 데디 서버는 화면용 몸 부품을 안 붙여 늘 기본 몸이고 클라는 부품이 무작위라, 그대로 두면 자세 차이에 몸 차이가 섞인다
 * 판정에 쓰는 줄은 전부 「MOB_DEDI」 로 시작한다.
 */
UCLASS()
class UMOBDediBaselineSubsystem : public UTickableWorldSubsystem
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
	ACharacter* FindClientFoe() const;
	void ForceSameBody(ACharacter* Target);
	void OpenWriter(const TCHAR* Side);
	void WriteFrame(ACharacter* Target, int32 Pass, const TCHAR* Segment, float SegmentTime);
	void LogMeshIfChanged(const ACharacter* Target, const TCHAR* Side);
	void LogTraffic(const TCHAR* When) const;

	FString Tag;
	FArchive* Writer = nullptr;		// 만드는 곳 = OpenWriter, 지우는 곳 = Deinitialize
	FString Buffer;
	TWeakObjectPtr<ACharacter> Foe;
	bool bHadFoe = false;
	int32 Frame = 0;
	int32 BoneRows = 0;
	FString LastMesh;
	double TrafficStartSeconds = -1.0;
	int32 TrafficStartBytes = 0;

	UPROPERTY()
	TObjectPtr<UPhysicalMaterial> PhysWeak;

	UPROPERTY()
	TObjectPtr<USkeletalMesh> BodyMesh;

	UPROPERTY()
	TSubclassOf<UAnimInstance> RifleLayer;
};
