//===- CASOutputBackend.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/CAS/CASOutputBackend.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/CAS/Utils.h"
#include "llvm/Support/AlignOf.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/StringSaver.h"

using namespace llvm;
using namespace llvm::cas;

void CASOutputBackend::anchor() {}

namespace {
class CASOutputFile final : public vfs::OutputFileImpl {
public:
  Error keep() override { return OnKeep(Path, Bytes); }
  Error discard() override { return Error::success(); }
  raw_pwrite_stream &getOS() override { return OS; }

  using OnKeepType = llvm::unique_function<Error(StringRef, StringRef)>;
  CASOutputFile(StringRef Path, OnKeepType OnKeep)
      : Path(Path.str()), OS(Bytes), OnKeep(std::move(OnKeep)) {}

private:
  std::string Path;
  SmallString<16> Bytes;
  raw_svector_ostream OS;
  OnKeepType OnKeep;
};
} // namespace

CASOutputBackend::CASOutputBackend(std::shared_ptr<ObjectStore> CAS)
    : CASOutputBackend(*CAS) {
  this->OwnedCAS = std::move(CAS);
}

CASOutputBackend::CASOutputBackend(ObjectStore &CAS) : CAS(CAS) {}

CASOutputBackend::~CASOutputBackend() = default;

struct CASOutputBackend::PrivateImpl {
  // FIXME: Use a NodeBuilder here once it exists.
  SmallVector<ObjectRef> Refs;
  BumpPtrAllocator Alloc;
  StringSaver Saver{Alloc};

  struct KindMap {
    StringRef Kind;
    StringRef Path;
  };
  SmallVector<KindMap> KindMaps;

  /// Returns the "kind" name for the path if one was added for it, otherwise
  /// returns the \p Path itself.
  StringRef tryRemapPath(StringRef Path) const {
    for (const KindMap &Map : KindMaps) {
      if (Map.Path == Path)
        return Map.Kind;
    }
    return Path;
  }
};

Expected<std::unique_ptr<vfs::OutputFileImpl>>
CASOutputBackend::createFileImpl(StringRef ResolvedPath,
                                 Optional<vfs::OutputConfig> Config) {
  if (!Impl)
    Impl = std::make_unique<PrivateImpl>();

  // FIXME: CASIDOutputBackend.createFile() should be called NOW (not inside
  // the OnKeep closure) so that if there are initialization errors (such as
  // output directory not existing) they're reported by createFileImpl().
  //
  // The opened file can be kept inside \a CASOutputFile and forwarded.
  return std::make_unique<CASOutputFile>(
      ResolvedPath, [&](StringRef Path, StringRef Bytes) -> Error {
        StringRef Name = Impl->tryRemapPath(Path);
        Optional<ObjectProxy> NameBlob;
        Optional<ObjectProxy> BytesBlob;
        if (Error E = CAS.createProxy(None, Name).moveInto(NameBlob))
          return E;
        if (Error E = CAS.createProxy(None, Bytes).moveInto(BytesBlob))
          return E;

        // FIXME: Should there be a lock taken before accessing PrivateImpl?
        Impl->Refs.push_back(NameBlob->getRef());
        Impl->Refs.push_back(BytesBlob->getRef());
        return Error::success();
      });
}

Expected<ObjectProxy> CASOutputBackend::getCASProxy() {
  // FIXME: Should there be a lock taken before accessing PrivateImpl?
  if (!Impl)
    return CAS.createProxy(None, "");

  SmallVector<ObjectRef> MovedRefs;
  std::swap(MovedRefs, Impl->Refs);

  return CAS.createProxy(MovedRefs, "");
}

Error CASOutputBackend::addObject(StringRef Name, ObjectRef Object) {
  if (!Impl)
    Impl = std::make_unique<PrivateImpl>();

  Name = Impl->tryRemapPath(Name);
  Optional<ObjectProxy> NameBlob;
  if (Error E = CAS.createProxy(None, Name).moveInto(NameBlob))
    return E;

  Impl->Refs.push_back(NameBlob->getRef());
  Impl->Refs.push_back(Object);
  return Error::success();
}

void CASOutputBackend::addKindMap(StringRef Kind, StringRef Path) {
  if (!Impl)
    Impl = std::make_unique<PrivateImpl>();

  StringSaver &SA = Impl->Saver;
  Impl->KindMaps.push_back({SA.save(Kind), SA.save(Path)});
}
