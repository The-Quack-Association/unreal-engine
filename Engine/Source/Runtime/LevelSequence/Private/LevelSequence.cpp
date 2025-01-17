// Copyright Epic Games, Inc. All Rights Reserved.

#include "LevelSequence.h"
#include "ILevelSequenceMetaData.h"
#include "Engine/EngineTypes.h"
#include "HAL/IConsoleManager.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "LevelSequenceDirector.h"
#include "Engine/Engine.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "Animation/AnimInstance.h"
#include "LevelSequenceModule.h"
#include "MovieSceneSpawnableAnnotation.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"
#include "Tracks/MovieSceneMaterialParameterCollectionTrack.h"
#include "Tracks/MovieSceneSlomoTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Modules/ModuleManager.h"
#include "LevelSequencePlayer.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "Engine/AssetUserData.h"


#if WITH_EDITOR
	#include "UObject/SequencerObjectVersion.h"
	#include "UObject/ObjectRedirector.h"
#endif

static TAutoConsoleVariable<int32> CVarDefaultEvaluationType(
	TEXT("LevelSequence.DefaultEvaluationType"),
	0,
	TEXT("0: Playback locked to playback frames\n1: Unlocked playback with sub frame interpolation"),
	ECVF_Default);

static TAutoConsoleVariable<FString> CVarDefaultTickResolution(
	TEXT("LevelSequence.DefaultTickResolution"),
	TEXT("24000fps"),
	TEXT("Specifies default a tick resolution for newly created level sequences. Examples: 30 fps, 120/1 (120 fps), 30000/1001 (29.97), 0.01s (10ms)."),
	ECVF_Default);

static TAutoConsoleVariable<FString> CVarDefaultDisplayRate(
	TEXT("LevelSequence.DefaultDisplayRate"),
	TEXT("30fps"),
	TEXT("Specifies default a display frame rate for newly created level sequences; also defines frame locked frame rate where sequences are set to be frame locked. Examples: 30 fps, 120/1 (120 fps), 30000/1001 (29.97), 0.01s (10ms)."),
	ECVF_Default);

ULevelSequence::ULevelSequence(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, MovieScene(nullptr)
{
	bParentContextsAreSignificant = true;
}

void ULevelSequence::Initialize()
{
	MovieScene = NewObject<UMovieScene>(this, NAME_None, RF_Transactional);

	const bool bFrameLocked = CVarDefaultEvaluationType.GetValueOnGameThread() != 0;

	MovieScene->SetEvaluationType( bFrameLocked ? EMovieSceneEvaluationType::FrameLocked : EMovieSceneEvaluationType::WithSubFrames );

	FFrameRate TickResolution(60000, 1);
	TryParseString(TickResolution, *CVarDefaultTickResolution.GetValueOnGameThread());
	MovieScene->SetTickResolutionDirectly(TickResolution);

	FFrameRate DisplayRate(30, 1);
	TryParseString(DisplayRate, *CVarDefaultDisplayRate.GetValueOnGameThread());
	MovieScene->SetDisplayRate(DisplayRate);
}

UObject* ULevelSequence::MakeSpawnableTemplateFromInstance(UObject& InSourceObject, FName ObjectName)
{
	UObject* NewInstance = NewObject<UObject>(MovieScene, InSourceObject.GetClass(), ObjectName);

	UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
	CopyParams.bNotifyObjectReplacement = false;
	CopyParams.bPreserveRootComponent = false;
	UEngine::CopyPropertiesForUnrelatedObjects(&InSourceObject, NewInstance, CopyParams);

	AActor* Actor = CastChecked<AActor>(NewInstance);
	if (Actor->GetAttachParentActor() != nullptr)
	{
		// We don't support spawnables and attachments right now
		// @todo: map to attach track?
		Actor->DetachFromActor(FDetachmentTransformRules(FAttachmentTransformRules(EAttachmentRule::KeepRelative, false), false));
	}

	// The spawnable source object was created with RF_Transient. The object generated from that needs its 
	// component flags cleared of RF_Transient so that the template object can be saved to the level sequence.
	for (UActorComponent* Component : Actor->GetComponents())
	{
		if (Component)
		{
			Component->ClearFlags(RF_Transient);
		}
	}

	return NewInstance;
}

bool ULevelSequence::CanAnimateObject(UObject& InObject) const 
{
	return InObject.IsA<AActor>() || InObject.IsA<UActorComponent>() || InObject.IsA<UAnimInstance>();
}

#if WITH_EDITOR

ETrackSupport ULevelSequence::IsTrackSupported(TSubclassOf<class UMovieSceneTrack> InTrackClass) const
{
	if (InTrackClass == UMovieSceneAudioTrack::StaticClass() ||
		InTrackClass == UMovieSceneCameraCutTrack::StaticClass() ||
		InTrackClass == UMovieSceneCinematicShotTrack::StaticClass() ||
		InTrackClass == UMovieSceneEventTrack::StaticClass() ||
		InTrackClass == UMovieSceneFadeTrack::StaticClass() ||
		InTrackClass == UMovieSceneLevelVisibilityTrack::StaticClass() ||
		InTrackClass == UMovieSceneMaterialParameterCollectionTrack::StaticClass() ||
		InTrackClass == UMovieSceneSlomoTrack::StaticClass() ||
		InTrackClass == UMovieSceneSpawnTrack::StaticClass() ||
		InTrackClass == UMovieSceneSubTrack::StaticClass())
	{
		return ETrackSupport::Supported;
	}

	return Super::IsTrackSupported(InTrackClass);
}

void ULevelSequence::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
#if WITH_EDITORONLY_DATA
	if (DirectorBlueprint)
	{
		DirectorBlueprint->GetAssetRegistryTags(OutTags);
	}
#endif

	for (UObject* MetaData : MetaDataObjects)
	{
		ILevelSequenceMetaData* MetaDataInterface = Cast<ILevelSequenceMetaData>(MetaData);
		if (MetaDataInterface)
		{
			MetaDataInterface->ExtendAssetRegistryTags(OutTags);
		}
	}

	Super::GetAssetRegistryTags(OutTags);
}

void ULevelSequence::GetAssetRegistryTagMetadata(TMap<FName, FAssetRegistryTagMetadata>& OutMetadata) const
{
	for (UObject* MetaData : MetaDataObjects)
	{
		ILevelSequenceMetaData* MetaDataInterface = Cast<ILevelSequenceMetaData>(MetaData);
		if (MetaDataInterface)
		{
			MetaDataInterface->ExtendAssetRegistryTagMetaData(OutMetadata);
		}
	}

	Super::GetAssetRegistryTagMetadata(OutMetadata);
}

void PurgeLegacyBlueprints(UObject* InObject, UPackage* Package)
{
	if (UBlueprint* BP = Cast<UBlueprint>(InObject))
	{
		UPackage* TransientPackage = GetTransientPackage();

		{
			FString OldName = BP->GetName();

			BP->ClearFlags(RF_Public);
			BP->SetFlags(RF_Transient);
			BP->RemoveFromRoot();

			FName NewName = MakeUniqueObjectName(TransientPackage, UBlueprint::StaticClass(), *FString::Printf(TEXT("DEAD_SPAWNABLE_BLUEPRINT_%s"), *BP->GetName()));
			BP->Rename(*NewName.ToString(), GetTransientPackage(), (REN_NonTransactional|REN_ForceNoResetLoaders|REN_DoNotDirty));

			UE_LOG(LogLevelSequence, Log, TEXT("Discarding blueprint '%s' from package '%s'."), *OldName, *Package->GetName());
		}

		if (BP->GeneratedClass)
		{
			FName    OldName    = BP->GeneratedClass->GetFName();
			UObject* OldOuter   = BP->GeneratedClass->GetOuter();
			UClass*  SuperClass = BP->GeneratedClass->GetSuperClass();

			if( BP->GeneratedClass->ClassDefaultObject )
			{
				BP->GeneratedClass->ClassDefaultObject->ClearFlags(RF_Public);
				BP->GeneratedClass->ClassDefaultObject->SetFlags(RF_Transient);
				BP->GeneratedClass->ClassDefaultObject->RemoveFromRoot();
			}

			BP->GeneratedClass->ClearFlags(RF_Public);
			BP->GeneratedClass->SetFlags(RF_Transient);
			BP->GeneratedClass->ClassFlags |= CLASS_Deprecated;
			BP->GeneratedClass->RemoveFromRoot();

			FName NewName = MakeUniqueObjectName(TransientPackage, BP->GeneratedClass, *FString::Printf(TEXT("DEAD_SPAWNABLE_BP_CLASS_%s_C"), *BP->GeneratedClass->ClassGeneratedBy->GetName()));
			BP->GeneratedClass->Rename(*NewName.ToString(), GetTransientPackage(), (REN_DoNotDirty|REN_NonTransactional|REN_ForceNoResetLoaders));

			if (SuperClass)
			{
				UObjectRedirector* Redirector = NewObject<UObjectRedirector>(OldOuter, OldName);
				Redirector->DestinationObject = SuperClass;

				UE_LOG(LogLevelSequence, Log, TEXT("Discarding generated class '%s' from package '%s'. Replacing with redirector to '%s'"), *OldName.ToString(), *Package->GetName(), *SuperClass->GetName());
			}
			else
			{
				UE_LOG(LogLevelSequence, Log, TEXT("Discarding generated class '%s' from package '%s'. Unable to create redirector due to no super class."), *OldName.ToString(), *Package->GetName());
			}
		}
	}
}
#endif

void ULevelSequence::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

#if WITH_EDITORONLY_DATA
	if (DirectorBlueprint)
	{
		DirectorClass = DirectorBlueprint->GeneratedClass.Get();

		// Remove the binding for the director blueprint recompilation and re-add it to be sure there is only one entry in the list
		DirectorBlueprint->OnCompiled().RemoveAll(this);
		DirectorBlueprint->OnCompiled().AddUObject(this, &ULevelSequence::OnDirectorRecompiled);
	}
	else
	{
		DirectorClass = nullptr;
	}
#endif
}

void ULevelSequence::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITOR
	if (!DirectorBlueprint)
	{
		UBlueprint* PhantomDirector = FindObject<UBlueprint>(this, TEXT("SequenceDirector"));
		if (!ensureMsgf(!PhantomDirector, TEXT("Phantom sequence director found in sequence '%s' which has a nullptr DirectorBlueprint. Re-assigning to prevent future crash."), *GetName()))
		{
			DirectorBlueprint = PhantomDirector;
		}
	}

	if (DirectorBlueprint)
	{
		DirectorBlueprint->ClearFlags(RF_Standalone);

		// Remove the binding for the director blueprint recompilation and re-add it to be sure there is only one entry in the list
		DirectorBlueprint->OnCompiled().RemoveAll(this);
		DirectorBlueprint->OnCompiled().AddUObject(this, &ULevelSequence::OnDirectorRecompiled);
	}

	TSet<FGuid> InvalidSpawnables;

	for (int32 Index = 0; Index < MovieScene->GetSpawnableCount(); ++Index)
	{
		FMovieSceneSpawnable& Spawnable = MovieScene->GetSpawnable(Index);
		if (!Spawnable.GetObjectTemplate())
		{
			if (Spawnable.GeneratedClass_DEPRECATED && Spawnable.GeneratedClass_DEPRECATED->ClassGeneratedBy)
			{
				const FName TemplateName = MakeUniqueObjectName(MovieScene, UObject::StaticClass(), Spawnable.GeneratedClass_DEPRECATED->ClassGeneratedBy->GetFName());

				UObject* NewTemplate = NewObject<UObject>(MovieScene, Spawnable.GeneratedClass_DEPRECATED->GetSuperClass(), TemplateName);
				if (NewTemplate)
				{
					Spawnable.CopyObjectTemplate(*NewTemplate, *this);
				}
			}
		}

		if (!Spawnable.GetObjectTemplate())
		{
			InvalidSpawnables.Add(Spawnable.GetGuid());
			UE_LOG(LogLevelSequence, Warning, TEXT("Discarding spawnable with ID '%s' since its generated class could not produce to a template actor"), *Spawnable.GetGuid().ToString());
		}
	}

	for (FGuid& ID : InvalidSpawnables)
	{
		MovieScene->RemoveSpawnable(ID);
	}

	if (GetLinkerCustomVersion(FSequencerObjectVersion::GUID) < FSequencerObjectVersion::PurgeSpawnableBlueprints)
	{
		// Remove any old generated classes from the package that will have been left behind from when we used blueprints for spawnables		
		{
			UPackage* Package = GetOutermost();
			TArray<UObject*> PackageSubobjects;
			GetObjectsWithOuter(Package, PackageSubobjects, false);
			for (UObject* ObjectInPackage : PackageSubobjects)
			{
				PurgeLegacyBlueprints(ObjectInPackage, Package);
			}
		}

		// Remove any invalid object bindings
		TSet<FGuid> ValidObjectBindings;
		for (int32 Index = 0; Index < MovieScene->GetSpawnableCount(); ++Index)
		{
			ValidObjectBindings.Add(MovieScene->GetSpawnable(Index).GetGuid());
		}
		for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
		{
			ValidObjectBindings.Add(MovieScene->GetPossessable(Index).GetGuid());
		}

		BindingReferences.RemoveInvalidBindings(ValidObjectBindings);
	}

#endif
}

bool ULevelSequence::Rename(const TCHAR* NewName, UObject* NewOuter, ERenameFlags Flags)
{
	bool bRetVal = Super::Rename(NewName, NewOuter, Flags);

#if WITH_EDITOR
	if (DirectorBlueprint)
	{
		DirectorBlueprint->Rename(*DirectorBlueprint->GetName(), this, Flags);
	}
#endif

	return bRetVal;
}

void ULevelSequence::ConvertPersistentBindingsToDefault(UObject* FixupContext)
{
	if (PossessedObjects_DEPRECATED.Num() == 0)
	{
		return;
	}

	MarkPackageDirty();
	for (auto& Pair : PossessedObjects_DEPRECATED)
	{
		UObject* Object = Pair.Value.GetObject();
		if (Object)
		{
			FGuid ObjectId;
			FGuid::Parse(Pair.Key, ObjectId);
			BindingReferences.AddBinding(ObjectId, Object, FixupContext);
		}
	}
	PossessedObjects_DEPRECATED.Empty();
}

void ULevelSequence::BindPossessableObject(const FGuid& ObjectId, UObject& PossessedObject, UObject* Context)
{
	if (Context)
	{
		BindingReferences.AddBinding(ObjectId, &PossessedObject, Context);
	}
}

bool ULevelSequence::CanPossessObject(UObject& Object, UObject* InPlaybackContext) const
{
	return Object.IsA<AActor>() || Object.IsA<UActorComponent>() || Object.IsA<UAnimInstance>();
}

void ULevelSequence::LocateBoundObjects(const FGuid& ObjectId, UObject* Context, TArray<UObject*, TInlineAllocator<1>>& OutObjects) const
{
	LocateBoundObjects(ObjectId, Context, NAME_None, OutObjects);
}

void ULevelSequence::LocateBoundObjects(const FGuid& ObjectId, UObject* Context, FName StreamedLevelAssetPath, TArray<UObject*, TInlineAllocator<1>>& OutObjects) const
{
	// Handle legacy object references
	UObject* Object = Context ? ObjectReferences.ResolveBinding(ObjectId, Context) : nullptr;
	if (Object)
	{
		OutObjects.Add(Object);
	}

	BindingReferences.ResolveBinding(ObjectId, Context, StreamedLevelAssetPath, OutObjects);
}

void ULevelSequence::GatherExpiredObjects(const FMovieSceneObjectCache& InObjectCache, TArray<FGuid>& OutInvalidIDs) const
{
	for (const FGuid& ObjectId : BindingReferences.GetBoundAnimInstances())
	{
		for (TWeakObjectPtr<> WeakObject : InObjectCache.IterateBoundObjects(ObjectId))
		{
			UAnimInstance* AnimInstance = Cast<UAnimInstance>(WeakObject.Get());
			if (!AnimInstance || !AnimInstance->GetOwningComponent() || AnimInstance->GetOwningComponent()->GetAnimInstance() != AnimInstance)
			{
				OutInvalidIDs.Add(ObjectId);
			}
		}
	}
}

UMovieScene* ULevelSequence::GetMovieScene() const
{
	return MovieScene;
}

UObject* ULevelSequence::GetParentObject(UObject* Object) const
{
	if (UActorComponent* Component = Cast<UActorComponent>(Object))
	{
		return Component->GetOwner();
	}

	if (UAnimInstance* AnimInstance = Cast<UAnimInstance>(Object))
	{
		if (AnimInstance->GetWorld())
		{
			return AnimInstance->GetOwningComponent();
		}
	}

	return nullptr;
}

bool ULevelSequence::AllowsSpawnableObjects() const
{
	return true;
}

bool ULevelSequence::CanRebindPossessable(const FMovieScenePossessable& InPossessable) const
{
	return !InPossessable.GetParent().IsValid();
}

void ULevelSequence::UnbindPossessableObjects(const FGuid& ObjectId)
{
	BindingReferences.RemoveBinding(ObjectId);

	// Legacy object references
	ObjectReferences.Map.Remove(ObjectId);
}

void ULevelSequence::UnbindObjects(const FGuid& ObjectId, const TArray<UObject*>& InObjects, UObject* InContext)
{
	BindingReferences.RemoveObjects(ObjectId, InObjects, InContext);
}

void ULevelSequence::UnbindInvalidObjects(const FGuid& ObjectId, UObject* InContext)
{
	BindingReferences.RemoveInvalidObjects(ObjectId, InContext);
}

#if WITH_EDITOR

UBlueprint* ULevelSequence::GetDirectorBlueprint() const
{
	return DirectorBlueprint;
}

void ULevelSequence::SetDirectorBlueprint(UBlueprint* NewDirectorBlueprint)
{
	if (DirectorBlueprint)
	{
		DirectorBlueprint->OnCompiled().RemoveAll(this);
	}

	DirectorBlueprint = NewDirectorBlueprint;

	if (DirectorBlueprint)
	{
		DirectorClass = NewDirectorBlueprint->GeneratedClass.Get();
		DirectorBlueprint->OnCompiled().AddUObject(this, &ULevelSequence::OnDirectorRecompiled);
	}
	else
	{
		DirectorClass = nullptr;
	}

	MarkAsChanged();

	FMovieSceneCompiledDataID DataID = UMovieSceneCompiledDataManager::GetPrecompiledData()->GetDataID(this);
	UMovieSceneCompiledDataManager::GetPrecompiledData()->DestroyTemplate(DataID);
}

void ULevelSequence::OnDirectorRecompiled(UBlueprint* InCompiledBlueprint)
{
	ensure(InCompiledBlueprint == DirectorBlueprint);
	DirectorClass = DirectorBlueprint->GeneratedClass.Get();

	MarkAsChanged();

	FMovieSceneCompiledDataID DataID = UMovieSceneCompiledDataManager::GetPrecompiledData()->GetDataID(this);
	UMovieSceneCompiledDataManager::GetPrecompiledData()->DestroyTemplate(DataID);
}

FGuid ULevelSequence::FindOrAddBinding(UObject* InObject)
{
	UObject* PlaybackContext = InObject ? InObject->GetWorld() : nullptr;
	if (!InObject || !PlaybackContext)
	{
		return FGuid();
	}

	AActor* Actor = Cast<AActor>(InObject);
	// @todo: sequencer-python: need to figure out how we go from a spawned object to an object binding without the spawn register or any IMovieScenePlayer interface
	// Normally this process would happen through sequencer, since it has more context than just the level sequence asset.
	// For now we cannot possess spawnables or anything within them since we have no way of retrieving the spawnable from the object
	if (Actor && Actor->ActorHasTag("SequencerActor"))
	{
		TOptional<FMovieSceneSpawnableAnnotation> Annotation = FMovieSceneSpawnableAnnotation::Find(Actor);
		if (Annotation.IsSet() && Annotation->OriginatingSequence == this)
		{
			return Annotation->ObjectBindingID;
		}

		UE_LOG(LogLevelSequence, Error, TEXT("Unable to possess object '%s' since it is, or is part of a spawnable that is not in this sequence."), *InObject->GetName());
		return FGuid();
	}

	UObject* ParentObject = GetParentObject(InObject);
	FGuid    ParentGuid   = ParentObject ? FindOrAddBinding(ParentObject) : FGuid();

	if (ParentObject && !ParentGuid.IsValid())
	{
		UE_LOG(LogLevelSequence, Error, TEXT("Unable to possess object '%s' because it's parent could not be bound."), *InObject->GetName());
		return FGuid();
	}

	// Perform a potentially slow lookup of every possessable binding in the sequence to see if we already have this
	{
		class FTransientPlayer : public IMovieScenePlayer
		{
		public:
			FMovieSceneRootEvaluationTemplateInstance Template;
			virtual FMovieSceneRootEvaluationTemplateInstance& GetEvaluationTemplate() override { check(false); return Template; }
			virtual void UpdateCameraCut(UObject* CameraObject, const EMovieSceneCameraCutParams& CameraCutParams) override {}
			virtual void SetViewportSettings(const TMap<FViewportClient*, EMovieSceneViewportParams>& ViewportParamsMap) override {}
			virtual void GetViewportSettings(TMap<FViewportClient*, EMovieSceneViewportParams>& ViewportParamsMap) const override {}
			virtual EMovieScenePlayerStatus::Type GetPlaybackStatus() const { return EMovieScenePlayerStatus::Stopped; }
			virtual void SetPlaybackStatus(EMovieScenePlayerStatus::Type InPlaybackStatus) override {}
		} Player;

		Player.State.AssignSequence(MovieSceneSequenceID::Root, *this, Player);

		FGuid ExistingID = Player.FindObjectId(*InObject, MovieSceneSequenceID::Root);
		if (ExistingID.IsValid())
		{
			return ExistingID;
		}
	}

	// We have to possess this object
	if (!CanPossessObject(*InObject, PlaybackContext))
	{
		return FGuid();
	}

	FString NewName = Actor ? Actor->GetActorLabel() : InObject->GetName();

	const FGuid NewGuid = MovieScene->AddPossessable(NewName, InObject->GetClass());

	// Attempt to use the parent as a context if necessary
	UObject* BindingContext = ParentObject && AreParentContextsSignificant() ? ParentObject : PlaybackContext;

	// Set up parent/child guids for possessables within spawnables
	if (ParentGuid.IsValid())
	{
		FMovieScenePossessable* ChildPossessable = MovieScene->FindPossessable(NewGuid);
		if (ensure(ChildPossessable))
		{
			ChildPossessable->SetParent(ParentGuid);
		}

		FMovieSceneSpawnable* ParentSpawnable = MovieScene->FindSpawnable(ParentGuid);
		if (ParentSpawnable)
		{
			ParentSpawnable->AddChildPossessable(NewGuid);
		}
	}

	BindPossessableObject(NewGuid, *InObject, BindingContext);

	return NewGuid;

}

FGuid ULevelSequence::CreatePossessable(UObject* ObjectToPossess)
{
	return FindOrAddBinding(ObjectToPossess);
}

FGuid ULevelSequence::CreateSpawnable(UObject* ObjectToSpawn)
{
	if (!MovieScene || !ObjectToSpawn)
	{
		return FGuid();
	}

	TArray<TSharedRef<IMovieSceneObjectSpawner>> ObjectSpawners;

	// In order to create a spawnable, we have to instantiate all the relevant object spawners for level sequences, and try to create a spawnable from each
	FLevelSequenceModule& LevelSequenceModule = FModuleManager::LoadModuleChecked<FLevelSequenceModule>("LevelSequence");
	LevelSequenceModule.GenerateObjectSpawners(ObjectSpawners);

	// The first object spawner to return a valid result will win
	for (TSharedRef<IMovieSceneObjectSpawner> Spawner : ObjectSpawners)
	{
		TValueOrError<FNewSpawnable, FText> Result = Spawner->CreateNewSpawnableType(*ObjectToSpawn, *MovieScene, nullptr);
		if (Result.IsValid())
		{
			FNewSpawnable& NewSpawnable = Result.GetValue();

			NewSpawnable.Name = MovieSceneHelpers::MakeUniqueSpawnableName(MovieScene, NewSpawnable.Name);			

			FGuid NewGuid = MovieScene->AddSpawnable(NewSpawnable.Name, *NewSpawnable.ObjectTemplate);

			UMovieSceneSpawnTrack* NewSpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(NewGuid);
			if (NewSpawnTrack)
			{
				NewSpawnTrack->AddSection(*NewSpawnTrack->CreateNewSection());
			}
			return NewGuid;
		}
	}

	return FGuid();
}

#endif // WITH_EDITOR

UObject* ULevelSequence::CreateDirectorInstance(IMovieScenePlayer& Player)
{
	ULevelSequencePlayer* LevelSequencePlayer = Cast<ULevelSequencePlayer>(Player.AsUObject());
	UObject*              DirectorOuter       = LevelSequencePlayer ? LevelSequencePlayer : Player.GetPlaybackContext();

	if (DirectorClass && DirectorOuter && DirectorClass->IsChildOf(ULevelSequenceDirector::StaticClass()))
	{
		FName DirectorName = NAME_None;

#if WITH_EDITOR
		// Give it a pretty name so it shows up in the debug instances drop down nicely
		DirectorName = MakeUniqueObjectName(DirectorOuter, DirectorClass, *(GetFName().ToString() + TEXT("_Director")));
#endif

		ULevelSequenceDirector* NewDirector = NewObject<ULevelSequenceDirector>(DirectorOuter, DirectorClass, DirectorName, RF_Transient);
		NewDirector->Player = LevelSequencePlayer;
		NewDirector->OnCreated();
		return NewDirector;
	}

	return nullptr;
}

void ULevelSequence::AddAssetUserData(UAssetUserData* InUserData)
{
	if (InUserData != NULL)
	{
		UAssetUserData* ExistingData = GetAssetUserDataOfClass(InUserData->GetClass());
		if (ExistingData != NULL)
		{
			AssetUserData.Remove(ExistingData);
		}
		AssetUserData.Add(InUserData);
	}
}

UAssetUserData* ULevelSequence::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return NULL;
}

void ULevelSequence::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIdx);
			return;
		}
	}
}

const TArray<UAssetUserData*>* ULevelSequence::GetAssetUserDataArray() const
{
	return &AssetUserData;
}