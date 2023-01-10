//===- ScanDependencies.cpp - TableGen scan-deps implementation -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/TableGen/ScanDependencies.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrefixMapper.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;
using namespace llvm::tablegen;

void tablegen::flattenStrings(ArrayRef<StringRef> List,
                              SmallVectorImpl<char> &Flattened) {
  for (StringRef Name : List) {
    Flattened.append(Name.begin(), Name.end());
    Flattened.push_back(0);
  }
}

void tablegen::splitFlattenedStrings(StringRef Flattened,
                                     SmallVectorImpl<StringRef> &List) {
  while (!Flattened.empty()) {
    auto Null = Flattened.find(0);
    // FIXME: Return an error, or bail out and try uncached?
    assert(Null != StringRef::npos &&
           "Expected valid list of null-terminated includes");
    List.push_back(Flattened.take_front(Null));
    Flattened = Flattened.drop_front(Null + 1);
  }
}

void tablegen::scanTextForIncludes(StringRef Input,
                                   SmallVectorImpl<StringRef> &Includes) {
  constexpr StringLiteral Keyword = "include";
  SmallString<256> ResultToCache;
  for (auto I = Input.find(Keyword); I != StringRef::npos;
       I = Input.find(Keyword)) {
    // Check (non-exhaustively!) if we can skip this match because it's part of
    // a longer identifier.
    bool Skip = I > 0 && (isalpha(Input[I - 1]) || isdigit(Input[I - 1]));
    Input = Input.drop_front(I + Keyword.size());
    if (Input.size() < 3 || Skip)
      return;
    auto Pos = Input.find_first_not_of(" \t\v\r\n");
    if (Pos == StringRef::npos)
      return;
    {
      auto Q = Input[Pos];
      Input = Input.drop_front(Pos + 1);
      if (Q != '"')
        continue;
    }

    // Assume that tablegen files don't have escaped quotes in their
    // filenames.
    Pos = Input.find_first_of("\r\n\"");
    if (Pos == StringRef::npos)
      return;
    StringRef Name = Input.take_front(Pos);
    {
      auto Q = Input[Pos];
      Input = Input.drop_front(Pos + 1);
      if (Q != '"')
        continue;
    }

    Includes.push_back(Name);
  }
}

static Error
fetchCachedIncludedFiles(cas::ObjectStore &CAS, cas::ObjectRef ID,
                         SmallVectorImpl<StringRef> &IncludedFiles) {
  Expected<cas::ObjectProxy> ExpectedBlob = CAS.getProxy(ID);
  if (!ExpectedBlob)
    return ExpectedBlob.takeError();
  splitFlattenedStrings(ExpectedBlob->getData(), IncludedFiles);
  return Error::success();
}

static Error computeIncludedFiles(cas::ObjectStore &CAS,
                                  cas::ActionCache &Cache, cas::CASID Key,
                                  StringRef Input,
                                  SmallVectorImpl<StringRef> &IncludedFiles) {
  SmallString<256> ResultToCache;
  scanTextForIncludes(Input, IncludedFiles);
  flattenStrings(IncludedFiles, ResultToCache);

  Expected<cas::ObjectProxy> ExpectedResult =
      CAS.createProxy(std::nullopt, ResultToCache);
  if (!ExpectedResult)
    return ExpectedResult.takeError();
  if (Error E = Cache.put(Key, *ExpectedResult))
    return E;
  return Error::success();
}

Error tablegen::scanTextForIncludes(cas::ObjectStore &CAS,
                                    cas::ActionCache &Cache,
                                    cas::ObjectRef ExecID,
                                    const cas::ObjectProxy &Blob,
                                    SmallVectorImpl<StringRef> &Includes) {
  constexpr StringLiteral CacheKeyData = "llvm::tablegen::scanTextForIncludes";

  Expected<cas::ObjectRef> Key =
      CAS.storeFromString({ExecID, Blob.getRef()}, CacheKeyData);
  if (!Key)
    return Key.takeError();

  cas::CASID KeyID = CAS.getID(*Key);
  auto Result = Cache.get(KeyID);
  if (!Result)
    return Result.takeError();
  if (*Result) {
    if (Optional<cas::ObjectRef> ResultID = CAS.getReference(**Result))
      return fetchCachedIncludedFiles(CAS, *ResultID, Includes);
  }
  return computeIncludedFiles(CAS, Cache, KeyID, Blob.getData(), Includes);
}

Optional<cas::ObjectRef>
tablegen::lookupIncludeID(cas::CachingOnDiskFileSystem &FS,
                          ArrayRef<std::string> IncludeDirs,
                          StringRef Filename) {
  SmallString<256> Path;
  Optional<cas::CASID> ID;
  if (!FS.statusAndFileID(Filename, ID).getError())
    return FS.getCAS().getReference(*ID);
  for (StringRef Dir : IncludeDirs) {
    // Match path logic from SourceMgr::AddIncludeFile.
    Path.assign(Dir);
    Path.append(sys::path::get_separator());
    Path.append(Filename);
    if (!FS.statusAndFileID(Path, ID).getError())
      return FS.getCAS().getReference(*ID);
  }
  return std::nullopt;
}

Error tablegen::accessAllIncludes(cas::CachingOnDiskFileSystem &FS,
                                  cas::ActionCache &Cache,
                                  cas::ObjectRef ExecID,
                                  ArrayRef<std::string> IncludeDirs,
                                  const cas::ObjectProxy &MainFileBlob) {
  cas::ObjectStore &CAS = FS.getCAS();

  // Helper for adding to the worklist.
  SmallVector<cas::ObjectProxy> Worklist = {MainFileBlob};
  SmallDenseSet<cas::ObjectRef, 16> Seen;
  Seen.insert(MainFileBlob.getRef());
  auto push = [&Seen, &Worklist, &CAS](cas::ObjectRef ID) -> Error {
    if (!Seen.insert(ID).second)
      return Error::success();

    Expected<cas::ObjectProxy> Blob = CAS.getProxy(ID);
    if (!Blob)
      return Blob.takeError();
    Worklist.push_back(*Blob);
    return Error::success();
  };

  // Find an overapproximation of 'include' directives.
  SmallVector<StringRef> IncludedFiles;
  while (!Worklist.empty()) {
    IncludedFiles.clear();
    if (Error E = scanTextForIncludes(CAS, Cache, ExecID,
                                      Worklist.pop_back_val(), IncludedFiles))
      return E;

    // Add included files to the worklist. Ignore files not found, since the
    // fuzzy search above could have found commented out includes.
    for (StringRef IncludedFile : IncludedFiles)
      if (Optional<cas::ObjectRef> ID =
              lookupIncludeID(FS, IncludeDirs, IncludedFile))
        if (Error E = push(*ID))
          return E;
  }
  return Error::success();
}

static Expected<cas::ObjectProxy> openMainFile(cas::CachingOnDiskFileSystem &FS,
                                               StringRef MainFilename) {
  cas::ObjectStore &CAS = FS.getCAS();

  // FIXME: FileSystem should virtualize stdin.
  if (MainFilename == "-") {
    // Get stdin, then add it to the CAS.
    ErrorOr<std::unique_ptr<MemoryBuffer>> MaybeFile = MemoryBuffer::getSTDIN();
    if (!MaybeFile)
      return createMainFileError(MainFilename, MaybeFile.getError());
    return CAS.createProxy(std::nullopt, (**MaybeFile).getBuffer());
  }

  Optional<cas::CASID> MainFileID;
  if (std::error_code EC =
          FS.statusAndFileID(MainFilename, MainFileID).getError())
    return createMainFileError(MainFilename, EC);
  return CAS.getProxy(*MainFileID);
}

Error tablegen::createMainFileError(StringRef MainFilename,
                                    std::error_code EC) {
  assert(EC && "Expected error");
  return createStringError(EC, "Could not open input file '" + MainFilename +
                                   "': " + EC.message());
}

Expected<ScanIncludesResult>
tablegen::scanIncludes(cas::ObjectStore &CAS, cas::ActionCache &Cache,
                       cas::ObjectRef ExecID, StringRef MainFilename,
                       ArrayRef<std::string> IncludeDirs,
                       ArrayRef<MappedPrefix> PrefixMappings,
                       Optional<TreePathPrefixMapper> *CapturedPM) {
  IntrusiveRefCntPtr<cas::CachingOnDiskFileSystem> FS;
  if (Error E = cas::createCachingOnDiskFileSystem(CAS).moveInto(FS))
    return std::move(E);

  FS->trackNewAccesses();
  Expected<cas::ObjectProxy> MainBlob = openMainFile(*FS, MainFilename);
  if (!MainBlob)
    return MainBlob.takeError();

  // Helper for adding to the worklist.
  if (Error E = accessAllIncludes(*FS, Cache, ExecID, IncludeDirs, *MainBlob))
    return std::move(E);

  Optional<TreePathPrefixMapper> LocalPM;
  Optional<TreePathPrefixMapper> &PM = CapturedPM ? *CapturedPM : LocalPM;
  if (!PrefixMappings.empty()) {
    PM.emplace(FS);
    PM->addRange(PrefixMappings);
    PM->sort();
  }

  Expected<cas::ObjectProxy> Tree =
      FS->createTreeFromNewAccesses([&](const vfs::CachedDirectoryEntry &Entry,
                                        SmallVectorImpl<char> &Storage) {
        return PM ? PM->mapDirEntry(Entry, Storage) : Entry.getTreePath();
      });
  if (!Tree)
    return Tree.takeError();

  return ScanIncludesResult{Tree->getRef(), *MainBlob};
}

Expected<ScanIncludesResult>
tablegen::scanIncludesAndRemap(cas::ObjectStore &CAS, cas::ActionCache &Cache,
                               cas::ObjectRef ExecID, std::string &MainFilename,
                               std::vector<std::string> &IncludeDirs,
                               ArrayRef<MappedPrefix> PrefixMappings) {
  Optional<TreePathPrefixMapper> PM;
  auto Result = scanIncludes(CAS, Cache, ExecID, MainFilename, IncludeDirs,
                             PrefixMappings, &PM);
  if (!Result)
    return Result.takeError();

  if (!PM)
    return Result;

  // Remap the main filename.
  PM->mapInPlace(MainFilename);

  // Remap includes.
  for (std::string &Dir : IncludeDirs)
    PM->mapInPlace(Dir);
  llvm::erase_value(IncludeDirs, "");

  return Result;
}
