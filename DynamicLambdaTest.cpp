#include "DynamicLambdaTest.h"
#include "DynamicLambda.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DynamicLambdaTestInternals
{
	UWorld* GetWorld()
	{
#if WITH_EDITOR
		if (GIsEditor)
		{
			return GWorld;
		}
#endif // WITH_EDITOR
		return GEngine->GetWorldContexts()[0].World();
	}

	struct FAliveTestFunctor
	{
		FAliveTestFunctor(int32& InCount) : CountRef(InCount) { CountRef++; }
		FAliveTestFunctor(FAliveTestFunctor&& Other) noexcept : CountRef(Other.CountRef) { CountRef++; }
		FAliveTestFunctor(const FAliveTestFunctor& Other) : CountRef(Other.CountRef) { CountRef++; }
		~FAliveTestFunctor() { CountRef--; }
		
		void operator()() const {};
		int32& CountRef;
	};
}

FDynamicLambdaTestBase::FDynamicLambdaTestBase(const FString& InName, const bool bInComplexTask)
	: FAutomationTestBase(InName, bInComplexTask)
{
}

TArray<UDynamicLambdaTest*> MakeTestObjects(int32 Count)
{
	TArray<UDynamicLambdaTest*> Result;

	for (int32 Idx = 0; Idx != Count; ++Idx)
	{
		Result.Add(NewObject<UDynamicLambdaTest>());
	}
	
	return Result;
}

// Bind lambda to dynamic delegate and execute it. Lambda must be invoked
bool FBoundToDynamicDelegateLambdaInvoking::RunTest(const FString& Parameters)
{
	UDynamicLambdaTest* Test = NewObject<UDynamicLambdaTest>();
	bool LambdaInvoked = false; 

	FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(Test->SimpleTestDelegate, [&]
	{
		LambdaInvoked = true;
	}, __FILE__, __LINE__);

	TestTrue("Delegate bound", Test->SimpleTestDelegate.IsBound());
	Test->SimpleTestDelegate.ExecuteIfBound();
	TestTrue("Lambda invoked", LambdaInvoked);
	
	return LambdaInvoked;
}

// Bind lambda to dynamic multicast delegate and broadcast it. Lambda must be invoked
bool FBoundToDynamicMulticastDelegateLambdaInvoking::RunTest(const FString& Parameters)
{
	UDynamicLambdaTest* Test = NewObject<UDynamicLambdaTest>();
	bool LambdaInvoked = false;

	FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(Test->SimpleTestMulticastDelegate, [&]
	{
		LambdaInvoked = true;
	}, __FILE__, __LINE__);

	TestTrue("Delegate bound", Test->SimpleTestMulticastDelegate.IsBound());
	Test->SimpleTestMulticastDelegate.Broadcast();
	TestTrue("Lambda invoked", LambdaInvoked);

	return LambdaInvoked;
}

bool FLambdaBoundToDifferentDynamicDelegateInvokingOnlyOncePerExecution::RunTest(const FString& Parameters)
{
	TArray<UDynamicLambdaTest*> TestObjects = MakeTestObjects(5);
	int32 InvocationCounter = 0; 

	for (UDynamicLambdaTest* TestObject : TestObjects)
	{
		FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(TestObject->SimpleTestDelegate, [&]
		{
			InvocationCounter++;
		}, __FILE__, __LINE__);
	}

	TestEqual("Lambda wasn't invoked during binding", InvocationCounter, 0);

	for (int32 Idx = 0; Idx != TestObjects.Num(); ++Idx)
	{
		TestObjects[Idx]->SimpleTestDelegate.Execute();
		TestEqual("Lambda was invoked as planned", InvocationCounter, Idx + 1);
	}
	
	return InvocationCounter == TestObjects.Num();
}

bool FLambdaBoundToDifferentDynamicMulticastDelegateInvokingOnlyOncePerBroadcast::RunTest(const FString& Parameters)
{
	TArray<UDynamicLambdaTest*> TestObjects = MakeTestObjects(5);
	int32 InvocationCounter = 0; 

	for (UDynamicLambdaTest* TestObject : TestObjects)
	{
		FDynamicLambdaManager::Get().BindLambdaToDynamicDelegate(TestObject->SimpleTestMulticastDelegate, [&]
		{
			InvocationCounter++;
		}, __FILE__, __LINE__);
	}

	TestEqual("Lambda wasn't invoked during binding", InvocationCounter, 0);

	for (int32 Idx = 0; Idx != TestObjects.Num(); ++Idx)
	{
		TestObjects[Idx]->SimpleTestMulticastDelegate.Broadcast();
		TestEqual("Lambda was invoked as planned", InvocationCounter, Idx + 1);
	}
	
	return InvocationCounter == TestObjects.Num();
}

bool FDynamicLambdaWorksAfterGC::RunTest(const FString& Parameters)
{
	TWeakObjectPtr<UDynamicLambdaTest> Test = NewObject<UDynamicLambdaTest>();
	Test->AddToRoot();
	
	int32 AliveCount = 0;
	bool LambdaInvoked = false;
	Test->SimpleTestMulticastDelegate += DynamicLambdaTestInternals::FAliveTestFunctor(AliveCount);
	Test->SimpleTestMulticastDelegate += [&] { LambdaInvoked = true; };

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	TestTrue("Object is alive after GC", Test.IsValid());
	TestEqual("FAliveTestFunctor is alive", AliveCount, 1);

	Test->SimpleTestMulticastDelegate.Broadcast();
	TestTrue("Lambda is invocable after GC", LambdaInvoked);

	Test->RemoveFromRoot();
	return LambdaInvoked && AliveCount == 1;
}

// Bind lambda to delegate and trigger GC. Delegate owner dies and lambda must be freed
bool FDynamicLambdaIsDestroyedAfterGCIfDelegateOwnerIsKilled::RunTest(const FString& Parameters)
{
	int32 AliveCount = 0;
	
	TWeakObjectPtr<UDynamicLambdaTest> Test = NewObject<UDynamicLambdaTest>();
	Test->SimpleTestDelegate += DynamicLambdaTestInternals::FAliveTestFunctor(AliveCount);
	TestTrue("Object is alive before GC", Test.IsValid());
	TestEqual("Only one instance of FAliveTestFunctor is alive", AliveCount, 1);

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	TestFalse("Object is dead after GC", Test.IsValid());
	TestTrue("Lambda was freed", AliveCount == 0);

	return AliveCount == 0;
}

// Test of short subscription form (via +=) 
bool FShortSubscriptionFormTest::RunTest(const FString& Parameters)
{
	UDynamicLambdaTest* Test = NewObject<UDynamicLambdaTest>();
	int32 InvocationCounter = 0;
	int32 WeakInvocationCounter = 0;

	Test->SimpleTestDelegate += [&] { InvocationCounter++; };
	Test->SimpleTestMulticastDelegate += [&] { InvocationCounter++; };
	Test->SimpleTestDelegate.Execute();
	Test->SimpleTestMulticastDelegate.Broadcast();
	TestEqual("Lambdas invoked via short subscription form", InvocationCounter, 2);

	UDynamicLambdaTest* Test2 = NewObject<UDynamicLambdaTest>();
	UDynamicLambdaReceiverTest* ReceiverTest = NewObject<UDynamicLambdaReceiverTest>();

	Test2->SimpleTestDelegate += (ReceiverTest, [&]{ WeakInvocationCounter++; });
	Test2->SimpleTestMulticastDelegate += (ReceiverTest, [&]{ WeakInvocationCounter++; });
	Test2->SimpleTestDelegate.Execute();
	Test2->SimpleTestMulticastDelegate.Broadcast();
	TestEqual("Weak lambdas invoked via short subscription form", WeakInvocationCounter, 2);
	
	return InvocationCounter == 2 && WeakInvocationCounter == 2;
}

bool FBoundWeakLambdaIsDestroyedAfterOwnerDestroy::RunTest(const FString& Parameters)
{
	TWeakObjectPtr<UDynamicLambdaTest> Test = NewObject<UDynamicLambdaTest>();
	TWeakObjectPtr<UDynamicLambdaTest> WeakTest = NewObject<UDynamicLambdaTest>();
	
	Test->AddToRoot();
	int32 AliveCount = 0;
	bool LambdaInvoked = false;
	Test->SimpleTestDelegate += (WeakTest, [&] { LambdaInvoked = true; });
	Test->SimpleTestMulticastDelegate += (WeakTest, DynamicLambdaTestInternals::FAliveTestFunctor(AliveCount));

	TestTrue("Dynamic delegate bound", Test->SimpleTestDelegate.IsBound());
	TestTrue("Dynamic multicast delegate bound", Test->SimpleTestMulticastDelegate.IsBound());
	TestEqual("Lambda lives", AliveCount, 1);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	
	TestTrue("Strong object lives", Test.IsValid());
	TestFalse("Weak object is dead", WeakTest.IsValid());

	Test->SimpleTestDelegate.ExecuteIfBound();
	Test->SimpleTestMulticastDelegate.Broadcast();

	TestEqual("Lambda dead", AliveCount, 0);
	TestFalse("Lambda isn't invoked", LambdaInvoked);
	TestFalse("Dynamic delegate not bound after GC", Test->SimpleTestDelegate.IsBound());
	TestFalse("Dynamic multicast delegate not bound after GC", Test->SimpleTestMulticastDelegate.IsBound());

	Test->RemoveFromRoot();
	return AliveCount == 0 && LambdaInvoked == false;
}

bool FUFunctionListClearedAfterGC::RunTest(const FString& Parameters)
{
	// Trigger GC to make clean start conditions
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);

	TWeakObjectPtr<UDynamicLambdaTest> Test = NewObject<UDynamicLambdaTest>();
	TWeakObjectPtr<UDummy> DummyObj = NewObject<UDummy>();

	UClass* DummyClass = DummyObj->GetClass();
	TestEqual("UDummy's class has no native functions", DummyClass->NativeFunctionLookupTable.Num(), 0);

	Test->SimpleTestDelegate += (DummyObj, [] {});
	TestEqual("UDummy's class has one native function", DummyClass->NativeFunctionLookupTable.Num(), 1);

	FName FuncName = DummyClass->NativeFunctionLookupTable[0].Name;
	UFunction* Function = DummyClass->FindFunctionByName(FuncName);
	TestNotNull("UDummy's class has that UFunction", Function);

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	Function = DummyClass->FindFunctionByName(FuncName);

	TestEqual("UDummy's class has no native functions after GC", DummyClass->NativeFunctionLookupTable.Num(), 0);
	TestNull("UDummy's class hasn't that UFunction", Function);

	return Function == nullptr;
}

bool FUFunctionsReusedAfterAfterGC::RunTest(const FString& Parameters)
{
	// Trigger GC to make clean start conditions
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	
	TWeakObjectPtr<UDynamicLambdaTest> Test = NewObject<UDynamicLambdaTest>();
	TWeakObjectPtr<UDummy> DummyObj = NewObject<UDummy>();
	UClass* DummyClass = DummyObj->GetClass();
	bool LambdaInvoked = false;

	Test->SimpleTestDelegate += (DummyObj, [] {});
	FName FuncName = DummyClass->NativeFunctionLookupTable[0].Name;
	UFunction* Function = DummyClass->FindFunctionByName(FuncName);

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, false);
	Test = NewObject<UDynamicLambdaTest>();
	DummyObj = NewObject<UDummy>();
	
	Test->SimpleTestDelegate += (DummyObj, [&] { LambdaInvoked = true; });
	FName NewFuncName = DummyClass->NativeFunctionLookupTable[0].Name;
	UFunction* NewFunction = DummyClass->FindFunctionByName(NewFuncName);
	Test->SimpleTestDelegate.Execute();
	
	TestNotEqual("New lambda has new name", FuncName, NewFuncName);
	TestEqual("New lambda's UFunction has the same address", Function, NewFunction);
	TestTrue("Lambda works after UFunction reuse", LambdaInvoked);

	return FuncName != NewFuncName && Function == NewFunction && LambdaInvoked;
}

#endif // WITH_DEV_AUTOMATION_TESTS
