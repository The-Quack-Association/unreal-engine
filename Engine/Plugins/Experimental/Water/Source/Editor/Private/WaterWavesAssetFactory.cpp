// Copyright Epic Games, Inc. All Rights Reserved.

#include "WaterWavesAssetFactory.h"
#include "AssetTypeCategories.h"
#include "WaterWaves.h"

#define LOCTEXT_NAMESPACE "WaterWavesAssetFactory"

UWaterWavesAssetFactory::UWaterWavesAssetFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UWaterWavesAsset::StaticClass();
}

UObject* UWaterWavesAssetFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	UWaterWavesAsset* NewAsset = NewObject<UWaterWavesAsset>(InParent, Class, Name, Flags | RF_Transactional);
	return NewAsset;
}

FText UWaterWavesAssetFactory::GetToolTip() const
{
	return LOCTEXT("WavesAssetTooltip", "testing");
}

#undef LOCTEXT_NAMESPACE