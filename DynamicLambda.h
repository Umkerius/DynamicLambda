#pragma once
#include <CoreMinimal.h>
#include "DynamicLambda.generated.h"

UCLASS()
class UAnonymousObject : public UObject
{
	GENERATED_BODY()

	DECLARE_DYNAMIC_DELEGATE(FTestDelegate);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FTestMulticastDelegate);

	// Dynamic lambda's feature based on a few assumptions about inner delegate structures
	// One of them is: delegate class is inherited from some base class and only the very base class has data
	// According to this delegate pointer can be interpreted as pointer to base delegate
	static_assert(sizeof(FTestDelegate) == sizeof(TBaseDynamicDelegate<FWeakObjectPtr, void>),
		"Dynamic Delegate must have the same size as superclass");
	static_assert(sizeof(FTestDelegate) == sizeof(TScriptDelegate<FWeakObjectPtr>),
		"Dynamic Delegate must have the same size as superclass");

	static_assert(sizeof(FTestMulticastDelegate) == sizeof(TBaseDynamicMulticastDelegate<FWeakObjectPtr, void>),
		"Dynamic Multicast Delegate must have the same size as superclass");
	static_assert(sizeof(FTestMulticastDelegate) == sizeof(TMulticastScriptDelegate<FWeakObjectPtr>),
		"Dynamic Delegate must have the same size as superclass");
};

// All delegates data needed for its owner resolving
struct FDelegateData
{
	const void* Pointer; /* just a raw pointer to delegate */
	bool IsMulticast;	 /* dynamic multicast delegate flag */
};

struct FLambdaStorage
{
	FDelegateData DelegateData;
	TWeakObjectPtr<UObject> DelegateOwner;
	TWeakObjectPtr<UObject> LambdaOwner;
	TFunction<void()> Lambda;

	bool IsValid() const { return DelegateOwner.IsValid() && LambdaOwner.IsValid(); }
};

struct FDelegateResolvingData
{
	FDelegateResolvingData() = default;
	FDelegateResolvingData(const FDelegateResolvingData&) = default;
	
	FDelegateResolvingData(FLambdaStorage& Storage, FName InLambdaName)
		: DelegateData(Storage.DelegateData),
		DelegateOwnerPtr(&Storage.DelegateOwner),
		LambdaOwner(Storage.LambdaOwner),
		LambdaName(InLambdaName)
	{
	}
	
	FDelegateData DelegateData;
	TWeakObjectPtr<UObject>* DelegateOwnerPtr;
	TWeakObjectPtr<UObject> LambdaOwner;
	FName LambdaName;
};

class FDynamicLambdaManager
{
public:
	FDynamicLambdaManager();
	~FDynamicLambdaManager();

	static FDynamicLambdaManager& Get();
	static FName GenerateLambdaName(FAnsiStringView FileName, int32 LineNumber);

	template <typename TDelegate, typename TCallable>
	void BindLambdaToDynamicDelegate(TDelegate& Delegate, TCallable&& Callable, FAnsiStringView File, int32 Line);

	template <typename TDelegate, typename TCallable>
	void BindWeakLambdaToDynamicDelegate(UObject* Object, TDelegate& Delegate, TCallable&& Callable, FAnsiStringView File, int32 Line);

protected:
	void CreateLambdaRouter(UClass* ObjectClass, FName LambdaName);
	UFunction* CreateFunction(UClass* ObjectClass, FName Name);
	
	static void RouteToLambda(UObject* Context, FFrame& Stack, RESULT_DECL);
	void StoreLambda(FName LambdaName, UObject* Object, FDelegateData DelegateData, TFunction<void()>&& Lambda);

	template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
	static void BindDelegate(TBaseDynamicDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, UObject* Object, FName LambdaName);

	template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
	static void BindDelegate(TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, UObject* Object, FName LambdaName);

	template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
	static FDelegateData MakeDelegateData(TBaseDynamicDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate);

	template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
	static FDelegateData MakeDelegateData(TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate);
	
	void OnPreGarbageCollect();
	void OnPostGarbageCollect();

	using FDelegateResolvingDataItems = TArray<FDelegateResolvingData>;
	void GatherDelegatesToResolve(FDelegateResolvingDataItems& ObjectsToResolve);
	static void ResolveDelegates(FDelegateResolvingDataItems& ObjectsToResolve);
	static void TryResolveDelegate(FUObjectItem* Item, FDelegateResolvingDataItems& ObjectsToResolve, std::atomic<int32>& Counter);
	static bool IsTheSameDelegate(const void* Pointer, FProperty* Property, const FDelegateResolvingData& ObjectToResolve);
	static bool ShouldSkipObject(FUObjectItem* Item, const void* MaxDelegatePtr);
	void CleanUpLambda(UClass* Class, FName LambdaName);

	FDelegateHandle PreGarbageCollectHandle;
	FDelegateHandle PostGarbageCollectHandle;
	FDelegateHandle EnginePreExitHandle;
	UAnonymousObject* AnonymousObject;
	TMap<UClass*, TMap<FName, FLambdaStorage>> Storage;
	TArray<UFunction*> FunctionPool;
};

// ---------------------------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------------------------
template <typename TDelegate, typename TCallable>
void FDynamicLambdaManager::BindLambdaToDynamicDelegate(TDelegate& Delegate, TCallable&& Callable, FAnsiStringView File, int32 Line)
{
	BindWeakLambdaToDynamicDelegate(AnonymousObject, Delegate, Forward<TCallable>(Callable), File, Line);
}

template <typename TDelegate, typename TCallable>
void FDynamicLambdaManager::BindWeakLambdaToDynamicDelegate(UObject* Object, TDelegate& Delegate, TCallable&& Callable, FAnsiStringView File, int32 Line)
{
	FName LambdaName = GenerateLambdaName(File, Line);
	CreateLambdaRouter(Object->GetClass(), LambdaName);

	BindDelegate(Delegate, Object, LambdaName);

	TFunction<void()> Lambda = Forward<TCallable>(Callable);
	StoreLambda(LambdaName, Object, MakeDelegateData(Delegate), MoveTemp(Lambda));
}

template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
void FDynamicLambdaManager::BindDelegate(TBaseDynamicDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, UObject* Object, FName LambdaName)
{
	Delegate.BindUFunction(Object, LambdaName);
}

template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
void FDynamicLambdaManager::BindDelegate(TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, UObject* Object, FName LambdaName)
{
	using TDelegateType = typename TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>::FDelegate;
	
	TDelegateType SingleDelegate;
	SingleDelegate.BindUFunction(Object, LambdaName);
	Delegate.Add(SingleDelegate);
}

template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
FDelegateData FDynamicLambdaManager::MakeDelegateData(TBaseDynamicDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate)
{
	return { &Delegate, false };
}

template <typename TWeakPtr, typename RetValType, typename... ParamTypes>
FDelegateData FDynamicLambdaManager::MakeDelegateData(TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate)
{
	return { &Delegate, true };
}

// ---------------------------------------------------------------------------------------------------------------------
// Short subscription form
// ---------------------------------------------------------------------------------------------------------------------
template <typename TCallable, typename TWeakPtr, typename RetValType, typename... ParamTypes>
void operator+=(TBaseDynamicDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, TCallable&& Callable)
{
	FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(Delegate, Forward<TCallable>(Callable), "unknown", 0);
}

template <typename TCallable, typename TWeakPtr, typename RetValType, typename... ParamTypes>
void operator+=(TBaseDynamicMulticastDelegate<TWeakPtr, RetValType, ParamTypes...>& Delegate, TCallable&& Callable)
{
	FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(Delegate, Forward<TCallable>(Callable), "unknown", 0);
}

// python-like tuple support
template <typename TCallable>
TPair<UObject*, TCallable> operator,(TWeakObjectPtr<UObject> Object, TCallable&& Callable)
{
	return MakeTuple(Object.Get(), Forward<TCallable>(Callable));
}

template <typename TCallable, typename TWeakPtr, typename TRet, typename... TParamTypes>
void operator+=(TBaseDynamicDelegate<TWeakPtr, TRet, TParamTypes...>& Delegate, TPair<UObject*, TCallable>&& WeakCallable)
{
	FDynamicLambdaManager::Get()
		.BindWeakLambdaToDynamicDelegate(WeakCallable.Key, Delegate, MoveTemp(WeakCallable.Value), "unknown", 0);
}

template <typename TCallable, typename TWeakPtr, typename TRet, typename... TParamTypes>
void operator+=(TBaseDynamicMulticastDelegate<TWeakPtr, TRet, TParamTypes...>& Delegate, TPair<UObject*, TCallable>&& WeakCallable)
{
	FDynamicLambdaManager::Get()
		.BindWeakLambdaToDynamicDelegate(WeakCallable.Key, Delegate, MoveTemp(WeakCallable.Value), "unknown", 0);
}
