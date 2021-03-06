// Copyright 2004-present Facebook. All Rights Reserved.

#include <android/input.h>
#include <fb/log.h>
#include <folly/json.h>
#include <jni/Countable.h>
#include <jni/Environment.h>
#include <jni/fbjni.h>
#include <jni/LocalReference.h>
#include <jni/LocalString.h>
#include <jni/WeakReference.h>
#include <jni/fbjni/Exceptions.h>
#include <react/Bridge.h>
#include <react/Executor.h>
#include <react/JSCExecutor.h>
#include "JSLoader.h"
#include "NativeArray.h"
#include "ProxyExecutor.h"

using namespace facebook::jni;

namespace facebook {
namespace react {

static jclass gReadableNativeMapClass;
static jmethodID gReadableNativeMapCtor;

namespace exceptions {

static const char *gUnknownNativeTypeExceptionClass =
  "com/facebook/react/bridge/UnexpectedNativeTypeException";

template <typename T>
void throwIfObjectAlreadyConsumed(const T& t, const char* msg) {
  if (t->isConsumed) {
    throwNewJavaException("com/facebook/react/bridge/ObjectAlreadyConsumedException", msg);
  }
}

}

struct NativeMap : public Countable {
  // Whether this map has been added to another array or map and no longer has a valid map value
  bool isConsumed = false;
  folly::dynamic map = folly::dynamic::object;
};

struct ReadableNativeMapKeySeyIterator : public Countable {
  folly::dynamic::const_item_iterator iterator;
  RefPtr<NativeMap> mapRef;

  ReadableNativeMapKeySeyIterator(folly::dynamic::const_item_iterator&& it,
                                  const RefPtr<NativeMap>& mapRef_)
    : iterator(std::move(it))
    , mapRef(mapRef_) {}
};

struct NativeRunnable : public Countable {
  std::function<void()> callable;
};

static jobject createReadableNativeMapWithContents(JNIEnv* env, folly::dynamic map) {
  if (map.isNull()) {
    return nullptr;
  }

  if (!map.isObject()) {
    throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass,
                          "expected Map, got a %s", map.typeName());
  }

  jobject jnewMap = env->NewObject(gReadableNativeMapClass, gReadableNativeMapCtor);
  if (env->ExceptionCheck()) {
    return nullptr;
  }
  auto nativeMap = extractRefPtr<NativeMap>(env, jnewMap);
  nativeMap->map = std::move(map);
  return jnewMap;
}

namespace type {

static jclass gReadableReactType;
static jobject gTypeNullValue;
static jobject gTypeBooleanValue;
static jobject gTypeNumberValue;
static jobject gTypeStringValue;
static jobject gTypeMapValue;
static jobject gTypeArrayValue;

static jobject getTypeValue(JNIEnv* env, const char* fieldName) {
  jfieldID fieldID = env->GetStaticFieldID(
    gReadableReactType, fieldName, "Lcom/facebook/react/bridge/ReadableType;");
  jobject typeValue = env->GetStaticObjectField(gReadableReactType, fieldID);
  return env->NewGlobalRef(typeValue);
}

static void initialize(JNIEnv* env) {
  gTypeNullValue = getTypeValue(env, "Null");
  gTypeBooleanValue = getTypeValue(env, "Boolean");
  gTypeNumberValue = getTypeValue(env, "Number");
  gTypeStringValue = getTypeValue(env, "String");
  gTypeMapValue = getTypeValue(env, "Map");
  gTypeArrayValue = getTypeValue(env, "Array");
}

static jobject getType(folly::dynamic::Type type) {
  switch (type) {
    case folly::dynamic::Type::NULLT:
      return type::gTypeNullValue;
    case folly::dynamic::Type::BOOL:
      return type::gTypeBooleanValue;
    case folly::dynamic::Type::DOUBLE:
    case folly::dynamic::Type::INT64:
      return type::gTypeNumberValue;
    case folly::dynamic::Type::STRING:
      return type::gTypeStringValue;
    case folly::dynamic::Type::OBJECT:
      return type::gTypeMapValue;
    case folly::dynamic::Type::ARRAY:
      return type::gTypeArrayValue;
    default:
      throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass, "Unknown type");
  }
}

}

struct ReadableNativeArray : public NativeArray {
  static void mapException(const std::exception& ex) {
    if (dynamic_cast<const folly::TypeError*>(&ex) != 0) {
      throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass, ex.what());
    }
  }

  jint getSize() {
    return array.size();
  }

  jboolean isNull(jint index) {
    return array.at(index).isNull() ? JNI_TRUE : JNI_FALSE;
  }

  jboolean getBoolean(jint index) {
    return array.at(index).getBool() ? JNI_TRUE : JNI_FALSE;
  }

  jdouble getDouble(jint index) {
    const folly::dynamic& val = array.at(index);
    if (val.isInt()) {
      return val.getInt();
    }
    return val.getDouble();
  }

  jstring getString(jint index) {
    const folly::dynamic& dyn = array.at(index);
    if (dyn.isNull()) {
      return nullptr;
    }
    return make_jstring(dyn.getString().c_str()).release();
  }

  jobject getArray(jint index) {
    return createReadableNativeArrayWithContents(array.at(index)).release();
  }

  jobject getMap(jint index) {
    return createReadableNativeMapWithContents(Environment::current(), array.at(index));
  }

  jobject getType(jint index) {
    return type::getType(array.at(index).type());
  }

  static void registerNatives() {
    jni::registerNatives("com/facebook/react/bridge/ReadableNativeArray", {
        makeNativeMethod("size", ReadableNativeArray::getSize),
        makeNativeMethod("isNull", ReadableNativeArray::isNull),
        makeNativeMethod("getBoolean", ReadableNativeArray::getBoolean),
        makeNativeMethod("getDouble", ReadableNativeArray::getDouble),
        makeNativeMethod("getString", ReadableNativeArray::getString),
        makeNativeMethod("getArray", "(I)Lcom/facebook/react/bridge/ReadableNativeArray;",
                         ReadableNativeArray::getArray),
        makeNativeMethod("getMap", "(I)Lcom/facebook/react/bridge/ReadableNativeMap;",
                         ReadableNativeArray::getMap),
        makeNativeMethod("getType", "(I)Lcom/facebook/react/bridge/ReadableType;",
                         ReadableNativeArray::getType),
    });
  }
};

struct WritableNativeArray : public ReadableNativeArray {
  void pushNull() {
    exceptions::throwIfObjectAlreadyConsumed(this, "Array already consumed");
    array.push_back(nullptr);
  }

  void pushBoolean(jboolean value) {
    exceptions::throwIfObjectAlreadyConsumed(this, "Array already consumed");
    array.push_back(value == JNI_TRUE);
  }

  void pushDouble(jdouble value) {
    exceptions::throwIfObjectAlreadyConsumed(this, "Receiving array already consumed");
    array.push_back(value);
  }

  void pushString(jstring value) {
    if (value == NULL) {
      pushNull();
      return;
    }
    exceptions::throwIfObjectAlreadyConsumed(this, "Receiving array already consumed");
    array.push_back(wrap_alias(value)->toStdString());
  }

  void pushArray(NativeArray* otherArray) {
    if (otherArray == NULL) {
      pushNull();
      return;
    }
    exceptions::throwIfObjectAlreadyConsumed(this, "Receiving array already consumed");
    exceptions::throwIfObjectAlreadyConsumed(otherArray, "Array to push already consumed");
    array.push_back(std::move(otherArray->array));
    otherArray->isConsumed = true;
  }

  void pushMap(jobject jmap) {
    if (jmap == NULL) {
      pushNull();
      return;
    }
    exceptions::throwIfObjectAlreadyConsumed(this, "Receiving array already consumed");
    auto map = extractRefPtr<NativeMap>(Environment::current(), jmap);
    exceptions::throwIfObjectAlreadyConsumed(map, "Map to push already consumed");
    array.push_back(std::move(map->map));
    map->isConsumed = true;
  }

  static void registerNatives() {
    jni::registerNatives("com/facebook/react/bridge/WritableNativeArray", {
        makeNativeMethod("pushNull", WritableNativeArray::pushNull),
        makeNativeMethod("pushBoolean", WritableNativeArray::pushBoolean),
        makeNativeMethod("pushDouble", WritableNativeArray::pushDouble),
        makeNativeMethod("pushString", WritableNativeArray::pushString),
        makeNativeMethod("pushNativeArray", "(Lcom/facebook/react/bridge/WritableNativeArray;)V",
                         WritableNativeArray::pushArray),
        makeNativeMethod("pushNativeMap", "(Lcom/facebook/react/bridge/WritableNativeMap;)V",
                         WritableNativeArray::pushMap),
    });
  }
};

namespace map {

static void initialize(JNIEnv* env, jobject obj) {
  auto map = createNew<NativeMap>();
  setCountableForJava(env, obj, std::move(map));
}

static jstring toString(JNIEnv* env, jobject obj) {
  auto nativeMap = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(nativeMap, "Map already consumed");
  LocalString string(
    ("{ NativeMap: " + folly::toJson(nativeMap->map) + " }").c_str());
  return static_cast<jstring>(env->NewLocalRef(string.string()));
}

namespace writable {

static void putNull(JNIEnv* env, jobject obj, jstring key) {
  auto map = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(map, "Receiving map already consumed");
  map->map.insert(fromJString(env, key), nullptr);
}

static void putBoolean(JNIEnv* env, jobject obj, jstring key, jboolean value) {
  auto map = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(map, "Receiving map already consumed");
  map->map.insert(fromJString(env, key), value == JNI_TRUE);
}

static void putDouble(JNIEnv* env, jobject obj, jstring key, jdouble value) {
  auto map = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(map, "Receiving map already consumed");
  map->map.insert(fromJString(env, key), value);
}

static void putString(JNIEnv* env, jobject obj, jstring key, jstring value) {
  if (value == NULL) {
    putNull(env, obj, key);
    return;
  }
  auto map = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(map, "Receiving map already consumed");
  map->map.insert(fromJString(env, key), fromJString(env, value));
}

static void putArray(JNIEnv* env, jobject obj, jstring key, NativeArray::jhybridobject value) {
  if (value == NULL) {
    putNull(env, obj, key);
    return;
  }
  auto parentMap = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(parentMap, "Receiving map already consumed");
  auto arrayValue = cthis(wrap_alias(value));
  exceptions::throwIfObjectAlreadyConsumed(arrayValue, "Array to put already consumed");
  parentMap->map.insert(fromJString(env, key), std::move(arrayValue->array));
  arrayValue->isConsumed = true;
}

static void putMap(JNIEnv* env, jobject obj, jstring key, jobject value) {
  if (value == NULL) {
    putNull(env, obj, key);
    return;
  }
  auto parentMap = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(parentMap, "Receiving map already consumed");
  auto mapValue = extractRefPtr<NativeMap>(env, value);
  exceptions::throwIfObjectAlreadyConsumed(mapValue, "Map to put already consumed");
  parentMap->map.insert(fromJString(env, key), std::move(mapValue->map));
  mapValue->isConsumed = true;
}

static void mergeMap(JNIEnv* env, jobject obj, jobject source) {
  auto sourceMap = extractRefPtr<NativeMap>(env, source);
  exceptions::throwIfObjectAlreadyConsumed(sourceMap, "Source map already consumed");
  auto destMap = extractRefPtr<NativeMap>(env, obj);
  exceptions::throwIfObjectAlreadyConsumed(destMap, "Destination map already consumed");

  // std::map#insert doesn't overwrite the value, therefore we need to clean values for keys
  // that already exists before merging dest map into source map
  for (auto sourceIt : sourceMap->map.items()) {
    destMap->map.erase(sourceIt.first);
    destMap->map.insert(std::move(sourceIt.first), std::move(sourceIt.second));
  }
}

} // namespace writable

namespace readable {

static const char *gNoSuchKeyExceptionClass = "com/facebook/react/bridge/NoSuchKeyException";

static jboolean hasKey(JNIEnv* env, jobject obj, jstring keyName) {
  auto nativeMap = extractRefPtr<NativeMap>(env, obj);
  auto& map = nativeMap->map;
  bool found = map.find(fromJString(env, keyName)) != map.items().end();
  return found ? JNI_TRUE : JNI_FALSE;
}

static const folly::dynamic& getMapValue(JNIEnv* env, jobject obj, jstring keyName) {
  auto nativeMap = extractRefPtr<NativeMap>(env, obj);
  std::string key = fromJString(env, keyName);
  try {
    return nativeMap->map.at(key);
  } catch (const std::out_of_range& ex) {
    throwNewJavaException(gNoSuchKeyExceptionClass, ex.what());
  }
}

static jboolean isNull(JNIEnv* env, jobject obj, jstring keyName) {
  return getMapValue(env, obj, keyName).isNull() ? JNI_TRUE : JNI_FALSE;
}

static jboolean getBooleanKey(JNIEnv* env, jobject obj, jstring keyName) {
  try {
    return getMapValue(env, obj, keyName).getBool() ? JNI_TRUE : JNI_FALSE;
  } catch (const folly::TypeError& ex) {
    throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass, ex.what());
  }
}

static jdouble getDoubleKey(JNIEnv* env, jobject obj, jstring keyName) {
  const folly::dynamic& val = getMapValue(env, obj, keyName);
  if (val.isInt()) {
    return val.getInt();
  }
  try {
    return val.getDouble();
  } catch (const folly::TypeError& ex) {
    throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass, ex.what());
  }
}

static jstring getStringKey(JNIEnv* env, jobject obj, jstring keyName) {
  const folly::dynamic& val = getMapValue(env, obj, keyName);
  if (val.isNull()) {
    return nullptr;
  }
  try {
    LocalString value(val.getString().c_str());
    return static_cast<jstring>(env->NewLocalRef(value.string()));
  } catch (const folly::TypeError& ex) {
    throwNewJavaException(exceptions::gUnknownNativeTypeExceptionClass, ex.what());
  }
}

static jobject getArrayKey(JNIEnv* env, jobject obj, jstring keyName) {
  return createReadableNativeArrayWithContents(getMapValue(env, obj, keyName)).release();
}

static jobject getMapKey(JNIEnv* env, jobject obj, jstring keyName) {
  return createReadableNativeMapWithContents(env, getMapValue(env, obj, keyName));
}

static jobject getValueType(JNIEnv* env, jobject obj, jstring keyName) {
  return type::getType(getMapValue(env, obj, keyName).type());
}

} // namespace readable

namespace iterator {

static void initialize(JNIEnv* env, jobject obj, jobject nativeMapObj) {
  auto nativeMap = extractRefPtr<NativeMap>(env, nativeMapObj);
  auto mapIterator = createNew<ReadableNativeMapKeySeyIterator>(
    nativeMap->map.items().begin(), nativeMap);
  setCountableForJava(env, obj, std::move(mapIterator));
}

static jboolean hasNextKey(JNIEnv* env, jobject obj) {
  auto nativeIterator = extractRefPtr<ReadableNativeMapKeySeyIterator>(env, obj);
  return ((nativeIterator->iterator != nativeIterator->mapRef.get()->map.items().end())
          ? JNI_TRUE : JNI_FALSE);
}

static jstring getNextKey(JNIEnv* env, jobject obj) {
  auto nativeIterator = extractRefPtr<ReadableNativeMapKeySeyIterator>(env, obj);
  if (JNI_FALSE == hasNextKey(env, obj)) {
    throwNewJavaException("com/facebook/react/bridge/InvalidIteratorException",
                          "No such element exists");
  }
  LocalString value(nativeIterator->iterator->first.c_str());
  ++nativeIterator->iterator;
  return static_cast<jstring>(env->NewLocalRef(value.string()));
}

} // namespace iterator
} // namespace map

namespace runnable {

static jclass gNativeRunnableClass;
static jmethodID gNativeRunnableCtor;

static jobject createNativeRunnable(JNIEnv* env, decltype(NativeRunnable::callable)&& callable) {
  jobject jRunnable = env->NewObject(gNativeRunnableClass, gNativeRunnableCtor);
  if (env->ExceptionCheck()) {
    return nullptr;
  }
  auto nativeRunnable = createNew<NativeRunnable>();
  nativeRunnable->callable = std::move(callable);
  setCountableForJava(env, jRunnable, std::move(nativeRunnable));
  return jRunnable;
}

static void run(JNIEnv* env, jobject jNativeRunnable) {
  auto nativeRunnable = extractRefPtr<NativeRunnable>(env, jNativeRunnable);
  nativeRunnable->callable();
}

} // namespace runnable

namespace queue {

static jmethodID gRunOnQueueThreadMethod;

static void enqueueNativeRunnableOnQueue(JNIEnv* env, jobject callbackQueueThread, jobject nativeRunnable) {
  env->CallVoidMethod(callbackQueueThread, gRunOnQueueThreadMethod, nativeRunnable);
}

} // namespace queue

namespace bridge {

static jmethodID gCallbackMethod;
static jmethodID gOnBatchCompleteMethod;

static void makeJavaCall(JNIEnv* env, jobject callback, MethodCall&& call) {
  if (call.arguments.isNull()) {
    return;
  }
  auto newArray = createReadableNativeArrayWithContents(std::move(call.arguments));
  env->CallVoidMethod(callback, gCallbackMethod, call.moduleId, call.methodId, newArray.get());
}

static void signalBatchComplete(JNIEnv* env, jobject callback) {
  env->CallVoidMethod(callback, gOnBatchCompleteMethod);
}

static void dispatchCallbacksToJava(const RefPtr<WeakReference>& weakCallback,
                                    const RefPtr<WeakReference>& weakCallbackQueueThread,
                                    std::vector<MethodCall>&& calls) {
  auto env = Environment::current();
  if (env->ExceptionCheck()) {
    FBLOGW("Dropped calls because of pending exception");
    return;
  }

  ResolvedWeakReference callbackQueueThread(weakCallbackQueueThread);
  if (!callbackQueueThread) {
    FBLOGW("Dropped calls because of callback queue thread went away");
    return;
  }

  auto runnableFunction = std::bind([weakCallback] (std::vector<MethodCall>& calls) {
    auto env = Environment::current();
    if (env->ExceptionCheck()) {
      FBLOGW("Dropped calls because of pending exception");
      return;
    }
    ResolvedWeakReference callback(weakCallback);
    if (callback) {
      for (auto&& call : calls) {
        makeJavaCall(env, callback, std::move(call));
        if (env->ExceptionCheck()) {
          return;
        }
      }
      signalBatchComplete(env, callback);
    }
  }, std::move(calls));

  jobject jNativeRunnable = runnable::createNativeRunnable(env, std::move(runnableFunction));
  queue::enqueueNativeRunnableOnQueue(env, callbackQueueThread, jNativeRunnable);
}

static void create(JNIEnv* env, jobject obj, jobject executor, jobject callback,
                   jobject callbackQueueThread) {
  auto weakCallback = createNew<WeakReference>(callback);
  auto weakCallbackQueueThread = createNew<WeakReference>(callbackQueueThread);
  auto bridgeCallback = [weakCallback, weakCallbackQueueThread] (std::vector<MethodCall> calls) {
    dispatchCallbacksToJava(weakCallback, weakCallbackQueueThread, std::move(calls));
  };
  auto nativeExecutorFactory = extractRefPtr<JSExecutorFactory>(env, executor);
  auto bridge = createNew<Bridge>(nativeExecutorFactory, bridgeCallback);
  setCountableForJava(env, obj, std::move(bridge));
}

static void loadScriptFromAssets(JNIEnv* env, jobject obj, jobject assetManager,
                                 jstring assetName) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  auto assetNameStr = fromJString(env, assetName);
  auto script = react::loadScriptFromAssets(env, assetManager, assetNameStr);
  bridge->executeApplicationScript(script, assetNameStr);
}

static void loadScriptFromNetworkCached(JNIEnv* env, jobject obj, jstring sourceURL,
                                   jstring tempFileName) {
  auto bridge = jni::extractRefPtr<Bridge>(env, obj);
  std::string script = "";
  if (tempFileName != NULL) {
    script = react::loadScriptFromFile(jni::fromJString(env, tempFileName));
  }
  bridge->executeApplicationScript(script, jni::fromJString(env, sourceURL));
}

static void callFunction(JNIEnv* env, jobject obj, jint moduleId, jint methodId,
                         NativeArray::jhybridobject args) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  auto arguments = cthis(wrap_alias(args));
  std::vector<folly::dynamic> call{
    (double) moduleId,
    (double) methodId,
    std::move(arguments->array),
  };
  try {
    bridge->executeJSCall("BatchedBridge", "callFunctionReturnFlushedQueue", std::move(call));
  } catch (...) {
    translatePendingCppExceptionToJavaException();
  }
}

static void invokeCallback(JNIEnv* env, jobject obj, jint callbackId,
                           NativeArray::jhybridobject args) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  auto arguments = cthis(wrap_alias(args));
  std::vector<folly::dynamic> call{
    (double) callbackId,
    std::move(arguments->array)
  };
  try {
    bridge->executeJSCall("BatchedBridge", "invokeCallbackAndReturnFlushedQueue", std::move(call));
  } catch (...) {
    translatePendingCppExceptionToJavaException();
  }
}

static void setGlobalVariable(JNIEnv* env, jobject obj, jstring propName, jstring jsonValue) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  bridge->setGlobalVariable(fromJString(env, propName), fromJString(env, jsonValue));
}

static jboolean supportsProfiling(JNIEnv* env, jobject obj) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  return bridge->supportsProfiling() ? JNI_TRUE : JNI_FALSE;
}

static void startProfiler(JNIEnv* env, jobject obj, jstring title) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  bridge->startProfiler(fromJString(env, title));
}

static void stopProfiler(JNIEnv* env, jobject obj, jstring title, jstring filename) {
  auto bridge = extractRefPtr<Bridge>(env, obj);
  bridge->stopProfiler(fromJString(env, title), fromJString(env, filename));
}

} // namespace bridge

namespace executors {

static void createJSCExecutor(JNIEnv *env, jobject obj) {
  auto executor = createNew<JSCExecutorFactory>();
  setCountableForJava(env, obj, std::move(executor));
}

static void createProxyExecutor(JNIEnv *env, jobject obj, jobject executorInstance) {
  auto executor =
    createNew<ProxyExecutorOneTimeFactory>(jni::make_global(jni::adopt_local(executorInstance)));
  setCountableForJava(env, obj, std::move(executor));
}

} // namespace executors

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  return initialize(vm, [] {
    // get the current env
    JNIEnv* env = Environment::current();

    auto readableTypeClass = findClassLocal("com/facebook/react/bridge/ReadableType");
    type::gReadableReactType = (jclass)env->NewGlobalRef(readableTypeClass.get());
    type::initialize(env);

    NativeArray::registerNatives();
    ReadableNativeArray::registerNatives();
    WritableNativeArray::registerNatives();

    registerNatives("com/facebook/react/bridge/NativeMap", {
        makeNativeMethod("initialize", map::initialize),
        makeNativeMethod("toString", map::toString),
    });

    jclass readableMapClass = env->FindClass("com/facebook/react/bridge/ReadableNativeMap");
    gReadableNativeMapClass = (jclass)env->NewGlobalRef(readableMapClass);
    gReadableNativeMapCtor = env->GetMethodID(readableMapClass, "<init>", "()V");
    wrap_alias(readableMapClass)->registerNatives({
        makeNativeMethod("hasKey", map::readable::hasKey),
        makeNativeMethod("isNull", map::readable::isNull),
        makeNativeMethod("getBoolean", map::readable::getBooleanKey),
        makeNativeMethod("getDouble", map::readable::getDoubleKey),
        makeNativeMethod("getString", map::readable::getStringKey),
        makeNativeMethod(
          "getArray", "(Ljava/lang/String;)Lcom/facebook/react/bridge/ReadableNativeArray;",
          map::readable::getArrayKey),
        makeNativeMethod(
          "getMap", "(Ljava/lang/String;)Lcom/facebook/react/bridge/ReadableNativeMap;",
          map::readable::getMapKey),
        makeNativeMethod(
          "getType", "(Ljava/lang/String;)Lcom/facebook/react/bridge/ReadableType;",
          map::readable::getValueType),
    });

    registerNatives("com/facebook/react/bridge/WritableNativeMap", {
        makeNativeMethod("putNull", map::writable::putNull),
        makeNativeMethod("putBoolean", map::writable::putBoolean),
        makeNativeMethod("putDouble", map::writable::putDouble),
        makeNativeMethod("putString", map::writable::putString),
        makeNativeMethod(
          "putNativeArray", "(Ljava/lang/String;Lcom/facebook/react/bridge/WritableNativeArray;)V",
          map::writable::putArray),
        makeNativeMethod(
          "putNativeMap", "(Ljava/lang/String;Lcom/facebook/react/bridge/WritableNativeMap;)V",
          map::writable::putMap),
        makeNativeMethod(
          "mergeNativeMap", "(Lcom/facebook/react/bridge/ReadableNativeMap;)V",
          map::writable::mergeMap)
    });

    registerNatives("com/facebook/react/bridge/ReadableNativeMap$ReadableNativeMapKeySeyIterator", {
      makeNativeMethod("initialize", "(Lcom/facebook/react/bridge/ReadableNativeMap;)V",
                       map::iterator::initialize),
      makeNativeMethod("hasNextKey", map::iterator::hasNextKey),
      makeNativeMethod("nextKey", map::iterator::getNextKey),
    });

    registerNatives("com/facebook/react/bridge/JSCJavaScriptExecutor", {
      makeNativeMethod("initialize", executors::createJSCExecutor),
    });

    registerNatives("com/facebook/react/bridge/ProxyJavaScriptExecutor", {
        makeNativeMethod(
          "initialize", "(Lcom/facebook/react/bridge/ProxyJavaScriptExecutor$JavaJSExecutor;)V",
          executors::createProxyExecutor),
    });

    jclass callbackClass = env->FindClass("com/facebook/react/bridge/ReactCallback");
    bridge::gCallbackMethod = env->GetMethodID(callbackClass, "call", "(IILcom/facebook/react/bridge/ReadableNativeArray;)V");
    bridge::gOnBatchCompleteMethod = env->GetMethodID(callbackClass, "onBatchComplete", "()V");

    registerNatives("com/facebook/react/bridge/ReactBridge", {
        makeNativeMethod("initialize", "(Lcom/facebook/react/bridge/JavaScriptExecutor;Lcom/facebook/react/bridge/ReactCallback;Lcom/facebook/react/bridge/queue/MessageQueueThread;)V", bridge::create),
        makeNativeMethod(
          "loadScriptFromAssets", "(Landroid/content/res/AssetManager;Ljava/lang/String;)V",
          bridge::loadScriptFromAssets),
        makeNativeMethod("loadScriptFromNetworkCached", bridge::loadScriptFromNetworkCached),
        makeNativeMethod("callFunction", bridge::callFunction),
        makeNativeMethod("invokeCallback", bridge::invokeCallback),
        makeNativeMethod("setGlobalVariable", bridge::setGlobalVariable),
        makeNativeMethod("supportsProfiling", bridge::supportsProfiling),
        makeNativeMethod("startProfiler", bridge::startProfiler),
        makeNativeMethod("stopProfiler", bridge::stopProfiler),
    });

    jclass nativeRunnableClass = env->FindClass("com/facebook/react/bridge/queue/NativeRunnable");
    runnable::gNativeRunnableClass = (jclass)env->NewGlobalRef(nativeRunnableClass);
    runnable::gNativeRunnableCtor = env->GetMethodID(nativeRunnableClass, "<init>", "()V");
    wrap_alias(nativeRunnableClass)->registerNatives({
        makeNativeMethod("run", runnable::run),
    });

    jclass messageQueueThreadClass =
      env->FindClass("com/facebook/react/bridge/queue/MessageQueueThread");
    queue::gRunOnQueueThreadMethod =
      env->GetMethodID(messageQueueThreadClass, "runOnQueue", "(Ljava/lang/Runnable;)V");
  });
}

} }
