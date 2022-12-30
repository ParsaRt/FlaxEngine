// Copyright (c) 2012-2022 Wojciech Figat. All rights reserved.

#include "TestScripting.h"
#include "Engine/Scripting/Scripting.h"
#include <ThirdParty/catch2/catch.hpp>

TestClassNative::TestClassNative(const SpawnParams& params)
    : ScriptingObject(params)
{
}

TEST_CASE("Scripting")
{
    SECTION("Test Class")
    {
        // Test native class
        ScriptingTypeHandle type = Scripting::FindScriptingType("FlaxEngine.TestClassNative");
        CHECK(type == TestClassNative::TypeInitializer);
        ScriptingObject* object = Scripting::NewObject(type.GetType().ManagedClass);
        CHECK(object);
        CHECK(object->Is<TestClassNative>());
        TestClassNative* testClass = (TestClassNative*)object;
        CHECK(testClass->SimpleField == 1);
        CHECK(testClass->SimpleStruct.Object == nullptr);
        CHECK(testClass->SimpleStruct.Vector == Float3::One);
        int32 methodResult = testClass->TestMethod(TEXT("123"));
        CHECK(methodResult == 3);

        // Test managed class
        type = Scripting::FindScriptingType("FlaxEngine.TestClassManaged");
        CHECK(type);
        object = Scripting::NewObject(type.GetType().ManagedClass);
        CHECK(object);
        CHECK(object->Is<TestClassNative>());
        testClass = (TestClassNative*)object;
        MObject* managed = testClass->GetOrCreateManagedInstance(); // Ensure to create C# object and run it's ctor
        CHECK(managed);
        CHECK(testClass->SimpleField == 2);
        CHECK(testClass->SimpleStruct.Object == testClass);
        CHECK(testClass->SimpleStruct.Vector == Float3::UnitX);
        methodResult = testClass->TestMethod(TEXT("123"));
        CHECK(methodResult == 6);
    }

    SECTION("Test Event")
    {
        ScriptingTypeHandle type = Scripting::FindScriptingType("FlaxEngine.TestClassManaged");
        CHECK(type);
        ScriptingObject* object = Scripting::NewObject(type.GetType().ManagedClass);
        CHECK(object);
        MObject* managed = object->GetOrCreateManagedInstance(); // Ensure to create C# object and run it's ctor
        CHECK(managed);
        TestClassNative* testClass = (TestClassNative*)object;
        CHECK(testClass->SimpleField == 2);
        String str1 = TEXT("1");
        String str2 = TEXT("2");
        Array<TestStruct> arr1 = { testClass->SimpleStruct };
        Array<TestStruct> arr2 = { testClass->SimpleStruct };
        testClass->SimpleEvent(1, Float3::One, str1, str2, arr1, arr2);
        CHECK(testClass->SimpleField == 4);
        CHECK(str2 == TEXT("4"));
        CHECK(arr2.Count() == 2);
        CHECK(arr2[0].Vector == Float3::Half);
        CHECK(arr2[0].Object == nullptr);
        CHECK(arr2[1].Vector == testClass->SimpleStruct.Vector);
        CHECK(arr2[1].Object == testClass);
    }
}