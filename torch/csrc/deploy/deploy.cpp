#include <c10/util/Exception.h>
#include <torch/csrc/deploy/deploy.h>
#include <torch/cuda.h>

#include <dlfcn.h>
#include <libgen.h>
#include <unistd.h>

struct InterpreterSymbol {
  const char* startSym;
  const char* endSym;
  bool customLoader;
};

// these symbols are generated by cmake, using ld -r -b binary
// libtorch_deployinterpreter.so which takes the contents of the so and embeds
// it into a symbol that is then linked into libtorch_deploy.so. This enables us
// to simply copy the contents of this symbol to disk and dlopen it to create an
// instance of python.

namespace torch {
namespace deploy {

const std::initializer_list<InterpreterSymbol> kInterpreterSearchPath = {
    {"_binary_libtorch_deployinterpreter_all_so_start",
     "_binary_libtorch_deployinterpreter_all_so_end",
     true},
    {"_binary_libtorch_deployinterpreter_cuda_so_start",
     "_binary_libtorch_deployinterpreter_cuda_so_end",
     false},
    {"_binary_libtorch_deployinterpreter_cpu_so_start",
     "_binary_libtorch_deployinterpreter_cpu_so_end",
     false},
};

static bool writeDeployInterpreter(FILE* dst) {
  TORCH_INTERNAL_ASSERT(dst);
  const char* libStart = nullptr;
  const char* libEnd = nullptr;
  bool customLoader = false;
  for (const auto& s : kInterpreterSearchPath) {
    libStart = (const char*)dlsym(nullptr, s.startSym);
    if (libStart) {
      libEnd = (const char*)dlsym(nullptr, s.endSym);
      customLoader = s.customLoader;
      break;
    }
  }
  TORCH_CHECK(
      libStart != nullptr && libEnd != nullptr,
      "torch::deploy requires a build-time dependency on embedded_interpreter or embedded_interpreter_cuda, neither of which were found.  torch::cuda::is_available()=",
      torch::cuda::is_available());

  size_t size = libEnd - libStart;
  size_t written = fwrite(libStart, 1, size, dst);
  TORCH_INTERNAL_ASSERT(size == written, "expected written == size");
  return customLoader;
}

InterpreterManager::InterpreterManager(size_t nInterp) : resources_(nInterp) {
  TORCH_DEPLOY_TRY
  for (const auto i : c10::irange(nInterp)) {
    instances_.emplace_back(this);
    auto I = instances_.back().acquireSession();
    // make torch.version.interp be the interpreter id
    // can be used for balancing work across GPUs
    I.global("torch", "version").attr("__setattr__")({"interp", int(i)});
    // std::cerr << "Interpreter " << i << " initialized\n";
    instances_.back().pImpl_->setFindModule(
        [this](const std::string& name) -> at::optional<std::string> {
          auto it = registeredModuleSource_.find(name);
          if (it != registeredModuleSource_.end()) {
            return it->second;
          } else {
            return at::nullopt;
          }
        });
  }

  // Pre-registered modules.
  // Since torch::deploy::Obj.toIValue cannot infer empty list, we hack it to
  // return None for empty list.
  // TODO(jwtan): Make the discovery of these modules easier.
  reigsterModuleSource(
      "GetArgumentNamesModule",
      "from inspect import signature\n"
      "from typing import Callable, Optional\n"
      "def getArgumentNames(function: Callable) -> Optional[list]:\n"
      "    names = list(signature(function).parameters.keys())\n"
      "    if len(names) == 0:\n"
      "        return None\n"
      "    return names\n");
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Package InterpreterManager::loadPackage(const std::string& uri) {
  TORCH_DEPLOY_TRY
  return Package(uri, this);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Package InterpreterManager::loadPackage(
    std::shared_ptr<caffe2::serialize::ReadAdapterInterface> reader) {
  TORCH_DEPLOY_TRY
  return Package(reader, this);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

Obj InterpreterSession::fromMovable(const ReplicatedObj& obj) {
  TORCH_DEPLOY_TRY
  return impl_->unpickleOrGet(obj.pImpl_->objectId_, obj.pImpl_->data_);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

InterpreterSession ReplicatedObj::acquireSession(
    const Interpreter* onThisInterpreter) const {
  TORCH_DEPLOY_TRY
  InterpreterSession I = onThisInterpreter
      ? onThisInterpreter->acquireSession()
      : pImpl_->manager_->acquireOne();
  I.self = I.fromMovable(*this);
  return I;
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

// NOLINTNEXTLINE(bugprone-exception-escape)
InterpreterSession::~InterpreterSession() {
  if (manager_ && notifyIdx_ >= 0) {
    manager_->resources_.free(notifyIdx_);
  }
}

void ReplicatedObjImpl::unload(const Interpreter* onThisInterpreter) {
  TORCH_DEPLOY_TRY
  if (!onThisInterpreter) {
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    for (auto& interp : manager_->allInstances()) {
      unload(&interp);
    }
    return;
  }

  InterpreterSession I = onThisInterpreter->acquireSession();
  I.impl_->unload(objectId_);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

// NOLINTNEXTLINE(bugprone-exception-escape)
ReplicatedObjImpl::~ReplicatedObjImpl() {
  unload(nullptr);
}

void ReplicatedObj::unload(const Interpreter* onThisInterpreter) {
  TORCH_DEPLOY_TRY
  pImpl_->unload(onThisInterpreter);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

ReplicatedObj InterpreterSession::createMovable(Obj obj) {
  TORCH_DEPLOY_TRY
  TORCH_CHECK(
      manager_,
      "Can only create a movable object when the session was created from an interpreter that is part of a InterpreterManager");
  auto pickled = impl_->pickle(self, obj);
  return ReplicatedObj(std::make_shared<ReplicatedObjImpl>(
      manager_->nextObjectId_++, std::move(pickled), manager_));
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

using dlopen_t = void* (*)(const char*, int);

// ASAN overrides dlopen and errors when it sees the RTLD_DEEPBIND flags because
// it thinks that the library being loaded will not link against its overrides
// for things like malloc/free. However, our specially crafted library doesn't
// have any DT_NEEDED entries -- all undefined symbols will be resolved from the
// process's link map. So it is actually safe to use RTLD_DEEPBIND with ASAN. We
// have to get around its check though, so we do it by finding the real dlopen
// function.
static dlopen_t find_real_dlopen() {
  void* libc = dlopen("libdl.so.2", RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
  TORCH_INTERNAL_ASSERT(libc);
  auto dlopen_ = (dlopen_t)dlsym(libc, "dlopen");
  TORCH_INTERNAL_ASSERT(dlopen_);
  return dlopen_;
}

Interpreter::Interpreter(InterpreterManager* manager)
    : handle_(nullptr), manager_(manager) {
  // NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
  char libraryName[] = "/tmp/torch_deployXXXXXX";
  int fd = mkstemp(libraryName);
  TORCH_INTERNAL_ASSERT(fd != -1, "failed to create temporary file");
  libraryName_ = libraryName;
  FILE* dst = fdopen(fd, "wb");

  customLoader_ = writeDeployInterpreter(dst);
  fclose(dst);
  int flags = RTLD_LOCAL | RTLD_LAZY;
  if (customLoader_) {
    flags |= RTLD_DEEPBIND;
  }

#ifdef FBCODE_CAFFE2
  static dlopen_t dlopen_ = find_real_dlopen();
  handle_ = dlopen_(libraryName, flags);
#else
  handle_ = dlopen(libraryName, flags);
#endif

  if (!handle_) {
    throw std::runtime_error(dlerror());
  }

  // note: if you want better debugging symbols for things inside
  // new_intepreter_impl, comment out this line so that the so lasts long enough
  // for the debugger to see it.
  unlink(libraryName_.c_str());

  if (customLoader_) {
    // when using the custom loader we need to link python symbols against
    // the right version of the symbols for the interpreter which an be looked
    // up from the handle_ to this shared library. here we register the handle
    // with the code that does custom loading of python extensions.
    auto deploySetSelfPtr =
        (void (*)(void*))dlsym(handle_, "deploy_set_self");
    AT_ASSERT(deploySetSelfPtr);
    deploySetSelfPtr(handle_);
  }

  void* newInterpreterImpl = dlsym(handle_, "newInterpreterImpl");
  AT_ASSERT(newInterpreterImpl);
  pImpl_ = std::unique_ptr<InterpreterImpl>(
      ((InterpreterImpl * (*)()) newInterpreterImpl)());
}

Interpreter::~Interpreter() {
  if (handle_) {
    // ensure python uninitialization runs before we dlclose the library
    pImpl_.reset();
    if (customLoader_) {
      auto deploy_flush_python_libs =
          (void (*)())dlsym(handle_, "deploy_flush_python_libs");
      deploy_flush_python_libs();
    }
    dlclose(handle_);
  }
}

int LoadBalancer::acquire() {
  TORCH_DEPLOY_TRY
  thread_local int last = 0;
  size_t minusers = SIZE_MAX;
  int minIdx = 0;
  for (size_t i = 0; i < n_; ++i, ++last) {
    // NOLINTNEXTLINE(clang-diagnostic-sign-compare)
    if (last >= n_) {
      last = 0;
    }
    uint64_t prev = 0;
    bool acquired = __atomic_compare_exchange_n(
        &uses_[8 * last],
        &prev,
        1ULL,
        false,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    if (acquired) {
      // fast path, we found an interpreter with no users
      return last;
    }
    // slow path, we don't want to use this interpreter because it is being
    // used by someone else.

    if (prev < minusers) {
      minusers = prev;
      minIdx = last;
    }
  }
  // we failed to find a completely free interpreter. heuristically use the
  // one with the least number of user (note that this may have changed since
  // then, so this is only a heuristic).
  __atomic_fetch_add(&uses_[8 * minIdx], 1ULL, __ATOMIC_SEQ_CST);
  return minIdx;
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

void LoadBalancer::free(int where) {
  TORCH_DEPLOY_TRY
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
  __atomic_fetch_sub(&uses_[8 * where], 1ULL, __ATOMIC_SEQ_CST);
  TORCH_DEPLOY_SAFE_CATCH_RETHROW
}

void PythonMethodWrapper::setArgumentNames(
    std::vector<std::string>& argumentNamesOut) const {
  auto session = model_.acquireSession();
  auto method = session.self.attr(methodName_.c_str());
  auto iArgumentNames =
      session.global("GetArgumentNamesModule", "getArgumentNames")({method})
          .toIValue();
  if (iArgumentNames.isNone()) {
    return;
  }

  TORCH_INTERNAL_ASSERT(iArgumentNames.isList());
  auto argumentNames = iArgumentNames.toListRef();

  argumentNamesOut.reserve(argumentNames.size());
  for (auto& argumentName : argumentNames) {
    TORCH_INTERNAL_ASSERT(argumentName.isString());
    argumentNamesOut.push_back(argumentName.toStringRef());
  }
}

} // namespace deploy
} // namespace torch
