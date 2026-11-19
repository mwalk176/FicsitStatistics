#include "FicsitStatisticsSubsystem.h"

#include "Buildables/FGBuildableFrackingExtractor.h"
#include "EngineUtils.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableGeneratorNuclear.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePipeReservoir.h"
#include "Buildables/FGBuildablePowerStorage.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGCentralStorageContainer.h"
#include "FGCircuitSubsystem.h"
#include "FicsitMilestoneNeeds.h"
#include "FicsitSupplyChain.h"
#include "FGPowerCircuit.h"
#include "FGInventoryComponent.h"
#include "FGPowerInfoComponent.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "FGBuildableSubsystem.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"
#include "FGRecipe.h"
#include "FicsitStatistics.h"
#include "FicsitStatisticsConfig.h"
#include "FicsitGameAccess.h"
#include "FicsitStatisticsHooks.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGItemDescriptorNuclearFuel.h"
#include "Resources/FGResourceDescriptor.h"
#include "TimerManager.h"
#include "Misc/ScopeExit.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

using FicsitUnits::ToDisplayAmount;
using FicsitUnits::ToDisplayRate;

AFicsitStatisticsSubsystem::AFicsitStatisticsSubsystem()
{
	// Stats are computed where the factory simulation runs (the server) and replicated to clients.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(1.f);
	LiveHistory.Layout = FFicsitHistory::ELayout::Minute;
}

void AFicsitStatisticsSubsystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AFicsitStatisticsSubsystem, NetSnapshot);
	DOREPLIFETIME(AFicsitStatisticsSubsystem, NetLive);
}

void AFicsitStatisticsSubsystem::OnRep_NetSnapshot()
{
	NetSnapshot.ToSnapshot(LatestSnapshot);
}

void AFicsitStatisticsSubsystem::OnRep_NetLive()
{
	for (const FFicsitLiveSecond& Live : NetLive.TakeNew(LastAppliedLiveSeq))
	{
		ApplyLiveSecond(Live);
	}
}

double AFicsitStatisticsSubsystem::GetWorldTime() const
{
	return GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
}

void AFicsitStatisticsSubsystem::UpdateRealtime()
{
	if (!HasAuthority())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	ON_SCOPE_EXIT
	{
		const double Ms = (FPlatformTime::Seconds() - Now) * 1000.0;
		++RealtimeUpdateCount;
		RealtimeUpdateMsTotal += Ms;
		RealtimeUpdateMsMax = FMath::Max(RealtimeUpdateMsMax, Ms);
	};
	FFicsitEventCounters::DrainEvents(RealtimeScratch);
	Realtime.Advance(RealtimeScratch, Now, LatestSnapshot);
	RealtimeScratch.Reset();
}

void AFicsitStatisticsSubsystem::TakeLiveSample()
{
	// The 5s history only matters while a view shows it (it costs about as much as the scan). Otherwise its events are
	// dropped, so the list stays short, and the next view fills the range in from the right over 5 s.
	if (bBenchmark || FPlatformTime::Seconds() - LastRealtimeViewTime < RealtimeViewTimeoutSeconds)
	{
		UpdateRealtime();
	}
	else
	{
		FFicsitEventCounters::DrainEvents(RealtimeScratch);
		RealtimeScratch.Reset();
	}

	const double Now = GetWorld()->GetTimeSeconds();
	// Real elapsed game time (the timer can run late), capped so a hitch doesn't stretch one point over many.
	const float Delta = LastLiveWorldTime >= 0.0 ? FMath::Clamp(static_cast<float>(Now - LastLiveWorldTime), 0.f, LiveIntervalSeconds * 3.f) : LiveIntervalSeconds;
	LastLiveWorldTime = Now;

	FFicsitCounts Counts;
	FFicsitEventCounters::DrainLive(Counts);
	if (Delta <= 0.f)
	{
		return;
	}

	FFicsitLiveSecond Live = FFicsitLiveSecond::FromCounts(Counts);
	Live.Seq = ++LiveSeq;
	Live.Seconds = Delta;
	ApplyLiveSecond(Live);
	if (bBenchmark)
	{
		WriteBenchmarkSecond(Live, Counts, Now);
	}
	NetLive.Push(Live);
}

void AFicsitStatisticsSubsystem::WriteBenchmarkSecond(const FFicsitLiveSecond& Live, const FFicsitCounts& Counts, double WorldTime)
{
	// One row per item per second. made / used: raw hook amounts in this second. rt_made: what the 5s path closed into
	// its points during the same second. smoothed_*: the "right now" rate. history_made: the rate the saved history (10m+) recorded for the latest sample.
	// window_s: the counting window behind the smoothed rate. cycle_s: the slowest cycle among the item's machines.
	const TMap<UClass*, float> BenchRealtimeMade = Realtime.TakeProducedTally();
	TSet<UClass*> Items;
	for (const auto& Pair : Live.Produced) { Items.Add(Pair.Key); }
	for (const auto& Pair : Live.Consumed) { Items.Add(Pair.Key); }
	for (const auto& Pair : BenchRealtimeMade) { Items.Add(Pair.Key); }
	for (const auto& Pair : LatestSnapshot.Produced) { Items.Add(Pair.Key.Get()); }
	for (const auto& Pair : LatestSnapshot.Consumed) { Items.Add(Pair.Key.Get()); }

	FString Rows;
	for (UClass* Item : Items)
	{
		const FFicsitItemFlow* Made = LatestSnapshot.Produced.Find(Item);
		const FFicsitItemFlow* Used = LatestSnapshot.Consumed.Find(Item);
		Rows += FString::Printf(TEXT("%.3f,%.3f,%s,%g,%g,%g,%g,%g,%g,%g,%d,%d,%g\n"), WorldTime, Live.Seconds, *GetNameSafe(Item),
			Live.Produced.FindRef(Item), Live.Consumed.FindRef(Item), BenchRealtimeMade.FindRef(Item),
			Made ? Made->Total.Actual : 0.f, Used ? Used->Total.Actual : 0.f,
			RateEstimator.GetWindowSeconds(Item), RateEstimator.GetCycleSeconds(Item), Counts.Events[0].FindRef(Item), Counts.Events[1].FindRef(Item), BenchHistoryMade.FindRef(Item));
	}
	FFileHelper::SaveStringToFile(Rows, *BenchmarkPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

void AFicsitStatisticsSubsystem::ApplyLiveSecond(const FFicsitLiveSecond& Live)
{
	if (Live.Seconds <= 0.f)
	{
		return;
	}
	FFicsitProductionSnapshot Sample;
	Live.ToSample(LatestSnapshot, GetWorldTime(), Sample);
	LiveHistory.AddSample(Sample, Live.Seconds);
}

void AFicsitStatisticsSubsystem::LogLive()
{
	// The counters start over every minute even when nothing is logged, so that turning on Verbose shows the last
	// minute rather than everything since the save loaded.
	const FFicsitRealtimeHistory::FStats RealtimeStats = Realtime.ConsumeStats();
	const FFicsitRateEstimator::FStats Smoothing = RateEstimator.ConsumeStats();
	const int32 UpdateCount = RealtimeUpdateCount;
	const double UpdateMsTotal = RealtimeUpdateMsTotal;
	const double UpdateMsMax = RealtimeUpdateMsMax;
	RealtimeUpdateCount = 0;
	RealtimeUpdateMsTotal = 0.0;
	RealtimeUpdateMsMax = 0.0;
	if (!UE_LOG_ACTIVE(LogFicsitStatistics, Verbose))
	{
		return;
	}

	TMap<TSubclassOf<UFGItemDescriptor>, FFicsitItemRate> Produced, Consumed;
	LiveHistory.GetAverages(0, Produced, Consumed);
	Produced.ValueSort([](const FFicsitItemRate& A, const FFicsitItemRate& B) { return A.Actual > B.Actual; });
	TMap<TSubclassOf<UFGItemDescriptor>, FFicsitItemRate> RealtimeProduced, RealtimeConsumed;
	const FFicsitHistory& RealtimeHistory = Realtime.GetHistory();
	RealtimeHistory.GetAverages(0, RealtimeProduced, RealtimeConsumed);

	const int32 Points = LiveHistory.Tiers.Num() > 0 ? LiveHistory.Tiers[0].Num() : 0;
	const int32 RealtimePoints = RealtimeHistory.Tiers.Num() > 0 ? RealtimeHistory.Tiers[0].Num() : 0;
	UE_LOG(LogFicsitStatistics, Verbose, TEXT("Live: %d one-second points (seq %d), %d realtime points spanning %.2f s; per-minute rates over 1m and 5s vs the smoothed rate:"),
		Points, LiveSeq, RealtimePoints, RealtimePoints > 0 ? RealtimeHistory.Tiers[0].GetOrdered(RealtimePoints - 1).Time - RealtimeHistory.Tiers[0].GetOrdered(0).Time : 0.0);
	int32 Logged = 0;
	for (const auto& Pair : Produced)
	{
		if (++Logged > LogTopN)
		{
			break;
		}
		const FFicsitItemFlow* Smoothed = LatestSnapshot.Produced.Find(Pair.Key);
		const FFicsitItemRate* Used = Consumed.Find(Pair.Key);
		const FFicsitItemRate* FiveSeconds = RealtimeProduced.Find(Pair.Key);
		UE_LOG(LogFicsitStatistics, Verbose, TEXT("  Live %s: made %.2f/min over 1m, %.2f/min over 5s (smoothed %.2f), used %.2f/min over 1m"), *GetNameSafe(Pair.Key.Get()),
			Pair.Value.Actual, FiveSeconds ? FiveSeconds->Actual : 0.f, Smoothed ? Smoothed->Total.Actual : 0.f, Used ? Used->Actual : 0.f);
	}
	UE_LOG(LogFicsitStatistics, Verbose, TEXT("Realtime updates: %d in the last minute, avg %.3f ms, max %.3f ms; %lld events placed (%lld arrived after their point closed), %lld from before a reset left out"),
		UpdateCount, UpdateCount > 0 ? UpdateMsTotal / UpdateCount : 0.0, UpdateMsMax,
		RealtimeStats.Placed, RealtimeStats.Late, RealtimeStats.Dropped);
	UE_LOG(LogFicsitStatistics, Verbose, TEXT("Smoothing: %d change restarts in the last minute; %d items averaged, median window %.0f s"),
		Smoothing.Restarts, Smoothing.Items, Smoothing.MedianWindowSeconds);
}

const FFicsitHistory& AFicsitStatisticsSubsystem::GetFreshHistory(EFicsitTimeRange Range)
{
	switch (Range)
	{
	case EFicsitTimeRange::FiveSeconds:
		LastRealtimeViewTime = FPlatformTime::Seconds();
		UpdateRealtime();
		return Realtime.GetHistory();
	case EFicsitTimeRange::OneMinute:
		// Its live seconds are replicated as they happen, so there's nothing to ask for.
		return LiveHistory;
	default:
		RequestHistory(GetHistory().GetTierForRange(Range));
		return GetHistory();
	}
}

void AFicsitStatisticsSubsystem::RequestHistory(int32 Tier)
{
	if (HasAuthority())
	{
		return;
	}
	// A client has one local player, and the server answers it through that player's RCO.
	AFGPlayerController* PlayerController = Cast<AFGPlayerController>(GetWorld()->GetFirstPlayerController());
	UFicsitStatsRCO* RCO = PlayerController ? PlayerController->GetRemoteCallObjectOfClass<UFicsitStatsRCO>() : nullptr;
	if (!RCO)
	{
		return;
	}
	if (const TOptional<double> SinceTime = ClientHistory.NextRequest(Tier, GetWorld()->GetTimeSeconds()))
	{
		RCO->Server_RequestHistory(Tier, *SinceTime);
	}
}

TOptional<double> AFicsitStatisticsSubsystem::ApplyHistoryChunk(const FFicsitHistoryChunk& Chunk)
{
	return ClientHistory.Apply(Chunk, GetWorld()->GetTimeSeconds());
}

AFicsitStatisticsSubsystem* AFicsitStatisticsSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AFicsitStatisticsSubsystem> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AFicsitStatisticsSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
	const int32 PointsBefore = History.Tiers.Num() > 0 ? History.Tiers[0].Num() : 0;
	History.EnsureLayout();
	UE_LOG(LogFicsitStatistics, Log, TEXT("Loaded history from save: %.0f s tracked, %d items, %d points in the 10-minute tier"),
		History.TrackedSeconds, History.ItemTable.Num(), PointsBefore);
}

void AFicsitStatisticsSubsystem::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		return;
	}

	History.EnsureLayout();
	SampleIntervalSeconds = FFicsitStatsSettings::GetSampleIntervalSeconds(this);

	// Discard anything counted before this world existed (e.g. a previous save in the same session).
	FFicsitCounts Discard;
	FFicsitEventCounters::Drain(Discard);
	FFicsitEventCounters::DrainLive(Discard);
	FFicsitEventCounters::DrainEvents(RealtimeScratch);
	RealtimeScratch.Reset();
	FFicsitEventCounters::ConsumeEventCount();
	const double CountingStartTime = GetWorld()->GetTimeSeconds();
	RateEstimator.SetSettings(GetRateEstimatorSettings());
	RateEstimator.Reset(CountingStartTime);
	LastSampleWorldTime = CountingStartTime;
	LastLiveWorldTime = CountingStartTime;

	// Dev-only benchmark: a marker file in Saved/ turns on a per-second CSV of raw vs smoothed rates (Saved/Logs).
	bBenchmark = FPaths::FileExists(FPaths::ProjectSavedDir() / TEXT("FicsitStatsBenchmark.txt"));
	Realtime.bTallyProduced = bBenchmark;
	if (bBenchmark)
	{
		BenchmarkPath = FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT("FicsitStatsBenchmark.csv");
		FFileHelper::SaveStringToFile(TEXT("t,seconds,item,made,used,rt_made,smoothed_made,smoothed_used,window_s,cycle_s,made_events,used_events,history_made\n"), *BenchmarkPath);
		UE_LOG(LogFicsitStatistics, Log, TEXT("Benchmark recording on: %s"), *BenchmarkPath);
	}

	UE_LOG(LogFicsitStatistics, Log, TEXT("Subsystem started; sampling every %.1f s, live counts every %.1f s; history has %.0f s tracked"),
		SampleIntervalSeconds, LiveIntervalSeconds, History.TrackedSeconds);
	GetWorldTimerManager().SetTimer(SampleTimer, this, &AFicsitStatisticsSubsystem::TakeSample, SampleIntervalSeconds, true);
	GetWorldTimerManager().SetTimer(LiveTimer, this, &AFicsitStatisticsSubsystem::TakeLiveSample, LiveIntervalSeconds, true);
}

void AFicsitStatisticsSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(SampleTimer);
	GetWorldTimerManager().ClearTimer(LiveTimer);
	Super::EndPlay(EndPlayReason);
}

void AFicsitStatisticsSubsystem::TakeSample()
{
	const double StartTime = FPlatformTime::Seconds();

	FFicsitProductionSnapshot Snapshot;
	Snapshot.WorldTimeSeconds = GetWorld()->GetTimeSeconds();

	// One pass over every buildable (foundations included, so this list can be very long in big factories): cheap
	// class checks only, and no per-type lookups that would walk the list again.
	ManufacturerScratch.Reset();
	ExtractorScratch.Reset();
	GeneratorScratch.Reset();
	FactoryScratch.Reset();
	StorageScratch.Reset();
	ReservoirScratch.Reset();
	int32 BuildablesVisited = 0;
	if (AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(GetWorld()))
	{
		const TArray<AFGBuildable*>& All = BuildableSubsystem->GetAllBuildablesRef();
		BuildablesVisited = All.Num();
		for (AFGBuildable* Buildable : All)
		{
			// Every factory-type building has power info (machines, generators, batteries, pumps, stations, ...).
			if (AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable))
			{
				FactoryScratch.Add(Factory);
			}
			if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Buildable))
			{
				ManufacturerScratch.Add(Manufacturer);
			}
			else if (AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(Buildable))
			{
				ExtractorScratch.Add(Extractor);
			}
			else if (AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(Buildable))
			{
				GeneratorScratch.Add(Generator);
			}
			else if (AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable))
			{
				// Dimensional Depot uploaders only hold items on their way into the Depot, which is counted on its own.
				if (!Storage->IsA<AFGCentralStorageContainer>())
				{
					StorageScratch.Add(Storage);
				}
			}
			else if (AFGBuildablePipeReservoir* Reservoir = Cast<AFGBuildablePipeReservoir>(Buildable))
			{
				ReservoirScratch.Add(Reservoir);
			}
		}
	}

	// Once per run, before the scans, so it is in the log even if a scan fails: do the reflected reads agree with the game's
	// inline accessors? A MISMATCH means the game is newer than the headers this mod was built against.
	static bool bLoggedLayoutCheck = false;
	if (!bLoggedLayoutCheck && (ManufacturerScratch.Num() > 0 || GeneratorScratch.Num() > 0 || StorageScratch.Num() > 0 || ExtractorScratch.Num() > 0))
	{
		bLoggedLayoutCheck = true;
		FicsitGame::LogLayoutCheck(
			ManufacturerScratch.IsEmpty() ? nullptr : ManufacturerScratch[0],
			GeneratorScratch.IsEmpty() ? nullptr : GeneratorScratch[0],
			StorageScratch.IsEmpty() ? nullptr : StorageScratch[0],
			ExtractorScratch.IsEmpty() ? nullptr : ExtractorScratch[0]);
	}

	ScanManufacturers(ManufacturerScratch, Snapshot);
	ScanExtractors(ExtractorScratch, Snapshot);
	ScanFuelGenerators(GeneratorScratch, Snapshot);
	ScanPower(FactoryScratch, Snapshot);
	ScanStorage(Snapshot);
	{
		// The sample interval can change at runtime (settings panel), and the buckets follow it.
		RateEstimator.SetSettings(GetRateEstimatorSettings());
		FFicsitCounts Counted;
		FFicsitEventCounters::Drain(Counted);
		RateEstimator.Apply(MoveTemp(Counted), Snapshot);
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	PerfScanMsTotal += ElapsedMs;
	PerfScanMsMax = FMath::Max(PerfScanMsMax, ElapsedMs);
	++PerfSamples;
	PerfBuildablesVisited = BuildablesVisited;

	// Use real elapsed game time (the timer can run late under load), but never more than a few intervals so a
	// long pause or hitch doesn't distort averages.
	const double Now = Snapshot.WorldTimeSeconds;
	const float Delta = LastSampleWorldTime >= 0.0
		? FMath::Clamp(static_cast<float>(Now - LastSampleWorldTime), 0.f, SampleIntervalSeconds * 3.f)
		: SampleIntervalSeconds;
	// The real time this sample's counts cover (Delta is capped; the rate must use what actually passed).
	const double Covered = LastSampleWorldTime >= 0.0 ? Now - LastSampleWorldTime : SampleIntervalSeconds;
	LastSampleWorldTime = Now;

	// The saved history (10m and longer) records exact counts: what the hooks counted in this sample, as a rate over the
	// time it covers, plus this scan's rates for what has no hook and its power. Each range's average is then the true
	// average over exactly that range. The smoothed rates in Snapshot are for "right now" (rows, details, HUD, ETAs).
	if (Covered > 0.0)
	{
		FFicsitLiveSecond Raw = FFicsitLiveSecond::FromCounts(RateEstimator.GetLatestCounts());
		for (int32 Kind = 0; Kind < FicsitManualKindCount; ++Kind)
		{
			for (const auto& Pair : Snapshot.Manual[Kind]) { Raw.Manual[Kind].FindOrAdd(Pair.Key.Get()) += Pair.Value; }
		}
		Raw.Seconds = static_cast<float>(Covered);
		FFicsitProductionSnapshot HistorySample;
		Raw.ToSample(Snapshot, GetWorldTime(), HistorySample);
		History.AddSample(HistorySample, Delta);
		if (bBenchmark)
		{
			BenchHistoryMade.Reset();
			for (const auto& Pair : HistorySample.Produced) { BenchHistoryMade.Add(Pair.Key.Get(), Pair.Value.Total.Actual); }
		}
	}

	// Each manual amount at Verbose (handy for checking the hooks: -LogCmds="LogFicsitStatistics Verbose").
	static const TCHAR* ManualNames[FicsitManualKindCount] = {TEXT("crafted"), TEXT("used crafting"), TEXT("spent building"), TEXT("refunded")};
	for (int32 Kind = 0; Kind < FicsitManualKindCount; ++Kind)
	{
		for (const auto& Pair : Snapshot.Manual[Kind])
		{
			UE_LOG(LogFicsitStatistics, Verbose, TEXT("Manual: %s %.0f x %s"), ManualNames[Kind], Pair.Value, *GetNameSafe(Pair.Key.Get()));
		}
	}

	LatestSnapshot = MoveTemp(Snapshot);
	NetSnapshot = FFicsitNetSnapshot::FromSnapshot(LatestSnapshot);

	// Pick up a changed sample interval (in-game settings panel or Mods menu) without reloading the save.
	const float ConfiguredInterval = FFicsitStatsSettings::GetSampleIntervalSeconds(this);
	if (!FMath::IsNearlyEqual(ConfiguredInterval, SampleIntervalSeconds))
	{
		UE_LOG(LogFicsitStatistics, Log, TEXT("Sample interval changed from %.1f s to %.1f s"), SampleIntervalSeconds, ConfiguredInterval);
		SampleIntervalSeconds = ConfiguredInterval;
		GetWorldTimerManager().SetTimer(SampleTimer, this, &AFicsitStatisticsSubsystem::TakeSample, SampleIntervalSeconds, true);
	}

	// A summary of everything, once a minute. It's at Verbose so that players' logs stay short. To turn it on, launch
	// with -LogCmds="LogFicsitStatistics Verbose" or add LogFicsitStatistics=Verbose under [Core.Log] in Engine.ini.
	if (++SamplesSinceFullLog >= FMath::Max(1, FMath::RoundToInt(60.f / SampleIntervalSeconds)))
	{
		SamplesSinceFullLog = 0;
		// Read outside UE_LOG, since its arguments aren't evaluated while Verbose is off.
		const int64 EventsLastMinute = FFicsitEventCounters::ConsumeEventCount();
		LogSnapshot(LatestSnapshot);
		LogLive();
		LogSupplyChains();

		UE_LOG(LogFicsitStatistics, Verbose, TEXT("Power check: buildings produce %.1f MW / consume %.1f MW; %d circuits report %.1f MW / %.1f MW"),
			LastBuildingPower[0], LastBuildingPower[1], LastCircuitCount, LastCircuitPower[0], LastCircuitPower[1]);
		const FFicsitPowerSnapshot& Power = LatestSnapshot.Power;
		UE_LOG(LogFicsitStatistics, Verbose, TEXT("Power: produced %.1f MW of %.1f MW capacity, consumed %.1f MW (max %.1f MW), batteries %.1f / %.1f MWh (+%.1f / -%.1f MW); %d producer types, %d consumer types"),
			Power.Get(EFicsitPowerTotal::Produced), Power.Get(EFicsitPowerTotal::Capacity), Power.Get(EFicsitPowerTotal::Consumed),
			Power.Get(EFicsitPowerTotal::MaxConsumption), Power.Get(EFicsitPowerTotal::BatteryStored), Power.Get(EFicsitPowerTotal::BatteryCapacity),
			Power.Get(EFicsitPowerTotal::BatteryInput), Power.Get(EFicsitPowerTotal::BatteryOutput), Power.Producers.Num(), Power.Consumers.Num());

		int32 TotalMachines = 0;
		for (const int32 Count : LatestSnapshot.MachinesScanned) { TotalMachines += Count; }
		UE_LOG(LogFicsitStatistics, Verbose, TEXT("Performance: scan avg %.2f ms, max %.2f ms over %d samples (%d buildables visited, %d machines, %d storages); %lld counted item events in the last minute"),
			PerfSamples > 0 ? PerfScanMsTotal / PerfSamples : 0.0, PerfScanMsMax, PerfSamples, PerfBuildablesVisited, TotalMachines,
			PerfStoragesScanned, EventsLastMinute);
		UE_LOG(LogFicsitStatistics, Verbose, TEXT("Storage: %d items stored, trends cover %.0f s, reading storage took avg %.2f ms of each scan"),
			LatestSnapshot.Stored.Num(), LatestSnapshot.StorageTrendSeconds, PerfSamples > 0 ? PerfStorageMsTotal / PerfSamples : 0.0);
		PerfStorageMsTotal = 0.0;
		PerfScanMsTotal = 0.0;
		PerfScanMsMax = 0.0;
		PerfSamples = 0;

		FString TierInfo;
		if (UE_LOG_ACTIVE(LogFicsitStatistics, Verbose))
		{
			for (const FFicsitHistoryTier& Tier : History.Tiers)
			{
				TierInfo += FString::Printf(TEXT(" %gs:%d/%d"), Tier.IntervalSeconds, Tier.Num(), Tier.Capacity);
			}
		}
		UE_LOG(LogFicsitStatistics, Verbose, TEXT("History: %.0f s tracked, %d items;%s"), History.TrackedSeconds, History.ItemTable.Num(), *TierInfo);
	}
}

EFicsitMachineState AFicsitStatisticsSubsystem::GetMachineState(const AFGBuildableFactory* Factory, bool bNeedsPower)
{
	if (FicsitGame::IsProductionPaused(Factory))
	{
		return EFicsitMachineState::Paused;
	}
	if (bNeedsPower && !FicsitGame::HasPower(Factory))
	{
		return EFicsitMachineState::Unpowered;
	}
	return Factory->IsProducing() ? EFicsitMachineState::Running : EFicsitMachineState::Idle;
}

void AFicsitStatisticsSubsystem::ScanManufacturers(const TArray<AFGBuildableManufacturer*>& Manufacturers, FFicsitProductionSnapshot& Snapshot)
{
	for (const AFGBuildableManufacturer* Manufacturer : Manufacturers)
	{
		const TSubclassOf<UFGRecipe> RecipeClass = FicsitGame::CurrentRecipe(Manufacturer);
		// Cycle time already includes the clock speed (potential).
		const float CycleTime = Manufacturer->GetProductionCycleTime();
		if (!RecipeClass || CycleTime <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		++Snapshot.MachinesScanned[static_cast<int32>(EFicsitRateSource::Manufacturing)];

		const EFicsitMachineState State = GetMachineState(Manufacturer, true);
		const float CyclesPerMinute = 60.f / CycleTime;
		const float Productivity = State == EFicsitMachineState::Paused ? 0.f : Manufacturer->GetProductivity();
		// The Somersloop boost only multiplies output (inputs are unchanged).
		const float OutputBoost = FicsitGame::CurrentProductionBoost(Manufacturer);

		const UFGRecipe* Recipe = RecipeClass.GetDefaultObject();
		// Includes the session's parts-cost multiplier, if the player changed it.
		const TArray<FItemAmount> Ingredients = Recipe->GetIngredientsWithPartsCostMultiplier(GetWorld());

		// Why an idle machine stopped: which ingredients it doesn't have enough of for a cycle, or (if it has them all)
		// that its output is full.
		ShortScratch.Reset();
		if (State == EFicsitMachineState::Idle)
		{
			if (const UFGInventoryComponent* Input = FicsitGame::InputInventory(Manufacturer))
			{
				for (const FItemAmount& Ingredient : Ingredients)
				{
					if (Ingredient.ItemClass && Input->GetNumItems(Ingredient.ItemClass) < Ingredient.Amount)
					{
						ShortScratch.Add(Ingredient.ItemClass.Get());
					}
				}
			}
		}
		const bool bShortOfAny = ShortScratch.Num() > 0;
		const EFicsitMachineState ProductState = FicsitMachineState::ForProduct(State, bShortOfAny);

		for (const FItemAmount& Product : Recipe->GetProducts())
		{
			const float MaxRate = ToDisplayAmount(Product.Amount, Product.ItemClass) * CyclesPerMinute * OutputBoost;
			Snapshot.Produced.FindOrAdd(Product.ItemClass).Add(EFicsitRateSource::Manufacturing, MaxRate * Productivity, MaxRate);
			Snapshot.Producers.FindOrAdd(Product.ItemClass).Add(ProductState);
			Snapshot.RecipeOutput.FindOrAdd(Product.ItemClass).FindOrAdd(RecipeClass) += MaxRate;
			RateEstimator.NoteCycle(Product.ItemClass.Get(), CycleTime);
		}

		for (const FItemAmount& Ingredient : Ingredients)
		{
			const float MaxRate = ToDisplayAmount(Ingredient.Amount, Ingredient.ItemClass) * CyclesPerMinute;
			Snapshot.Consumed.FindOrAdd(Ingredient.ItemClass).Add(EFicsitRateSource::Manufacturing, MaxRate * Productivity, MaxRate);
			Snapshot.Consumers.FindOrAdd(Ingredient.ItemClass).Add(
				FicsitMachineState::ForInput(State, ShortScratch.Contains(Ingredient.ItemClass.Get()), bShortOfAny));
			RateEstimator.NoteCycle(Ingredient.ItemClass.Get(), CycleTime);
		}
	}
}

void AFicsitStatisticsSubsystem::ScanExtractors(const TArray<AFGBuildableResourceExtractor*>& Extractors, FFicsitProductionSnapshot& Snapshot)
{
	// Miners, water extractors, oil extractors and resource-well (fracking) extractors.

	int32 CrossChecked = 0;
	int32 CrossCheckMismatches = 0;

	for (const AFGBuildableResourceExtractor* Extractor : Extractors)
	{
		const TSubclassOf<UFGItemDescriptor> Item = FicsitGame::ExtractorItem(Extractor);
		const float CycleTime = Extractor->GetProductionCycleTime();
		if (!Item || CycleTime <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		++Snapshot.MachinesScanned[static_cast<int32>(EFicsitRateSource::Extraction)];

		// Resource-well extractors are powered through their pressurizer, not directly.
		const bool bNeedsPower = !Extractor->IsA<AFGBuildableFrackingExtractor>();
		// Extractors have no inputs: one that's powered but not extracting has a full output (belt or pipe backed up).
		const EFicsitMachineState State = FicsitMachineState::ForProduct(GetMachineState(Extractor, bNeedsPower), false);
		const float Productivity = State == EFicsitMachineState::Paused ? 0.f : Extractor->GetProductivity();

		// Items per cycle already reflects node purity, and cycle time reflects clock speed.
		const float MaxRate = Extractor->GetNumExtractedItemsPerCycleConverted() * 60.f / CycleTime;
		Snapshot.Produced.FindOrAdd(Item).Add(EFicsitRateSource::Extraction, MaxRate * Productivity, MaxRate);
		Snapshot.Producers.FindOrAdd(Item).Add(State);
		RateEstimator.NoteCycle(Item.Get(), CycleTime);

		if (!bLoggedExtractorCrossCheck)
		{
			++CrossChecked;
			const float GameRate = Extractor->GetExtractionPerMinute();
			if (!FMath::IsNearlyEqual(GameRate, MaxRate, FMath::Max(0.01f, MaxRate * 0.01f)))
			{
				++CrossCheckMismatches;
				if (CrossCheckMismatches <= 5)
				{
					UE_LOG(LogFicsitStatistics, Warning, TEXT("Extractor cross-check: %s ours %.2f vs game %.2f /min"),
						*UFGItemDescriptor::GetItemName(Item).ToString(), MaxRate, GameRate);
				}
			}
		}
	}

	if (!bLoggedExtractorCrossCheck && CrossChecked > 0)
	{
		bLoggedExtractorCrossCheck = true;
		UE_LOG(LogFicsitStatistics, Log, TEXT("Extractor cross-check: %d of %d extractors match the game's own per-minute rate"),
			CrossChecked - CrossCheckMismatches, CrossChecked);
	}
}

TSubclassOf<UFGItemDescriptor> AFicsitStatisticsSubsystem::GetBuildingDescriptor(const AFGBuildable* Buildable)
{
	UClass* BuildableClass = Buildable ? Buildable->GetClass() : nullptr;
	if (!BuildableClass)
	{
		return nullptr;
	}
	if (const TObjectPtr<UClass>* Cached = BuildingDescriptorCache.Find(BuildableClass))
	{
		return Cached->Get();
	}
	UClass* Descriptor = nullptr;
	if (const TSubclassOf<UFGRecipe> Recipe = FicsitGame::BuiltWithRecipe(Buildable))
	{
		for (const FItemAmount& Product : Recipe.GetDefaultObject()->GetProducts())
		{
			if (Product.ItemClass)
			{
				Descriptor = Product.ItemClass.Get();
				break;
			}
		}
	}
	BuildingDescriptorCache.Add(BuildableClass, Descriptor);
	return Descriptor;
}

void AFicsitStatisticsSubsystem::ScanStorage(FFicsitProductionSnapshot& Snapshot)
{
	const double StartTime = FPlatformTime::Seconds();
	StockScratch.Reset();
	FInventoryStack Stack;
	for (AFGBuildableStorage* Storage : StorageScratch)
	{
		const UFGInventoryComponent* Inventory = FicsitGame::StorageInventory(Storage);
		if (!Inventory || Inventory->IsEmpty())
		{
			continue;
		}
		for (int32 Index = 0; Index < Inventory->GetSizeLinear(); ++Index)
		{
			// Checking first skips copying empty slots.
			if (!Inventory->IsIndexEmpty(Index) && Inventory->GetStackFromIndex(Index, Stack) && Stack.HasItems())
			{
				StockScratch.FindOrAdd(Stack.Item.GetItemClass()) += ToDisplayAmount(Stack.NumItems, Stack.Item.GetItemClass());
			}
		}
	}
	// Fluid buffers already report m³, the unit the UI shows fluids in.
	for (const AFGBuildablePipeReservoir* Reservoir : ReservoirScratch)
	{
		const TSubclassOf<UFGItemDescriptor> Fluid = Reservoir ? Reservoir->GetFluidDescriptor() : nullptr;
		const float Content = Fluid ? Reservoir->GetFluidContent() : 0.f;
		if (Content > 0.f)
		{
			StockScratch.FindOrAdd(Fluid) += Content;
		}
	}
	PerfStoragesScanned = StorageScratch.Num() + ReservoirScratch.Num();

	StockTrend.Add(Snapshot.WorldTimeSeconds, StockScratch);
	StockTrend.Fill(Snapshot.Stored);
	Snapshot.StorageTrendSeconds = static_cast<float>(StockTrend.GetCoveredSeconds());
	PerfStorageMsTotal += (FPlatformTime::Seconds() - StartTime) * 1000.0;
}

void AFicsitStatisticsSubsystem::ScanPower(const TArray<AFGBuildableFactory*>& Factories, FFicsitProductionSnapshot& Snapshot)
{
	FFicsitPowerSnapshot& Power = Snapshot.Power;
	auto Total = [&Power](EFicsitPowerTotal Which) -> float& { return Power.Totals[static_cast<int32>(Which)]; };

	for (AFGBuildableFactory* Factory : Factories)
	{
		if (!Factory)
		{
			continue;
		}
		if (const AFGBuildablePowerStorage* Battery = Cast<AFGBuildablePowerStorage>(Factory))
		{
			Total(EFicsitPowerTotal::BatteryStored) += Battery->GetPowerStore();
			Total(EFicsitPowerTotal::BatteryCapacity) += FicsitGame::PowerStoreCapacity(Battery);
			Total(EFicsitPowerTotal::BatteryInput) += Battery->GetPowerInput();
			Total(EFicsitPowerTotal::BatteryOutput) += Battery->GetPowerOutput();
			continue;
		}
		const UFGPowerInfoComponent* Info = FicsitGame::PowerInfo(Factory);
		if (!Info)
		{
			continue;
		}

		const float Capacity = Info->GetBaseProduction() + Info->GetDynamicProductionCapacity();
		const float Produced = Info->GetBaseProduction() + Info->GetRegulatedDynamicProduction() + Info->GetActualProductionBoost();
		const float Consumed = Info->GetActualConsumption();
		const float MaxConsumed = FMath::Max(Info->GetMaximumTargetConsumption(), FMath::Max(Info->GetTargetConsumption(), Consumed));
		const TSubclassOf<UFGItemDescriptor> Building = GetBuildingDescriptor(Factory);

		if (Capacity > 0.f || Produced > 0.f || Factory->IsA<AFGBuildableGenerator>())
		{
			Total(EFicsitPowerTotal::Produced) += Produced;
			Total(EFicsitPowerTotal::Capacity) += FMath::Max(Capacity, Produced);
			if (Building)
			{
				FFicsitPowerBuilding& Entry = Power.Producers.FindOrAdd(Building);
				Entry.MW += Produced;
				Entry.MaxMW += FMath::Max(Capacity, Produced);
				++Entry.Count;
				Entry.Active += Produced > 0.01f ? 1 : 0;
			}
		}
		else if (MaxConsumed > 0.f)
		{
			Total(EFicsitPowerTotal::Consumed) += Consumed;
			Total(EFicsitPowerTotal::MaxConsumption) += MaxConsumed;
			if (Building)
			{
				FFicsitPowerBuilding& Entry = Power.Consumers.FindOrAdd(Building);
				Entry.MW += Consumed;
				Entry.MaxMW += MaxConsumed;
				++Entry.Count;
				Entry.Active += Consumed > 0.01f ? 1 : 0;
			}
		}
	}

	// The game's own circuit totals (what power poles show) also include things the scan above can't see, like electric
	// trains and lights. They're the headline numbers, so whatever the buildings don't explain is shown as "Other".
	float CircuitTotals[4] = {};
	int32 Circuits = 0;
	if (const AFGCircuitSubsystem* CircuitSubsystem = AFGCircuitSubsystem::GetCircuitSubsystem(this))
	{
		FicsitGame::ForEachCircuit(CircuitSubsystem, [&](const UFGCircuit* Circuit)
		{
			if (const UFGPowerCircuit* PowerCircuit = Cast<UFGPowerCircuit>(Circuit))
			{
				FPowerCircuitStats Stats;
				PowerCircuit->GetStats(Stats);
				CircuitTotals[0] += Stats.PowerProduced;
				CircuitTotals[1] += Stats.PowerConsumed;
				CircuitTotals[2] += Stats.PowerProductionCapacity;
				CircuitTotals[3] += Stats.MaximumPowerConsumption;
				++Circuits;
			}
		});
	}
	LastBuildingPower[0] = Total(EFicsitPowerTotal::Produced);
	LastBuildingPower[1] = Total(EFicsitPowerTotal::Consumed);
	LastCircuitPower[0] = CircuitTotals[0];
	LastCircuitPower[1] = CircuitTotals[1];
	LastCircuitCount = Circuits;
	if (Circuits > 0)
	{
		Total(EFicsitPowerTotal::OtherProduced) = FMath::Max(0.f, CircuitTotals[0] - Total(EFicsitPowerTotal::Produced));
		Total(EFicsitPowerTotal::OtherConsumed) = FMath::Max(0.f, CircuitTotals[1] - Total(EFicsitPowerTotal::Consumed));
		Total(EFicsitPowerTotal::Produced) = CircuitTotals[0];
		Total(EFicsitPowerTotal::Consumed) = CircuitTotals[1];
		Total(EFicsitPowerTotal::Capacity) = FMath::Max(CircuitTotals[2], CircuitTotals[0]);
		Total(EFicsitPowerTotal::MaxConsumption) = FMath::Max(CircuitTotals[3], CircuitTotals[1]);
	}
}

void AFicsitStatisticsSubsystem::ScanFuelGenerators(const TArray<AFGBuildableGeneratorFuel*>& Generators, FFicsitProductionSnapshot& Snapshot)
{
	// Biomass burners, coal, fuel and nuclear power plants. Geothermal burns nothing, so it isn't relevant here.

	for (const AFGBuildableGeneratorFuel* Generator : Generators)
	{
		const TSubclassOf<UFGItemDescriptor> Fuel = FicsitGame::CurrentFuelClass(Generator);
		if (!Fuel)
		{
			continue;
		}
		const float EnergyPerUnit = UFGItemDescriptor::GetEnergyValue(Fuel); // MJ per item (per liter for fluids)
		const float CapacityMW = Generator->GetPowerProductionCapacity();  // includes clock speed
		if (EnergyPerUnit <= KINDA_SMALL_NUMBER || CapacityMW <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		++Snapshot.MachinesScanned[static_cast<int32>(EFicsitRateSource::PowerGeneration)];

		const EFicsitMachineState State = GetMachineState(Generator, false);
		// Generators burn fuel in proportion to the power actually drawn from them.
		const float Load = State == EFicsitMachineState::Running ? FMath::Clamp(FicsitGame::LoadPercentage(Generator), 0.f, 1.f) : 0.f;

		// Why an idle generator stopped: out of fuel, out of water, or (nuclear) its waste output is full. Other
		// generators have no output that can fill, so one that has its inputs just stays idle.
		const bool bIdle = State == EFicsitMachineState::Idle;
		const bool bNoFuel = bIdle && !Generator->HasFuel();
		const bool bNoWater = bIdle && FicsitGame::RequiresSupplementalResource(Generator) && !Generator->HasSupplementalResource();
		const bool bNuclear = Generator->IsA<AFGBuildableGeneratorNuclear>();
		auto StateFor = [&](bool bIsShortOfThis)
		{
			return FicsitMachineState::ForInput(State, bIsShortOfThis, bNoFuel || bNoWater, bNuclear);
		};

		const float MaxFuelRate = ToDisplayRate(CapacityMW * 60.f / EnergyPerUnit, Fuel);
		Snapshot.Consumed.FindOrAdd(Fuel).Add(EFicsitRateSource::PowerGeneration, MaxFuelRate * Load, MaxFuelRate);
		Snapshot.Consumers.FindOrAdd(Fuel).Add(StateFor(bNoFuel));

		// Water for coal and nuclear plants (rates are in m³/s).
		if (const TSubclassOf<UFGItemDescriptor> Supplemental = FicsitGame::SupplementalResourceClass(Generator))
		{
			if (FicsitGame::RequiresSupplementalResource(Generator))
			{
				const float MaxWater = Generator->GetSupplementalConsumptionRateMaximum() * 60.f;
				const float ActualWater = State == EFicsitMachineState::Running ? Generator->GetSupplementalConsumptionRateCurrent() * 60.f : 0.f;
				Snapshot.Consumed.FindOrAdd(Supplemental).Add(EFicsitRateSource::PowerGeneration, ActualWater, MaxWater);
				Snapshot.Consumers.FindOrAdd(Supplemental).Add(StateFor(bNoWater));
			}
		}

		// Nuclear waste is produced per fuel rod burned.
		if (Generator->IsA<AFGBuildableGeneratorNuclear>() && Fuel->IsChildOf(UFGItemDescriptorNuclearFuel::StaticClass()))
		{
			const TSubclassOf<UFGItemDescriptorNuclearFuel> NuclearFuel(Fuel.Get());
			const TSubclassOf<UFGItemDescriptor> Waste = UFGItemDescriptorNuclearFuel::GetSpentFuelClass(NuclearFuel);
			const int32 WastePerRod = UFGItemDescriptorNuclearFuel::GetAmountWasteCreated(NuclearFuel);
			if (Waste && WastePerRod > 0)
			{
				Snapshot.Produced.FindOrAdd(Waste).Add(EFicsitRateSource::PowerGeneration, MaxFuelRate * Load * WastePerRod, MaxFuelRate * WastePerRod);
				Snapshot.Producers.FindOrAdd(Waste).Add(FicsitMachineState::ForProduct(State, bNoFuel || bNoWater));
			}
		}
	}
}

FFicsitRateEstimator::FSettings AFicsitStatisticsSubsystem::GetRateEstimatorSettings() const
{
	// The window lengths and change threshold keep the estimator's own tuned defaults.
	FFicsitRateEstimator::FSettings Settings;
	Settings.BucketSeconds = SampleIntervalSeconds;
	Settings.IsFluid = [](UClass* Item) { return FicsitUnits::IsFluid(Item); };
	return Settings;
}

void AFicsitStatisticsSubsystem::LogSupplyChains()
{
	// Building the trees is the expensive part, so skip it when nothing would be logged.
	if (!UE_LOG_ACTIVE(LogFicsitStatistics, Verbose))
	{
		return;
	}
	const TMap<UClass*, int32> Remaining = FFicsitMilestoneNeeds::Read(this).SpaceElevator;

	// Each tree is many lines, so only log when the milestone's parts change (and once after loading).
	uint32 Parts = 0;
	for (const auto& Pair : Remaining)
	{
		Parts = HashCombine(Parts, GetTypeHash(Pair.Key));
	}
	if (Parts == LastLoggedMilestoneParts)
	{
		return;
	}
	LastLoggedMilestoneParts = Parts;

	FicsitSupplyChain::FRecipeIndex Recipes;
	Recipes.Rebuild(this);
	UE_LOG(LogFicsitStatistics, Verbose, TEXT("Supply chains for the current milestone: %d parts, %d unlocked production recipes"),
		Remaining.Num(), Recipes.NumRecipes());

	auto LogTree = [](const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			UE_LOG(LogFicsitStatistics, Verbose, TEXT("  %s"), *Line);
		}
	};

	const FicsitSupplyChain::FInputs Inputs{this, &LatestSnapshot, &Recipes, nullptr};
	bool bLoggedPick = false;
	for (const auto& Pair : Remaining)
	{
		const TSubclassOf<UFGItemDescriptor> Part(Pair.Key);
		const FicsitSupplyChain::FTree Tree = FicsitSupplyChain::Build(Inputs, Part);
		LogTree(FicsitSupplyChain::Describe(Tree, 1.f));

		// Exercise a player's pick once: the first item in the tree with another recipe available, switched to it.
		for (const FicsitSupplyChain::FNode& Node : Tree.Nodes)
		{
			if (bLoggedPick || Node.Kind != FicsitSupplyChain::ENodeKind::Made || Node.Options.Num() < 2)
			{
				continue;
			}
			const TSubclassOf<UFGRecipe> Other = *Node.Options.FindByPredicate([&Node](const TSubclassOf<UFGRecipe>& Option) { return Option != Node.Recipes[0].Recipe; });
			TMap<FicsitSupplyChain::FItem, FicsitSupplyChain::FRecipe> Chosen;
			Chosen.Add(Node.Item, Other);
			FicsitSupplyChain::FInputs Picked = Inputs;
			Picked.Chosen = &Chosen;
			UE_LOG(LogFicsitStatistics, Verbose, TEXT("  With %s picked for %s:"), *UFGRecipe::GetRecipeName(Other).ToString(), *UFGItemDescriptor::GetItemName(Node.Item).ToString());
			LogTree(FicsitSupplyChain::Describe(FicsitSupplyChain::Build(Picked, Part), 1.f));
			bLoggedPick = true;
		}
	}
}

void AFicsitStatisticsSubsystem::LogSnapshot(const FFicsitProductionSnapshot& Snapshot) const
{
	if (!UE_LOG_ACTIVE(LogFicsitStatistics, Verbose))
	{
		return;
	}
	auto LogList = [this](const TCHAR* Label, const FFicsitItemFlowMap& Flows, const FFicsitMachineCountMap& Machines)
	{
		TArray<TPair<TSubclassOf<UFGItemDescriptor>, FFicsitItemFlow>> Sorted = Flows.Array();
		Sorted.Sort([](const auto& A, const auto& B) { return A.Value.Total.Actual > B.Value.Total.Actual; });

		UE_LOG(LogFicsitStatistics, Verbose, TEXT("  %s (top %d of %d):"), Label, FMath::Min(LogTopN, Sorted.Num()), Sorted.Num());
		for (int32 Index = 0; Index < Sorted.Num() && Index < LogTopN; ++Index)
		{
			const TSubclassOf<UFGItemDescriptor> Item = Sorted[Index].Key;
			const FFicsitItemFlow& Flow = Sorted[Index].Value;
			const float Efficiency = Flow.Total.Max > 0.f ? 100.f * Flow.Total.Actual / Flow.Total.Max : 0.f;

			FString Sources;
			for (int32 Source = 0; Source < FicsitRateSourceCount; ++Source)
			{
				if (Flow.BySource[Source].Max > 0.f)
				{
					Sources += FString::Printf(TEXT(" %s=%.1f"), LexToString(static_cast<EFicsitRateSource>(Source)), Flow.BySource[Source].Actual);
				}
			}

			FString States;
			if (const FFicsitMachineCounts* Counts = Machines.Find(Item))
			{
				States = FString::Printf(TEXT(" | machines run %d starved %d other-input %d blocked %d idle %d paused %d unpowered %d"),
					Counts->Get(EFicsitMachineState::Running), Counts->Get(EFicsitMachineState::Starved), Counts->Get(EFicsitMachineState::OtherInputs),
					Counts->Get(EFicsitMachineState::OutputBlocked), Counts->Get(EFicsitMachineState::Idle),
					Counts->Get(EFicsitMachineState::Paused), Counts->Get(EFicsitMachineState::Unpowered));
			}

			UE_LOG(LogFicsitStatistics, Verbose, TEXT("    %-28s %10.2f /min (max %10.2f, %5.1f%%) [%s ]%s"),
				*UFGItemDescriptor::GetItemName(Item).ToString(), Flow.Total.Actual, Flow.Total.Max, Efficiency, *Sources, *States);
		}
	};

	LogList(TEXT("Produced"), Snapshot.Produced, Snapshot.Producers);
	LogList(TEXT("Consumed"), Snapshot.Consumed, Snapshot.Consumers);

	// Event sources are often small, so list them explicitly rather than hoping they make the top N.
	for (const auto& Pair : Snapshot.Consumed)
	{
		const FFicsitItemRate& Sink = Pair.Value.BySource[static_cast<int32>(EFicsitRateSource::ResourceSink)];
		const FFicsitItemRate& Elevator = Pair.Value.BySource[static_cast<int32>(EFicsitRateSource::SpaceElevator)];
		if (Sink.Actual > 0.f || Elevator.Actual > 0.f)
		{
			UE_LOG(LogFicsitStatistics, Verbose, TEXT("  Event: %-28s sink %.2f /min, elevator %.2f /min"),
				*UFGItemDescriptor::GetItemName(Pair.Key).ToString(), Sink.Actual, Elevator.Actual);
		}
	}
}
