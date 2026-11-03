#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBWeaponDumpSubsystem.generated.h"

class APawn;

/**
 * 조각 1-d — 무기 수치의 실행값을 파일로 뽑는다. 블루프린트가 C++ 기본값을 덮으므로 소스가 아니라 실행값을 본다.
 * 명령줄로만 켜진다(없으면 아무것도 안 함).
 *   -MOBWeaponDump=<이름>   결과 파일 Saved/MOB/weapons_<이름>.txt
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
	void DumpBlueprintGraphs(const FString& Path);
	void DumpPawnBodies(APawn* Pawn, const TCHAR* Label);
	void DumpMatch();
	void Finish();

	FString Tag;
	FString Out;
	double StartAt = -1;
	bool bAssetsDone = false;
	bool bDone = false;
};
