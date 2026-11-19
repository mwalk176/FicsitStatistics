#include "FicsitGameAccess.h"

#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePowerStorage.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableStorage.h"
#include "FGCircuit.h"
#include "FGCircuitSubsystem.h"
#include "FGInventoryComponent.h"
#include "FGPowerInfoComponent.h"
#include "FGRecipe.h"
#include "FicsitStatistics.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * A reflected property of a game class, or null if a game update renamed or removed it. Each function below calls this
	 * from a function-local static: initialised once and thread-safely, which matters because the hooks run on worker threads.
	 */
	template <typename TProperty, typename TOwner>
	const TProperty* FindGameProperty(const TCHAR* Name)
	{
		const TProperty* Property = CastField<TProperty>(TOwner::StaticClass()->FindPropertyByName(Name));
		if (!Property)
		{
			UE_LOG(LogFicsitStatistics, Warning, TEXT("%s::%s was not found (renamed or retyped by a game update?). Falling back to the inline accessor, which can read the wrong memory if the game's layout changed."),
				*TOwner::StaticClass()->GetName(), Name);
		}
		return Property;
	}

	/** A float or bool property, or Fallback() when the property is missing. */
	template <typename TProperty, typename TFallback>
	auto ReadValue(const UObject* Owner, const TProperty* Property, TFallback&& Fallback)
	{
		return Property ? Property->GetPropertyValue_InContainer(Owner) : Fallback();
	}

	/** An object pointer property, or Fallback() when the property is missing. */
	template <typename TObject, typename TFallback>
	TObject* ReadObject(const UObject* Owner, const FObjectPropertyBase* Property, TFallback&& Fallback)
	{
		return Property ? Cast<TObject>(Property->GetObjectPropertyValue_InContainer(Owner)) : Fallback();
	}

	/** A TSubclassOf<TBase> property, or Fallback() when the property is missing. Null if the value isn't a TBase subclass. */
	template <typename TBase, typename TFallback>
	TSubclassOf<TBase> ReadClass(const UObject* Owner, const FObjectPropertyBase* Property, TFallback&& Fallback)
	{
		if (!Property)
		{
			return Fallback();
		}
		UClass* Class = Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Owner));
		return (Class && Class->IsChildOf(TBase::StaticClass())) ? TSubclassOf<TBase>(Class) : TSubclassOf<TBase>();
	}
}

TSubclassOf<UFGRecipe> FicsitGame::CurrentRecipe(const AFGBuildableManufacturer* Manufacturer)
{
	if (!IsValid(Manufacturer))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableManufacturer>(TEXT("mCurrentRecipe"));
	return ReadClass<UFGRecipe>(Manufacturer, Property, [Manufacturer] { return Manufacturer->GetCurrentRecipe(); });
}

UFGInventoryComponent* FicsitGame::InputInventory(const AFGBuildableManufacturer* Manufacturer)
{
	if (!IsValid(Manufacturer))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableManufacturer>(TEXT("mInputInventory"));
	return ReadObject<UFGInventoryComponent>(Manufacturer, Property, [Manufacturer] { return Manufacturer->GetInputInventory(); });
}

float FicsitGame::CurrentProductionBoost(const AFGBuildableFactory* Factory)
{
	if (!IsValid(Factory))
	{
		return 1.f;
	}
	static const FFloatProperty* const Property = FindGameProperty<FFloatProperty, AFGBuildableFactory>(TEXT("mCurrentProductionBoost"));
	return ReadValue(Factory, Property, [Factory] { return Factory->GetCurrentProductionBoost(); });
}

bool FicsitGame::IsProductionPaused(const AFGBuildableFactory* Factory)
{
	if (!IsValid(Factory))
	{
		return false;
	}
	static const FBoolProperty* const Property = FindGameProperty<FBoolProperty, AFGBuildableFactory>(TEXT("mIsProductionPaused"));
	return ReadValue(Factory, Property, [Factory] { return Factory->IsProductionPaused(); });
}

bool FicsitGame::HasPower(const AFGBuildableFactory* Factory)
{
	if (!IsValid(Factory))
	{
		return false;
	}
	// A bitfield (uint8 mHasPower : 1), which FBoolProperty reads correctly.
	static const FBoolProperty* const Property = FindGameProperty<FBoolProperty, AFGBuildableFactory>(TEXT("mHasPower"));
	return ReadValue(Factory, Property, [Factory] { return Factory->HasPower(); });
}

UFGPowerInfoComponent* FicsitGame::PowerInfo(const AFGBuildableFactory* Factory)
{
	if (!IsValid(Factory))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableFactory>(TEXT("mPowerInfo"));
	return ReadObject<UFGPowerInfoComponent>(Factory, Property, [Factory] { return Factory->GetPowerInfo(); });
}

TSubclassOf<UFGRecipe> FicsitGame::BuiltWithRecipe(const AFGBuildable* Buildable)
{
	if (!IsValid(Buildable))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildable>(TEXT("mBuiltWithRecipe"));
	return ReadClass<UFGRecipe>(Buildable, Property, [Buildable] { return Buildable->GetBuiltWithRecipe(); });
}

UFGInventoryComponent* FicsitGame::StorageInventory(const AFGBuildableStorage* Storage)
{
	if (!IsValid(Storage))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableStorage>(TEXT("mStorageInventory"));
	// GetStorageInventory() isn't const, so the fallback needs a cast.
	return ReadObject<UFGInventoryComponent>(Storage, Property, [Storage] { return const_cast<AFGBuildableStorage*>(Storage)->GetStorageInventory(); });
}

float FicsitGame::PowerStoreCapacity(const AFGBuildablePowerStorage* Battery)
{
	if (!IsValid(Battery))
	{
		return 0.f;
	}
	static const FFloatProperty* const Property = FindGameProperty<FFloatProperty, AFGBuildablePowerStorage>(TEXT("mPowerStoreCapacity"));
	return ReadValue(Battery, Property, [Battery] { return Battery->GetPowerStoreCapacity(); });
}

TSubclassOf<UFGItemDescriptor> FicsitGame::CurrentFuelClass(const AFGBuildableGeneratorFuel* Generator)
{
	if (!IsValid(Generator))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableGeneratorFuel>(TEXT("mCurrentFuelClass"));
	return ReadClass<UFGItemDescriptor>(Generator, Property, [Generator] { return Generator->GetCurrentFuelClass(); });
}

bool FicsitGame::RequiresSupplementalResource(const AFGBuildableGeneratorFuel* Generator)
{
	if (!IsValid(Generator))
	{
		return false;
	}
	static const FBoolProperty* const Property = FindGameProperty<FBoolProperty, AFGBuildableGeneratorFuel>(TEXT("mRequiresSupplementalResource"));
	return ReadValue(Generator, Property, [Generator] { return Generator->GetRequiresSupplementalResource(); });
}

TSubclassOf<UFGItemDescriptor> FicsitGame::SupplementalResourceClass(const AFGBuildableGeneratorFuel* Generator)
{
	if (!IsValid(Generator))
	{
		return nullptr;
	}
	static const FObjectPropertyBase* const Property = FindGameProperty<FObjectPropertyBase, AFGBuildableGeneratorFuel>(TEXT("mSupplementalResourceClass"));
	return ReadClass<UFGItemDescriptor>(Generator, Property, [Generator] { return Generator->GetSupplementalResourceClass(); });
}

float FicsitGame::LoadPercentage(const AFGBuildableGenerator* Generator)
{
	if (!IsValid(Generator))
	{
		return 0.f;
	}
	static const FFloatProperty* const Property = FindGameProperty<FFloatProperty, AFGBuildableGenerator>(TEXT("mLoadPercentage"));
	return ReadValue(Generator, Property, [Generator] { return Generator->GetLoadPercentage(); });
}

void FicsitGame::ForEachCircuit(const AFGCircuitSubsystem* Subsystem, TFunctionRef<void(const UFGCircuit*)> Visit)
{
	if (!IsValid(Subsystem))
	{
		return;
	}
	static const FMapProperty* const Property = FindGameProperty<FMapProperty, AFGCircuitSubsystem>(TEXT("mCircuits"));
	const FObjectPropertyBase* ValueProperty = Property ? CastField<FObjectPropertyBase>(Property->ValueProp) : nullptr;
	if (!ValueProperty)
	{
		// The map is gone or holds something else (a future game update); FindGameProperty already logged it. There is no
		// inline fallback here: mCircuits is private and only AFicsitStatisticsSubsystem is allowed to touch it, so the
		// circuit totals are skipped and the building totals are shown on their own.
		return;
	}

	FScriptMapHelper Map(Property, Property->ContainerPtrToValuePtr<void>(Subsystem));
	for (int32 Index = 0; Index < Map.GetMaxIndex(); ++Index)
	{
		if (Map.IsValidIndex(Index))
		{
			Visit(Cast<UFGCircuit>(ValueProperty->GetObjectPropertyValue(Map.GetValuePtr(Index))));
		}
	}
}

namespace
{
	/**
	 * The object an extractor's mExtractableResource points at, read by reflection. The property is an interface
	 * (TScriptInterface): an object plus an interface offset, of which only the object is needed. bOutFound is false when
	 * the property is gone or changed type (a future game update), and the caller falls back to the inline accessor.
	 */
	UObject* ReadExtractorResource(const AFGBuildableResourceExtractor* Extractor, bool& bOutFound)
	{
		static const FProperty* const ResourceProperty =
			AFGBuildableResourceExtractor::StaticClass()->FindPropertyByName(TEXT("mExtractableResource"));

		bOutFound = true;
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ResourceProperty))
		{
			return ObjectProperty->GetObjectPropertyValue_InContainer(Extractor);
		}
		if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(ResourceProperty))
		{
			return InterfaceProperty->GetPropertyValuePtr_InContainer(Extractor)->GetObject();
		}
		bOutFound = false;
		return nullptr;
	}

	/**
	 * The pointer a TSubclassOf holds, copied out as raw bytes. Even TSubclassOf::Get() touches the pointed-to class, so on
	 * a garbage value (what an inline accessor returns when the layout differs) it crashes; this never dereferences.
	 */
	template <typename T>
	const void* RawPointer(const TSubclassOf<T>& Class)
	{
		static_assert(sizeof(TSubclassOf<T>) == sizeof(void*), "TSubclassOf is expected to hold a single pointer");
		const void* Pointer = nullptr;
		FMemory::Memcpy(&Pointer, &Class, sizeof(Pointer));
		return Pointer;
	}

	FString Describe(const void* Pointer) { return FString::Printf(TEXT("%p"), Pointer); }
	FString Describe(float Value) { return FString::SanitizeFloat(Value); }
	FString Describe(bool bValue) { return bValue ? TEXT("true") : TEXT("false"); }

	struct FLayoutCheck
	{
		int32 Checked = 0;
		int32 Mismatches = 0;

		template <typename T>
		void Compare(const TCHAR* Name, const T& ViaReflection, const T& ViaInline)
		{
			++Checked;
			// Garbage floats can be NaN, which never compares equal, so a bad read is counted as a mismatch.
			const bool bSame = ViaReflection == ViaInline;
			if (bSame)
			{
				UE_LOG(LogFicsitStatistics, Log, TEXT("Layout check: %s OK (%s)"), Name, *Describe(ViaReflection));
			}
			else
			{
				++Mismatches;
				UE_LOG(LogFicsitStatistics, Warning, TEXT("Layout check: %s MISMATCH, reflection %s but the inline accessor reads %s"),
					Name, *Describe(ViaReflection), *Describe(ViaInline));
			}
		}
	};
}

void FicsitGame::LogLayoutCheck(const AFGBuildableManufacturer* Manufacturer, const AFGBuildableGeneratorFuel* Generator,
	const AFGBuildableStorage* Storage, const AFGBuildableResourceExtractor* Extractor)
{
	FLayoutCheck Check;
	if (IsValid(Manufacturer))
	{
		Check.Compare<const void*>(TEXT("Manufacturer.CurrentRecipe"), RawPointer(CurrentRecipe(Manufacturer)), RawPointer(Manufacturer->GetCurrentRecipe()));
		Check.Compare<const void*>(TEXT("Manufacturer.InputInventory"), InputInventory(Manufacturer), Manufacturer->GetInputInventory());
		Check.Compare<float>(TEXT("Factory.CurrentProductionBoost"), CurrentProductionBoost(Manufacturer), Manufacturer->GetCurrentProductionBoost());
		Check.Compare<bool>(TEXT("Factory.IsProductionPaused"), IsProductionPaused(Manufacturer), Manufacturer->IsProductionPaused());
		Check.Compare<bool>(TEXT("Factory.HasPower"), HasPower(Manufacturer), Manufacturer->HasPower());
		Check.Compare<const void*>(TEXT("Factory.PowerInfo"), PowerInfo(Manufacturer), Manufacturer->GetPowerInfo());
		Check.Compare<const void*>(TEXT("Buildable.BuiltWithRecipe"), RawPointer(BuiltWithRecipe(Manufacturer)), RawPointer(Manufacturer->GetBuiltWithRecipe()));
	}
	if (IsValid(Generator))
	{
		Check.Compare<const void*>(TEXT("Generator.CurrentFuelClass"), RawPointer(CurrentFuelClass(Generator)), RawPointer(Generator->GetCurrentFuelClass()));
		Check.Compare<float>(TEXT("Generator.LoadPercentage"), LoadPercentage(Generator), Generator->GetLoadPercentage());
		Check.Compare<bool>(TEXT("Generator.RequiresSupplementalResource"), RequiresSupplementalResource(Generator), Generator->GetRequiresSupplementalResource());
		Check.Compare<const void*>(TEXT("Generator.SupplementalResourceClass"), RawPointer(SupplementalResourceClass(Generator)), RawPointer(Generator->GetSupplementalResourceClass()));
	}
	if (IsValid(Storage))
	{
		Check.Compare<const void*>(TEXT("Storage.StorageInventory"), StorageInventory(Storage), const_cast<AFGBuildableStorage*>(Storage)->GetStorageInventory());
	}
	if (IsValid(Extractor))
	{
		// The inline accessor can't be compared safely: it builds a TScriptInterface from the raw bytes, and on a layout
		// mismatch that constructor itself crashes (EXCEPTION_ACCESS_VIOLATION in GetInterfaceAddress, the original crash
		// of this mod on newer game builds). So only what the reflected read returns is logged.
		bool bFound = false;
		const UObject* Resource = ReadExtractorResource(Extractor, bFound);
		UE_LOG(LogFicsitStatistics, Log, TEXT("Layout check: Extractor.ExtractableResource read by reflection (%s), not compared with the inline accessor, which crashes on a mismatched layout"),
			bFound ? *Describe(Resource) : TEXT("property not found"));
	}

	if (Check.Mismatches > 0)
	{
		UE_LOG(LogFicsitStatistics, Warning, TEXT("Layout check: %d of %d reads disagree with the game's inline accessors. The game build is newer than the headers this mod was compiled against; the mod reads these members by reflection so it stays correct."),
			Check.Mismatches, Check.Checked);
	}
	else if (Check.Checked > 0)
	{
		UE_LOG(LogFicsitStatistics, Log, TEXT("Layout check: all %d reads agree with the game's inline accessors."), Check.Checked);
	}
}

TSubclassOf<UFGItemDescriptor> FicsitGame::ExtractorItem(const AFGBuildableResourceExtractor* Extractor)
{
	if (!IsValid(Extractor))
	{
		return nullptr;
	}

	bool bFound = false;
	UObject* ResourceObject = ReadExtractorResource(Extractor, bFound);
	if (!bFound)
	{
		// The property is gone or changed type (a future game update). Fall back to the accessor rather than counting nothing.
		const TScriptInterface<IFGExtractableResourceInterface> Resource = Extractor->GetExtractableResource();
		return Resource.GetInterface() ? Resource.GetInterface()->GetResourceClass() : nullptr;
	}

	if (!IsValid(ResourceObject))
	{
		return nullptr;
	}
	IFGExtractableResourceInterface* Resource = Cast<IFGExtractableResourceInterface>(ResourceObject);
	return Resource ? Resource->GetResourceClass() : nullptr;
}
