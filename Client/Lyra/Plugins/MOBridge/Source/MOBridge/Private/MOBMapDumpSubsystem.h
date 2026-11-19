#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBMapDumpSubsystem.generated.h"

/**
 * 맵 충돌을 파일로 뽑는다 — 서버가 언리얼 없이 같은 벽으로 판정하게 하려는 것.
 * 언리얼 물리가 실제로 들고 있는 모양을 읽으므로, 스크립트로 만들어진 벽도 같이 나온다.
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBDumpMap=<출력 경로>     출력 파일. 같은 이름 + .csv 로 물체별 목록도 쓴다
 *   -MOBDumpDelay=<초>          내 캐릭터가 뜬 뒤 기다릴 시간(기본 15)
 *   -MOBTraceRecord=<출력 경로> 같은 실행에서 광선·캡슐 결과도 기록한다(MOBTraceRecorder.h). -MOBTraceRays=<개수> · -MOBTraceSweeps=<개수>
 * 판정에 쓰는 줄은 「MOB_MAP」 으로 시작한다(Saved/Logs/Lyra.log).
 *
 * 파일 형식(리틀 엔디언, 좌표 = 언리얼 월드 cm)
 *   머리말  char magic[4]="MOCL" · u32 version=3 · u32 objectCount · u32 triangleCount · float boundsMin[3] · float boundsMax[3]
 *   물체    u32 flags · u32 kind · 그 뒤 kind 별 본문
 *     kind 0 삼각형  u32 triCount · float[9] × triCount
 *     kind 1 구      float center[3] · float radius
 *     kind 2 캡슐    float a[3] · float b[3] · float radius
 *   flags   1 = 이동을 막음 · 2 = 총알을 막음 · 4 = 움직일 수 있는 물체 · 8 = 뒷면으로 온 광선은 통과 · 16 = 속이 찬 볼록 물체(삼각형이 모두 바깥을 봄)
 *           · 32 = 수류탄 몸을 막음(단순 충돌) · 64 = 폭발 선을 막음(복잡 충돌) — 판 3 부터(판 2 파일은 서버가 읽지 않는다)
 */
UCLASS()
class UMOBMapDumpSubsystem : public UTickableWorldSubsystem
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
	double PawnReadyAt = -1;
	bool bDone = false;
};
