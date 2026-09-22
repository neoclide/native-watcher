#include <unordered_set>
#include <node_api.h>
#include <napi.h>
#include "Glob.hh"
#include "Event.hh"
#include "Backend.hh"
#include "Watcher.hh"
#include "PromiseRunner.hh"

using namespace Napi;

std::unordered_set<std::string> getIgnorePaths(Env env, Value opts) {
  std::unordered_set<std::string> result;

  if (opts.IsObject()) {
    Value v = opts.As<Object>().Get(String::New(env, "ignorePaths"));
    if (v.IsArray()) {
      Array items = v.As<Array>();
      for (size_t i = 0; i < items.Length(); i++) {
        Value item = items.Get(Number::New(env, static_cast<double>(i)));
        if (item.IsString()) {
          result.insert(std::string(item.As<String>().Utf8Value().c_str()));
        }
      }
    }
  }

  return result;
}

bool getIgnoreGlobs(Env env, Value opts, std::unordered_set<Glob> &result) {
  if (opts.IsObject()) {
    Value v = opts.As<Object>().Get(String::New(env, "ignoreGlobs"));
    if (v.IsArray()) {
      Array items = v.As<Array>();
      for (size_t i = 0; i < items.Length(); i++) {
        Value item = items.Get(Number::New(env, static_cast<double>(i)));
        if (item.IsString()) {
          auto key = item.As<String>().Utf8Value();
          try {
            result.emplace(key);
          } catch (const std::regex_error& e) {
            Error::New(env, e.what()).ThrowAsJavaScriptException();
            return false;
          }
        }
      }
    }
  }

  return true;
}

std::shared_ptr<Backend> getBackend(Env env, Value opts) {
  Value b = opts.As<Object>().Get(String::New(env, "backend"));
  std::string backendName;
  if (b.IsString()) {
    backendName = std::string(b.As<String>().Utf8Value().c_str());
  }

  try {
    return Backend::getShared(backendName);
  } catch (const std::exception &error) {
    Error::New(env, error.what()).ThrowAsJavaScriptException();
    return nullptr;
  }
}

class SubscribeRunner : public PromiseRunner {
public:
  SubscribeRunner(
    Env env,
    std::string dir,
    Function fn,
    std::unordered_set<std::string> ignorePaths,
    std::unordered_set<Glob> ignoreGlobs,
    std::shared_ptr<Backend> selectedBackend
  ) : PromiseRunner(env) {
    callbackEnv = env;
    watcher = Watcher::getShared(
      dir,
      ignorePaths,
      ignoreGlobs
    );

    backend = selectedBackend;
    try {
      callback = Napi::Persistent(fn);
      addedCallback = watcher->watch(fn);
      watcher->releaseShared();
    } catch (...) {
      watcher->releaseShared();
      throw;
    }
  }

  ~SubscribeRunner() override {
    if (cleanupAfterFailure && addedCallback) {
      watcher->unwatch(callback.Value());
    }
    releaseBackend();
  }

private:
  WatcherRef watcher;
  std::shared_ptr<Backend> backend;
  FunctionReference callback;
  napi_env callbackEnv;
  bool hasBackendReservation = true;
  bool cleanupAfterFailure = false;
  bool addedCallback = false;

  void releaseBackend() {
    if (hasBackendReservation) {
      hasBackendReservation = false;
      backend->releaseShared();
    }
  }

  void execute() override {
    try {
      backend->watch(watcher);
      watcher->addBackend(backend);
      if (!watcher->hasCallbacksForEnvironment(callbackEnv)) {
        backend->unwatch(watcher);
      }
      releaseBackend();
    } catch (std::exception&) {
      cleanupAfterFailure = true;
      releaseBackend();
      throw;
    }
  }
};

class UnsubscribeRunner : public PromiseRunner {
public:
  UnsubscribeRunner(
    Env env,
    std::string dir,
    Function fn,
    std::unordered_set<std::string> ignorePaths,
    std::unordered_set<Glob> ignoreGlobs,
    std::shared_ptr<Backend> selectedBackend
  ) : PromiseRunner(env) {
    watcher = Watcher::getShared(
      dir,
      ignorePaths,
      ignoreGlobs
    );

    backend = selectedBackend;
    try {
      shouldUnwatch = watcher->unwatch(fn);
      watcher->releaseShared();
    } catch (...) {
      watcher->releaseShared();
      throw;
    }
  }

  ~UnsubscribeRunner() override {
    releaseBackend();
  }

private:
  WatcherRef watcher;
  std::shared_ptr<Backend> backend;
  bool shouldUnwatch;
  bool hasBackendReservation = true;

  void releaseBackend() {
    if (hasBackendReservation) {
      hasBackendReservation = false;
      backend->releaseShared();
    }
  }

  void execute() override {
    try {
      if (shouldUnwatch) {
        backend->unwatch(watcher);
      }
      releaseBackend();
    } catch (...) {
      releaseBackend();
      throw;
    }
  }
};

template<class Runner>
Value queueSubscriptionWork(const CallbackInfo& info) {
  Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    TypeError::New(env, "Expected a string").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 2 || !info[1].IsFunction()) {
    TypeError::New(env, "Expected a function").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() >= 3 && !info[2].IsObject()) {
    TypeError::New(env, "Expected an object").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto ignorePaths = getIgnorePaths(env, info[2]);
  std::unordered_set<Glob> ignoreGlobs;
  if (!getIgnoreGlobs(env, info[2], ignoreGlobs)) {
    return env.Null();
  }

  auto backend = getBackend(env, info[2]);
  if (!backend) {
    return env.Null();
  }
  Runner *runner;
  try {
    runner = new Runner(
      env,
      info[0].As<String>().Utf8Value(),
      info[1].As<Function>(),
      std::move(ignorePaths),
      std::move(ignoreGlobs),
      backend
    );
  } catch (...) {
    backend->releaseShared();
    throw;
  }
  return runner->queue();
}

Value subscribe(const CallbackInfo& info) {
  return queueSubscriptionWork<SubscribeRunner>(info);
}

Value unsubscribe(const CallbackInfo& info) {
  return queueSubscriptionWork<UnsubscribeRunner>(info);
}

void cleanupEnvironment(void *data) {
  Watcher::cleanupEnvironment(static_cast<napi_env>(data));
}

Object Init(Env env, Object exports) {
  napi_status status = napi_add_env_cleanup_hook(env, cleanupEnvironment, env);
  if (status != napi_ok) {
    Error::New(env, "Unable to register environment cleanup hook")
      .ThrowAsJavaScriptException();
    return exports;
  }

  exports.Set(
    String::New(env, "subscribe"),
    Function::New(env, subscribe)
  );
  exports.Set(
    String::New(env, "unsubscribe"),
    Function::New(env, unsubscribe)
  );
  return exports;
}

NODE_API_MODULE(watcher, Init)
