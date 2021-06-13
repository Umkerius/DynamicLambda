#pragma once
#include <Misc/AutomationTest.h>
#include "DynamicLambdaTest.generated.h"

DECLARE_DYNAMIC_DELEGATE(FSimpleTestDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSimpleTestMulticastDelegate);

UCLASS()
class UDynamicLambdaTest : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY()
	FSimpleTestDelegate SimpleTestDelegate;

	UPROPERTY()
	FSimpleTestMulticastDelegate SimpleTestMulticastDelegate;
};

UCLASS()
class UDynamicLambdaReceiverTest : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION()
	void Receive() { InvocationCount++; }

	UFUNCTION()
	void Dummy() {}

	int32 InvocationCount = 0;
};

UCLASS()
class UDummy : public UObject
{
	GENERATED_BODY()
};

#if WITH_DEV_AUTOMATION_TESTS

class FDynamicLambdaTestBase : public FAutomationTestBase
{
public:
	FDynamicLambdaTestBase(const FString& InName, const bool bInComplexTask);
};

#define IMPLEMENT_DYNAMIC_LAMBDA_TEST(Name) \
	IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST( \
		F##Name, \
		FDynamicLambdaTestBase, \
		"Orbit.Generic.DynamicLambda." #Name, \
		EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter);

IMPLEMENT_DYNAMIC_LAMBDA_TEST(BoundToDynamicDelegateLambdaInvoking);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(BoundToDynamicMulticastDelegateLambdaInvoking);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(LambdaBoundToDifferentDynamicDelegateInvokingOnlyOncePerExecution);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(LambdaBoundToDifferentDynamicMulticastDelegateInvokingOnlyOncePerBroadcast);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(DynamicLambdaWorksAfterGC);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(DynamicLambdaIsDestroyedAfterGCIfDelegateOwnerIsKilled);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(ShortSubscriptionFormTest);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(BoundWeakLambdaIsDestroyedAfterOwnerDestroy);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(UFunctionListClearedAfterGC);
IMPLEMENT_DYNAMIC_LAMBDA_TEST(UFunctionsReusedAfterAfterGC);

#undef IMPLEMENT_DYNAMIC_LAMBDA_TEST
#endif // WITH_DEV_AUTOMATION_TESTS
