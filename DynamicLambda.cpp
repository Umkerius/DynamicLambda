#include "DynamicLambda.h"

#include <chrono>
#include "Misc/CoreDelegates.h"
#include "Misc/StringBuilder.h"

TUniquePtr<FDynamicLambdaManager> GDynamicLambdaManager;

FDynamicLambdaManager& FDynamicLambdaManager::Get()
{
	if (!GDynamicLambdaManager.IsValid())
	{
		GDynamicLambdaManager = MakeUnique<FDynamicLambdaManager>();
	}

	return *GDynamicLambdaManager;
}

FName FDynamicLambdaManager::GenerateLambdaName(FAnsiStringView FileName, int32 LineNumber)
{
	static int32 ID = 0;
	TStringBuilder<256> Name;

	Name << "lambda_" << ++ID << '_' << FileName << ':' << LineNumber;
	
	return FName(Name.ToString());
}

FDynamicLambdaManager::FDynamicLambdaManager()
{
	auto PreGCHandler = [this] { OnPreGarbageCollect(); };
	auto PostGCHandler = [this] { OnPostGarbageCollect(); };
	EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([] { GDynamicLambdaManager.Reset(); });

	// Subscribe on GC to manage lambda's lifetime: it must be destroyed if delegate owner or subscriber is destroyed
	PreGarbageCollectHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda(PreGCHandler);
	PostGarbageCollectHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda(PostGCHandler);

	// Create an anonymous object to able binding lambda without UObject
	AnonymousObject = NewObject<UAnonymousObject>();
	AnonymousObject->AddToRoot();
}

FDynamicLambdaManager::~FDynamicLambdaManager()
{
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGarbageCollectHandle);
	FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);

	AnonymousObject->RemoveFromRoot();
	AnonymousObject->MarkPendingKill();
}

void FDynamicLambdaManager::CreateLambdaRouter(UClass* ObjectClass, FName LambdaName)
{
	ObjectClass->AddNativeFunction(*LambdaName.ToString(), RouteToLambda);

	UFunction* Function = CreateFunction(ObjectClass, LambdaName);
	Function->Bind();
		
	ObjectClass->AddFunctionToFunctionMap(Function, LambdaName);
}

UFunction* FDynamicLambdaManager::CreateFunction(UClass* ObjectClass, FName Name)
{
	if (FunctionPool.Num() != 0)
	{
		UFunction* Function = FunctionPool.Pop(false);
		Function->Rename(*Name.ToString(), ObjectClass, REN_DontCreateRedirectors | REN_DoNotDirty);

		checkf(Function->GetFName() == Name, TEXT("Name is different"));
		return Function;
	}

	EObjectFlags ObjectFlags = RF_Public | RF_MarkAsNative | RF_Transient;
	EFunctionFlags FunctionFlags = FUNC_Public | FUNC_Native | FUNC_Final;

	return new (EC_InternalUseOnlyConstructor, ObjectClass, *Name.ToString(), ObjectFlags) UFunction(
		FObjectInitializer(),
		nullptr,
		FunctionFlags,
		0
	);
}

void FDynamicLambdaManager::RouteToLambda(UObject* Context, FFrame& Stack, RESULT_DECL)
{
	P_FINISH;
	P_NATIVE_BEGIN;

	FName LambdaName = Stack.CurrentNativeFunction->GetFName();
	Get().Storage[Context->GetClass()][LambdaName].Lambda();
			
	P_NATIVE_END;
}

void FDynamicLambdaManager::StoreLambda(FName LambdaName, UObject* Object, FDelegateData DelegateData, TFunction<void()>&& Lambda)
{
	UClass* ObjectClass = Object->GetClass();

	auto ClassStorage = Storage.Find(ObjectClass);
	if (ClassStorage == nullptr)
	{
		ClassStorage = &Storage.Add(ObjectClass);
	}

	FLambdaStorage& LambdaStorage = ClassStorage->Add(LambdaName);
	LambdaStorage.DelegateData = DelegateData;
	LambdaStorage.LambdaOwner = Object;
	LambdaStorage.Lambda = MoveTemp(Lambda);
}

void FDynamicLambdaManager::OnPreGarbageCollect()
{
	// All delegate owners must be resolved before GC, when all objects are alive
	// When user's code bind dynamic delegate only pointer to that delegate is known
	// Usually it's enough information to find delegate's owner: dynamic delegate usually marked as UPROPERTY
	// Every UObject's property has an offset relative to the UObject's class beginning
	// By using this information delegate's owner can be found in the system
	// Delegate owner resolving allow manager to destroy lambda that bound to GCed object
	FDelegateResolvingDataItems DelegatesToResolve;

	// First of all, gather all unresolved delegates (without owner)
	GatherDelegatesToResolve(DelegatesToResolve);
	if (DelegatesToResolve.Num() == 0)
	{
		// nothing to do
		return;
	}

	// Then find delegate owners in the global array of UObjects
	// This code are running in the GC operation context, so no one can change UObjects and parallelization is allowed
	typedef std::chrono::high_resolution_clock Clock;
	auto Start = Clock::now();
	ResolveDelegates(DelegatesToResolve);
	auto End = Clock::now();
	
	double Ms = (End - Start).count() / 1000000.0;
	UE_LOG(LogTemp, Display, TEXT("Delegates resolving time: %f ms"), Ms);
}

void FDynamicLambdaManager::OnPostGarbageCollect()
{
	// Time after GC is perfect time to process some housekeeping tasks
	for (auto& ClassKV : Storage)
	{
		// Gather lambdas bound to GCed objects
		TArray<FName, TInlineAllocator<64>> LambdasToRemove;
		for (auto& KV : ClassKV.Value)
		{
			if (!KV.Value.IsValid())
			{
				LambdasToRemove.Add(KV.Key);
			}
		}

		// Clean up. Your cpt
		for (FName LambdaToRemove : LambdasToRemove)
		{
			CleanUpLambda(ClassKV.Key, LambdaToRemove);
		}
	}
}

void FDynamicLambdaManager::GatherDelegatesToResolve(FDelegateResolvingDataItems& DelegatesToResolve)
{
	for (auto& ClassKV : Storage)
	{
		for (auto& KV : ClassKV.Value)
		{
			// Empty DelegateOwner means that it's never been resolved
			// If lambda owner is already dead, skip resolving: lambda will be destroyed after GC 
			if (KV.Value.DelegateOwner.IsExplicitlyNull() && KV.Value.LambdaOwner.IsValid())
			{
				DelegatesToResolve.Add({ KV.Value, KV.Key });
			}
		}
	}

	// Sort items to allow skipping UObjects located in memory after delegates to resolve
	DelegatesToResolve.Sort([] (const FDelegateResolvingData& Lhs, const FDelegateResolvingData& Rhs)
	{
		return Lhs.DelegateData.Pointer < Rhs.DelegateData.Pointer;
	});
}

void FDynamicLambdaManager::ResolveDelegates(FDelegateResolvingDataItems& ObjectsToResolve)
{
	std::atomic<int32> ResolvedDelegatesCounter;

	int32 MaxObjects = GUObjectArray.GetObjectArrayNum() - GUObjectArray.GetFirstGCIndex();
	int32 Threads = FMath::Max(1, FTaskGraphInterface::Get().GetNumWorkerThreads());
	int32 ObjectsPerThread = (GUObjectArray.GetObjectArrayNum() / Threads) + 1;

	// Iterate over all objects in parallel way
	// ObjectsToResolve doesn't need any synchronization: there are no intersections between threads
	// All threads are processing different UObject sets
	ParallelFor(Threads, [&] (int32 Id)
	{
		int32 FirstObjectIndex = Id * ObjectsPerThread + GUObjectArray.GetFirstGCIndex();
		int32 NumObjects = Id < Threads - 1 ? ObjectsPerThread : MaxObjects - (Threads - 1) * ObjectsPerThread;
		int32 LastObjectIndex = FMath::Min(GUObjectArray.GetObjectArrayNum() - 1, FirstObjectIndex + NumObjects - 1);

		for (int32 Idx = FirstObjectIndex; Idx <= LastObjectIndex; ++Idx)
		{
			FUObjectItem* Item = GUObjectArray.IndexToObjectUnsafeForGC(Idx);
			TryResolveDelegate(Item, ObjectsToResolve, ResolvedDelegatesCounter);

			// at the end of iteration check if all delegates were resolved
			if (ResolvedDelegatesCounter.load(std::memory_order_relaxed) == ObjectsToResolve.Num())
			{
				break;
			}
		}
	});
}

void FDynamicLambdaManager::TryResolveDelegate(FUObjectItem* Item, FDelegateResolvingDataItems& ObjectsToResolve, std::atomic<int32>& Counter)
{
	UObject* Object = static_cast<UObject*>(Item->Object);
	if (ShouldSkipObject(Item, ObjectsToResolve.Last().DelegateData.Pointer))
	{
		return;
	}

	const char* ObjectBytePtr = reinterpret_cast<const char*>(Object); 
	for (TFieldIterator<FProperty> PropsIt(Object->GetClass()); PropsIt; ++PropsIt)
	{
		for (FDelegateResolvingData& ObjectToResolve : ObjectsToResolve)
		{
			const void* Pointer = ObjectToResolve.DelegateData.Pointer;
			if ((Pointer != nullptr) & (ObjectBytePtr + PropsIt->GetOffset_ForInternal() == Pointer))
			{
				// check that found property literally is the same delegate
				if (IsTheSameDelegate(Pointer, *PropsIt, ObjectToResolve))
				{
					// delegate owner found, mark it as resolved
					*ObjectToResolve.DelegateOwnerPtr = Object;
					ObjectToResolve.DelegateData.Pointer = nullptr;
					++Counter;
				}
			}
		}
	}
}

bool FDynamicLambdaManager::IsTheSameDelegate(const void* Pointer, FProperty* Property, const FDelegateResolvingData& ObjectToResolve)
{
	if (ObjectToResolve.DelegateData.IsMulticast && Property->IsA<FMulticastDelegateProperty>())
	{
		const FMulticastScriptDelegate* Delegate = static_cast<const FMulticastScriptDelegate*>(Pointer);
		return Delegate->Contains(ObjectToResolve.LambdaOwner.Get(), ObjectToResolve.LambdaName);
	}
	
	if (!ObjectToResolve.DelegateData.IsMulticast && Property->IsA<FDelegateProperty>())
	{
		const FScriptDelegate* Delegate = static_cast<const FScriptDelegate*>(Pointer);
		return Delegate->GetUObject() == ObjectToResolve.LambdaOwner.Get() &&
			Delegate->GetFunctionName() == ObjectToResolve.LambdaName;
	}

	return false;
}

bool FDynamicLambdaManager::ShouldSkipObject(FUObjectItem* Item, const void* MaxDelegatePtr)
{
	UObject* Object = static_cast<UObject*>(Item->Object);

	// skip objects that located after the delegates
	if (Object == nullptr || MaxDelegatePtr < Object)
	{
		return true;
	}

	// skip objects that are about to be GCed or purged
	if (Item->HasAnyFlags(EInternalObjectFlags::PendingKill | EInternalObjectFlags::Unreachable))
	{
		return true;
	}

	// Skip CDOs and assets
	if (Object->HasAnyFlags(RF_ClassDefaultObject) || Object->IsAsset())
	{
		return true;
	}

	return false;
}

void FDynamicLambdaManager::CleanUpLambda(UClass* Class, FName LambdaName)
{
	// Remove lambda storage
	Storage[Class].Remove(LambdaName);

	// remove native function from class
	auto Pred = [=] (const auto& Item) { return Item.Name == LambdaName; };
	TArray<FNativeFunctionLookup>& LookupTable = Class->NativeFunctionLookupTable;
	int32 Index = LookupTable.IndexOfByPredicate(Pred);
	checkf(Index != -1, TEXT("Can't find lambda"));
	LookupTable.RemoveAtSwap(Index, 1, false);

	// remove lambda's UFunction and put it to pool
	UFunction* Function = Class->FindFunctionByName(LambdaName);
	Class->RemoveFunctionFromFunctionMap(Function);
	FunctionPool.Add(Function);
}
