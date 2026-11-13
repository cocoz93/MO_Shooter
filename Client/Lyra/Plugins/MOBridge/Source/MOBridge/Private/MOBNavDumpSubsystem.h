#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBNavDumpSubsystem.generated.h"

/**
 * 맵의 길찾기 자료를 파일로 뽑는다 — 언리얼 없는 봇이 같은 바닥 위에서 경로를 찾게 하려는 것.
 * 언리얼이 들고 있는 바닥 다각형과 그 연결을 그대로 읽는다.
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBDumpNav=<출력 경로>   출력 파일. 같은 이름 + .pairs.csv 로 언리얼의 경로 탐색 결과, + .floor.csv 로 다각형 가운데와 충돌 바닥의 거리도 쓴다
 *   -MOBDumpDelay=<초>        내 캐릭터가 뜬 뒤 기다릴 시간(기본 15)
 *   -MOBNavPairs=<개수>       경로를 물어볼 쌍의 수. 무작위 쌍과 위아래로 겹친 쌍을 각각 이만큼(기본 300)
 * 판정에 쓰는 줄은 「MOB_NAV」 로 시작한다.
 *
 * 파일 형식(리틀 엔디언, 좌표 = 언리얼 월드 cm)
 *   머리말  char magic[4]="MONV" · u32 version=1 · u32 nodeCount · u32 linkCount · float boundsMin[3] · float boundsMax[3] · float agentRadius · float agentHeight
 *   노드    u32 kind(0 바닥 · 1 바닥 사이 연결선) · u32 area · float center[3] · u32 vertCount · float[3] × vertCount · u32 neighborCount · u32 neighbor × neighborCount
 */
UCLASS()
class UMOBNavDumpSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	void DumpAndQuit();

	FString OutPath;
	float DelaySec = 15.f;
	int32 PairCount = 300;
	double PawnReadyAt = -1;
	bool bDone = false;
};
