//===- CDependencies.cpp - Dependency Discovery C Interface ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implements the dependency discovery interface. It provides a C library for
// the functionality that clang-scan-deps provides.
//
//===----------------------------------------------------------------------===//

#include "CASUtils.h"
#include "CXDiagnosticSetDiagnosticConsumer.h"
#include "CXString.h"

#include "clang-c/Dependencies.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningWorker.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/Support/Process.h"

using namespace clang;
using namespace clang::tooling::dependencies;

namespace {
struct DependencyScannerServiceOptions {
  ScanningOutputFormat ConfiguredFormat = ScanningOutputFormat::Full;
  CASOptions CASOpts;
  std::shared_ptr<cas::ObjectStore> CAS;
  std::shared_ptr<cas::ActionCache> Cache;

  ScanningOutputFormat getFormat() const;
};
} // end anonymous namespace

DEFINE_SIMPLE_CONVERSION_FUNCTIONS(DependencyScannerServiceOptions,
                                   CXDependencyScannerServiceOptions)

DEFINE_SIMPLE_CONVERSION_FUNCTIONS(DependencyScanningService,
                                   CXDependencyScannerService)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(DependencyScanningWorker,
                                   CXDependencyScannerWorker)

inline ScanningOutputFormat unwrap(CXDependencyMode Format) {
  switch (Format) {
  case CXDependencyMode_Flat:
    return ScanningOutputFormat::Make;
  case CXDependencyMode_Full:
    return ScanningOutputFormat::Full;
  }
}

static CXOutputKind wrap(ModuleOutputKind MOK) {
  switch (MOK) {
  case ModuleOutputKind::ModuleFile:
    return CXOutputKind_ModuleFile;
  case ModuleOutputKind::DependencyFile:
    return CXOutputKind_Dependencies;
  case ModuleOutputKind::DependencyTargets:
    return CXOutputKind_DependenciesTarget;
  case ModuleOutputKind::DiagnosticSerializationFile:
    return CXOutputKind_SerializedDiagnostics;
  }
  llvm::report_fatal_error("unhandled ModuleOutputKind");
}

void clang_experimental_ModuleDependencySet_dispose(
    CXModuleDependencySet *MDS) {
  for (int I = 0; I < MDS->Count; ++I) {
    CXModuleDependency &MD = MDS->Modules[I];
    clang_disposeString(MD.Name);
    clang_disposeString(MD.ContextHash);
    clang_disposeString(MD.ModuleMapPath);
    clang_disposeStringSet(MD.FileDeps);
    clang_disposeStringSet(MD.ModuleDeps);
    clang_disposeStringSet(MD.BuildArguments);
  }
  delete[] MDS->Modules;
  delete MDS;
}

CXDependencyScannerServiceOptions
clang_experimental_DependencyScannerServiceOptions_create() {
  return wrap(new DependencyScannerServiceOptions);
}

void clang_experimental_DependencyScannerServiceOptions_dispose(
    CXDependencyScannerServiceOptions Opts) {
  delete unwrap(Opts);
}

void clang_experimental_DependencyScannerServiceOptions_setDependencyMode(
    CXDependencyScannerServiceOptions Opts, CXDependencyMode Mode) {
  unwrap(Opts)->ConfiguredFormat = unwrap(Mode);
}

void clang_experimental_DependencyScannerServiceOptions_setCASDatabases(
    CXDependencyScannerServiceOptions Opts, CXCASDatabases CDBs) {
  cas::WrappedCASDatabases &DBs = *cas::unwrap(CDBs);
  unwrap(Opts)->CASOpts = DBs.CASOpts;
  unwrap(Opts)->CAS = DBs.CAS;
  unwrap(Opts)->Cache = DBs.Cache;
}

void clang_experimental_DependencyScannerServiceOptions_setObjectStore(
    CXDependencyScannerServiceOptions Opts, CXCASObjectStore CAS) {
  unwrap(Opts)->CAS = cas::unwrap(CAS)->CAS;
  unwrap(Opts)->CASOpts.CASPath = cas::unwrap(CAS)->CASPath;
}
void clang_experimental_DependencyScannerServiceOptions_setActionCache(
    CXDependencyScannerServiceOptions Opts, CXCASActionCache Cache) {
  unwrap(Opts)->Cache = cas::unwrap(Cache)->Cache;
  unwrap(Opts)->CASOpts.CASPath = cas::unwrap(Cache)->CachePath;
}

CXDependencyScannerService
clang_experimental_DependencyScannerService_create_v0(CXDependencyMode Format) {
  // FIXME: Pass default CASOpts and nullptr as CachingOnDiskFileSystem now.
  CASOptions CASOpts;
  IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> FS;
  return wrap(new DependencyScanningService(
      ScanningMode::DependencyDirectivesScan, unwrap(Format), CASOpts,
      /*CAS=*/nullptr, /*ActionCache=*/nullptr, FS,
      /*ReuseFilemanager=*/false));
}

ScanningOutputFormat DependencyScannerServiceOptions::getFormat() const {
  if (ConfiguredFormat != ScanningOutputFormat::Full)
    return ConfiguredFormat;

  if (!CAS || !Cache)
    return ConfiguredFormat;

  if (llvm::sys::Process::GetEnv("CLANG_CACHE_USE_INCLUDE_TREE"))
    return ScanningOutputFormat::FullIncludeTree;

  if (llvm::sys::Process::GetEnv("CLANG_CACHE_USE_CASFS_DEPSCAN"))
    return ScanningOutputFormat::FullTree;

  // Note: default caching behaviour is currently cas-fs.
  return ConfiguredFormat;
}

CXDependencyScannerService
clang_experimental_DependencyScannerService_create_v1(
    CXDependencyScannerServiceOptions Opts) {
  // FIXME: Pass default CASOpts and nullptr as CachingOnDiskFileSystem now.
  std::shared_ptr<llvm::cas::ObjectStore> CAS = unwrap(Opts)->CAS;
  std::shared_ptr<llvm::cas::ActionCache> Cache = unwrap(Opts)->Cache;
  IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> FS;
  if (CAS && Cache) {
    assert(unwrap(Opts)->CASOpts.getKind() != CASOptions::UnknownCAS &&
           "CAS and ActionCache must match CASOptions");
    FS = llvm::cantFail(
        llvm::cas::createCachingOnDiskFileSystem(CAS));
  }
  return wrap(new DependencyScanningService(
      ScanningMode::DependencyDirectivesScan, unwrap(Opts)->getFormat(),
      unwrap(Opts)->CASOpts, std::move(CAS), std::move(Cache), std::move(FS),
      /*ReuseFilemanager=*/false));
}

void clang_experimental_DependencyScannerService_dispose_v0(
    CXDependencyScannerService Service) {
  delete unwrap(Service);
}

void clang_experimental_FileDependencies_dispose(CXFileDependencies *ID) {
  clang_disposeString(ID->ContextHash);
  clang_disposeStringSet(ID->FileDeps);
  clang_disposeStringSet(ID->ModuleDeps);
  clang_disposeStringSet(ID->BuildArguments);
  delete ID;
}

void clang_experimental_FileDependenciesList_dispose(
    CXFileDependenciesList *FD) {
  for (size_t I = 0; I < FD->NumCommands; ++I) {
    clang_disposeString(FD->Commands[I].ContextHash);
    clang_disposeStringSet(FD->Commands[I].FileDeps);
    clang_disposeStringSet(FD->Commands[I].ModuleDeps);
    clang_disposeString(FD->Commands[I].Executable);
    clang_disposeStringSet(FD->Commands[I].BuildArguments);
  }
  delete[] FD->Commands;
  delete FD;
}

CXDependencyScannerWorker clang_experimental_DependencyScannerWorker_create_v0(
    CXDependencyScannerService Service) {
  return wrap(new DependencyScanningWorker(
      *unwrap(Service), llvm::vfs::createPhysicalFileSystem()));
}

void clang_experimental_DependencyScannerWorker_dispose_v0(
    CXDependencyScannerWorker Worker) {
  delete unwrap(Worker);
}

using HandleTUDepsCallback = llvm::function_ref<void(TranslationUnitDeps)>;

static CXErrorCode getFullDependencies(DependencyScanningWorker *Worker,
                                       ArrayRef<std::string> Compilation,
                                       const char *WorkingDirectory,
                                       CXModuleDiscoveredCallback *MDC,
                                       void *Context, CXString *Error,
                                       DiagnosticConsumer *DiagConsumer,
                                       LookupModuleOutputCallback LookupOutput,
                                       std::optional<StringRef> ModuleName,
                                       HandleTUDepsCallback HandleTUDeps) {
  llvm::StringSet<> AlreadySeen;
  FullDependencyConsumer DepConsumer(AlreadySeen);
  auto Controller = DependencyScanningTool::createActionController(
      *Worker, std::move(LookupOutput), /*PrefixMapping=*/{});

  bool HasDiagConsumer = DiagConsumer;
  bool HasError = Error;
  assert(HasDiagConsumer ^ HasError && "Both DiagConsumer and Error provided");

  if (DiagConsumer) {
    bool Result =
        Worker->computeDependencies(WorkingDirectory, Compilation, DepConsumer,
                                    *Controller, *DiagConsumer, ModuleName);
    if (!Result)
      return CXError_Failure;
  } else if (Error) {
    auto Result = Worker->computeDependencies(
        WorkingDirectory, Compilation, DepConsumer, *Controller, ModuleName);
    if (Result) {
      *Error = cxstring::createDup(llvm::toString(std::move(Result)));
      return CXError_Failure;
    }
  }

  TranslationUnitDeps TU = DepConsumer.takeTranslationUnitDeps();

  if (!TU.ModuleGraph.empty()) {
    CXModuleDependencySet *MDS = new CXModuleDependencySet;
    MDS->Count = TU.ModuleGraph.size();
    MDS->Modules = new CXModuleDependency[MDS->Count];
    for (int I = 0; I < MDS->Count; ++I) {
      CXModuleDependency &M = MDS->Modules[I];
      const ModuleDeps &MD = TU.ModuleGraph[I];
      M.Name = cxstring::createDup(MD.ID.ModuleName);
      M.ContextHash = cxstring::createDup(MD.ID.ContextHash);
      M.ModuleMapPath = cxstring::createDup(MD.ClangModuleMapFile);
      M.FileDeps = cxstring::createSet(MD.FileDeps);
      std::vector<std::string> Modules;
      for (const ModuleID &MID : MD.ClangModuleDeps)
        Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
      M.ModuleDeps = cxstring::createSet(Modules);
      M.BuildArguments = cxstring::createSet(MD.BuildArguments);
    }
    MDC(Context, MDS);
  }

  HandleTUDeps(std::move(TU));
  return CXError_Success;
}

static CXErrorCode getFileDependencies(CXDependencyScannerWorker W, int argc,
                                       const char *const *argv,
                                       const char *WorkingDirectory,
                                       CXModuleDiscoveredCallback *MDC,
                                       void *Context, CXString *Error,
                                       DiagnosticConsumer *DiagConsumer,
                                       LookupModuleOutputCallback LookupOutput,
                                       std::optional<StringRef> ModuleName,
                                       HandleTUDepsCallback HandleTUDeps) {
  if (!W || argc < 2 || !argv)
    return CXError_InvalidArguments;

  DependencyScanningWorker *Worker = unwrap(W);

  if (Worker->getScanningFormat() != ScanningOutputFormat::Full &&
      Worker->getScanningFormat() != ScanningOutputFormat::FullTree &&
      Worker->getScanningFormat() != ScanningOutputFormat::FullIncludeTree)
    return CXError_InvalidArguments;

  std::vector<std::string> Compilation{argv, argv + argc};

  return getFullDependencies(Worker, Compilation, WorkingDirectory, MDC,
                             Context, Error, DiagConsumer, LookupOutput,
                             ModuleName, HandleTUDeps);
}

namespace {
class OutputLookup {
public:
  OutputLookup(void *MLOContext, CXModuleLookupOutputCallback *MLO)
      : MLOContext(MLOContext), MLO(MLO) {}
  std::string lookupModuleOutput(const ModuleID &ID, ModuleOutputKind MOK);

private:
  llvm::DenseMap<ModuleID, std::string> PCMPaths;
  void *MLOContext;
  CXModuleLookupOutputCallback *MLO;
};
} // end anonymous namespace

CXFileDependencies *
clang_experimental_DependencyScannerWorker_getFileDependencies_v3(
    CXDependencyScannerWorker W, int argc, const char *const *argv,
    const char *ModuleName, const char *WorkingDirectory, void *MDCContext,
    CXModuleDiscoveredCallback *MDC, void *MLOContext,
    CXModuleLookupOutputCallback *MLO, unsigned, CXString *Error) {
  OutputLookup OL(MLOContext, MLO);
  auto LookupOutputs = [&](const ModuleID &ID, ModuleOutputKind MOK) {
    return OL.lookupModuleOutput(ID, MOK);
  };
  CXFileDependencies *FDeps = nullptr;
  CXErrorCode Result = getFileDependencies(
      W, argc, argv, WorkingDirectory, MDC, MDCContext, Error, nullptr,
      LookupOutputs,
      ModuleName ? std::optional<StringRef>(ModuleName) : std::nullopt,
      [&](TranslationUnitDeps TU) {
        assert(!TU.DriverCommandLine.empty());
        std::vector<std::string> Modules;
        for (const ModuleID &MID : TU.ClangModuleDeps)
          Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
        FDeps = new CXFileDependencies;
        FDeps->ContextHash = cxstring::createDup(TU.ID.ContextHash);
        FDeps->FileDeps = cxstring::createSet(TU.FileDeps);
        FDeps->ModuleDeps = cxstring::createSet(Modules);
        FDeps->BuildArguments = cxstring::createSet(TU.DriverCommandLine);
      });
  assert(Result != CXError_Success || FDeps);
  (void)Result;
  return FDeps;
}

CXErrorCode clang_experimental_DependencyScannerWorker_getFileDependencies_v4(
    CXDependencyScannerWorker W, int argc, const char *const *argv,
    const char *ModuleName, const char *WorkingDirectory, void *MDCContext,
    CXModuleDiscoveredCallback *MDC, void *MLOContext,
    CXModuleLookupOutputCallback *MLO, unsigned, CXFileDependenciesList **Out,
    CXString *Error) {
  OutputLookup OL(MLOContext, MLO);
  auto LookupOutputs = [&](const ModuleID &ID, ModuleOutputKind MOK) {
    return OL.lookupModuleOutput(ID, MOK);
  };

  if (!Out)
    return CXError_InvalidArguments;
  *Out = nullptr;

  CXErrorCode Result = getFileDependencies(
      W, argc, argv, WorkingDirectory, MDC, MDCContext, Error, nullptr,
      LookupOutputs,
      ModuleName ? std::optional<StringRef>(ModuleName) : std::nullopt,
      [&](TranslationUnitDeps TU) {
        assert(TU.DriverCommandLine.empty());
        std::vector<std::string> Modules;
        for (const ModuleID &MID : TU.ClangModuleDeps)
          Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
        auto *Commands = new CXTranslationUnitCommand[TU.Commands.size()];
        for (size_t I = 0, E = TU.Commands.size(); I < E; ++I) {
          Commands[I].ContextHash = cxstring::createDup(TU.ID.ContextHash);
          Commands[I].FileDeps = cxstring::createSet(TU.FileDeps);
          Commands[I].ModuleDeps = cxstring::createSet(Modules);
          Commands[I].Executable =
              cxstring::createDup(TU.Commands[I].Executable);
          Commands[I].BuildArguments =
              cxstring::createSet(TU.Commands[I].Arguments);
        }
        *Out = new CXFileDependenciesList{TU.Commands.size(), Commands};
      });

  return Result;
}

CXErrorCode clang_experimental_DependencyScannerWorker_getFileDependencies_v5(
    CXDependencyScannerWorker W, int argc, const char *const *argv,
    const char *ModuleName, const char *WorkingDirectory, void *MDCContext,
    CXModuleDiscoveredCallback *MDC, void *MLOContext,
    CXModuleLookupOutputCallback *MLO, unsigned, CXFileDependenciesList **Out,
    CXDiagnosticSet *OutDiags) {
  OutputLookup OL(MLOContext, MLO);
  auto LookupOutputs = [&](const ModuleID &ID, ModuleOutputKind MOK) {
    return OL.lookupModuleOutput(ID, MOK);
  };

  if (!Out)
    return CXError_InvalidArguments;
  *Out = nullptr;

  CXDiagnosticSetDiagnosticConsumer DiagConsumer;

  CXErrorCode Result = getFileDependencies(
      W, argc, argv, WorkingDirectory, MDC, MDCContext, nullptr, &DiagConsumer,
      LookupOutputs,
      ModuleName ? std::optional<StringRef>(ModuleName) : std::nullopt,
      [&](TranslationUnitDeps TU) {
        assert(TU.DriverCommandLine.empty());
        std::vector<std::string> Modules;
        for (const ModuleID &MID : TU.ClangModuleDeps)
          Modules.push_back(MID.ModuleName + ":" + MID.ContextHash);
        auto *Commands = new CXTranslationUnitCommand[TU.Commands.size()];
        for (size_t I = 0, E = TU.Commands.size(); I < E; ++I) {
          Commands[I].ContextHash = cxstring::createDup(TU.ID.ContextHash);
          Commands[I].FileDeps = cxstring::createSet(TU.FileDeps);
          Commands[I].ModuleDeps = cxstring::createSet(Modules);
          Commands[I].Executable =
              cxstring::createDup(TU.Commands[I].Executable);
          Commands[I].BuildArguments =
              cxstring::createSet(TU.Commands[I].Arguments);
        }
        *Out = new CXFileDependenciesList{TU.Commands.size(), Commands};
      });

  *OutDiags = DiagConsumer.getDiagnosticSet();

  return Result;
}

static std::string lookupModuleOutput(const ModuleID &ID, ModuleOutputKind MOK,
                                      void *MLOContext,
                                      CXModuleLookupOutputCallback *MLO) {
  SmallVector<char, 256> Buffer(256);
  size_t Len = MLO(MLOContext, ID.ModuleName.c_str(), ID.ContextHash.c_str(),
                   wrap(MOK), Buffer.data(), Buffer.size());
  if (Len > Buffer.size()) {
    Buffer.resize(Len);
    Len = MLO(MLOContext, ID.ModuleName.c_str(), ID.ContextHash.c_str(),
              wrap(MOK), Buffer.data(), Buffer.size());
  }
  return std::string(Buffer.begin(), Len);
}

std::string OutputLookup::lookupModuleOutput(const ModuleID &ID,
                                             ModuleOutputKind MOK) {
  if (MOK != ModuleOutputKind::ModuleFile)
    return ::lookupModuleOutput(ID, MOK, MLOContext, MLO);
  // PCM paths are looked up repeatedly, so cache them.
  auto PCMPath = PCMPaths.insert({ID, ""});
  if (PCMPath.second)
    PCMPath.first->second = ::lookupModuleOutput(ID, MOK, MLOContext, MLO);
  return PCMPath.first->second;
}
