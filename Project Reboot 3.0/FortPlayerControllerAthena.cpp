#include "FortPlayerControllerAthena.h"
#include "FortPlayerPawn.h"
#include "FortKismetLibrary.h"

#include "SoftObjectPtr.h"
#include "globals.h"
#include "GameplayStatics.h"
#include "hooking.h"
#include "FortAthenaMutator_GiveItemsAtGamePhaseStep.h"
#include "DataTableFunctionLibrary.h"
#include "FortAthenaMutator_InventoryOverride.h"

void AFortPlayerControllerAthena::StartGhostModeHook(UObject* Context, FFrame* Stack, void* Ret)
{
	LOG_INFO(LogDev, __FUNCTION__);

	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	UFortWorldItemDefinition* ItemProvidingGhostMode = nullptr;

	Stack->StepCompiledIn(&ItemProvidingGhostMode);

	if (!ItemProvidingGhostMode)
	{
		LOG_INFO(LogDev, "Null item!");
		return StartGhostModeOriginal(Context, Stack, Ret);
	}

	if (!PlayerController->HasAuthority()) // for real
		return StartGhostModeOriginal(Context, Stack, Ret);
	
	LOG_INFO(LogDev, "Attempting to give item {}", ItemProvidingGhostMode->IsValidLowLevel() ? ItemProvidingGhostMode->GetFullName() : "BadRead");

	auto GhostModeRepData = PlayerController->GetGhostModeRepData();

	if (GhostModeRepData->IsInGhostMode())
	{
		LOG_INFO(LogDev, "Player is already in ghost mode!");
		return StartGhostModeOriginal(Context, Stack, Ret);
	}

	auto WorldInventory = PlayerController->GetWorldInventory();
	
	if (!WorldInventory)
		return StartGhostModeOriginal(Context, Stack, Ret);

	bool bShouldUpdate = false;
	auto NewAndModifiedInstances = WorldInventory->AddItem(ItemProvidingGhostMode, &bShouldUpdate, 1);
	auto GhostModeItemInstance = NewAndModifiedInstances.first[0];

	if (!GhostModeItemInstance)
		return StartGhostModeOriginal(Context, Stack, Ret);

	if (bShouldUpdate)
		WorldInventory->Update();

	PlayerController->ServerExecuteInventoryItemHook(PlayerController, GhostModeItemInstance->GetItemEntry()->GetItemGuid());
	PlayerController->ClientEquipItem(GhostModeItemInstance->GetItemEntry()->GetItemGuid(), true);
	LOG_INFO(LogDev, "Finished!");

	return StartGhostModeOriginal(Context, Stack, Ret);
}

void AFortPlayerControllerAthena::EndGhostModeHook(AFortPlayerControllerAthena* PlayerController)
{
	// I believe there are a lot of other places we should remove it (go to XREFs of K2_RemoveItemFromPlayer on a version like 6.21, and there will be something checking ghost stuff).

	LOG_INFO(LogDev, __FUNCTION__);

	if (!PlayerController->HasAuthority()) // for real
		return EndGhostModeOriginal(PlayerController);

	auto WorldInventory = PlayerController->GetWorldInventory();

	if (!WorldInventory)
		return EndGhostModeOriginal(PlayerController);

	FGhostModeRepData* GhostModeRepData = PlayerController->GetGhostModeRepData();
	UFortWorldItemDefinition* GhostModeItemDef = GhostModeRepData->GetGhostModeItemDef();

	LOG_INFO(LogDev, "GhostModeItemDef: {}", GhostModeItemDef->IsValidLowLevel() ? GhostModeItemDef->GetFullName() : "BadRead");

	if (!GhostModeItemDef) // bro IDFK
	{
		GhostModeItemDef = FindObject<UFortWorldItemDefinition>("/Game/Athena/Items/Gameplay/SpookyMist/AGID_SpookyMist.AGID_SpookyMist");
	}

	if (!GhostModeItemDef)
		return EndGhostModeOriginal(PlayerController);

	auto GhostModeItemInstance = WorldInventory->FindItemInstance(GhostModeItemDef);

	LOG_INFO(LogDev, "GhostModeItemInstance: {}", GhostModeItemInstance->IsValidLowLevel() ? GhostModeItemInstance->GetFullName() : "BadRead");

	if (!GhostModeItemInstance)
		return EndGhostModeOriginal(PlayerController);

	bool bShouldUpdate = false;
	int Count = GhostModeItemInstance->GetItemEntry()->GetCount(); // 1
	bool bForceRemoval = true; // false
	WorldInventory->RemoveItem(GhostModeItemInstance->GetItemEntry()->GetItemGuid(), &bShouldUpdate, Count, bForceRemoval);

	if (bShouldUpdate)
		WorldInventory->Update();

	return EndGhostModeOriginal(PlayerController);
}

void AFortPlayerControllerAthena::EnterAircraftHook(UObject* PC, AActor* Aircraft)
{
	auto PlayerController = Cast<AFortPlayerController>(Engine_Version < 424 ? PC : ((UActorComponent*)PC)->GetOwner());
	
	if (!PlayerController)
		return;

	// LOG_INFO(LogDev, "EnterAircraftHook");

	EnterAircraftOriginal(PC, Aircraft);

	// TODO Check if the player successfully got in the aircraft.

	auto WorldInventory = PlayerController->GetWorldInventory();

	if (!WorldInventory)
		return;

	std::vector<std::pair<FGuid, int>> GuidAndCountsToRemove;

	auto& InventoryList = WorldInventory->GetItemList();

	auto& ItemInstances = InventoryList.GetItemInstances();

	for (int i = 0; i < ItemInstances.Num(); i++)
	{
		auto ItemEntry = ItemInstances.at(i)->GetItemEntry();
		auto ItemDefinition = Cast<UFortWorldItemDefinition>(ItemEntry->GetItemDefinition());

		if (!ItemDefinition)
			continue;

		if (!ItemDefinition->CanBeDropped())
			continue;

		GuidAndCountsToRemove.push_back({ ItemEntry->GetItemGuid(), ItemEntry->GetCount() });
	}

	for (auto& Pair : GuidAndCountsToRemove)
	{
		WorldInventory->RemoveItem(Pair.first, nullptr, Pair.second, true);
	}

	std::vector<std::pair<AFortAthenaMutator*, UFunction*>> FunctionsToCall;
	LoopMutators([&](AFortAthenaMutator* Mutator) { FunctionsToCall.push_back(std::make_pair(Mutator, Mutator->FindFunction("OnGamePhaseStepChanged"))); });

	auto HandleGiveItemsAtGamePhaseStepMutator = [&](AFortAthenaMutator* Mutator) {
		if (auto GiveItemsAtGamePhaseStepMutator = Cast<AFortAthenaMutator_GiveItemsAtGamePhaseStep>(Mutator))
		{
			auto PhaseToGive = GiveItemsAtGamePhaseStepMutator->GetPhaseToGiveItems();
			auto& ItemsToGive = GiveItemsAtGamePhaseStepMutator->GetItemsToGive();

			LOG_INFO(LogDev, "PhaseToGiveItems: {} ItemsToGive.Num(): {}", (int)PhaseToGive, ItemsToGive.Num());

			if (PhaseToGive <= 5) // Flying or lower
			{
				for (int j = 0; j < ItemsToGive.Num(); j++)
				{
					auto ItemToGive = ItemsToGive.AtPtr(j, FItemsToGive::GetStructSize());

					if (!ItemToGive->GetItemToDrop())
						continue;

					float Out2 = 0;

					if (!IsBadReadPtr(ItemToGive->GetNumberToGive().GetCurve().CurveTable, 8) && ItemToGive->GetNumberToGive().GetCurve().RowName.IsValid())
					{
						Out2 = UDataTableFunctionLibrary::EvaluateCurveTableRow(ItemToGive->GetNumberToGive().GetCurve().CurveTable, ItemToGive->GetNumberToGive().GetCurve().RowName, 0.f);
					}

					LOG_INFO(LogDev, "[{}] Out2: {} ItemToGive.ItemToDrop: {}", j, Out2, ItemToGive->GetItemToDrop()->IsValidLowLevel() ? ItemToGive->GetItemToDrop()->GetFullName() : "BadRead");

					if (!Out2) // ?
						continue;

					WorldInventory->AddItem(ItemToGive->GetItemToDrop(), nullptr, Out2);
				}
			}
		}
	};

	LoopMutators(HandleGiveItemsAtGamePhaseStepMutator);

	/* if (auto GGMutator = Cast<AFortAthenaMutator_GG>(Mutator))
	{
		auto& WeaponEntries = GGMutator->GetWeaponEntries();

		LOG_INFO(LogDev, "[{}] WeaponEntries.Num(): {}", i, WeaponEntries.Num());

		for (int j = 0; j < WeaponEntries.Num(); j++)
		{
			WorldInventory->AddItem(WeaponEntries.at(j).Weapon, nullptr, 1);
		}
	} */

	auto PlayerStateAthena = Cast<AFortPlayerStateAthena>(PlayerController->GetPlayerState());

	auto AddInventoryOverrideTeamLoadouts = [&](AFortAthenaMutator* Mutator)
	{
		if (auto InventoryOverride = Cast<AFortAthenaMutator_InventoryOverride>(Mutator))
		{
			auto TeamIndex = PlayerStateAthena->GetTeamIndex();
			auto LoadoutTeam = InventoryOverride->GetLoadoutTeamForTeamIndex(TeamIndex);

			if (LoadoutTeam.UpdateOverrideType == EAthenaInventorySpawnOverride::AircraftPhaseOnly)
			{
				auto LoadoutContainer = InventoryOverride->GetLoadoutContainerForTeamIndex(TeamIndex);

				for (int i = 0; i < LoadoutContainer.Loadout.Num(); i++)
				{
					auto& ItemAndCount = LoadoutContainer.Loadout.at(i);
					WorldInventory->AddItem(ItemAndCount.GetItem(), nullptr, ItemAndCount.GetCount());
				}
			}
		}
	};

	LoopMutators(AddInventoryOverrideTeamLoadouts);

	static int LastNum1 = 3125;

	if (LastNum1 != Globals::AmountOfListens)
	{
		LastNum1 = Globals::AmountOfListens;

		for (auto& FunctionToCallPair : FunctionsToCall)
		{
			// On newer versions there is a second param.

			LOG_INFO(LogDev, "FunctionToCallPair.second: {}", __int64(FunctionToCallPair.second));

			if (FunctionToCallPair.second)
			{
				{
					// mem leak btw

					auto a = ConstructOnGamePhaseStepChangedParams(EAthenaGamePhaseStep::GetReady);

					if (a)
					{
						FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, a);
						FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, ConstructOnGamePhaseStepChangedParams(EAthenaGamePhaseStep::BusLocked));
						FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, ConstructOnGamePhaseStepChangedParams(EAthenaGamePhaseStep::BusFlying));
					}

					// FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, &StormFormingGamePhaseStep);
					// FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, &StormHoldingGamePhaseStep);
					// FunctionToCallPair.first->ProcessEvent(FunctionToCallPair.second, &StormShrinkingGamePhaseStep);
				}
			}
		}
	}

	WorldInventory->Update();
	
	// Should we equip the pickaxe for older builds here?

	if (Fortnite_Version < 2.5) // idk
	{
		/* auto PickaxeInstance = WorldInventory->GetPickaxeInstance();

		if (!PickaxeInstance)
			return;
				
		AFortPlayerController::ServerExecuteInventoryItemHook(PlayerController, PickaxeInstance->GetItemEntry()->GetItemGuid()); */
	}
}

void AFortPlayerControllerAthena::ServerRequestSeatChangeHook(AFortPlayerControllerAthena* PlayerController, int TargetSeatIndex)
{
	auto Pawn = Cast<AFortPlayerPawn>(PlayerController->GetPawn());
	
	if (!Pawn)
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

	auto Vehicle = Pawn->GetVehicle();

	if (!Vehicle)
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

	auto OldVehicleWeaponDefinition = Pawn->GetVehicleWeaponDefinition(Vehicle);
	
	LOG_INFO(LogDev, "OldVehicleWeaponDefinition: {}", OldVehicleWeaponDefinition ? OldVehicleWeaponDefinition->GetFullName() : "BadRead");

	if (!OldVehicleWeaponDefinition)
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

	auto WorldInventory = PlayerController->GetWorldInventory();

	if (!WorldInventory)
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

	auto OldVehicleWeaponInstance = WorldInventory->FindItemInstance(OldVehicleWeaponDefinition);

	if (OldVehicleWeaponInstance)
	{
		bool bShouldUpdate = false;
		WorldInventory->RemoveItem(OldVehicleWeaponInstance->GetItemEntry()->GetItemGuid(), &bShouldUpdate, OldVehicleWeaponInstance->GetItemEntry()->GetCount(), true);

		if (bShouldUpdate)
			WorldInventory->Update();
	}

	auto RequestingVehicleWeaponDefinition = Vehicle->GetVehicleWeaponForSeat(TargetSeatIndex);

	if (!RequestingVehicleWeaponDefinition)
	{
		auto PickaxeInstance = WorldInventory->GetPickaxeInstance();

		if (!PickaxeInstance)
			return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

		AFortPlayerController::ServerExecuteInventoryItemHook(PlayerController, PickaxeInstance->GetItemEntry()->GetItemGuid()); // Bad, we should equip the last weapon.
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);
	}

	auto NewAndModifiedInstances = WorldInventory->AddItem(RequestingVehicleWeaponDefinition, nullptr);
	auto RequestedVehicleInstance = NewAndModifiedInstances.first[0];

	if (!RequestedVehicleInstance)
		return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);

	WorldInventory->Update();

	auto RequestedVehicleWeapon = Pawn->EquipWeaponDefinition(RequestingVehicleWeaponDefinition, RequestedVehicleInstance->GetItemEntry()->GetItemGuid());

	return ServerRequestSeatChangeOriginal(PlayerController, TargetSeatIndex);
}

void AFortPlayerControllerAthena::ServerRestartPlayerHook(AFortPlayerControllerAthena* Controller)
{
	static auto FortPlayerControllerZoneDefault = FindObject<UClass>(L"/Script/FortniteGame.Default__FortPlayerControllerZone");
	static auto ServerRestartPlayerFn = FindObject<UFunction>(L"/Script/Engine.PlayerController.ServerRestartPlayer");
	static auto ZoneServerRestartPlayer = __int64(FortPlayerControllerZoneDefault->VFTable[GetFunctionIdxOrPtr(ServerRestartPlayerFn) / 8]);
	static void (*ZoneServerRestartPlayerOriginal)(AFortPlayerController*) = decltype(ZoneServerRestartPlayerOriginal)(__int64(ZoneServerRestartPlayer));
	
	LOG_INFO(LogDev, "Call 0x{:x}!", ZoneServerRestartPlayer - __int64(_ReturnAddress()));
	return ZoneServerRestartPlayerOriginal(Controller);
}

void AFortPlayerControllerAthena::ServerGiveCreativeItemHook(AFortPlayerControllerAthena* Controller, FFortItemEntry CreativeItem)
{
	// Don't worry, the validate has a check if it is a creative enabled mode or not, but we need to add a volume check and permission check I think.

	auto CreativeItemPtr = &CreativeItem;
	auto ItemDefinition = CreativeItemPtr->GetItemDefinition();

	if (!ItemDefinition)
		return;

	bool bShouldUpdate = false;
	auto LoadedAmmo = -1; // CreativeItemPtr->GetLoadedAmmo()
	Controller->GetWorldInventory()->AddItem(ItemDefinition, &bShouldUpdate, CreativeItemPtr->GetCount(), LoadedAmmo, false);

	if (bShouldUpdate)
		Controller->GetWorldInventory()->Update(Controller);
}

void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIslandHook(AFortPlayerControllerAthena* Controller)
{
	auto Pawn = Controller->GetMyFortPawn();

	if (!Pawn)
		return;

	// TODO IsTeleportToCreativeHubAllowed

	static auto FortPlayerStartCreativeClass = FindObject<UClass>("/Script/FortniteGame.FortPlayerStartCreative");
	auto AllCreativePlayerStarts = UGameplayStatics::GetAllActorsOfClass(GetWorld(), FortPlayerStartCreativeClass);

	for (int i = 0; i < AllCreativePlayerStarts.Num(); i++)
	{
		auto CurrentPlayerStart = AllCreativePlayerStarts.at(i);

		static auto PlayerStartTagsOffset = CurrentPlayerStart->GetOffset("PlayerStartTags");
		auto bHasSpawnTag = CurrentPlayerStart->Get<FGameplayTagContainer>(PlayerStartTagsOffset).Contains("Playground.LobbyIsland.Spawn");

		if (!bHasSpawnTag)
			continue;

		Pawn->TeleportTo(CurrentPlayerStart->GetActorLocation(), Pawn->GetActorRotation());
		break;
	}

	AllCreativePlayerStarts.Free();
}

void AFortPlayerControllerAthena::ServerAcknowledgePossessionHook(APlayerController* Controller, APawn* Pawn)
{
	static auto AcknowledgedPawnOffset = Controller->GetOffset("AcknowledgedPawn");
	Controller->Get<APawn*>(AcknowledgedPawnOffset) = Pawn;

	auto ControllerAsFort = Cast<AFortPlayerController>(Controller);
	auto PawnAsFort = Cast<AFortPlayerPawn>(Pawn);
	auto PlayerStateAsFort = Cast<AFortPlayerState>(Pawn->GetPlayerState());

	if (!PawnAsFort)
		return;

	if (Globals::bNoMCP)
	{
		static auto CustomCharacterPartClass = FindObject<UClass>("/Script/FortniteGame.CustomCharacterPart");
		static auto backpackPart = LoadObject("/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack", CustomCharacterPartClass);

		// PawnAsFort->ServerChoosePart(EFortCustomPartType::Backpack, backpackPart);

		return;
	}

	ControllerAsFort->ApplyCosmeticLoadout();
}

void AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessageHook(AFortPlayerControllerAthena* PlayerController, __int64 ChatEntry, __int64 SenderID)
{
	using UAthenaEmojiItemDefinition = UFortItemDefinition;

	static auto EmojiComm = FindObject<UAthenaEmojiItemDefinition>("/Game/Athena/Items/Cosmetics/Dances/Emoji/Emoji_Comm.Emoji_Comm");
	PlayerController->ServerPlayEmoteItemHook(PlayerController, EmojiComm);
}

void AFortPlayerControllerAthena::GetPlayerViewPointHook(AFortPlayerControllerAthena* PlayerController, FVector& Location, FRotator& Rotation)
{
	// I don't know why but GetActorEyesViewPoint only works on some versions.
	/* static auto GetActorEyesViewPointFn = FindObject<UFunction>(L"/Script/Engine.Actor.GetActorEyesViewPoint");
	static auto GetActorEyesViewPointIndex = GetFunctionIdxOrPtr(GetActorEyesViewPointFn) / 8;

	void (*GetActorEyesViewPointOriginal)(AActor* Actor, FVector* OutLocation, FRotator* OutRotation) = decltype(GetActorEyesViewPointOriginal)(PlayerController->VFTable[GetActorEyesViewPointIndex]);
	return GetActorEyesViewPointOriginal(PlayerController, &Location, &Rotation); */

	if (auto MyFortPawn = PlayerController->GetMyFortPawn())
	{
		Location = MyFortPawn->GetActorLocation();
		Rotation = PlayerController->GetControlRotation();
		return;
	}

	return AFortPlayerControllerAthena::GetPlayerViewPointOriginal(PlayerController, Location, Rotation);
}

void AFortPlayerControllerAthena::ServerReadyToStartMatchHook(AFortPlayerControllerAthena* PlayerController)
{
	LOG_INFO(LogDev, "ServerReadyToStartMatch!");

	if (Fortnite_Version <= 2.5) // techinally we should do this at the end of OnReadyToStartMatch
	{
		static auto QuickBarsOffset = PlayerController->GetOffset("QuickBars", false);

		if (QuickBarsOffset != -1)
		{
			auto& QuickBars = PlayerController->Get<AActor*>(QuickBarsOffset);

			LOG_INFO(LogDev, "QuickBarsOld: {}", __int64(QuickBars));

			if (QuickBars)
				return ServerReadyToStartMatchOriginal(PlayerController);

			static auto FortQuickBarsClass = FindObject<UClass>("/Script/FortniteGame.FortQuickBars");

			QuickBars = GetWorld()->SpawnActor<AActor>(FortQuickBarsClass);

			LOG_INFO(LogDev, "QuickBarsNew: {}", __int64(QuickBars));

			if (!QuickBars)
				return ServerReadyToStartMatchOriginal(PlayerController);

			PlayerController->Get<AActor*>(QuickBarsOffset)->SetOwner(PlayerController);
		}
	}

	return ServerReadyToStartMatchOriginal(PlayerController);
}