// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

#include "Engine/Scripting/Types.h"
#include "Engine/Scripting/ManagedCLR/MCore.h"

#if USE_NETCORE

#pragma warning(default : 4297)

#include "Engine/Core/Log.h"
#include "Engine/Core/Types/DateTime.h"
#include "Engine/Core/Types/TimeSpan.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/File.h"
#include "Engine/Platform/FileSystem.h"
#include "Engine/Scripting/ManagedCLR/MAssembly.h"
#include "Engine/Scripting/ManagedCLR/MClass.h"
#include "Engine/Scripting/ManagedCLR/MDomain.h"
#include "Engine/Scripting/ManagedCLR/MEvent.h"
#include "Engine/Scripting/ManagedCLR/MField.h"
#include "Engine/Scripting/ManagedCLR/MMethod.h"
#include "Engine/Scripting/ManagedCLR/MProperty.h"
#include "Engine/Scripting/ManagedCLR/MException.h"
#include "Engine/Scripting/ManagedCLR/MUtils.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Engine/Globals.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Debug/Exceptions/CLRInnerException.h"
#if DOTNET_HOST_CORECRL
#include <nethost.h>
#include <coreclr_delegates.h>
#include <hostfxr.h>
#elif DOTNET_HOST_MONO
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>
#include <mono/utils/mono-logger.h>
typedef char char_t;
#else
#error "Unknown .NET runtime host."
#endif
#if PLATFORM_WINDOWS
#include <combaseapi.h>
#undef SetEnvironmentVariable
#undef LoadLibrary
#undef LoadImage
#endif

#if defined(_WIN32)
#define CORECLR_DELEGATE_CALLTYPE __stdcall
#define FLAX_CORECLR_STRING String
#define FLAX_CORECLR_TEXT(x) TEXT(x)
#else
#define CORECLR_DELEGATE_CALLTYPE
#define FLAX_CORECLR_STRING StringAnsi
#define FLAX_CORECLR_TEXT(x) x
#endif

// System.Reflection.TypeAttributes
enum class MTypeAttributes : uint32
{
    VisibilityMask = 0x00000007,
    NotPublic = 0x00000000,
    Public = 0x00000001,
    NestedPublic = 0x00000002,
    NestedPrivate = 0x00000003,
    NestedFamily = 0x00000004,
    NestedAssembly = 0x00000005,
    NestedFamANDAssem = 0x00000006,
    NestedFamORAssem = 0x00000007,
    LayoutMask = 0x00000018,
    AutoLayout = 0x00000000,
    SequentialLayout = 0x00000008,
    ExplicitLayout = 0x00000010,
    ClassSemanticsMask = 0x00000020,
    Class = 0x00000000,
    Interface = 0x00000020,
    Abstract = 0x00000080,
    Sealed = 0x00000100,
    SpecialName = 0x00000400,
    Import = 0x00001000,
    Serializable = 0x00002000,
    WindowsRuntime = 0x00004000,
    StringFormatMask = 0x00030000,
    AnsiClass = 0x00000000,
    UnicodeClass = 0x00010000,
    AutoClass = 0x00020000,
    CustomFormatClass = 0x00030000,
    CustomFormatMask = 0x00C00000,
    BeforeFieldInit = 0x00100000,
    RTSpecialName = 0x00000800,
    HasSecurity = 0x00040000,
    ReservedMask = 0x00040800,
};

// System.Reflection.MethodAttributes
enum class MMethodAttributes : uint32
{
    MemberAccessMask = 0x0007,
    PrivateScope = 0x0000,
    Private = 0x0001,
    FamANDAssem = 0x0002,
    Assembly = 0x0003,
    Family = 0x0004,
    FamORAssem = 0x0005,
    Public = 0x0006,
    Static = 0x0010,
    Final = 0x0020,
    Virtual = 0x0040,
    HideBySig = 0x0080,
    CheckAccessOnOverride = 0x0200,
    VtableLayoutMask = 0x0100,
    ReuseSlot = 0x0000,
    NewSlot = 0x0100,
    Abstract = 0x0400,
    SpecialName = 0x0800,
    PinvokeImpl = 0x2000,
    UnmanagedExport = 0x0008,
    RTSpecialName = 0x1000,
    HasSecurity = 0x4000,
    RequireSecObject = 0x8000,
    ReservedMask = 0xd000,
};

// System.Reflection.FieldAttributes
enum class MFieldAttributes : uint32
{
    FieldAccessMask = 0x0007,
    PrivateScope = 0x0000,
    Private = 0x0001,
    FamANDAssem = 0x0002,
    Assembly = 0x0003,
    Family = 0x0004,
    FamORAssem = 0x0005,
    Public = 0x0006,
    Static = 0x0010,
    InitOnly = 0x0020,
    Literal = 0x0040,
    NotSerialized = 0x0080,
    SpecialName = 0x0200,
    PinvokeImpl = 0x2000,
    RTSpecialName = 0x0400,
    HasFieldMarshal = 0x1000,
    HasDefault = 0x8000,
    HasFieldRVA = 0x0100,
    ReservedMask = 0x9500,
};

DECLARE_ENUM_OPERATORS(MTypeAttributes);
DECLARE_ENUM_OPERATORS(MMethodAttributes);
DECLARE_ENUM_OPERATORS(MFieldAttributes);

extern MDomain* MRootDomain;
extern MDomain* MActiveDomain;
extern Array<MDomain*, FixedAllocation<4>> MDomains;

Dictionary<String, void*> CachedFunctions;
const char_t* NativeInteropTypeName = FLAX_CORECLR_TEXT("FlaxEngine.NativeInterop, FlaxEngine.CSharp");

Dictionary<void*, MClass*> classHandles;
Dictionary<void*, MAssembly*> assemblyHandles;

/// <summary>
/// Returns the function pointer to the managed static method in NativeInterop class.
/// </summary>
void* GetStaticMethodPointer(const String& methodName);

/// <summary>
/// Calls the managed static method in NativeInterop class with given parameters.
/// </summary>
template<typename RetType, typename... Args>
inline RetType CallStaticMethodByName(const String& methodName, Args... args)
{
    typedef RetType (CORECLR_DELEGATE_CALLTYPE* fun)(Args...);
    return ((fun)GetStaticMethodPointer(methodName))(args...);
}

/// <summary>
/// Calls the managed static method with given parameters.
/// </summary>
template<typename RetType, typename... Args>
inline RetType CallStaticMethod(void* methodPtr, Args... args)
{
    typedef RetType (CORECLR_DELEGATE_CALLTYPE* fun)(Args...);
    return ((fun)methodPtr)(args...);
}

void RegisterNativeLibrary(const char* moduleName, const char* modulePath)
{
    static void* RegisterNativeLibraryPtr = GetStaticMethodPointer(TEXT("RegisterNativeLibrary"));
    CallStaticMethod<void, const char*, const char*>(RegisterNativeLibraryPtr, moduleName, modulePath);
}

bool InitHostfxr(const String& configPath, const String& libraryPath);
void ShutdownHostfxr();

MAssembly* GetAssembly(void* assemblyHandle);
MClass* GetClass(void* typeHandle);
MClass* GetOrCreateClass(void* typeHandle);

bool HasCustomAttribute(const MClass* klass, const MClass* attributeClass);
bool HasCustomAttribute(const MClass* klass);
void* GetCustomAttribute(const MClass* klass, const MClass* attributeClass);

// Structures used to pass information from runtime, must match with the structures in managed side
struct NativeClassDefinitions
{
    void* typeHandle;
    const char* name;
    const char* fullname;
    const char* namespace_;
    MTypeAttributes typeAttributes;
};

struct NativeMethodDefinitions
{
    const char* name;
    int numParameters;
    void* handle;
    MMethodAttributes methodAttributes;
};

struct NativeFieldDefinitions
{
    const char* name;
    void* fieldHandle;
    void* fieldType;
    MFieldAttributes fieldAttributes;
};

struct NativePropertyDefinitions
{
    const char* name;
    void* getterHandle;
    void* setterHandle;
    MMethodAttributes getterAttributes;
    MMethodAttributes setterAttributes;
};

struct NativeString
{
    int32 length;
    Char chars[1];
};

MDomain* MCore::CreateDomain(const StringAnsi& domainName)
{
    return nullptr;
}

void MCore::UnloadDomain(const StringAnsi& domainName)
{
}

bool MCore::LoadEngine()
{
    PROFILE_CPU();
    const ::String csharpLibraryPath = Globals::BinariesFolder / TEXT("FlaxEngine.CSharp.dll");
    const ::String csharpRuntimeConfigPath = Globals::BinariesFolder / TEXT("FlaxEngine.CSharp.runtimeconfig.json");
    if (!FileSystem::FileExists(csharpLibraryPath))
        LOG(Fatal, "Failed to initialize managed runtime, FlaxEngine.CSharp.dll is missing.");
    if (!FileSystem::FileExists(csharpRuntimeConfigPath))
        LOG(Fatal, "Failed to initialize managed runtime, FlaxEngine.CSharp.runtimeconfig.json is missing.");

    // Initialize hostfxr
    if (InitHostfxr(csharpRuntimeConfigPath, csharpLibraryPath))
        return true;

    // Prepare managed side
    CallStaticMethodByName<void>(TEXT("Init"));
#ifdef MCORE_MAIN_MODULE_NAME
    // MCORE_MAIN_MODULE_NAME define is injected by Scripting.Build.cs on platforms that use separate shared library for engine symbols
    const StringAnsi flaxLibraryPath(Platform::GetMainDirectory() / TEXT(MACRO_TO_STR(MCORE_MAIN_MODULE_NAME)));
#else
    const StringAnsi flaxLibraryPath(Platform::GetExecutableFilePath());
#endif
    RegisterNativeLibrary("FlaxEngine", flaxLibraryPath.Get());

    MRootDomain = New<MDomain>("Root");
    MDomains.Add(MRootDomain);

    char* buildInfo = CallStaticMethodByName<char*>(TEXT("GetRuntimeInformation"));
    LOG(Info, ".NET runtime version: {0}", ::String(buildInfo));
    MCore::GC::FreeMemory(buildInfo);

    return false;
}

void MCore::UnloadEngine()
{
    if (!MRootDomain)
        return;
    PROFILE_CPU();
    CallStaticMethodByName<void>(TEXT("Exit"));
    MDomains.ClearDelete();
    MRootDomain = nullptr;
    ShutdownHostfxr();
}

MObject* MCore::Object::Box(void* value, const MClass* klass)
{
    static void* BoxValuePtr = GetStaticMethodPointer(TEXT("BoxValue"));
    return (MObject*)CallStaticMethod<void*, void*, void*>(BoxValuePtr, klass->_handle, value);
}

void* MCore::Object::Unbox(MObject* obj)
{
    static void* UnboxValuePtr = GetStaticMethodPointer(TEXT("UnboxValue"));
    return CallStaticMethod<void*, void*>(UnboxValuePtr, obj);
}

MObject* MCore::Object::New(const MClass* klass)
{
    static void* NewObjectPtr = GetStaticMethodPointer(TEXT("NewObject"));
    return (MObject*)CallStaticMethod<void*, void*>(NewObjectPtr, klass->_handle);
}

void MCore::Object::Init(MObject* obj)
{
    static void* ObjectInitPtr = GetStaticMethodPointer(TEXT("ObjectInit"));
    CallStaticMethod<void, void*>(ObjectInitPtr, obj);
}

MClass* MCore::Object::GetClass(MObject* obj)
{
    static void* GetObjectTypePtr = GetStaticMethodPointer(TEXT("GetObjectType"));
    void* classHandle = CallStaticMethod<void*, void*>(GetObjectTypePtr, obj);
    return GetOrCreateClass((void*)classHandle);
}

MString* MCore::Object::ToString(MObject* obj)
{
    MISSING_CODE("TODO: MCore::Object::ToString"); // TODO: MCore::Object::ToString
    return nullptr;
}

int32 MCore::Object::GetHashCode(MObject* obj)
{
    MISSING_CODE("TODO: MCore::Object::GetHashCode"); // TODO: MCore::Object::GetHashCode
    return 0;
}

MString* MCore::String::GetEmpty(MDomain* domain)
{
    static void* GetStringEmptyPtr = GetStaticMethodPointer(TEXT("GetStringEmpty"));
    return (MString*)CallStaticMethod<void*>(GetStringEmptyPtr);
}

MString* MCore::String::New(const char* str, int32 length, MDomain* domain)
{
    static void* NewStringLengthPtr = GetStaticMethodPointer(TEXT("NewStringLength"));
    return (MString*)CallStaticMethod<void*, const char*, int>(NewStringLengthPtr, str, length);
}

MString* MCore::String::New(const Char* str, int32 length, MDomain* domain)
{
    static void* NewStringUTF16Ptr = GetStaticMethodPointer(TEXT("NewStringUTF16"));
    return (MString*)CallStaticMethod<void*, const Char*, int>(NewStringUTF16Ptr, str, length);
}

StringView MCore::String::GetChars(MString* obj)
{
    static void* GetStringPointerPtr = GetStaticMethodPointer(TEXT("GetStringPointer"));
    NativeString* str = (NativeString*)CallStaticMethod<void*, void*>(GetStringPointerPtr, obj);
    return StringView(str->chars, str->length);
}

MArray* MCore::Array::New(const MClass* elementKlass, int32 length)
{
    static void* NewArrayPtr = GetStaticMethodPointer(TEXT("NewArray"));
    return (MArray*)CallStaticMethod<void*, void*, long long>(NewArrayPtr, elementKlass->_handle, length);
}

MClass* MCore::Array::GetClass(MClass* elementKlass)
{
    static void* GetArrayLengthPtr = GetStaticMethodPointer(TEXT("GetArrayTypeFromElementType"));
    void* classHandle = CallStaticMethod<void*, void*>(GetArrayLengthPtr, elementKlass->_handle);
    return GetOrCreateClass((void*)classHandle);
}

int32 MCore::Array::GetLength(const MArray* obj)
{
    static void* GetArrayLengthPtr = GetStaticMethodPointer(TEXT("GetArrayLength"));
    return CallStaticMethod<int, void*>(GetArrayLengthPtr, (void*)obj);
}

void* MCore::Array::GetAddress(const MArray* obj)
{
    static void* GetArrayPointerPtr = GetStaticMethodPointer(TEXT("GetArrayPointer"));
    return CallStaticMethod<void*, void*>(GetArrayPointerPtr, (void*)obj);
}

MGCHandle MCore::GCHandle::New(MObject* obj, bool pinned)
{
    static void* NewGCHandlePtr = GetStaticMethodPointer(TEXT("NewGCHandle"));
    return (MGCHandle)CallStaticMethod<void*, void*, bool>(NewGCHandlePtr, obj, pinned);
}

MGCHandle MCore::GCHandle::NewWeak(MObject* obj, bool trackResurrection)
{
    static void* NewGCHandleWeakPtr = GetStaticMethodPointer(TEXT("NewGCHandleWeak"));
    return (MGCHandle)CallStaticMethod<void*, void*, bool>(NewGCHandleWeakPtr, obj, trackResurrection);
}

MObject* MCore::GCHandle::GetTarget(const MGCHandle& handle)
{
    return (MObject*)(void*)handle;
}

void MCore::GCHandle::Free(const MGCHandle& handle)
{
    static void* FreeGCHandlePtr = GetStaticMethodPointer(TEXT("FreeGCHandle"));
    CallStaticMethod<void, void*>(FreeGCHandlePtr, (void*)handle);
}

void MCore::GC::Collect()
{
    PROFILE_CPU();
    // TODO: call System.GC.Collect()
}

void MCore::GC::Collect(int32 generation)
{
    PROFILE_CPU();
    // TODO: call System.GC.Collect(int32)
}

void MCore::GC::WaitForPendingFinalizers()
{
    PROFILE_CPU();
    // TODO: call System.GC.WaitForPendingFinalizers()
}

void MCore::GC::WriteRef(void* ptr, MObject* ref)
{
    *(void**)ptr = ref;
}

void MCore::GC::WriteValue(void* dst, void* src, int32 count, const MClass* klass)
{
    const int32 size = klass->GetInstanceSize();
    memcpy(dst, src, count * size);
}

void MCore::GC::WriteArrayRef(MArray* dst, MObject* ref, int32 index)
{
    static void* SetArrayValueReferencePtr = GetStaticMethodPointer(TEXT("SetArrayValueReference"));
    CallStaticMethod<void, void*, void*, int32>(SetArrayValueReferencePtr, dst, ref, index);
}

void* MCore::GC::AllocateMemory(int32 size, bool coTaskMem)
{
    static void* AllocMemoryPtr = GetStaticMethodPointer(TEXT("AllocMemory"));
    return CallStaticMethod<void*, int, bool>(AllocMemoryPtr, size, coTaskMem);
}

void MCore::GC::FreeMemory(void* ptr, bool coTaskMem)
{
    if (!ptr)
        return;
    static void* FreeMemoryPtr = GetStaticMethodPointer(TEXT("FreeMemory"));
    CallStaticMethod<void, void*, bool>(FreeMemoryPtr, ptr, coTaskMem);
}

void MCore::Thread::Attach()
{
}

void MCore::Thread::Exit()
{
}

bool MCore::Thread::IsAttached()
{
    return true;
}

void MCore::Exception::Throw(MObject* exception)
{
    static void* RaiseExceptionPtr = GetStaticMethodPointer(TEXT("RaiseException"));
    CallStaticMethod<void*, void*>(RaiseExceptionPtr, exception);
}

MObject* MCore::Exception::GetNullReference()
{
    static void* GetNullReferenceExceptionPtr = GetStaticMethodPointer(TEXT("GetNullReferenceException"));
    return (MObject*)CallStaticMethod<void*>(GetNullReferenceExceptionPtr);
}

MObject* MCore::Exception::Get(const char* msg)
{
    return nullptr; // TODO: implement generic exception with custom message
}

MObject* MCore::Exception::GetArgument(const char* arg, const char* msg)
{
    static void* GetArgumentExceptionPtr = GetStaticMethodPointer(TEXT("GetArgumentException"));
    return (MObject*)CallStaticMethod<void*>(GetArgumentExceptionPtr);
}

MObject* MCore::Exception::GetArgumentNull(const char* arg)
{
    static void* GetArgumentNullExceptionPtr = GetStaticMethodPointer(TEXT("GetArgumentNullException"));
    return (MObject*)CallStaticMethod<void*>(GetArgumentNullExceptionPtr);
}

MObject* MCore::Exception::GetArgumentOutOfRange(const char* arg)
{
    static void* GetArgumentOutOfRangeExceptionPtr = GetStaticMethodPointer(TEXT("GetArgumentOutOfRangeException"));
    return (MObject*)CallStaticMethod<void*>(GetArgumentOutOfRangeExceptionPtr);
}

MObject* MCore::Exception::GetNotSupported(const char* msg)
{
    static void* GetNotSupportedExceptionPtr = GetStaticMethodPointer(TEXT("GetNotSupportedException"));
    return (MObject*)CallStaticMethod<void*>(GetNotSupportedExceptionPtr);
}

::String MCore::Type::ToString(MType* type)
{
    MClass* klass = GetOrCreateClass(type);
    return ::String(klass->GetFullName());
}

MClass* MCore::Type::GetClass(MType* type)
{
    static void* GetTypeClassPtr = GetStaticMethodPointer(TEXT("GetTypeClass"));
    type = (MType*)CallStaticMethod<void*, void*>(GetTypeClassPtr, type);
    return GetOrCreateClass(type);
}

MType* MCore::Type::GetElementType(MType* type)
{
    static void* GetElementClassPtr = GetStaticMethodPointer(TEXT("GetElementClass"));
    return (MType*)CallStaticMethod<void*, void*>(GetElementClassPtr, type);
}

int32 MCore::Type::GetSize(MType* type)
{
    return GetOrCreateClass(type)->GetInstanceSize();
}

MTypes MCore::Type::GetType(MType* type)
{
    MClass* klass = GetOrCreateClass((void*)type);
    if (klass->_types == 0)
    {
        static void* GetTypeMTypesEnumPtr = GetStaticMethodPointer(TEXT("GetTypeMTypesEnum"));
        klass->_types = CallStaticMethod<uint32, void*>(GetTypeMTypesEnumPtr, klass->_handle);
    }
    return (MTypes)klass->_types;
}

bool MCore::Type::IsPointer(MType* type)
{
    static void* GetTypeIsPointerPtr = GetStaticMethodPointer(TEXT("GetTypeIsPointer"));
    return CallStaticMethod<bool, void*>(GetTypeIsPointerPtr, type);
}

bool MCore::Type::IsReference(MType* type)
{
    static void* GetTypeIsReferencePtr = GetStaticMethodPointer(TEXT("GetTypeIsReference"));
    return CallStaticMethod<bool, void*>(GetTypeIsReferencePtr, type);
}

const MAssembly::ClassesDictionary& MAssembly::GetClasses() const
{
    if (_hasCachedClasses || !IsLoaded())
        return _classes;
    PROFILE_CPU();
    const auto startTime = DateTime::NowUTC();

#if TRACY_ENABLE
    ZoneText(*_name, _name.Length());
#endif
    ScopeLock lock(_locker);
    if (_hasCachedClasses)
        return _classes;
    ASSERT(_classes.IsEmpty());

    NativeClassDefinitions* managedClasses;
    int classCount;
    static void* GetManagedClassesPtr = GetStaticMethodPointer(TEXT("GetManagedClasses"));
    CallStaticMethod<void, void*, NativeClassDefinitions**, int*>(GetManagedClassesPtr, _handle, &managedClasses, &classCount);
    _classes.EnsureCapacity(classCount);
    for (int32 i = 0; i < classCount; i++)
    {
        NativeClassDefinitions& managedClass = managedClasses[i];

        // Create class object
        MClass* klass = New<MClass>(this, managedClass.typeHandle, managedClass.name, managedClass.fullname, managedClass.namespace_, managedClass.typeAttributes);
        _classes.Add(klass->GetFullName(), klass);

        MCore::GC::FreeMemory((void*)managedClasses[i].name);
        MCore::GC::FreeMemory((void*)managedClasses[i].fullname);
        MCore::GC::FreeMemory((void*)managedClasses[i].namespace_);
    }
    MCore::GC::FreeMemory(managedClasses);

    const auto endTime = DateTime::NowUTC();
    LOG(Info, "Caching classes for assembly {0} took {1}ms", String(_name), (int32)(endTime - startTime).GetTotalMilliseconds());

#if 0
    for (auto i = _classes.Begin(); i.IsNotEnd(); ++i)
        LOG(Info, "Class: {0}", String(i->Value->GetFullName()));
#endif

    _hasCachedClasses = true;
    return _classes;
}

bool MAssembly::LoadCorlib()
{
    if (IsLoaded())
        return false;
    PROFILE_CPU();
#if TRACY_ENABLE
    const StringAnsiView name("Corlib");
    ZoneText(*name, name.Length());
#endif

    // Ensure to be unloaded
    Unload();

    // Start
    const auto startTime = DateTime::NowUTC();
    OnLoading();

    // Load
    {
        const char* name;
        const char* fullname;
        static void* GetAssemblyByNamePtr = GetStaticMethodPointer(TEXT("GetAssemblyByName"));
        _handle = CallStaticMethod<void*, const char*, const char**, const char**>(GetAssemblyByNamePtr, "System.Private.CoreLib", &name, &fullname);
        _name = name;
        _fullname = fullname;
        MCore::GC::FreeMemory((void*)name);
        MCore::GC::FreeMemory((void*)fullname);
    }
    if (_handle == nullptr)
    {
        OnLoadFailed();
        return true;
    }
    _hasCachedClasses = false;
    assemblyHandles.Add(_handle, this);

    // End
    OnLoaded(startTime);
    return false;
}

bool MAssembly::LoadImage(const String& assemblyPath, const StringView& nativePath)
{
    // Load assembly file data
    Array<byte> data;
    File::ReadAllBytes(assemblyPath, data);

    // Open .Net assembly
    const StringAnsi assemblyPathAnsi = assemblyPath.ToStringAnsi();
    const char* name;
    const char* fullname;
    static void* LoadAssemblyImagePtr = GetStaticMethodPointer(TEXT("LoadAssemblyImage"));
    _handle = CallStaticMethod<void*, char*, int, const char*, const char**, const char**>(LoadAssemblyImagePtr, (char*)data.Get(), data.Count(), assemblyPathAnsi.Get(), &name, &fullname);
    _name = name;
    _fullname = fullname;
    MCore::GC::FreeMemory((void*)name);
    MCore::GC::FreeMemory((void*)fullname);
    if (_handle == nullptr)
    {
        Log::CLRInnerException(TEXT(".NET assembly image is invalid at ") + assemblyPath);
        return true;
    }
    assemblyHandles.Add(_handle, this);

    // Provide new path of hot-reloaded native library path for managed DllImport
    if (nativePath.HasChars())
    {
        RegisterNativeLibrary(assemblyPathAnsi.Get(), StringAnsi(nativePath).Get());
    }

    _hasCachedClasses = false;
    _assemblyPath = assemblyPath;
    return false;
}

bool MAssembly::UnloadImage(bool isReloading)
{
    if (_handle)
    {
        // TODO: closing assembly on reload only is copy-paste from mono, do we need do this on .NET too?
        if (isReloading)
        {
            LOG(Info, "Unloading managed assembly \'{0}\' (is reloading)", String(_name));

            static void* CloseAssemblyPtr = GetStaticMethodPointer(TEXT("CloseAssembly"));
            CallStaticMethod<void, const void*>(CloseAssemblyPtr, _handle);
        }

        assemblyHandles.Remove(_handle);
        _handle = nullptr;
    }
    return false;
}

MClass::MClass(const MAssembly* parentAssembly, void* handle, const char* name, const char* fullname, const char* namespace_, MTypeAttributes attributes)
    : _handle(handle)
    , _name(name)
    , _namespace_(namespace_)
    , _assembly(parentAssembly)
    , _fullname(fullname)
    , _hasCachedProperties(false)
    , _hasCachedFields(false)
    , _hasCachedMethods(false)
    , _hasCachedAttributes(false)
    , _hasCachedEvents(false)
    , _hasCachedInterfaces(false)
{
    ASSERT(handle != nullptr);
    switch (attributes & MTypeAttributes::VisibilityMask)
    {
    case MTypeAttributes::NotPublic:
    case MTypeAttributes::NestedPrivate:
        _visibility = MVisibility::Private;
        break;
    case MTypeAttributes::Public:
    case MTypeAttributes::NestedPublic:
        _visibility = MVisibility::Public;
        break;
    case MTypeAttributes::NestedFamily:
    case MTypeAttributes::NestedAssembly:
        _visibility = MVisibility::Internal;
        break;
    case MTypeAttributes::NestedFamORAssem:
        _visibility = MVisibility::ProtectedInternal;
        break;
    case MTypeAttributes::NestedFamANDAssem:
        _visibility = MVisibility::PrivateProtected;
        break;
    default:
        CRASH;
    }

    const MTypeAttributes staticClassFlags = MTypeAttributes::Abstract | MTypeAttributes::Sealed;
    _isStatic = (attributes & staticClassFlags) == staticClassFlags;
    _isSealed = !_isStatic && (attributes & MTypeAttributes::Sealed) == MTypeAttributes::Sealed;
    _isAbstract = !_isStatic && (attributes & MTypeAttributes::Abstract) == MTypeAttributes::Abstract;
    _isInterface = (attributes & MTypeAttributes::ClassSemanticsMask) == MTypeAttributes::Interface;

    // TODO: pass type info from C# side at once (pack into flags)

    static void* TypeIsValueTypePtr = GetStaticMethodPointer(TEXT("TypeIsValueType"));
    _isValueType = CallStaticMethod<bool, void*>(TypeIsValueTypePtr, handle);

    static void* TypeIsEnumPtr = GetStaticMethodPointer(TEXT("TypeIsEnum"));
    _isEnum = CallStaticMethod<bool, void*>(TypeIsEnumPtr, handle);

    classHandles.Add(handle, this);
}

MClass::~MClass()
{
    _methods.ClearDelete();
    _fields.ClearDelete();
    _properties.ClearDelete();
    _events.ClearDelete();

    classHandles.Remove(_handle);
}

StringAnsiView MClass::GetName() const
{
    return _name;
}

StringAnsiView MClass::GetNamespace() const
{
    return _namespace_;
}

MType* MClass::GetType() const
{
    return (MType*)_handle;
}

MClass* MClass::GetBaseClass() const
{
    static void* GetClassParentPtr = GetStaticMethodPointer(TEXT("GetClassParent"));
    void* parentHandle = CallStaticMethod<void*, void*>(GetClassParentPtr, _handle);
    return GetOrCreateClass(parentHandle);
}

bool MClass::IsSubClassOf(const MClass* klass, bool checkInterfaces) const
{
    static void* TypeIsSubclassOfPtr = GetStaticMethodPointer(TEXT("TypeIsSubclassOf"));
    return klass && CallStaticMethod<bool, void*, void*, bool>(TypeIsSubclassOfPtr, _handle, klass->_handle, checkInterfaces);
}

bool MClass::HasInterface(const MClass* klass) const
{
    static void* TypeIsAssignableFrom = GetStaticMethodPointer(TEXT("TypeIsAssignableFrom"));
    return klass && CallStaticMethod<bool, void*, void*>(TypeIsAssignableFrom, _handle, klass->_handle);
}

bool MClass::IsInstanceOfType(MObject* object) const
{
    if (object == nullptr)
        return false;
    MClass* objectClass = MCore::Object::GetClass(object);
    return IsSubClassOf(objectClass, false);
}

uint32 MClass::GetInstanceSize() const
{
    if (_size != 0)
        return _size;
    static void* NativeSizeOfPtr = GetStaticMethodPointer(TEXT("NativeSizeOf"));
    _size = CallStaticMethod<int, void*>(NativeSizeOfPtr, _handle);
    return _size;
}

MClass* MClass::GetElementClass() const
{
    static void* GetElementClassPtr = GetStaticMethodPointer(TEXT("GetElementClass"));
    void* elementType = CallStaticMethod<void*, void*>(GetElementClassPtr, _handle);
    return GetOrCreateClass(elementType);
}

MMethod* MClass::GetMethod(const char* name, int32 numParams) const
{
    GetMethods();
    for (int32 i = 0; i < _methods.Count(); i++)
    {
        if (_methods[i]->GetName() == name && _methods[i]->GetParametersCount() == numParams)
            return _methods[i];
    }
    return nullptr;
}

const Array<MMethod*>& MClass::GetMethods() const
{
    if (_hasCachedMethods)
        return _methods;

    NativeMethodDefinitions* methods;
    int methodsCount;
    static void* GetClassMethodsPtr = GetStaticMethodPointer(TEXT("GetClassMethods"));
    CallStaticMethod<void, void*, NativeMethodDefinitions**, int*>(GetClassMethodsPtr, _handle, &methods, &methodsCount);
    for (int32 i = 0; i < methodsCount; i++)
    {
        NativeMethodDefinitions& definition = methods[i];
        MMethod* method = New<MMethod>(const_cast<MClass*>(this), StringAnsi(definition.name), definition.handle, definition.numParameters, definition.methodAttributes);
        _methods.Add(method);

        MCore::GC::FreeMemory((void*)definition.name);
    }
    MCore::GC::FreeMemory(methods);

    _hasCachedMethods = true;
    return _methods;
}

MField* MClass::GetField(const char* name) const
{
    GetFields();
    for (int32 i = 0; i < _fields.Count(); i++)
    {
        if (_fields[i]->GetName() == name)
            return _fields[i];
    }
    return nullptr;
}

const Array<MField*>& MClass::GetFields() const
{
    if (_hasCachedFields)
        return _fields;

    NativeFieldDefinitions* fields;
    int numFields;
    static void* GetClassFieldsPtr = GetStaticMethodPointer(TEXT("GetClassFields"));
    CallStaticMethod<void, void*, NativeFieldDefinitions**, int*>(GetClassFieldsPtr, _handle, &fields, &numFields);
    for (int32 i = 0; i < numFields; i++)
    {
        NativeFieldDefinitions& definition = fields[i];
        MField* field = New<MField>(const_cast<MClass*>(this), definition.fieldHandle, definition.name, definition.fieldType, definition.fieldAttributes);
        _fields.Add(field);

        MCore::GC::FreeMemory((void*)definition.name);
    }
    MCore::GC::FreeMemory(fields);

    _hasCachedFields = true;
    return _fields;
}

const Array<MEvent*>& MClass::GetEvents() const
{
    if (_hasCachedEvents)
        return _events;

    // TODO: implement MEvent in .NET

    _hasCachedEvents = true;
    return _events;
}

MProperty* MClass::GetProperty(const char* name) const
{
    GetProperties();
    for (int32 i = 0; i < _properties.Count(); i++)
    {
        if (_properties[i]->GetName() == name)
            return _properties[i];
    }
    return nullptr;
}

const Array<MProperty*>& MClass::GetProperties() const
{
    if (_hasCachedProperties)
        return _properties;

    NativePropertyDefinitions* foundProperties;
    int numProperties;
    static void* GetClassPropertiesPtr = GetStaticMethodPointer(TEXT("GetClassProperties"));
    CallStaticMethod<void, void*, NativePropertyDefinitions**, int*>(GetClassPropertiesPtr, _handle, &foundProperties, &numProperties);
    for (int i = 0; i < numProperties; i++)
    {
        const NativePropertyDefinitions& definition = foundProperties[i];
        MProperty* property = New<MProperty>(const_cast<MClass*>(this), definition.name, definition.getterHandle, definition.setterHandle, definition.getterAttributes, definition.setterAttributes);
        _properties.Add(property);

        MCore::GC::FreeMemory((void*)definition.name);
    }
    MCore::GC::FreeMemory(foundProperties);

    _hasCachedProperties = true;
    return _properties;
}

const Array<MClass*>& MClass::GetInterfaces() const
{
    if (_hasCachedInterfaces)
        return _interfaces;

    void** foundInterfaces;
    int numInterfaces;
    static void* GetClassInterfacesPtr = GetStaticMethodPointer(TEXT("GetClassInterfaces"));
    CallStaticMethod<void, void*, void***, int*>(GetClassInterfacesPtr, _handle, &foundInterfaces, &numInterfaces);
    for (int32 i = 0; i < numInterfaces; i++)
    {
        MClass* interfaceClass = GetOrCreateClass(foundInterfaces[i]);
        _interfaces.Add(interfaceClass);
    }
    MCore::GC::FreeMemory(foundInterfaces);

    _hasCachedInterfaces = true;
    return _interfaces;
}

bool MClass::HasAttribute(const MClass* monoClass) const
{
    return HasCustomAttribute(this, monoClass);
}

bool MClass::HasAttribute() const
{
    return HasCustomAttribute(this);
}

MObject* MClass::GetAttribute(const MClass* monoClass) const
{
    return (MObject*)GetCustomAttribute(this, monoClass);
}

const Array<MObject*>& MClass::GetAttributes() const
{
    if (_hasCachedAttributes)
        return _attributes;

    MObject** attributes;
    int numAttributes;
    static void* GetClassAttributesPtr = GetStaticMethodPointer(TEXT("GetClassAttributes"));
    CallStaticMethod<void, void*, MObject***, int*>(GetClassAttributesPtr, _handle, &attributes, &numAttributes);
    _attributes.Resize(numAttributes);
    for (int i = 0; i < numAttributes; i++)
    {
        _attributes.Add(attributes[i]);
    }
    MCore::GC::FreeMemory(attributes);

    _hasCachedAttributes = true;
    return _attributes;
}

bool MDomain::SetCurrentDomain(bool force)
{
    MActiveDomain = this;
    return true;
}

void MDomain::Dispatch() const
{
}

MEvent::MEvent(MClass* parentClass, void* handle, const char* name)
    : _handle(handle)
    , _addMethod(nullptr)
    , _removeMethod(nullptr)
    , _parentClass(parentClass)
    , _name(name)
    , _hasCachedAttributes(false)
    , _hasAddMonoMethod(true)
    , _hasRemoveMonoMethod(true)
{
}

MMethod* MEvent::GetAddMethod() const
{
    return nullptr; // TODO: implement MEvent in .NET
}

MMethod* MEvent::GetRemoveMethod() const
{
    return nullptr; // TODO: implement MEvent in .NET
}

bool MEvent::HasAttribute(MClass* monoClass) const
{
    return false; // TODO: implement MEvent in .NET
}

bool MEvent::HasAttribute() const
{
    return false; // TODO: implement MEvent in .NET
}

MObject* MEvent::GetAttribute(MClass* monoClass) const
{
    return nullptr; // TODO: implement MEvent in .NET
}

const Array<MObject*>& MEvent::GetAttributes() const
{
    if (_hasCachedAttributes)
        return _attributes;
    _hasCachedAttributes = true;

    // TODO: implement MEvent in .NET
    return _attributes;
}

MException::MException(MObject* exception)
    : InnerException(nullptr)
{
    ASSERT(exception);
    MClass* exceptionClass = MCore::Object::GetClass(exception);

    MProperty* exceptionMsgProp = exceptionClass->GetProperty("Message");
    MMethod* exceptionMsgGetter = exceptionMsgProp->GetGetMethod();
    MString* exceptionMsg = (MString*)exceptionMsgGetter->Invoke(exception, nullptr, nullptr);
    Message = MUtils::ToString(exceptionMsg);

    MProperty* exceptionStackProp = exceptionClass->GetProperty("StackTrace");
    MMethod* exceptionStackGetter = exceptionStackProp->GetGetMethod();
    MString* exceptionStackTrace = (MString*)exceptionStackGetter->Invoke(exception, nullptr, nullptr);
    StackTrace = MUtils::ToString(exceptionStackTrace);

    MProperty* innerExceptionProp = exceptionClass->GetProperty("InnerException");
    MMethod* innerExceptionGetter = innerExceptionProp->GetGetMethod();
    MObject* innerException = (MObject*)innerExceptionGetter->Invoke(exception, nullptr, nullptr);
    if (innerException)
        InnerException = New<MException>(innerException);
}

MException::~MException()
{
    if (InnerException)
        Delete(InnerException);
}

MField::MField(MClass* parentClass, void* handle, const char* name, void* type, MFieldAttributes attributes)
    : _handle(handle)
    , _type(type)
    , _parentClass(parentClass)
    , _name(name)
    , _hasCachedAttributes(false)
{
    switch (attributes & MFieldAttributes::FieldAccessMask)
    {
    case MFieldAttributes::Private:
        _visibility = MVisibility::Private;
        break;
    case MFieldAttributes::FamANDAssem:
        _visibility = MVisibility::PrivateProtected;
        break;
    case MFieldAttributes::Assembly:
        _visibility = MVisibility::Internal;
        break;
    case MFieldAttributes::Family:
        _visibility = MVisibility::Protected;
        break;
    case MFieldAttributes::FamORAssem:
        _visibility = MVisibility::ProtectedInternal;
        break;
    case MFieldAttributes::Public:
        _visibility = MVisibility::Public;
        break;
    default:
        CRASH;
    }
    _isStatic = (attributes & MFieldAttributes::Static) == MFieldAttributes::Static;
}

MType* MField::GetType() const
{
    return (MType*)_type;
}

int32 MField::GetOffset() const
{
    MISSING_CODE("TODO: MField::GetOffset"); // TODO: MField::GetOffset
    return 0;
}

void MField::GetValue(MObject* instance, void* result) const
{
    static void* FieldGetValuePtr = GetStaticMethodPointer(TEXT("FieldGetValue"));
    CallStaticMethod<void, void*, void*, void*>(FieldGetValuePtr, instance, _handle, result);
}

MObject* MField::GetValueBoxed(MObject* instance) const
{
    MISSING_CODE("TODO: MField::GetValueBoxed"); // TODO: MField::GetValueBoxed
    return nullptr;
}

void MField::SetValue(MObject* instance, void* value) const
{
    static void* FieldSetValuePtr = GetStaticMethodPointer(TEXT("FieldSetValue"));
    CallStaticMethod<void, void*, void*, void*>(FieldSetValuePtr, instance, _handle, value);
}

bool MField::HasAttribute(MClass* monoClass) const
{
    // TODO: implement MField attributes in .NET
    return false;
}

bool MField::HasAttribute() const
{
    // TODO: implement MField attributes in .NET
    return false;
}

MObject* MField::GetAttribute(MClass* monoClass) const
{
    // TODO: implement MField attributes in .NET
    return nullptr;
}

const Array<MObject*>& MField::GetAttributes() const
{
    if (_hasCachedAttributes)
        return _attributes;
    _hasCachedAttributes = true;

    // TODO: implement MField attributes in .NET
    return _attributes;
}

MMethod::MMethod(MClass* parentClass, StringAnsi&& name, void* handle, int32 paramsCount, MMethodAttributes attributes)
    : _handle(handle)
    , _paramsCount(paramsCount)
    , _parentClass(parentClass)
    , _name(MoveTemp(name))
    , _hasCachedAttributes(false)
    , _hasCachedSignature(false)
{
    switch (attributes & MMethodAttributes::MemberAccessMask)
    {
    case MMethodAttributes::Private:
        _visibility = MVisibility::Private;
        break;
    case MMethodAttributes::FamANDAssem:
        _visibility = MVisibility::PrivateProtected;
        break;
    case MMethodAttributes::Assembly:
        _visibility = MVisibility::Internal;
        break;
    case MMethodAttributes::Family:
        _visibility = MVisibility::Protected;
        break;
    case MMethodAttributes::FamORAssem:
        _visibility = MVisibility::ProtectedInternal;
        break;
    case MMethodAttributes::Public:
        _visibility = MVisibility::Public;
        break;
    default:
        CRASH;
    }
    _isStatic = (attributes & MMethodAttributes::Static) == MMethodAttributes::Static;

#if COMPILE_WITH_PROFILER
    const StringAnsi& className = parentClass->GetFullName();
    ProfilerName.Resize(className.Length() + 2 + _name.Length());
    Platform::MemoryCopy(ProfilerName.Get(), className.Get(), className.Length());
    ProfilerName.Get()[className.Length()] = ':';
    ProfilerName.Get()[className.Length() + 1] = ':';
    Platform::MemoryCopy(ProfilerName.Get() + className.Length() + 2, _name.Get(), _name.Length());
    ProfilerData.name = ProfilerName.Get();
    ProfilerData.function = _name.Get();
    ProfilerData.file = nullptr;
    ProfilerData.line = 0;
    ProfilerData.color = 0;
#endif
}

void MMethod::CacheSignature() const
{
    _hasCachedSignature = true;

    static void* GetMethodReturnTypePtr = GetStaticMethodPointer(TEXT("GetMethodReturnType"));
    static void* GetMethodParameterTypesPtr = GetStaticMethodPointer(TEXT("GetMethodParameterTypes"));

    _returnType = CallStaticMethod<void*, void*>(GetMethodReturnTypePtr, _handle);

    if (_paramsCount == 0)
        return;
    void** parameterTypeHandles;
    CallStaticMethod<void, void*, void***>(GetMethodParameterTypesPtr, _handle, &parameterTypeHandles);
    _parameterTypes.Set(parameterTypeHandles, _paramsCount);
    MCore::GC::FreeMemory(parameterTypeHandles);
}

MObject* MMethod::Invoke(void* instance, void** params, MObject** exception) const
{
    PROFILE_CPU_SRC_LOC(ProfilerData);
    static void* InvokeMethodPtr = GetStaticMethodPointer(TEXT("InvokeMethod"));
    return (MObject*)CallStaticMethod<void*, void*, void*, void*, void*>(InvokeMethodPtr, instance, _handle, params, exception);
}

MObject* MMethod::InvokeVirtual(MObject* instance, void** params, MObject** exception) const
{
    return Invoke(instance, params, exception);
}

#if !USE_MONO_AOT

void* MMethod::GetThunk()
{
    if (!_cachedThunk)
    {
        static void* GetMethodUnmanagedFunctionPointerPtr = GetStaticMethodPointer(TEXT("GetMethodUnmanagedFunctionPointer"));
        _cachedThunk = CallStaticMethod<void*, void*>(GetMethodUnmanagedFunctionPointerPtr, _handle);
    }
    return _cachedThunk;
}

#endif

MMethod* MMethod::InflateGeneric() const
{
    // This seams to be unused on .NET (Mono required inflating generic class of the script)
    return const_cast<MMethod*>(this);
}

MType* MMethod::GetReturnType() const
{
    if (!_hasCachedSignature)
        CacheSignature();
    return (MType*)_returnType;
}

int32 MMethod::GetParametersCount() const
{
    return _paramsCount;
}

MType* MMethod::GetParameterType(int32 paramIdx) const
{
    if (!_hasCachedSignature)
        CacheSignature();
    ASSERT_LOW_LAYER(paramIdx >= 0 && paramIdx < _paramsCount);
    return (MType*)_parameterTypes[paramIdx];
}

bool MMethod::GetParameterIsOut(int32 paramIdx) const
{
    if (!_hasCachedSignature)
        CacheSignature();
    ASSERT_LOW_LAYER(paramIdx >= 0 && paramIdx < _paramsCount);
    // TODO: cache GetParameterIsOut maybe?
    static void* GetMethodParameterIsOutPtr = GetStaticMethodPointer(TEXT("GetMethodParameterIsOut"));
    return CallStaticMethod<bool, void*, int>(GetMethodParameterIsOutPtr, _handle, paramIdx);
}

bool MMethod::HasAttribute(MClass* monoClass) const
{
    // TODO: implement MMethod attributes in .NET
    return false;
}

bool MMethod::HasAttribute() const
{
    // TODO: implement MMethod attributes in .NET
    return false;
}

MObject* MMethod::GetAttribute(MClass* monoClass) const
{
    // TODO: implement MMethod attributes in .NET
    return nullptr;
}

const Array<MObject*>& MMethod::GetAttributes() const
{
    if (_hasCachedAttributes)
        return _attributes;
    _hasCachedAttributes = true;

    // TODO: implement MMethod attributes in .NET
    return _attributes;
}

MProperty::MProperty(MClass* parentClass, const char* name, void* getterHandle, void* setterHandle, MMethodAttributes getterAttributes, MMethodAttributes setterAttributes)
    : _parentClass(parentClass)
    , _name(name)
    , _hasCachedAttributes(false)
{
    _hasGetMethod = getterHandle != nullptr;
    if (_hasGetMethod)
        _getMethod = New<MMethod>(parentClass, StringAnsi("get_" + _name), getterHandle, 1, getterAttributes);
    else
        _getMethod = nullptr;
    _hasSetMethod = setterHandle != nullptr;
    if (_hasSetMethod)
        _setMethod = New<MMethod>(parentClass, StringAnsi("set_" + _name), setterHandle, 1, setterAttributes);
    else
        _setMethod = nullptr;
}

MProperty::~MProperty()
{
    if (_getMethod)
        Delete(_getMethod);
    if (_setMethod)
        Delete(_setMethod);
}

MMethod* MProperty::GetGetMethod() const
{
    return _getMethod;
}

MMethod* MProperty::GetSetMethod() const
{
    return _setMethod;
}

MObject* MProperty::GetValue(MObject* instance, MObject** exception) const
{
    CHECK_RETURN(_getMethod, nullptr);
    return _getMethod->Invoke(instance, nullptr, exception);
}

void MProperty::SetValue(MObject* instance, void* value, MObject** exception) const
{
    CHECK(_setMethod);
    void* params[1];
    params[0] = value;
    _setMethod->Invoke(instance, params, exception);
}

bool MProperty::HasAttribute(MClass* monoClass) const
{
    // TODO: implement MProperty attributes in .NET
    return false;
}

bool MProperty::HasAttribute() const
{
    // TODO: implement MProperty attributes in .NET
    return false;
}

MObject* MProperty::GetAttribute(MClass* monoClass) const
{
    // TODO: implement MProperty attributes in .NET
    return nullptr;
}

const Array<MObject*>& MProperty::GetAttributes() const
{
    if (_hasCachedAttributes)
        return _attributes;
    _hasCachedAttributes = true;

    // TODO: implement MProperty attributes in .NET
    return _attributes;
}

MAssembly* GetAssembly(void* assemblyHandle)
{
    MAssembly* assembly;
    if (assemblyHandles.TryGet(assemblyHandle, assembly))
        return assembly;
    return nullptr;
}

MClass* GetClass(void* typeHandle)
{
    MClass* klass = nullptr;
    classHandles.TryGet(typeHandle, klass);
    return nullptr;
}

MClass* GetOrCreateClass(void* typeHandle)
{
    if (!typeHandle)
        return nullptr;
    MClass* klass;
    if (!classHandles.TryGet(typeHandle, klass))
    {
        NativeClassDefinitions classInfo;
        void* assemblyHandle;
        static void* GetManagedClassFromTypePtr = GetStaticMethodPointer(TEXT("GetManagedClassFromType"));
        CallStaticMethod<void, void*, void*>(GetManagedClassFromTypePtr, typeHandle, &classInfo, &assemblyHandle);
        MAssembly* assembly = GetAssembly(assemblyHandle);
        klass = New<MClass>(assembly, classInfo.typeHandle, classInfo.name, classInfo.fullname, classInfo.namespace_, classInfo.typeAttributes);
        if (assembly != nullptr)
        {
            const_cast<MAssembly::ClassesDictionary&>(assembly->GetClasses()).Add(klass->GetFullName(), klass);
        }

        if (typeHandle != classInfo.typeHandle)
            CallStaticMethod<void, void*, void*>(GetManagedClassFromTypePtr, typeHandle, &classInfo);

        MCore::GC::FreeMemory((void*)classInfo.name);
        MCore::GC::FreeMemory((void*)classInfo.fullname);
        MCore::GC::FreeMemory((void*)classInfo.namespace_);
    }
    ASSERT(klass != nullptr);
    return klass;
}

bool HasCustomAttribute(const MClass* klass, const MClass* attributeClass)
{
    return GetCustomAttribute(klass, attributeClass) != nullptr;
}

bool HasCustomAttribute(const MClass* klass)
{
    return GetCustomAttribute(klass, nullptr) != nullptr;
}

void* GetCustomAttribute(const MClass* klass, const MClass* attributeClass)
{
    static void* GetCustomAttributePtr = GetStaticMethodPointer(TEXT("GetCustomAttribute"));
    return CallStaticMethod<void*, void*, void*>(GetCustomAttributePtr, klass->GetNative(), attributeClass ? attributeClass->GetNative() : nullptr);
}

#if DOTNET_HOST_CORECRL

hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config;
hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate;
hostfxr_close_fn hostfxr_close;
load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer;
get_function_pointer_fn get_function_pointer;
hostfxr_set_error_writer_fn hostfxr_set_error_writer;
hostfxr_get_dotnet_environment_info_result_fn hostfxr_get_dotnet_environment_info_result;
hostfxr_run_app_fn hostfxr_run_app;

bool InitHostfxr(const String& configPath, const String& libraryPath)
{
    const FLAX_CORECLR_STRING& library_path = FLAX_CORECLR_STRING(libraryPath);

    // Get path to hostfxr library
    get_hostfxr_parameters get_hostfxr_params;
    get_hostfxr_params.size = sizeof(hostfxr_initialize_parameters);
    get_hostfxr_params.assembly_path = library_path.Get();
    FLAX_CORECLR_STRING dotnetRoot;
    // TODO: implement proper lookup for dotnet instalation folder and handle standalone build of FlaxGame
#if PLATFORM_MAC
    get_hostfxr_params.dotnet_root = "/usr/local/share/dotnet";
#else
    get_hostfxr_params.dotnet_root = nullptr;
#endif
#if !USE_EDITOR
    const String& bundledDotnetPath = Globals::ProjectFolder / TEXT("Dotnet");
    if (FileSystem::DirectoryExists(bundledDotnetPath))
    {
        dotnetRoot = FLAX_CORECLR_STRING(bundledDotnetPath);
#if PLATFORM_WINDOWS_FAMILY
        dotnetRoot.Replace('/', '\\');
#endif
        get_hostfxr_params.dotnet_root = dotnetRoot.Get();
    }
#endif
    char_t hostfxrPath[1024];
    size_t hostfxrPathSize = sizeof(hostfxrPath) / sizeof(char_t);
    int rc = get_hostfxr_path(hostfxrPath, &hostfxrPathSize, &get_hostfxr_params);
    if (rc != 0)
    {
        LOG(Error, "Failed to find hostfxr: {0:x} ({1})", (unsigned int)rc, String(get_hostfxr_params.dotnet_root));

        // Warn user about missing .Net
#if PLATFORM_DESKTOP
        Platform::OpenUrl(TEXT("https://dotnet.microsoft.com/en-us/download/dotnet/7.0"));
#endif
#if USE_EDITOR
        LOG(Fatal, "Missing .NET 7 SDK installation requried to run Flax Editor.");
#else
        LOG(Fatal, "Missing .NET 7 Runtime installation requried to run this application.");
#endif
        return true;
    }
    String path(hostfxrPath);
    LOG(Info, "Found hostfxr in {0}", path);

    // Get API from hostfxr library
    void* hostfxr = Platform::LoadLibrary(path.Get());
    if (hostfxr == nullptr)
    {
        LOG(Fatal, "Failed to load hostfxr library ({0})", path);
        return true;
    }
    hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)Platform::GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config");
    hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)Platform::GetProcAddress(hostfxr, "hostfxr_initialize_for_dotnet_command_line");
    hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)Platform::GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate");
    hostfxr_close = (hostfxr_close_fn)Platform::GetProcAddress(hostfxr, "hostfxr_close");
    hostfxr_set_error_writer = (hostfxr_set_error_writer_fn)Platform::GetProcAddress(hostfxr, "hostfxr_set_error_writer");
    hostfxr_get_dotnet_environment_info_result = (hostfxr_get_dotnet_environment_info_result_fn)Platform::GetProcAddress(hostfxr, "hostfxr_get_dotnet_environment_info_result");
    hostfxr_run_app = (hostfxr_run_app_fn)Platform::GetProcAddress(hostfxr, "hostfxr_run_app");
    if (!hostfxr_get_runtime_delegate || !hostfxr_run_app)
    {
        LOG(Fatal, "Failed to setup hostfxr API ({0})", path);
        return true;
    }

    // Initialize hosting component
    const char_t* argv[1] = { library_path.Get() };
    hostfxr_initialize_parameters init_params;
    init_params.size = sizeof(hostfxr_initialize_parameters);
    init_params.host_path = library_path.Get();
    path = String(StringUtils::GetDirectoryName(path)) / TEXT("/../../../");
    StringUtils::PathRemoveRelativeParts(path);
    dotnetRoot = FLAX_CORECLR_STRING(path);
    init_params.dotnet_root = dotnetRoot.Get();
    hostfxr_handle handle = nullptr;
    rc = hostfxr_initialize_for_dotnet_command_line(ARRAY_COUNT(argv), argv, &init_params, &handle);
    if (rc != 0 || handle == nullptr)
    {
        hostfxr_close(handle);
        LOG(Fatal, "Failed to initialize hostfxr: {0:x} ({1})", (unsigned int)rc, String(init_params.dotnet_root));
        return true;
    }

    void* pget_function_pointer = nullptr;
    rc = hostfxr_get_runtime_delegate(handle, hdt_get_function_pointer, &pget_function_pointer);
    if (rc != 0 || pget_function_pointer == nullptr)
    {
        hostfxr_close(handle);
        LOG(Fatal, "Failed to get runtime delegate hdt_get_function_pointer: 0x{0:x}", (unsigned int)rc);
        return true;
    }

    hostfxr_close(handle);
    get_function_pointer = (get_function_pointer_fn)pget_function_pointer;
    return false;
}

void ShutdownHostfxr()
{
}

void* GetStaticMethodPointer(const String& methodName)
{
    void* fun;
    if (CachedFunctions.TryGet(methodName, fun))
        return fun;
    const int rc = get_function_pointer(NativeInteropTypeName, FLAX_CORECLR_STRING(methodName).Get(), UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, &fun);
    if (rc != 0)
        LOG(Fatal, "Failed to get unmanaged function pointer for method {0}: 0x{1:x}", methodName.Get(), (unsigned int)rc);
    CachedFunctions.Add(methodName, fun);
    return fun;
}

#elif DOTNET_HOST_MONO

#ifdef USE_MONO_AOT_MODULE
void* MonoAotModuleHandle = nullptr;
#endif
MonoDomain* MonoDomainHandle = nullptr;

void OnLogCallback(const char* logDomain, const char* logLevel, const char* message, mono_bool fatal, void* userData)
{
    String currentDomain(logDomain);
    String msg(message);
    msg.Replace('\n', ' ');

    static const char* monoErrorLevels[] =
    {
        nullptr,
        "error",
        "critical",
        "warning",
        "message",
        "info",
        "debug"
    };

    uint32 errorLevel = 0;
    if (logLevel != nullptr)
    {
        for (uint32 i = 1; i < 7; i++)
        {
            if (strcmp(monoErrorLevels[i], logLevel) == 0)
            {
                errorLevel = i;
                break;
            }
        }
    }

    if (currentDomain.IsEmpty())
    {
        auto domain = MCore::GetActiveDomain();
        if (domain != nullptr)
        {
            currentDomain = domain->GetName().Get();
        }
        else
        {
            currentDomain = "null";
        }
    }

#if 0
	// Print C# stack trace (crash may be caused by the managed code)
	if (mono_domain_get() && Assemblies::FlaxEngine.Assembly->IsLoaded())
	{
		const auto managedStackTrace = DebugLog::GetStackTrace();
		if (managedStackTrace.HasChars())
		{
			LOG(Warning, "Managed stack trace:");
			LOG_STR(Warning, managedStackTrace);
		}
	}
#endif

    if (errorLevel == 0)
    {
        Log::CLRInnerException(String::Format(TEXT("Message: {0} | Domain: {1}"), msg, currentDomain)).SetLevel(LogType::Error);
    }
    else if (errorLevel <= 2)
    {
        Log::CLRInnerException(String::Format(TEXT("Message: {0} | Domain: {1}"), msg, currentDomain)).SetLevel(LogType::Error);
    }
    else if (errorLevel <= 3)
    {
        LOG(Warning, "Message: {0} | Domain: {1}", msg, currentDomain);
    }
    else
    {
        LOG(Info, "Message: {0} | Domain: {1}", msg, currentDomain);
    }
}

void OnPrintCallback(const char* string, mono_bool isStdout)
{
    LOG_STR(Warning, String(string));
}

void OnPrintErrorCallback(const char* string, mono_bool isStdout)
{
    // HACK: ignore this message
    if (string && Platform::MemoryCompare(string, "debugger-agent: Unable to listen on ", 36) == 0)
        return;

    LOG_STR(Error, String(string));
}

bool InitHostfxr(const String& configPath, const String& libraryPath)
{
#if 1
    // Enable detailed Mono logging
    Platform::SetEnvironmentVariable(TEXT("MONO_LOG_LEVEL"), TEXT("debug"));
    Platform::SetEnvironmentVariable(TEXT("MONO_LOG_MASK"), TEXT("all"));
    //Platform::SetEnvironmentVariable(TEXT("MONO_GC_DEBUG"), TEXT("6:gc-log.txt,check-remset-consistency,nursery-canaries"));
#endif

#if defined(USE_MONO_AOT_MODE)
    // Enable AOT mode (per-platform)
    mono_jit_set_aot_mode(USE_MONO_AOT_MODE);
#endif

#ifdef USE_MONO_AOT_MODULE
    // Load AOT module
    const DateTime aotModuleLoadStartTime = DateTime::Now();
    LOG(Info, "Loading Mono AOT module...");
    void* libAotModule = Platform::LoadLibrary(TEXT(USE_MONO_AOT_MODULE));
    if (libAotModule == nullptr)
    {
        LOG(Error, "Failed to laod Mono AOT module (" TEXT(USE_MONO_AOT_MODULE) ")");
        return true;
    }
    MonoAotModuleHandle = libAotModule;
    void* getModulesPtr = Platform::GetProcAddress(libAotModule, "GetMonoModules");
    if (getModulesPtr == nullptr)
    {
        LOG(Error, "Failed to get Mono AOT modules getter.");
        return true;
    }
    typedef int (*GetMonoModulesFunc)(void** buffer, int bufferSize);
    const auto getModules = (GetMonoModulesFunc)getModulesPtr;
    const int32 moduelsCount = getModules(nullptr, 0);
    void** modules = (void**)Allocator::Allocate(moduelsCount * sizeof(void*));
    getModules(modules, moduelsCount);
    for (int32 i = 0; i < moduelsCount; i++)
    {
        mono_aot_register_module((void**)modules[i]);
    }
    Allocator::Free(modules);
    LOG(Info, "Mono AOT module loaded in {0}ms", (int32)(DateTime::Now() - aotModuleLoadStartTime).GetTotalMilliseconds());
#endif

    // Setup debugger
    {
        int32 debuggerLogLevel = 0;
        if (CommandLine::Options.MonoLog.IsTrue())
        {
            LOG(Info, "Using detailed Mono logging");
            mono_trace_set_level_string("debug");
            debuggerLogLevel = 10;
        }
        else
        {
            mono_trace_set_level_string("warning");
        }

#if MONO_DEBUG_ENABLE && !PLATFORM_SWITCH
        StringAnsi debuggerIp = "127.0.0.1";
        uint16 debuggerPort = 41000 + Platform::GetCurrentProcessId() % 1000;
        if (CommandLine::Options.DebuggerAddress.HasValue())
        {
            const auto& address = CommandLine::Options.DebuggerAddress.GetValue();
            const int32 splitIndex = address.Find(':');
            if (splitIndex == INVALID_INDEX)
            {
                debuggerIp = address.ToStringAnsi();
            }
            else
            {
                debuggerIp = address.Left(splitIndex).ToStringAnsi();
                StringUtils::Parse(address.Right(address.Length() - splitIndex - 1).Get(), &debuggerPort);
            }
        }

        char buffer[150];
        sprintf(buffer, "--debugger-agent=transport=dt_socket,address=%s:%d,embedding=1,server=y,suspend=%s,loglevel=%d", debuggerIp.Get(), debuggerPort, CommandLine::Options.WaitForDebugger ? "y,timeout=5000" : "n", debuggerLogLevel);

        const char* options[] = {
            "--soft-breakpoints",
            //"--optimize=float32",
            buffer
        };
        mono_jit_parse_options(ARRAY_COUNT(options), (char**)options);

        mono_debug_init(MONO_DEBUG_FORMAT_MONO, 0);
        LOG(Info, "Mono debugger server at {0}:{1}", ::String(debuggerIp), debuggerPort);
#endif
    }

    // Connect to mono engine callback system
    mono_trace_set_log_handler(OnLogCallback, nullptr);
    mono_trace_set_print_handler(OnPrintCallback);
    mono_trace_set_printerr_handler(OnPrintErrorCallback);

    // Initialize Mono VM
    StringAnsi baseDirectory(Globals::ProjectFolder);
    const char* appctxKeys[] =
    {
        "RUNTIME_IDENTIFIER",
        "APP_CONTEXT_BASE_DIRECTORY",
    };
    const char* appctxValues[] =
    {
        MACRO_TO_STR(DOTNET_HOST_RUNTIME_IDENTIFIER),
        baseDirectory.Get(),
    };
    static_assert(ARRAY_COUNT(appctxKeys) == ARRAY_COUNT(appctxValues), "Invalid appctx setup");
    monovm_initialize(ARRAY_COUNT(appctxKeys), appctxKeys, appctxValues);

    // Init managed runtime
#if PLATFORM_ANDROID || PLATFORM_IOS
    const char* monoVersion = "mobile";
#else
    const char* monoVersion = ""; // ignored
#endif
    MonoDomainHandle = mono_jit_init_version("Flax", monoVersion);
    if (!MonoDomainHandle)
    {
        LOG(Fatal, "Failed to initialize Mono.");
        return true;
    }

    // Log info
    char* buildInfo = mono_get_runtime_build_info();
    LOG(Info, "Mono runtime version: {0}", String(buildInfo));
    mono_free(buildInfo);

    return false;
}

void ShutdownHostfxr()
{
    mono_jit_cleanup(MonoDomainHandle);
    MonoDomainHandle = nullptr;

#ifdef USE_MONO_AOT_MODULE
    Platform::FreeLibrary(MonoAotModuleHandle);
#endif
}

void* GetStaticMethodPointer(const String& methodName)
{
    MISSING_CODE("TODO: GetStaticMethodPointer for Mono host runtime"); // TODO: impl this
    return nullptr;
}

#endif

#endif
