#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MOBridgeSubsystem.generated.h"

class FSocket;
class FFrameGrabber;
class UAbilitySystemComponent;
class AAIController;
class ACharacter;
class APawn;
class APlayerState;
class ULyraEquipmentInstance;
struct FMOBCore;

namespace mo { namespace wire { struct PlayerState; struct Damage; struct Kill; struct Match; struct Shot; } }

// 남의 캐릭터 — 서버 상태를 틱 번호와 함께 쌓아 0.1초 뒤 시점을 보여 준다
struct FMOBSnapshot
{
	uint32 Tick = 0;
	FVector Pos = FVector::ZeroVector;
	FVector Vel = FVector::ZeroVector;
	float Yaw = 0, Pitch = 0;
	int8 MoveX = 0, MoveY = 0;
	bool bFalling = false;
};

struct FMOBRemote
{
	static constexpr int32 kSnapshots = 64;

	uint32 Id = 0;
	uint8 ServerTeam = 0;
	TWeakObjectPtr<AAIController> Ctrl;
	TWeakObjectPtr<ACharacter> Pawn;
	FMOBSnapshot Snaps[kSnapshots];
	uint32 SnapCount = 0;
	bool bSpawnFailed = false;
	bool bHasSet = false;
	FVector LastSet = FVector::ZeroVector;
	float LastSetYaw = 0;

	// 죽음 — 처치 패킷이 켜고, 상태 패킷이 살아났다고 하면 그 틱이 0.1초 뒤 시점에 닿을 때 새 캐릭터로 바꾼다
	bool bKilled = false;
	bool bRespawnPending = false;
	uint32 RespawnTick = 0;

	uint8 Weapon = 0;
	uint8 ShownWeapon = 0xFF;
	TWeakObjectPtr<ULyraEquipmentInstance> Equipped;
};

/**
 * Lyra 와 매치 서버를 잇는다 — 입력을 서버로 보내고 서버 결과를 받는다. 판정은 전부 서버 몫.
 * 와이어 형식 = MO_Shooter/Shared/GameProtocol.h(봇과 같은 한 벌).
 * 플랜 = MyGit/MyDocs/claude-plans/mo-shooter/authoritative-server-plan.md
 *
 * 명령줄로만 켜진다(없으면 아무것도 안 함 — 평소 Lyra 그대로).
 *   -MOBServer=127.0.0.1:7791   서버 접속(접속하면 막기 스위치 둘을 켠다 — 끄려면 -MOBBlockDamage=0)
 *   -MOBAutoInput               사람 없이 정해진 입력을 넣는다(걷기·점프·사격·재장전·무기 바꾸기)
 *   -MOBAutoAim                 자동 입력이 보이는 적 중 가장 가까운 적을 겨눠 쏜다(맞은 표시·내 처치를 보려고)
 *   -MOBKeepPickups             맵의 무기·체력 줍기 받침을 그대로 둔다(기본은 숨김 — 서버에 줍기가 아직 없다)
 *   -MOBBreakAt=N               준비된 뒤 N초에 Lyra 체력·팀 점수·탄을 일부러 틀어 비교 코드가 잡는지 본다
 *   -MOBEventShots              적이 보일 때·맞혔을 때·처치·내 죽음에 사진(Saved/MOB/<tag>_<사건>_<N>.png)
 *   -MOBShotAt=10,15            준비된 뒤 N초에 캡처(Saved/MOB/<tag>_<N>.png)
 *   -MOBRecord=start,dur,fps    프레임 연속 캡처(사람 확인 영상용)
 *   -MOBQuitAt=N                준비된 뒤 N초에 끝냄(서버 경기 결과가 먼저 오면 그쪽)
 *   -MOBTag=name                산출물 파일 이름 앞부분
 *   -MOBBreakPredictAt=N        일부러 틀리기 — 준비된 뒤 N초에 내 예측 위치를 1m 민다(보정이 잡는지 보려고)
 * 판정에 쓰는 줄은 전부 「MOB_」 로 시작한다.
 */
UCLASS()
class UMOBridgeSubsystem : public UTickableWorldSubsystem
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
	void ParseCommandLine();
	bool TryBecomeReady();
	void Connect();
	void PumpSocket();
	void FlushSend();
	void HandlePacket(const uint8* Data, int32 Size);
	void ApplySelfState(const mo::wire::PlayerState& P);
	void AddRemoteSnapshot(const mo::wire::PlayerState& P, uint32 ServerTick);
	void SpawnRemote(FMOBRemote& R);
	void TickSelfVisual(double Now, float DeltaTime);
	void TickRemotes(double Now);
	void MeasureRemoteAnim(ACharacter* Ch, uint32 Id, const FVector& Vel, int8 MoveX, int8 MoveY, double Now);
	void ApplyServerHp(uint32 TargetId, uint32 AttackerId, float HpAfter);
	void HandleKill(const mo::wire::Kill& M);
	void HandleShot(const mo::wire::Shot& M);
	void ApplyMatch(const mo::wire::Match& M);
	void SyncTeamScores();
	void SyncSelfWeapon(const mo::wire::PlayerState& P);
	void SyncRemoteWeapon(FMOBRemote& R, ACharacter* Ch);
	void CompareHp(const AActor* Actor, float ServerValue);
	void LoadWeaponClasses();
	void ForceCountDown(int32 Seconds, int32 TargetScore);
	int32 LyraTeamOf(uint8 ServerTeam) const;
	APawn* FindPlayerPawn(uint32 Id) const;
	APlayerState* FindPlayerState(uint32 Id) const;
	void QueuePacket(const void* Pkt, int32 Size);
	void TickInput(double Now);
	void TickAutoInput(double T);
	void TickCapture(double T);
	void TickPickups(double Now);
	void RequestEventShot(const TCHAR* Name, double Delay, int32 MaxCount);
	void TickRecord(double T);
	void DrainGrabber();
	void FinishAndQuit(const TCHAR* Why);

	UAbilitySystemComponent* FindASC(AActor* Actor) const;

	// 설정(명령줄)
	FString ServerAddr;
	TArray<float> ShotAt;
	float RecStart = -1, RecDur = 0, RecFps = 30;
	float QuitAt = -1;
	FString Tag = TEXT("mob");
	bool bBlockDamage = true;
	bool bAutoInput = false;
	bool bAutoAim = false;
	bool bEventShots = false;
	bool bKeepPickups = false;
	float BreakAt = -1;
	float BreakPredictAt = -1;

	// 상태
	double ReadyAt = -1;
	double ResultAt = -1;
	double NextInputAt = 0;
	double NextLogAt = 0;
	bool bQuitting = false;
	int32 ShotIndex = 0;

	FSocket* Socket = nullptr;
	TArray<uint8> RecvBuf;
	TArray<uint8> SendBuf;		// 소켓이 다 못 받은 나머지 — 패킷 중간이 잘리면 서버가 스트림을 잃는다

	uint32 PlayerId = 0;
	bool bWelcomed = false;
	bool bHasSpawn = false;
	uint8 Team = 0;

	uint32 Seq = 0;
	uint32 AckSeq = 0;
	FMOBCore* Core = nullptr;	// 맵·이동 계산·내 예측 — 만들고 지우는 곳 = Connect / Deinitialize
	uint8 Weapon = 0;
	bool bReloadWasDown = false;

	// 내 캐릭터 화면 — 예측은 60Hz 로 끊겨 있어 직전·지금 사이를 메우고, 보정으로 생긴 차이는 서서히 없앤다
	FVector SelfPrev = FVector::ZeroVector;
	FVector SelfCur = FVector::ZeroVector;
	FVector VisualError = FVector::ZeroVector;
	FVector SelfLastSet = FVector::ZeroVector;
	bool bSelfSet = false;
	FName LastPawnName;

	TMap<uint32, FMOBRemote> Remotes;
	double TickClockOffset = 0;		// 서버 틱 - 내 시계 * 60
	bool bHasTickClock = false;

	// 자동 입력이 고른 값 — 키보드 대신
	bool bAutoFire = false;
	bool bAutoSeesEnemy = false;
	bool bNamedSelf = false;
	double NextPickupCheckAt = 0;
	int32 PickupsHidden = 0;
	int32 HpDiffLinesLeft = 20;
	int64 HpFixes = 0;
	double InputRawMax = 0, InputSentMax = 0;
	int64 InputsDiagonal = 0;
	int64 BulletsLyra = 0, BulletsServer = 0, BulletsLyraImmune = 0, BulletsServerImmune = 0, BulletsLyraOther = 0, BulletsServerOther = 0;
	int32 LastSyncMag = -1;
	uint8 LastSyncWeapon = 0xFF;
	int32 AimFarLinesLeft = 8;
	int64 AimFarLocal = 0;
	double AimDistMax = 0, AimHorizMax = 0;
	bool bAutoReloadPressed = false;
	bool bJustReloaded = false;
	int32 MagDiffMax = 0;
	int64 MyShots[3] = { 0, 0, 0 };
	int64 ReloadStarts[3] = { 0, 0, 0 };
	int64 ReloadDones[3] = { 0, 0, 0 };
	bool bWasReloading = false;
	uint8 ReloadWeapon = 0;
	int32 ReloadMagBefore = 0;
	double NextSeeShotAt = 0;
	TMap<FString, int32> EventShotCounts;
	TArray<TPair<double, FString>> PendingEventShots;
	bool bAutoJump = false;
	bool bAutoReload = false;
	uint8 AutoWeapon = 0;

	// 관측값
	int64 InputsSent = 0, InputsSkipped = 0;
	int64 InputsFire = 0, InputsJump = 0, InputsReload = 0, InputsWeaponChange = 0;
	int64 StatesRecv = 0, StatesWithoutMe = 0, ShotsRecv = 0, DamagesRecv = 0, MatchesRecv = 0, KillsRecv = 0, SpawnsRecv = 0;
	int64 UnknownPackets = 0, SendShort = 0;
	int32 MaxAckLag = 0;
	double PosDiffSum = 0, PosDiffMax = 0, VelDiffMax = 0;
	int64 PosDiffCount = 0, PosDiffNonZero = 0;
	int64 Corrections = 0, VisualSnaps = 0, PawnChanges = 0;
	double CorrectionSum = 0, CorrectionMax = 0;
	double SelfDriftMax = 0;
	int64 SelfDriftOver1cm = 0;
	double MaxFrameGap = 0;
	int32 RemoteSpawned = 0, RemoteSpawnFail = 0;
	int64 RemoteFrames = 0, RemoteUnderrun = 0, RemoteTeleports = 0, RemoteDriftOver1cm = 0;
	// -MOBAnimLog 진단 — 남의 캐릭터가 움직이는 프레임에 애니가 무슨 상태였나
	bool bAnimLog = false;
	double NextAnimLogAt = 0;
	int64 AnimMoving = 0, AnimMovingRendered = 0, AnimMovingNoInput = 0, AnimMovingNoAccel = 0;
	TMap<FName, int64> AnimStates;
	double RemoteDriftMax = 0, RemoteYawDiffMax = 0, RemoteLagSum = 0, RemoteBehindSum = 0;
	float ServerHp = -1.f;
	int16 ServerMagazine = -1, ServerSpare = -1;
	uint8 ServerFlags = 0, ServerWeapon = 0, MatchPhase = 0;
	uint16 ScoreA = 0, ScoreB = 0;
	double LastServerPosX = 0, LastServerPosY = 0, LastServerPosZ = 0;
	int64 DamageApplied = 0, DamageMismatch = 0, DamageNoTarget = 0, HpCompared = 0, HpDiffCount = 0;
	double HpDiffMax = 0;
	int64 SelfRespawns = 0, RemoteRespawns = 0, ScoreFixes = 0, AmmoFixes = 0, WeaponSwaps = 0, RemoteEquips = 0, PhaseChanges = 0, ShotCues = 0, HitMarkers = 0, CountDownSets = 0;
	int32 LastCountDownShown = -1;
	bool bLoggedScoringMissing = false;
	bool bPostGamePrepared = false;
	int32 LyraScoreA = -1, LyraScoreB = -1;

	// Lyra 무기 — 인덱스는 mo::EWeapon 순서
	UPROPERTY()
	TArray<TObjectPtr<UClass>> WeaponItemDefs;
	UPROPERTY()
	TArray<TObjectPtr<UClass>> WeaponEquipDefs;
	uint8 HitMarkerId = 0;

	// 영상 — 엔진 비동기 프레임 수집기(시퀀서 녹화가 쓰는 것). 매 프레임 PNG 캡처는 초당 6~11프레임으로 게임을 끌어내렸다
	FFrameGrabber* Grabber = nullptr;	// 만들고 지우는 곳 = TickRecord
	FArchive* RawWriter = nullptr;
	int32 GrabbedFrames = 0;
	double GrabFirstAt = 0, GrabLastAt = 0;
	FIntPoint GrabSize = FIntPoint::ZeroValue;
	bool bGrabDone = false;
};
