#pragma once

#include "CoreMinimal.h"

class AFGBuildable;
class AFGBuildableFactory;
class AFGBuildableGenerator;
class AFGBuildableGeneratorFuel;
class AFGBuildableManufacturer;
class AFGBuildablePowerStorage;
class AFGBuildableResourceExtractor;
class AFGBuildableStorage;
class AFGCircuitSubsystem;
class UFGCircuit;
class UFGInventoryComponent;
class UFGItemDescriptor;
class UFGPowerInfoComponent;
class UFGRecipe;

/**
 * Reads of game members that go through reflection instead of the game's inline accessors.
 *
 * An inline accessor such as AFGBuildableManufacturer::GetCurrentRecipe() is compiled into this module with the class
 * layout of the headers it was built against. When the game is a newer build than those headers, that read lands on the
 * wrong offset and returns garbage, which crashes as soon as the value is used (EXCEPTION_ACCESS_VIOLATION). Reflection
 * takes the offset from the running game's UClass, so it can't go stale that way.
 *
 * Every function is null/false/0 safe and thread-safe (the production hooks run on worker threads). If a property can't
 * be found (renamed or retyped by a game update) the function falls back to the inline accessor, which is what the mod
 * did before: right when the headers match the game, wrong otherwise.
 */
namespace FicsitGame
{
	TSubclassOf<UFGRecipe> CurrentRecipe(const AFGBuildableManufacturer* Manufacturer);
	UFGInventoryComponent* InputInventory(const AFGBuildableManufacturer* Manufacturer);
	float CurrentProductionBoost(const AFGBuildableFactory* Factory);
	bool IsProductionPaused(const AFGBuildableFactory* Factory);
	bool HasPower(const AFGBuildableFactory* Factory);
	UFGPowerInfoComponent* PowerInfo(const AFGBuildableFactory* Factory);
	TSubclassOf<UFGRecipe> BuiltWithRecipe(const AFGBuildable* Buildable);
	UFGInventoryComponent* StorageInventory(const AFGBuildableStorage* Storage);
	float PowerStoreCapacity(const AFGBuildablePowerStorage* Battery);

	/** The fuel being burned right now, or null. */
	TSubclassOf<UFGItemDescriptor> CurrentFuelClass(const AFGBuildableGeneratorFuel* Generator);
	bool RequiresSupplementalResource(const AFGBuildableGeneratorFuel* Generator);
	/** The secondary resource (water for coal and nuclear plants), or null. */
	TSubclassOf<UFGItemDescriptor> SupplementalResourceClass(const AFGBuildableGeneratorFuel* Generator);
	/** The generator's current load, 0 to 1. */
	float LoadPercentage(const AFGBuildableGenerator* Generator);

	/** Calls Visit for every circuit the subsystem tracks. Reads the circuit map by reflection, like the rest. */
	void ForEachCircuit(const AFGCircuitSubsystem* Subsystem, TFunctionRef<void(const UFGCircuit*)> Visit);

	/**
	 * Diagnostic: logs whether each reflected read agrees with the game's inline accessor on real objects (any may be
	 * null; its checks are skipped). A MISMATCH means the game's class layout differs from the headers this mod was
	 * built against, which is exactly what the reflection reads protect against. Only pointers and numbers are compared,
	 * never dereferenced, so it is safe even when the inline read is wrong. Call it once per session.
	 */
	void LogLayoutCheck(const AFGBuildableManufacturer* Manufacturer, const AFGBuildableGeneratorFuel* Generator,
		const AFGBuildableStorage* Storage, const AFGBuildableResourceExtractor* Extractor);

	/**
	 * The item class an extractor produces (from its resource node, well or water volume), or null when it has no valid
	 * resource. Replaces AFGBuildableResourceExtractor::GetExtractableResource(), which returns a garbage TScriptInterface
	 * when the layout differs and crashes in GetInterfaceAddress.
	 */
	TSubclassOf<UFGItemDescriptor> ExtractorItem(const AFGBuildableResourceExtractor* Extractor);
}
