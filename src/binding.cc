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

std::unordered_set<Glob> getIgnoreGlobs(Env env, Value opts) {
  std::unordered_set<Glob> result;

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
          }
        }
      }
    }
  }

  return result;
}

std::shared_ptr<Backend> getBackend(Env env, Value opts) {
  Value b = opts.As<Object>().Get(String::New(env, "backend"));
  std::string backendName;
  if (b.IsString()) {
    backendName = std::string(b.As<String>().Utf8Value().c_str());
  }

  return Backend::getShared(backendName);
}

class SubscribeRunner : public PromiseRunner {
public:
  SubscribeRunner(Env env, Value dir, Value fn, Value opts) : PromiseRunner(env) {
    callbackEnv = env;
    watcher = Watcher::getShared(
      std::string(dir.As<String>().Utf8Value().c_str()),
      getIgnorePaths(env, opts),
      getIgnoreGlobs(env, opts)
    );

    backend = getBackend(env, opts);
    watcher->watch(fn.As<Function>());
  }

private:
  WatcherRef watcher;
  std::shared_ptr<Backend> backend;
  FunctionReference callback;
  napi_env callbackEnv;

  void execute() override {
    try {
      backend->watch(watcher);
      watcher->addBackend(backend);
      if (!watcher->hasCallbacksForEnvironment(callbackEnv)) {
        backend->unwatch(watcher);
      }
    } catch (std::exception&) {
      watcher->destroy();
      throw;
    }
  }
};

class UnsubscribeRunner : public PromiseRunner {
public:
  UnsubscribeRunner(Env env, Value dir, Value fn, Value opts) : PromiseRunner(env) {
    watcher = Watcher::getShared(
      std::string(dir.As<String>().Utf8Value().c_str()),
      getIgnorePaths(env, opts),
      getIgnoreGlobs(env, opts)
    );

    backend = getBackend(env, opts);
    shouldUnwatch = watcher->unwatch(fn.As<Function>());
  }

private:
  WatcherRef watcher;
  std::shared_ptr<Backend> backend;
  bool shouldUnwatch;

  void execute() override {
    if (shouldUnwatch) {
      backend->unwatch(watcher);
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

  Runner *runner = new Runner(info.Env(), info[0], info[1], info[2]);
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
