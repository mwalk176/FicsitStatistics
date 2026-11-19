#include "FicsitStatisticsHooks.h"

#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableFrackingExtractor.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "Equipment/FGBuildGunDismantle.h"
#include "FicsitGameAccess.h"
#include "FicsitMilestoneNeeds.h"
#include "FGInventoryComponent.h"
#include "FGWorkBench.h"
#include "FGGamePhaseManager.h"
#include "FGRecipe.h"
#include "FGResourceSinkSubsystem.h"
#include "FicsitStatistics.h"
#include "ItemAmount.h"
#include "Patching/NativeHookManager.h"
#include "Resources/FGItemDescriptor.h"

FCriticalSection FFicsitEventCounters::Lock;
FFicsitCounts FFicsitEventCounters::Pending;
FFicsitCounts FFicsitEventCounters::LivePending;
TArray<FFicsitLiveEvent> FFicsitEventCounters::PendingEvents;
int64 FFicsitEventCounters::EventCount = 0;
bool FFicsitEventCounters::bHooksInstalled = false;

namespace
{
	/** Item class -> amount. An alias, because a bare TMap<A, B> inside a hook lambda would split the hook macro's arguments. */
	using FItemCounts = TMap<UClass*, int32>;

	/** What the player can pay with or receive into: their inventory plus the Dimensional Depot. */
	void SnapshotPlayerItems(const UFGBuildGunState* State, FItemCounts& Out)
	{
		Out.Reset();
		const AFGBuildGun* Gun = State ? State->GetBuildGun() : nullptr;
		if (!Gun)
		{
			return;
		}
		Out = FicsitDepot::ReadStock(Gun);
		if (const UFGInventoryComponent* Inventory = Gun->GetInventory())
		{
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks);
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.HasItems() && Stack.Item.GetItemClass())
				{
					Out.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
				}
			}
		}
	}

	/** Records what left (as LeftKind) and what arrived (as Refunded) between two snapshots. */
	void RecordPlayerItemChange(const FItemCounts& Before, const FItemCounts& After, bool bCountLeft)
	{
		if (bCountLeft)
		{
			for (const auto& Pair : Before)
			{
				const int32 Now = After.FindRef(Pair.Key);
				if (Now < Pair.Value)
				{
					FFicsitEventCounters::AddManual(EFicsitManualKind::Built, Pair.Key, Pair.Value - Now);
				}
			}
		}
		for (const auto& Pair : After)
		{
			const int32 Was = Before.FindRef(Pair.Key);
			if (Pair.Value > Was)
			{
				FFicsitEventCounters::AddManual(EFicsitManualKind::Refunded, Pair.Key, Pair.Value - Was);
			}
		}
	}

	bool IsAuthority(const UFGBuildGunState* State)
	{
		const AFGBuildGun* Gun = State ? State->GetBuildGun() : nullptr;
		return Gun && Gun->HasAuthority();
	}

	// Build and dismantle run on the game thread and don't nest, so one pending snapshot each is enough.
	FItemCounts GBuildBefore;
	bool bBuildPending = false;
	FItemCounts GDismantleBefore;
	bool bDismantlePending = false;
}

void FFicsitEventCounters::InstallHooks()
{
	// Native hooks only apply to the packaged game, not the editor.
#if !WITH_EDITOR
	if (bHooksInstalled)
	{
		return;
	}
	bHooksInstalled = true;

	// A manufacturer or extractor finished a production cycle. Manufacturers and most extractors share the base
	// implementation (one detour covers them all), but resource-well extractors override it, so they get their own hook
	// and are skipped here in case their override calls the base.
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBuildableFactory::Factory_ProductionCycleCompleted, GetDefault<AFGBuildableManufacturer>(),
		[](AFGBuildableFactory* Self, float OverProductionRate)
		{
			if (Self && !Self->IsA<AFGBuildableFrackingExtractor>())
			{
				RecordCycleCompleted(Self);
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBuildableFrackingExtractor::Factory_ProductionCycleCompleted, GetDefault<AFGBuildableFrackingExtractor>(),
		[](AFGBuildableFrackingExtractor* Self, float OverProductionRate)
		{
			RecordCycleCompleted(Self);
		});

	// A manufacturer took the ingredients for its next cycle.
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGBuildableManufacturer::Factory_ConsumeIngredients, GetDefault<AFGBuildableManufacturer>(),
		[](AFGBuildableManufacturer* Self)
		{
			RecordIngredientsConsumed(Self);
		});

	// Called once per item the AWESOME Sink accepts. Returns false if the item couldn't be sunk.
	SUBSCRIBE_METHOD_AFTER(AFGResourceSinkSubsystem::AddPoints_ThreadSafe,
		[](const bool& bAccepted, AFGResourceSinkSubsystem* Self, TSubclassOf<UFGItemDescriptor> Item)
		{
			if (bAccepted && Item)
			{
				Add(false, EFicsitRateSource::ResourceSink, Item.Get(), 1.0);
			}
		});

	// Called when items are paid into the current Space Elevator phase. Returns how many were actually used.
	SUBSCRIBE_METHOD_AFTER(AFGGamePhaseManager::PayOffOnTargetGamePhase,
		[](const int32& AmountPaid, AFGGamePhaseManager* Self, FItemAmount PayOff)
		{
			if (AmountPaid > 0 && PayOff.ItemClass)
			{
				Add(false, EFicsitRateSource::SpaceElevator, PayOff.ItemClass.Get(), AmountPaid);
			}
		});

	// Hand-crafting (Craft Bench and Equipment Workshop): one call per finished craft. Clients run it too, so only the
	// server's call counts.
	SUBSCRIBE_METHOD_AFTER(UFGWorkBench::RemoveIngredientsAndAwardRewards,
		[](UFGWorkBench* Self, UFGInventoryComponent* Inventory, TSubclassOf<UFGRecipe> Recipe)
		{
			if (!Self || !Recipe || !Self->HasAuthority())
			{
				return;
			}
			for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
			{
				AddManual(EFicsitManualKind::Crafted, Product.ItemClass.Get(), Product.Amount);
			}
			for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(Self, Recipe))
			{
				AddManual(EFicsitManualKind::CraftUsed, Ingredient.ItemClass.Get(), Ingredient.Amount);
			}
		});

	// Building: measure what actually left the player's inventory and the Depot. That covers blueprints, zoops and
	// upgrades (whose refund arrives in the same call), and counts nothing for failed builds or with No Build Cost.
	SUBSCRIBE_METHOD(UFGBuildGunStateBuild::InternalConstructHologram,
		[](auto& Scope, UFGBuildGunStateBuild* Self, FNetConstructionID ConstructionId)
		{
			bBuildPending = IsAuthority(Self);
			if (bBuildPending)
			{
				SnapshotPlayerItems(Self, GBuildBefore);
			}
		});
	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStateBuild::InternalConstructHologram,
		[](UFGBuildGunStateBuild* Self, FNetConstructionID ConstructionId)
		{
			if (!bBuildPending)
			{
				return;
			}
			bBuildPending = false;
			FItemCounts After;
			SnapshotPlayerItems(Self, After);
			RecordPlayerItemChange(GBuildBefore, After, true);
		});

	// Dismantling: what arrived in the player's inventory (refunds, plus anything that was stored in the building).
	SUBSCRIBE_METHOD_VIRTUAL(UFGBuildGunStateDismantle::Server_DismantleActors_Implementation, GetDefault<UFGBuildGunStateDismantle>(),
		[](auto& Scope, UFGBuildGunStateDismantle* Self, const TArray<AActor*>& Selected, const TArray<FDismantleLightweightBundle>& Bundles)
		{
			bDismantlePending = IsAuthority(Self);
			if (bDismantlePending)
			{
				SnapshotPlayerItems(Self, GDismantleBefore);
			}
		});
	SUBSCRIBE_METHOD_VIRTUAL_AFTER(UFGBuildGunStateDismantle::Server_DismantleActors_Implementation, GetDefault<UFGBuildGunStateDismantle>(),
		[](UFGBuildGunStateDismantle* Self, const TArray<AActor*>& Selected, const TArray<FDismantleLightweightBundle>& Bundles)
		{
			if (!bDismantlePending)
			{
				return;
			}
			bDismantlePending = false;
			FItemCounts After;
			SnapshotPlayerItems(Self, After);
			RecordPlayerItemChange(GDismantleBefore, After, false);
		});

	UE_LOG(LogFicsitStatistics, Log, TEXT("Installed production, consumption, Sink, Space Elevator, crafting, building and dismantling hooks"));
#endif
}

void FFicsitEventCounters::RecordCycleCompleted(AFGBuildableFactory* Factory)
{
	if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Factory))
	{
		const TSubclassOf<UFGRecipe> RecipeClass = FicsitGame::CurrentRecipe(Manufacturer);
		if (!RecipeClass)
		{
			return;
		}
		// Somersloop amplification multiplies the output of each cycle.
		const float Boost = FicsitGame::CurrentProductionBoost(Manufacturer);
		for (const FItemAmount& Product : RecipeClass.GetDefaultObject()->GetProducts())
		{
			Add(true, EFicsitRateSource::Manufacturing, Product.ItemClass.Get(), FicsitUnits::ToDisplayAmount(Product.Amount, Product.ItemClass) * Boost);
		}
		return;
	}

	if (const AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(Factory))
	{
		if (const TSubclassOf<UFGItemDescriptor> Item = FicsitGame::ExtractorItem(Extractor))
		{
			Add(true, EFicsitRateSource::Extraction, Item.Get(), Extractor->GetNumExtractedItemsPerCycleConverted());
		}
	}
}

void FFicsitEventCounters::RecordIngredientsConsumed(AFGBuildableManufacturer* Manufacturer)
{
	const TSubclassOf<UFGRecipe> RecipeClass = FicsitGame::CurrentRecipe(Manufacturer);
	if (!RecipeClass)
	{
		return;
	}
	for (const FItemAmount& Ingredient : RecipeClass.GetDefaultObject()->GetIngredientsWithPartsCostMultiplier(Manufacturer))
	{
		Add(false, EFicsitRateSource::Manufacturing, Ingredient.ItemClass.Get(), FicsitUnits::ToDisplayAmount(Ingredient.Amount, Ingredient.ItemClass));
	}
}

void FFicsitEventCounters::Add(bool bProduced, EFicsitRateSource Source, UClass* Item, double Amount)
{
	if (!Item || Amount <= 0.0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const EFicsitLiveChannel Channel = bProduced ? EFicsitLiveChannel::Produced
		: Source == EFicsitRateSource::ResourceSink ? EFicsitLiveChannel::Sunk : EFicsitLiveChannel::Consumed;
	FScopeLock ScopeLock(&Lock);
	const int32 Index = static_cast<int32>(Source);
	(bProduced ? Pending.Produced[Index] : Pending.Consumed[Index]).FindOrAdd(Item) += Amount;
	(bProduced ? LivePending.Produced[Index] : LivePending.Consumed[Index]).FindOrAdd(Item) += Amount;
	++Pending.Events[bProduced ? 0 : 1].FindOrAdd(Item);
	++LivePending.Events[bProduced ? 0 : 1].FindOrAdd(Item);
	AddEventLocked(Now, Item, Amount, static_cast<uint8>(Channel));
	++EventCount;
}

void FFicsitEventCounters::AddEventLocked(double Time, UClass* Item, double Amount, uint8 Channel)
{
	// Drained every frame while the stats window is open and every second otherwise. The cap only matters if nothing
	// drains at all (e.g. hooks firing where no subsystem runs), so the list can't grow without bound.
	if (PendingEvents.Num() >= 1000000)
	{
		PendingEvents.Reset();
	}
	FFicsitLiveEvent& Event = PendingEvents.AddDefaulted_GetRef();
	Event.Time = Time;
	Event.Item = Item;
	Event.Amount = static_cast<float>(Amount);
	Event.Channel = Channel;
}

void FFicsitEventCounters::AddManual(EFicsitManualKind Kind, UClass* Item, double Amount)
{
	if (!Item || Amount <= 0.0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	FScopeLock ScopeLock(&Lock);
	Pending.Manual[static_cast<int32>(Kind)].FindOrAdd(Item) += Amount;
	LivePending.Manual[static_cast<int32>(Kind)].FindOrAdd(Item) += Amount;
	AddEventLocked(Now, Item, Amount, static_cast<uint8>(EFicsitLiveChannel::ManualBase) + static_cast<uint8>(Kind));
	++EventCount;
}

void FFicsitEventCounters::Drain(FFicsitCounts& Out)
{
	Out.Reset();
	FScopeLock ScopeLock(&Lock);
	Swap(Out, Pending);
}

void FFicsitEventCounters::DrainLive(FFicsitCounts& Out)
{
	Out.Reset();
	FScopeLock ScopeLock(&Lock);
	Swap(Out, LivePending);
}

void FFicsitEventCounters::DrainEvents(TArray<FFicsitLiveEvent>& Out)
{
	Out.Reset();
	FScopeLock ScopeLock(&Lock);
	Swap(Out, PendingEvents);
}

int64 FFicsitEventCounters::ConsumeEventCount()
{
	FScopeLock ScopeLock(&Lock);
	const int64 Count = EventCount;
	EventCount = 0;
	return Count;
}

FFicsitLiveSecond FFicsitLiveSecond::FromCounts(const FFicsitCounts& Counts)
{
	FFicsitLiveSecond Out;
	for (int32 Source = 0; Source < FicsitRateSourceCount; ++Source)
	{
		for (const auto& Pair : Counts.Produced[Source]) { Out.Produced.FindOrAdd(Pair.Key) += static_cast<float>(Pair.Value); }
		for (const auto& Pair : Counts.Consumed[Source])
		{
			Out.Consumed.FindOrAdd(Pair.Key) += static_cast<float>(Pair.Value);
			if (Source == static_cast<int32>(EFicsitRateSource::ResourceSink))
			{
				Out.Sunk.FindOrAdd(Pair.Key) += static_cast<float>(Pair.Value);
			}
		}
	}
	for (int32 Kind = 0; Kind < FicsitManualKindCount; ++Kind)
	{
		for (const auto& Pair : Counts.Manual[Kind]) { Out.Manual[Kind].FindOrAdd(Pair.Key) += static_cast<float>(Pair.Value); }
	}
	return Out;
}

void FFicsitLiveSecond::ToSample(const FFicsitProductionSnapshot& Reference, double WorldTime, FFicsitProductionSnapshot& Sample) const
{
	Sample.WorldTimeSeconds = WorldTime;
	Sample.Power = Reference.Power;

	// Generators' fuel and nuclear waste have no hook, so their current rate comes from the latest scan.
	const int32 Scanned = static_cast<int32>(EFicsitRateSource::PowerGeneration);
	for (const auto& Pair : Reference.Produced)
	{
		const float Rate = Pair.Value.BySource[Scanned].Actual;
		if (Rate > 0.f) { Sample.Produced.FindOrAdd(Pair.Key).Add(EFicsitRateSource::PowerGeneration, Rate, Rate); }
	}
	for (const auto& Pair : Reference.Consumed)
	{
		const float Rate = Pair.Value.BySource[Scanned].Actual;
		if (Rate > 0.f) { Sample.Consumed.FindOrAdd(Pair.Key).Add(EFicsitRateSource::PowerGeneration, Rate, Rate); }
	}

	// Everything the hooks counted, as a per-minute rate over the sample. Only the totals and the Sink's part are kept
	// by the history, so the counted amounts go in under one source.
	const float PerMinute = 60.f / FMath::Max(Seconds, KINDA_SMALL_NUMBER);
	for (const auto& Pair : Produced)
	{
		Sample.Produced.FindOrAdd(Pair.Key).Add(EFicsitRateSource::Manufacturing, Pair.Value * PerMinute, 0.f);
	}
	for (const auto& Pair : Consumed)
	{
		const float SunkAmount = Sunk.FindRef(Pair.Key);
		FFicsitItemFlow& Flow = Sample.Consumed.FindOrAdd(Pair.Key);
		Flow.Add(EFicsitRateSource::Manufacturing, (Pair.Value - SunkAmount) * PerMinute, 0.f);
		if (SunkAmount > 0.f)
		{
			Flow.Add(EFicsitRateSource::ResourceSink, SunkAmount * PerMinute, 0.f);
		}
	}
	for (int32 Kind = 0; Kind < FicsitManualKindCount; ++Kind)
	{
		for (const auto& Pair : Manual[Kind]) { Sample.Manual[Kind].Add(Pair.Key, Pair.Value); }
	}
}
