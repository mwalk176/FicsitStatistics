#include "UI/FicsitPlayerPrefs.h"

#include "FicsitStatistics.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FGGameState.h"
#include "Misc/ConfigCacheIni.h"
#include "Resources/FGItemDescriptor.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * The session name, read through reflection. AFGGameState::GetSessionName() is an inline accessor, so it is compiled
	 * into this module with the class layout of the headers it was built against. When the game is a newer build than
	 * those headers it reads the wrong offset and returns a garbage FString, which crashes on copy. Reflection looks the
	 * property up in the running game, so it stays correct across builds.
	 */
	FString ReadSessionName(const AFGGameState* GameState)
	{
		if (!IsValid(GameState))
		{
			return FString();
		}
		static const FStrProperty* const NameProperty =
			CastField<FStrProperty>(AFGGameState::StaticClass()->FindPropertyByName(TEXT("mReplicatedSessionName")));
		if (!NameProperty)
		{
			// The property was renamed or removed. An empty name is the same as a client that hasn't received one yet.
			return FString();
		}
		return NameProperty->GetPropertyValue_InContainer(GameState);
	}

	const TCHAR* ConfigSection = TEXT("FicsitStatistics");
	const TCHAR* PinsKey = TEXT("PinnedItems");

	/**
	 * The config key for one world's pins, e.g. "PinnedItems.My_0020World". Anything but letters and digits is written
	 * as _ and its hex code, so that any session name makes a valid key and two names never share one.
	 */
	FString PinsKeyFor(const FString& SessionName)
	{
		FString Key = FString(PinsKey) + TEXT(".");
		for (const TCHAR Char : SessionName)
		{
			if (FChar::IsAlnum(Char) && Char < 128)
			{
				Key.AppendChar(Char);
			}
			else
			{
				Key += FString::Printf(TEXT("_%04X"), static_cast<uint32>(Char));
			}
		}
		return Key;
	}
}

namespace FicsitPlayerPrefs
{
	TArray<FString> GetArray(const TCHAR* Key)
	{
		TArray<FString> Values;
		GConfig->GetArray(ConfigSection, Key, Values, GGameUserSettingsIni);
		return Values;
	}

	void SetArray(const TCHAR* Key, const TArray<FString>& Values)
	{
		GConfig->SetArray(ConfigSection, Key, Values, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	int32 GetInt(const TCHAR* Key, int32 Default)
	{
		int32 Value = Default;
		GConfig->GetInt(ConfigSection, Key, Value, GGameUserSettingsIni);
		return Value;
	}

	void SetInt(const TCHAR* Key, int32 Value)
	{
		GConfig->SetInt(ConfigSection, Key, Value, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	bool GetBool(const TCHAR* Key, bool Default)
	{
		bool bValue = Default;
		GConfig->GetBool(ConfigSection, Key, bValue, GGameUserSettingsIni);
		return bValue;
	}

	void SetBool(const TCHAR* Key, bool bValue)
	{
		GConfig->SetBool(ConfigSection, Key, bValue, GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}

	void KeepAlive(const TCHAR* Key, TConstArrayView<UClass*> Classes)
	{
		static TMap<FString, TArray<TStrongObjectPtr<UClass>>> Kept;
		TArray<TStrongObjectPtr<UClass>>& Refs = Kept.FindOrAdd(Key);
		Refs.Reset();
		for (UClass* Class : Classes)
		{
			if (Class)
			{
				Refs.Emplace(Class);
			}
		}
	}
}

TArray<TSubclassOf<UFGItemDescriptor>> FFicsitPins::Items;
FString FFicsitPins::SessionName;
bool FFicsitPins::bLoaded = false;
int32 FFicsitPins::Revision = 0;

namespace
{
	void KeepPinsAlive(const TArray<TSubclassOf<UFGItemDescriptor>>& Items)
	{
		TArray<UClass*> Classes;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Items)
		{
			Classes.Add(Item.Get());
		}
		FicsitPlayerPrefs::KeepAlive(PinsKey, Classes);
	}
}

void FFicsitPins::SetWorld(const UObject* WorldContext)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	const AFGGameState* GameState = World ? World->GetGameState<AFGGameState>() : nullptr;
	// Empty on a client until the session name replicates. The pins load once it arrives.
	const FString NewSessionName = ReadSessionName(GameState);
	if (NewSessionName == SessionName)
	{
		return;
	}
	SessionName = NewSessionName;
	bLoaded = false;
	Load();
	if (!SessionName.IsEmpty())
	{
		UE_LOG(LogFicsitStatistics, Log, TEXT("Pins: loaded %d for world \"%s\""), Items.Num(), *SessionName);
	}

	// Pins used to be one list shared by every world, which made a new game show the last world's pins. That list
	// can't be matched to a world, so it's dropped.
	static bool bRemovedSharedPins = false;
	if (!bRemovedSharedPins)
	{
		bRemovedSharedPins = true;
		if (GConfig->RemoveKey(ConfigSection, PinsKey, GGameUserSettingsIni))
		{
			GConfig->Flush(false, GGameUserSettingsIni);
			UE_LOG(LogFicsitStatistics, Log, TEXT("Pins: dropped the old pin list shared by every world"));
		}
	}
}

void FFicsitPins::Load()
{
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	Items.Reset();
	const TArray<FString> Paths = SessionName.IsEmpty() ? TArray<FString>() : FicsitPlayerPrefs::GetArray(*PinsKeyFor(SessionName));
	for (const FString& Path : Paths)
	{
		if (UClass* Class = FSoftClassPath(Path).TryLoadClass<UFGItemDescriptor>())
		{
			Items.AddUnique(Class);
		}
	}
	++Revision;
	KeepPinsAlive(Items);
}

void FFicsitPins::Save()
{
	KeepPinsAlive(Items);
	TArray<FString> Paths;
	for (const TSubclassOf<UFGItemDescriptor>& Item : Items)
	{
		if (Item)
		{
			Paths.Add(FSoftClassPath(Item.Get()).ToString());
		}
	}
	const FString Key = PinsKeyFor(SessionName);
	if (Paths.IsEmpty())
	{
		// No pins left, so drop the key rather than leave an empty entry per world behind.
		if (GConfig->RemoveKey(ConfigSection, *Key, GGameUserSettingsIni))
		{
			GConfig->Flush(false, GGameUserSettingsIni);
		}
		return;
	}
	FicsitPlayerPrefs::SetArray(*Key, Paths);
}

const TArray<TSubclassOf<UFGItemDescriptor>>& FFicsitPins::Get()
{
	Load();
	return Items;
}

bool FFicsitPins::IsPinned(TSubclassOf<UFGItemDescriptor> Item)
{
	return Get().Contains(Item);
}

void FFicsitPins::Toggle(TSubclassOf<UFGItemDescriptor> Item)
{
	// With no world there's nowhere to save the pin.
	if (!Item || SessionName.IsEmpty())
	{
		return;
	}
	Load();
	if (Items.Remove(Item) == 0)
	{
		Items.Add(Item);
	}
	++Revision;
	Save();
}

void FFicsitPins::Clear()
{
	Load();
	if (SessionName.IsEmpty() || Items.IsEmpty())
	{
		return;
	}
	UE_LOG(LogFicsitStatistics, Log, TEXT("Pins: cleared %d for world \"%s\""), Items.Num(), *SessionName);
	Items.Reset();
	++Revision;
	Save();
}

int32 FFicsitPins::GetRevision()
{
	Load();
	return Revision;
}
