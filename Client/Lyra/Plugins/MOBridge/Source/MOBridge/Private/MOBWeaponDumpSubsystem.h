#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBWeaponDumpSubsystem.generated.h"

class APawn;

/**
 * 조각 1-d — 무기 수치의 실행값을 파일로 뽑는다. 블루프린트가 C++ 기본값을 덮으므로 소스가 아니라 실행값을 본다.
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBWeaponDump=<이름>   결과 파일 Saved/MOB/weapons_<이름>.txt
 *   -MOBWDGraph=<경로;경로>  더 뽑을 블루프린트 그래프(L-2-a) — 노드 안 그래프(상태 기계 · 층)까지 걷고 애님 노드 속성도 적는다
 *   -MOBWDLaunchers          맵의 점프대(B_Launcher_C 자식)마다 청사진 변수 · 칸 모양 · 월드 변환을 「MOB_LAUNCHER」 줄로(점프대 j-1)
 * 봇 하나가 있는 맵(?NumBots=1)으로 실행하면 캐릭터 충돌 몸체까지 적는다.
 * 판정에 쓰는 줄은 「MOB_WDUMP」 로 시작한다.
 */
UCLASS()
class UMOBWeaponDumpSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

private:
	void DumpAssets();
	void DumpObject(UObject* O, const FString& Indent, TSet<UObject*>& Seen);
	void DumpBlueprintGraphs(const FString& Path, bool bDeep = false);
	void DumpPawnBodies(APawn* Pawn, const TCHAR* Label);
	void DumpMatch();
	void DumpLaunchers();
	void Finish();

	FString Tag;
	FString Out;
	double StartAt = -1;
	bool bAssetsDone = false;
	bool bDone = false;
};
