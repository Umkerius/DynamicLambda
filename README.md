# DynamicLambda
Lambda support for Unreal Engine dynamic delegates\
This is experimental feature. Now only parametless lambdas are supported \
To see more details, explore tests and implementation :)

## How to use
Usage is very simple:
1. Copy files from repo to your project
2. Include DynamicLambda.h
3. Bind lambdas to your delegates

## Example
```c++
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

...

// Short subscription form is preffered
Test->SimpleTestDelegate += [&] { DoSomeStuff(); };

// To bind 'weak' lambda, use 'tuple' syntax
Test->SimpleTestDelegate += (MyObjectPtr, [&]{ DoSomeStuff(); });
```

## Next steps
1. Write some docs
2. Dedicate this code to plugin
3. Support all dynamic delegates with parameters
4. Implement unsubscription 
5. TBD
