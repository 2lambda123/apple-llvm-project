//===- BuiltinCAS.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/LazyAtomicPointer.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CAS/BuiltinObjectHasher.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/HashMappedTrie.h"
#include "llvm/CAS/OnDiskHashMappedTrie.h"
#include "llvm/CAS/ThreadSafeAllocator.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/StringSaver.h"

#define DEBUG_TYPE "builtin-cas"

using namespace llvm;
using namespace llvm::cas;

namespace {

/// Dereference the inner value in \p E, adding an Optional to the outside.
/// Useful for stripping an inner Optional in return chaining.
///
/// \code
/// Expected<Optional<SomeType>> f1(...);
///
/// Expected<SomeType> f2(...) {
///   if (Optional<Expected<NoneType>> E = dereferenceValue(f1()))
///     return std::move(*E);
///
///   // Deal with None...
/// }
/// \endcode
template <class T>
static Optional<Expected<T>> dereferenceValue(Expected<Optional<T>> E) {
  if (!E)
    return Expected<T>(E.takeError());
  if (*E)
    return Expected<T>(std::move(**E));
  return None;
}

/// Dereference the inner value in \p E, generating an error on failure.
///
/// \code
/// Expected<Optional<SomeType>> f1(...);
///
/// Expected<SomeType> f2(...) {
///   if (Optional<Expected<NoneType>> E = dereferenceValue(f1()))
///     return std::move(*E);
///
///   // Deal with None...
/// }
/// \endcode
template <class T>
static Expected<T> dereferenceValue(Expected<Optional<T>> E,
                                    function_ref<Error()> OnNone) {
  if (Optional<Expected<T>> MaybeExpected = dereferenceValue(std::move(E)))
    return std::move(*MaybeExpected);
  return OnNone();
}

/// If \p E and \c *E, move \c **E into \p Sink.
///
/// Enables expected and optional chaining in one statement:
///
/// \code
/// Expected<Optional<Type1>> f1(...);
///
/// Expected<Optional<Type2>> f2(...) {
///   SomeType V;
///   if (Optional<Expected<NoneType>> E = moveValueInto(f1(), V))
///     return std::move(*E);
///
///   // Deal with value...
/// }
/// \endcode
template <class T, class SinkT>
static Optional<Expected<NoneType>>
moveValueInto(Expected<Optional<T>> ExpectedOptional, SinkT &Sink) {
  if (!ExpectedOptional)
    return Expected<NoneType>(ExpectedOptional.takeError());
  if (!*ExpectedOptional)
    return Expected<NoneType>(None);
  Sink = std::move(*ExpectedOptional);
  return None;
}

} // end anonymous namespace

namespace {

static StringRef toStringRef(ArrayRef<char> Data) {
  return StringRef(Data.data(), Data.size());
}

static ArrayRef<char> toArrayRef(StringRef Data) {
  return ArrayRef<char>(Data.data(), Data.size());
}

class BuiltinCAS : public CASDB {
public:
  StringRef getHashSchemaIdentifier() const final {
    return "llvm.cas.builtin.v1[SHA1]";
  }

  Expected<CASID> parseCASID(StringRef Reference) final;
  Error printCASID(raw_ostream &OS, CASID ID) const final;

  bool isKnownObject(CASID ID) final { return bool(getObjectKind(ID)); }

  virtual Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) = 0;

  SmallString<64> getPrintedIDOrHash(CASID ID) const;

  static size_t getPageSize() {
    static int PageSize = sys::Process::getPageSizeEstimate();
    return PageSize;
  }

  Expected<TreeRef> createTree(ArrayRef<NamedTreeEntry> Entries) final;
  virtual Expected<TreeRef>
  createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                 ArrayRef<NamedTreeEntry> SortedEntries) = 0;

  Expected<NodeRef> createNode(ArrayRef<CASID> References,
                               StringRef Data) final {
    return createNode(References, toArrayRef(Data));
  }
  Expected<NodeRef> createNode(ArrayRef<CASID> References, ArrayRef<char> Data);
  virtual Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                           ArrayRef<CASID> References,
                                           ArrayRef<char> Data) = 0;

  Expected<BlobRef>
  createBlobFromOpenFileImpl(sys::fs::file_t FD,
                             Optional<sys::fs::file_status> Status) override;
  virtual Expected<BlobRef>
  createBlobFromNullTerminatedRegion(ArrayRef<uint8_t> ComputedHash,
                                     sys::fs::mapped_file_region Map) {
    return createBlobImpl(ComputedHash, makeArrayRef(Map.data(), Map.size()));
  }

  Expected<BlobRef> createBlob(StringRef Data) final {
    return createBlob(makeArrayRef(Data.data(), Data.size()));
  }
  Expected<BlobRef> createBlob(ArrayRef<char> Data);
  virtual Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                           ArrayRef<char> Data) = 0;

  static StringRef getKindName(ObjectKind Kind) {
    switch (Kind) {
    case ObjectKind::Blob:
      return "blob";
    case ObjectKind::Node:
      return "node";
    case ObjectKind::Tree:
      return "tree";
    }
  }

  Error createUnknownObjectError(CASID ID) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "unknown object '" + getPrintedIDOrHash(ID) + "'");
  }

  Error createCorruptObjectError(CASID ID) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "corrupt object '" + getPrintedIDOrHash(ID) + "'");
  }

  Error createInvalidObjectError(CASID ID, ObjectKind Kind) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid object '" + getPrintedIDOrHash(ID) +
                                 "' for kind '" + getKindName(Kind) + "'");
  }

  /// FIXME: This should not use Error.
  Error createResultCacheMissError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "no result for '" + getPrintedIDOrHash(Input) +
                                 "'");
  }

  Error createResultCachePoisonedError(CASID Input, CASID Output,
                                       CASID ExistingOutput) const {
    return createStringError(
        std::make_error_code(std::errc::invalid_argument),
        "cache poisoned for '" + getPrintedIDOrHash(Input) + "' (new='" +
            getPrintedIDOrHash(Output) + "' vs. existing '" +
            getPrintedIDOrHash(ExistingOutput) + "')");
  }

  Error createResultCacheCorruptError(CASID Input) const {
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "result cache corrupt for '" +
                                 getPrintedIDOrHash(Input) + "'");
  }
};

/// Current hash type for the internal CAS.
using HasherT = SHA1;
using HashType = decltype(HasherT::hash(std::declval<ArrayRef<uint8_t> &>()));

} // end anonymous namespace

static StringRef getCASIDPrefix() { return "~{CASFS}:"; }

static void extractPrintableHash(CASID ID, SmallVectorImpl<char> &Dest) {
  ArrayRef<uint8_t> RawHash = ID.getHash();
  assert(Dest.empty());
  assert(RawHash.size() == 20);
  Dest.reserve(40);
  auto ToChar = [](uint8_t Bit) {
    if (Bit < 10)
      return '0' + Bit;
    return Bit - 10 + 'a';
  };
  for (uint8_t Bit : RawHash) {
    uint8_t High = Bit >> 4;
    uint8_t Low = Bit & 0xf;
    Dest.push_back(ToChar(High));
    Dest.push_back(ToChar(Low));
  }
}

SmallString<64> BuiltinCAS::getPrintedIDOrHash(CASID ID) const {
  SmallString<64> Printed;
  if (Error E = getPrintedCASID(ID, Printed)) {
    consumeError(std::move(E));
    extractPrintableHash(ID, Printed);
  }
  return Printed;
}

static HashType stringToHash(StringRef Chars) {
  auto FromChar = [](char Ch) -> unsigned {
    if (Ch >= '0' && Ch <= '9')
      return Ch - '0';
    assert(Ch >= 'a');
    assert(Ch <= 'f');
    return Ch - 'a' + 10;
  };

  HashType Hash;
  assert(Chars.size() == sizeof(Hash) * 2);
  for (int I = 0, E = sizeof(Hash); I != E; ++I) {
    uint8_t High = FromChar(Chars[I * 2]);
    uint8_t Low = FromChar(Chars[I * 2 + 1]);
    Hash[I] = (High << 4) | Low;
  }
  return Hash;
}

Expected<CASID> BuiltinCAS::parseCASID(StringRef Reference) {
  if (!Reference.consume_front(getCASIDPrefix()))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "invalid cas-id '" + Reference + "'");

  // FIXME: Allow shortened references?
  if (Reference.size() != 2 * sizeof(HashType))
    return createStringError(std::make_error_code(std::errc::invalid_argument),
                             "wrong size for cas-id hash '" + Reference + "'");

  // FIXME: Take parsing as a hint that the ID will be loaded and do a look up
  // of blobs and trees, rather than always allocating space for a hash.
  return parseCASIDImpl(stringToHash(Reference));
}

Error BuiltinCAS::printCASID(raw_ostream &OS, CASID ID) const {
  if (ID.getHash().size() != sizeof(HashType))
    return errorCodeToError(std::make_error_code(std::errc::invalid_argument));

  SmallString<64> Hash;
  extractPrintableHash(ID, Hash);
  OS << getCASIDPrefix() << Hash;
  return Error::success();
}

Expected<BlobRef> BuiltinCAS::createBlob(ArrayRef<char> Data) {
  return createBlobImpl(BuiltinObjectHasher<HasherT>::hashBlob(Data), Data);
}

Expected<BlobRef>
BuiltinCAS::createBlobFromOpenFileImpl(sys::fs::file_t FD,
                                       Optional<sys::fs::file_status> Status) {
  int PageSize = getPageSize();

  if (!Status) {
    Status.emplace();
    if (std::error_code EC = sys::fs::status(FD, *Status))
      return errorCodeToError(EC);
  }

  constexpr size_t MinMappedSize = 4 * 4096;
  auto readWithStream = [&]() -> Expected<BlobRef> {
    SmallString<MinMappedSize * 2> Data;
    if (Error E = sys::fs::readNativeFileToEOF(FD, Data, MinMappedSize))
      return std::move(E);
    return createBlob(makeArrayRef(Data.data(), Data.size()));
  };

  // Check whether we can trust the size from stat.
  if (Status->type() != sys::fs::file_type::regular_file &&
      Status->type() != sys::fs::file_type::block_file)
    return readWithStream();

  if (Status->getSize() < MinMappedSize)
    return readWithStream();

  std::error_code EC;
  sys::fs::mapped_file_region Map(FD, sys::fs::mapped_file_region::readonly,
                                  Status->getSize(),
                                  /*offset=*/0, EC);
  if (EC)
    return errorCodeToError(EC);

  // If the file is guaranteed to be null-terminated, use it directly. Note
  // that the file size may have changed from ::stat if this file is volatile,
  // so we need to check for an actual null character at the end.
  ArrayRef<char> Data(Map.data(), Map.size());
  HashType ComputedHash = BuiltinObjectHasher<HasherT>::hashBlob(Data);
  if (!isAligned(Align(PageSize), Data.size()) && Data.end()[0] == 0)
    return createBlobFromNullTerminatedRegion(ComputedHash, std::move(Map));
  return createBlobImpl(ComputedHash, Data);
}

Expected<TreeRef> BuiltinCAS::createTree(ArrayRef<NamedTreeEntry> Entries) {
  // Ensure a stable order for tree entries and ignore name collisions.
  SmallVector<NamedTreeEntry> Sorted(Entries.begin(), Entries.end());
  std::stable_sort(Sorted.begin(), Sorted.end());
  Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());

  return createTreeImpl(
      BuiltinObjectHasher<HasherT>::hashTree(Sorted), Sorted);
}

Expected<NodeRef> BuiltinCAS::createNode(ArrayRef<CASID> References,
                                         ArrayRef<char> Data) {
  return createNodeImpl(BuiltinObjectHasher<HasherT>::hashNode(References, Data),
                        References, Data);
}

namespace {

class InMemoryObject;
class InMemoryString;

/// Index of referenced IDs (map: Hash -> InMemoryObject*). Uses
/// LazyAtomicPointer to coordinate creation of objects.
using InMemoryIndexT =
    ThreadSafeHashMappedTrie<LazyAtomicPointer<const InMemoryObject>,
                             sizeof(HashType)>;

/// Values in \a InMemoryIndexT. \a InMemoryObject's point at this to access
/// their hash.
using InMemoryIndexValueT = InMemoryIndexT::value_type;

/// String pool.
using InMemoryStringPoolT =
    ThreadSafeHashMappedTrie<LazyAtomicPointer<const InMemoryString>,
                             sizeof(HashType)>;

/// Action cache type (map: Hash -> InMemoryObject*). Always refers to existing
/// objects.
using InMemoryCacheT =
    ThreadSafeHashMappedTrie<const InMemoryIndexValueT *, sizeof(HashType)>;

class InMemoryObject {
public:
  enum class Kind {
    /// Node with refs and data.
    RefNode,

    /// Blob with data (8-byte alignment guaranteed, null-terminated).
    RefBlob,

    /// Node with refs and data co-allocated.
    InlineNode,

    /// Blob with data (as above) co-allocated.
    InlineBlob,

    /// Tree: custom node. Pairs of refs pointing at target (arbitrary object)
    /// and names (String), and some data to describe the kind of the entry.
    Tree,

    Max = Tree,
  };

  Kind getKind() const { return IndexAndKind.getInt(); }
  const InMemoryIndexValueT &getIndex() const {
    assert(IndexAndKind.getPointer());
    return *IndexAndKind.getPointer();
  }

  ArrayRef<uint8_t> getHash() const { return getIndex().Hash; }

  InMemoryObject() = delete;
  InMemoryObject(InMemoryObject &&) = delete;
  InMemoryObject(const InMemoryObject &) = delete;

protected:
  InMemoryObject(Kind K, const InMemoryIndexValueT &I) : IndexAndKind(&I, K) {}

private:
  enum Counts : int {
    NumKindBits = 3,
  };
  PointerIntPair<const InMemoryIndexValueT *, NumKindBits, Kind> IndexAndKind;
  static_assert((1U << NumKindBits) <= alignof(InMemoryIndexValueT),
                "Kind will clobber pointer");
  static_assert(((int)Kind::Max >> NumKindBits) == 0, "Kind will be truncated");
};

template <class DerivedT> class GetDataString {
public:
  StringRef getDataString() const {
    ArrayRef<char> Array = static_cast<const DerivedT *>(this)->getData();
    return StringRef(Array.begin(), Array.size());
  }
};

class InMemoryBlob : public InMemoryObject, public GetDataString<InMemoryBlob> {
public:
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == Kind::RefBlob || O->getKind() == Kind::InlineBlob;
  }
  inline ArrayRef<char> getData() const;

private:
  using InMemoryObject::InMemoryObject;
};

class InMemoryRefBlob : public InMemoryBlob {
public:
  static constexpr Kind KindValue = Kind::RefBlob;
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == KindValue;
  }

  ArrayRef<char> getDataImpl() const { return Data; }
  ArrayRef<char> getData() const { return Data; }

  static InMemoryRefBlob &create(function_ref<void *(size_t Size)> Allocate,
                                 const InMemoryIndexValueT &I,
                                 ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryRefBlob));
    return *new (Mem) InMemoryRefBlob(I, Data);
  }

private:
  InMemoryRefBlob(const InMemoryIndexValueT &I, ArrayRef<char> Data)
      : InMemoryBlob(KindValue, I), Data(Data) {
    assert(isAddrAligned(Align(8), Data.data()) && "Expected 8-byte alignment");
    assert(*Data.end() == 0 && "Expected null-termination");
  }
  ArrayRef<char> Data;
};

class InMemoryInlineBlob : public InMemoryBlob {
public:
  static constexpr Kind KindValue = Kind::InlineBlob;
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == KindValue;
  }

  ArrayRef<char> getDataImpl() const {
    return makeArrayRef(reinterpret_cast<const char *>(this + 1), Size);
  }
  ArrayRef<char> getData() const { return getDataImpl(); }

  static InMemoryInlineBlob &create(function_ref<void *(size_t Size)> Allocate,
                                    const InMemoryIndexValueT &I,
                                    ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryInlineBlob) + Data.size() + 1);
    return *new (Mem) InMemoryInlineBlob(I, Data);
  }

private:
  InMemoryInlineBlob(const InMemoryIndexValueT &I, ArrayRef<char> Data)
      : InMemoryBlob(KindValue, I), Size(Data.size()) {
    auto *Begin = reinterpret_cast<char *>(this + 1);
    llvm::copy(Data, Begin);
    Begin[Data.size()] = 0;
  }
  size_t Size;
};

class InMemoryNode : public InMemoryObject, public GetDataString<InMemoryNode> {
public:
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == Kind::RefNode || O->getKind() == Kind::InlineNode;
  }
  inline ArrayRef<char> getData() const;
  inline ArrayRef<const InMemoryObject *> getRefs() const;

private:
  using InMemoryObject::InMemoryObject;
};

class InMemoryRefNode : public InMemoryNode {
public:
  static constexpr Kind KindValue = Kind::RefNode;
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == KindValue;
  }

  ArrayRef<const InMemoryObject *> getRefsImpl() const { return Refs; }
  ArrayRef<const InMemoryObject *> getRefs() const { return Refs; }
  ArrayRef<char> getDataImpl() const { return Data; }
  ArrayRef<char> getData() const { return Data; }

  static InMemoryRefNode &create(function_ref<void *(size_t Size)> Allocate,
                                 const InMemoryIndexValueT &I,
                                 ArrayRef<const InMemoryObject *> Refs,
                                 ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryRefNode));
    return *new (Mem) InMemoryRefNode(I, Refs, Data);
  }

private:
  InMemoryRefNode(const InMemoryIndexValueT &I,
                  ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data)
      : InMemoryNode(KindValue, I), Refs(Refs), Data(Data) {
    assert(isAddrAligned(Align(8), this) && "Expected 8-byte alignment");
    assert(isAddrAligned(Align(8), Data.data()) && "Expected 8-byte alignment");
    assert(*Data.end() == 0 && "Expected null-termination");
  }

  ArrayRef<const InMemoryObject *> Refs;
  ArrayRef<char> Data;
};

class InMemoryInlineNode : public InMemoryNode {
public:
  static constexpr Kind KindValue = Kind::InlineNode;
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == KindValue;
  }

  ArrayRef<const InMemoryObject *> getRefs() const { return getRefsImpl(); }
  ArrayRef<const InMemoryObject *> getRefsImpl() const {
    return makeArrayRef(
        reinterpret_cast<const InMemoryObject *const *>(this + 1), NumRefs);
  }

  ArrayRef<char> getData() const { return getDataImpl(); }
  ArrayRef<char> getDataImpl() const {
    ArrayRef<const InMemoryObject *> Refs = getRefs();
    return makeArrayRef(
        reinterpret_cast<const char *>(Refs.data() + Refs.size()), DataSize);
  }

  static InMemoryInlineNode &create(function_ref<void *(size_t Size)> Allocate,
                                    const InMemoryIndexValueT &I,
                                    ArrayRef<const InMemoryObject *> Refs,
                                    ArrayRef<char> Data) {
    void *Mem = Allocate(sizeof(InMemoryInlineNode) +
                         sizeof(uintptr_t) * Refs.size() + Data.size() + 1);
    return *new (Mem) InMemoryInlineNode(I, Refs, Data);
  }

private:
  InMemoryInlineNode(const InMemoryIndexValueT &I,
                     ArrayRef<const InMemoryObject *> Refs, ArrayRef<char> Data)
      : InMemoryNode(KindValue, I), NumRefs(Refs.size()),
        DataSize(Data.size()) {
    auto *BeginRefs = reinterpret_cast<const InMemoryObject **>(this + 1);
    llvm::copy(Refs, BeginRefs);
    auto *BeginData = reinterpret_cast<char *>(BeginRefs + NumRefs);
    llvm::copy(Data, BeginData);
    BeginData[Data.size()] = 0;
  }
  uint32_t NumRefs;
  uint32_t DataSize;
};

class InMemoryTree : public InMemoryObject {
public:
  static constexpr Kind KindValue = Kind::Tree;
  static bool classof(const InMemoryObject *O) {
    return O->getKind() == KindValue;
  }

  struct NamedRef {
    const InMemoryObject *Ref;
    const InMemoryString *Name;
  };

  struct NamedEntry {
    const InMemoryObject *Ref;
    const InMemoryString *Name;
    TreeEntry::EntryKind Kind;
  };

  ArrayRef<NamedRef> getNamedRefs() const {
    return makeArrayRef(reinterpret_cast<const NamedRef *>(this + 1), Size);
  }

  ArrayRef<TreeEntry::EntryKind> getKinds() const {
    ArrayRef<NamedRef> Refs = getNamedRefs();
    return makeArrayRef(reinterpret_cast<const TreeEntry::EntryKind *>(
                            Refs.data() + Refs.size()),
                        Size);
  }

  Optional<NamedEntry> find(StringRef Name) const;

  bool empty() const { return !Size; }
  size_t size() const { return Size; }

  NamedEntry operator[](ptrdiff_t I) const {
    assert((size_t)I < size());
    NamedRef NR = getNamedRefs()[I];
    return NamedEntry{NR.Ref, NR.Name, getKinds()[I]};
  }

  static InMemoryTree &create(function_ref<void *(size_t Size)> Allocate,
                              const InMemoryIndexValueT &I,
                              ArrayRef<NamedEntry> Entries);

private:
  InMemoryTree(const InMemoryIndexValueT &I, ArrayRef<NamedEntry> Entries);
  size_t Size;
};

/// Internal string type.
class InMemoryString {
public:
  StringRef get() const {
    return StringRef(reinterpret_cast<const char *>(this + 1), Size);
  }

  static InMemoryString &create(function_ref<void *(size_t Size)> Allocate,
                                StringRef String) {
    assert(String.size() <= UINT32_MAX && "Expected strings smaller than 4GB");
    void *Mem = Allocate(sizeof(InMemoryString) + String.size() + 1);
    return *new (Mem) InMemoryString(String);
  }

private:
  InMemoryString(StringRef String) : Size(String.size()) {
    auto *Begin = reinterpret_cast<char *>(this + 1);
    llvm::copy(String, Begin);
    Begin[String.size()] = 0;
  }
  size_t Size;
};

/// In-memory CAS database and action cache (the latter should be separated).
class InMemoryCAS : public BuiltinCAS {
public:
  Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) final {
    return getID(indexHash(Hash));
  }

  Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<char> Data) final;
  Expected<TreeRef>
  createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                 ArrayRef<NamedTreeEntry> SortedEntries) final;
  Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<CASID> References,
                                   ArrayRef<char> Data) final;
  Optional<ObjectKind> getObjectKind(CASID ID) final;

  Expected<BlobRef>
  createBlobFromNullTerminatedRegion(ArrayRef<uint8_t> ComputedHash,
                                     sys::fs::mapped_file_region Map) override;

  CASID getID(const InMemoryIndexValueT &I) const {
    return CASID::getFromInternalID(*this, reinterpret_cast<uintptr_t>(&I));
  }
  CASID getID(const InMemoryObject &O) const { return getID(O.getIndex()); }
  const InMemoryIndexValueT &extractIndexFromID(CASID ID) const {
    assert(&ID.getContext() == this);
    return *reinterpret_cast<const InMemoryIndexValueT *>(
        (uintptr_t)ID.getInternalID(*this));
  }
  InMemoryIndexValueT &extractIndexFromID(CASID ID) {
    return const_cast<InMemoryIndexValueT &>(
        const_cast<const InMemoryCAS *>(this)->extractIndexFromID(ID));
  }

  ArrayRef<uint8_t> getHashImpl(const CASID &ID) const final {
    return extractIndexFromID(ID).Hash;
  }

  Expected<BlobRef> getBlobRef(const InMemoryObject &Object) {
    if (auto *Blob = dyn_cast<InMemoryBlob>(&Object))
      return makeBlobRef(getID(Object), Blob->getDataString());
    return createInvalidObjectError(getID(Object), ObjectKind::Blob);
  }

  Expected<NodeRef> getNodeRef(const InMemoryObject &Object) {
    if (auto *Node = dyn_cast<InMemoryNode>(&Object))
      return makeNodeRef(getID(Object), Node, Node->getRefs().size(),
                         Node->getDataString());
    return createInvalidObjectError(getID(Object), ObjectKind::Node);
  }

  Expected<TreeRef> getTreeRef(const InMemoryObject &Object) {
    if (auto *Tree = dyn_cast<InMemoryTree>(&Object))
      return makeTreeRef(getID(Object), Tree, Tree->size());
    return createInvalidObjectError(getID(Object), ObjectKind::Tree);
  }

  InMemoryIndexValueT &indexHash(ArrayRef<uint8_t> Hash) {
    return *Index.insertLazy(
        Hash, [](auto ValueConstructor) { ValueConstructor.emplace(nullptr); });
  }

  /// TODO: Consider callers to actually do an insert and to return a handle to
  /// the slot in the trie.
  const InMemoryObject *getObject(CASID ID) const {
    if (&ID.getContext() == this)
      return extractIndexFromID(ID).Data;
    assert(ID.getContext().getHashSchemaIdentifier() ==
               getHashSchemaIdentifier() &&
           "Expected ID from same hash schema");
    if (InMemoryIndexT::const_pointer P = Index.find(ID.getHash()))
      return P->Data;
    return nullptr;
  }

  const InMemoryString &getOrCreateString(StringRef String);
  Expected<BlobRef> getBlob(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getBlobRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Blob);
  }
  Expected<NodeRef> getNode(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getNodeRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Node);
  }
  Expected<TreeRef> getTree(CASID ID) final {
    if (const InMemoryObject *Object = getObject(ID))
      return getTreeRef(*Object);
    return createInvalidObjectError(ID, ObjectKind::Tree);
  }
  void print(raw_ostream &OS) const final;

  Expected<CASID> getCachedResult(CASID InputID) final;
  Error putCachedResult(CASID InputID, CASID OutputID) final;

  InMemoryCAS() = default;

private:
  // TreeAPI.
  NamedTreeEntry makeTreeEntry(const InMemoryTree::NamedEntry &Entry) const {
    return NamedTreeEntry(getID(*Entry.Ref), Entry.Kind, Entry.Name->get());
  }
  Optional<NamedTreeEntry> lookupInTree(const TreeRef &Handle,
                                        StringRef Name) const final;
  NamedTreeEntry getInTree(const TreeRef &Handle, size_t I) const final;
  Error forEachEntryInTree(
      const TreeRef &Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const final;

  // NodeAPI.
  CASID getReferenceInNode(const NodeRef &Handle, size_t I) const final;
  Error forEachReferenceInNode(const NodeRef &Handle,
                               function_ref<Error(CASID)> Callback) const final;

  /// Index of referenced IDs (map: Hash -> InMemoryObject*). Mapped to nullptr
  /// as a convenient way to store hashes.
  ///
  /// - Insert nullptr on lookups.
  /// - InMemoryObject points back to here.
  InMemoryIndexT Index;

  /// String pool for trees.
  InMemoryStringPoolT StringPool;

  /// Action cache (map: Hash -> InMemoryObject*).
  InMemoryCacheT ActionCache;

  ThreadSafeAllocator<BumpPtrAllocator> Objects;
  ThreadSafeAllocator<BumpPtrAllocator> Strings;
  ThreadSafeAllocator<SpecificBumpPtrAllocator<sys::fs::mapped_file_region>>
      MemoryMaps;
};

} // end anonymous namespace

Optional<ObjectKind> InMemoryCAS::getObjectKind(CASID ID) {
  const InMemoryObject *Object = getObject(ID);
  if (!Object)
    return None;
  switch (Object->getKind()) {
  case InMemoryObject::Kind::RefNode:
  case InMemoryObject::Kind::InlineNode:
    return ObjectKind::Node;

  case InMemoryObject::Kind::RefBlob:
  case InMemoryObject::Kind::InlineBlob:
    return ObjectKind::Blob;

  case InMemoryObject::Kind::Tree:
    return ObjectKind::Tree;
  }
}

ArrayRef<char> InMemoryBlob::getData() const {
  if (auto *Derived = dyn_cast<InMemoryRefBlob>(this))
    return Derived->getDataImpl();
  return cast<InMemoryInlineBlob>(this)->getDataImpl();
}

ArrayRef<char> InMemoryNode::getData() const {
  if (auto *Derived = dyn_cast<InMemoryRefNode>(this))
    return Derived->getDataImpl();
  return cast<InMemoryInlineNode>(this)->getDataImpl();
}

ArrayRef<const InMemoryObject *> InMemoryNode::getRefs() const {
  if (auto *Derived = dyn_cast<InMemoryRefNode>(this))
    return Derived->getRefsImpl();
  return cast<InMemoryInlineNode>(this)->getRefsImpl();
}

void InMemoryCAS::print(raw_ostream &OS) const {
  OS << "index: ";
  Index.print(OS);
  OS << "strings: ";
  Index.print(OS);
  OS << "action-cache: ";
  ActionCache.print(OS);
}

InMemoryTree &InMemoryTree::create(function_ref<void *(size_t Size)> Allocate,
                                   const InMemoryIndexValueT &I,
                                   ArrayRef<NamedEntry> Entries) {
  void *Mem =
      Allocate(sizeof(InMemoryTree) + sizeof(NamedRef) * Entries.size() +
               sizeof(TreeEntry::EntryKind) * Entries.size());
  return *new (Mem) InMemoryTree(I, Entries);
}

InMemoryTree::InMemoryTree(const InMemoryIndexValueT &I,
                           ArrayRef<NamedEntry> Entries)
    : InMemoryObject(KindValue, I), Size(Entries.size()) {
  const InMemoryString *LastName = nullptr;
  auto *Ref = reinterpret_cast<NamedRef *>(this + 1);
  for (const NamedEntry &E : Entries) {
    assert(E.Ref);
    assert(E.Name);
    new (Ref++) NamedRef{E.Ref, E.Name};

    (void)(LastName);
    assert((!LastName || LastName->get() < E.Name->get()) &&
           "Expected names to be unique and sorted");
  }
  auto *Entry = reinterpret_cast<TreeEntry::EntryKind *>(Ref);
  for (const NamedEntry &E : Entries)
    new (Entry++) TreeEntry::EntryKind(E.Kind);
}

Optional<InMemoryTree::NamedEntry> InMemoryTree::find(StringRef Name) const {
  auto Compare = [](const NamedRef &LHS, StringRef RHS) {
    return LHS.Name->get().compare(RHS) < 0;
  };

  ArrayRef<NamedRef> Refs = getNamedRefs();
  const NamedRef *I = std::lower_bound(Refs.begin(), Refs.end(), Name, Compare);
  if (I == Refs.end() || I->Name->get() != Name)
    return None;
  return operator[](I - Refs.begin());
}

Expected<BlobRef> InMemoryCAS::createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                              ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);

  // Load or generate.
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryInlineBlob::create(Allocator, I, Data);
  };

  return getBlobRef(I.Data.loadOrGenerate(Generator));
}

Expected<BlobRef> InMemoryCAS::createBlobFromNullTerminatedRegion(
    ArrayRef<uint8_t> ComputedHash, sys::fs::mapped_file_region Map) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  ArrayRef<char> Data(Map.data(), Map.size());
  auto &I = indexHash(ComputedHash);

  // Load or generate.
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryRefBlob::create(Allocator, I, Data);
  };
  const InMemoryObject &Object = I.Data.loadOrGenerate(Generator);

  // Save Map if the winning blob uses it.
  if (auto *Blob = dyn_cast<InMemoryRefBlob>(&Object))
    if (Blob->getData().data() == Map.data())
      new (MemoryMaps.Allocate()) sys::fs::mapped_file_region(std::move(Map));

  return getBlobRef(Object);
}

Expected<NodeRef> InMemoryCAS::createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                              ArrayRef<CASID> Refs,
                                              ArrayRef<char> Data) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);
  if (const InMemoryObject *Node = I.Data.load())
    return getNodeRef(*Node);

  // Create the node.
  SmallVector<const InMemoryObject *> InternalRefs;
  for (CASID ID : Refs) {
    if (const InMemoryObject *Object = getObject(ID))
      InternalRefs.push_back(Object);
    else
      return createInvalidObjectError(ID, ObjectKind::Node);
  }
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryInlineNode::create(Allocator, I, InternalRefs, Data);
  };
  return getNodeRef(I.Data.loadOrGenerate(Generator));
}

const InMemoryString &InMemoryCAS::getOrCreateString(StringRef String) {
  InMemoryStringPoolT::value_type S =
      *StringPool.insertLazy(
          BuiltinObjectHasher<HasherT>::hashString(String),
          [&](auto ValueConstructor) {
        ValueConstructor.emplace(nullptr);
      });

  auto Allocator = [&](size_t Size) -> void * {
    return Strings.Allocate(Size, alignof(InMemoryString));
  };
  auto Generator = [&]() -> const InMemoryString * {
    return &InMemoryString::create(Allocator, String);
  };
  return S.Data.loadOrGenerate(Generator);
}

Expected<TreeRef>
InMemoryCAS::createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                            ArrayRef<NamedTreeEntry> SortedEntries) {
  // Look up the hash in the index, initializing to nullptr if it's new.
  auto &I = indexHash(ComputedHash);
  if (const InMemoryObject *Tree = I.Data.load())
    return getTreeRef(*Tree);

  // Create the tree.
  SmallVector<InMemoryTree::NamedEntry> InternalEntries;
  for (const NamedTreeEntry &E : SortedEntries) {
    InternalEntries.push_back(
        {getObject(E.getID()), &getOrCreateString(E.getName()), E.getKind()});
    if (!InternalEntries.back().Ref)
      return createUnknownObjectError(E.getID());
  }
  auto Allocator = [&](size_t Size) -> void * {
    return Objects.Allocate(Size, alignof(InMemoryObject));
  };
  auto Generator = [&]() -> const InMemoryObject * {
    return &InMemoryTree::create(Allocator, I, InternalEntries);
  };
  return getTreeRef(I.Data.loadOrGenerate(Generator));
}

Expected<CASID> InMemoryCAS::getCachedResult(CASID InputID) {
  InMemoryCacheT::pointer P = ActionCache.find(InputID.getHash());
  if (!P)
    return createResultCacheMissError(InputID);

  /// TODO: Although, consider inserting null on cache misses and returning a
  /// handle to avoid a second lookup!
  assert(P->Data && "Unexpected null in result cache");
  return getID(*P->Data);
}

Error InMemoryCAS::putCachedResult(CASID InputID, CASID OutputID) {
  const InMemoryIndexT::value_type &Expected = indexHash(OutputID.getHash());
  const InMemoryCacheT::value_type &Cached =
      *ActionCache.insertLazy(InputID.getHash(), [&](auto ValueConstructor) {
        ValueConstructor.emplace(&Expected);
      });

  /// TODO: Although, consider changing \a getCachedResult() to insert nullptr
  /// and returning a handle on cache misses!
  assert(Cached.Data && "Unexpected null in result cache");
  const InMemoryIndexT::value_type &Observed = *Cached.Data;
  if (&Expected == &Observed)
    return Error::success();

  return createResultCachePoisonedError(InputID, OutputID, getID(Observed));
}

Optional<NamedTreeEntry> InMemoryCAS::lookupInTree(const TreeRef &Handle,
                                                   StringRef Name) const {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  if (Optional<InMemoryTree::NamedEntry> E = Tree.find(Name))
    return makeTreeEntry(*E);
  return None;
}

NamedTreeEntry InMemoryCAS::getInTree(const TreeRef &Handle, size_t I) const {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  assert(I < Tree.size() && "Invalid index");
  return makeTreeEntry(Tree[I]);
}

Error InMemoryCAS::forEachEntryInTree(
    const TreeRef &Handle,
    function_ref<Error(const NamedTreeEntry &)> Callback) const {
  auto &Tree = *reinterpret_cast<const InMemoryTree *>(getTreePtr(Handle));
  for (size_t I = 0, E = Tree.size(); I != E; ++I)
    if (Error E = Callback(makeTreeEntry(Tree[I])))
      return E;
  return Error::success();
}

CASID InMemoryCAS::getReferenceInNode(const NodeRef &Handle, size_t I) const {
  auto &Node = *reinterpret_cast<const InMemoryNode *>(getNodePtr(Handle));
  assert(I < Node.getRefs().size() && "Invalid index");
  return getID(*Node.getRefs()[I]);
}

Error InMemoryCAS::forEachReferenceInNode(
    const NodeRef &Handle, function_ref<Error(CASID)> Callback) const {
  auto &Node = *reinterpret_cast<const InMemoryNode *>(getNodePtr(Handle));
  for (const InMemoryObject *Object : Node.getRefs())
    if (Error E = Callback(getID(*Object)))
      return E;
  return Error::success();
}

std::unique_ptr<CASDB> cas::createInMemoryCAS() {
  return std::make_unique<InMemoryCAS>();
}

namespace {

/// Trie record data: 8B, atomic<uint64_t>
/// - 1-byte: StorageKind
/// - 1-byte: ObjectKind
/// - 6-bytes: DataStoreOffset (offset into referenced file)
class TrieRecord {
public:
  enum class StorageKind : uint8_t {
    /// Unknown object.
    Unknown = 0,

    /// v1.data: main pool, full DataStore record.
    DataPool = 1,

    /// v1.data: main pool, string with 2B size field.
    DataPoolString2B = 2,

    /// v1.<TrieRecordOffset>.data: standalone, with a full DataStore record.
    Standalone = 10,

    /// v1.<TrieRecordOffset>.blob: standalone, just the data. File contents
    /// exactly the data content and file size matches the data size. No refs.
    StandaloneBlob = 11,

    /// v1.<TrieRecordOffset>.blob+0: standalone, just the data plus an
    /// extra null character ('\0'). File size is 1 bigger than the data size.
    /// No refs.
    StandaloneBlob0 = 12,
  };

  enum class ObjectKind : uint8_t {
    /// Node: refs and data.
    Invalid = 0,

    /// Node: refs and data.
    Node = 1,

    /// Blob: data, 8-byte alignment guaranteed, null-terminated.
    Blob = 2,

    /// Tree: custom node. Pairs of refs pointing at target (arbitrary object)
    /// and names (String), and some data to describe the kind of the entry.
    Tree = 3,

    /// String: data, no alignment guarantee, null-terminated.
    String = 4,
  };

  enum Limits : int64_t {
    // Saves files bigger than 64KB standalone instead of embedding them.
    MaxEmbeddedSize = 64LL * 1024LL - 1,
  };

  struct Data {
    StorageKind SK = StorageKind::Unknown;
    ObjectKind OK = ObjectKind::Invalid;
    FileOffset Offset;
  };

  static uint64_t pack(Data D) {
    assert(D.Offset.get() < (int64_t)(1ULL << 48));
    uint64_t Packed =
        uint64_t(D.SK) << 56 | uint64_t(D.OK) << 48 | D.Offset.get();
    assert(D.SK != StorageKind::Unknown || Packed == 0);
#ifndef NDEBUG
    Data RoundTrip = unpack(Packed);
    assert(D.SK == RoundTrip.SK);
    assert(D.OK == RoundTrip.OK);
    assert(D.Offset.get() == RoundTrip.Offset.get());
#endif
    return Packed;
  }

  static Data unpack(uint64_t Packed) {
    Data D;
    if (!Packed)
      return D;
    D.SK = (StorageKind)(Packed >> 56);
    D.OK = (ObjectKind)((Packed >> 48) & 0xFF);
    D.Offset = FileOffset(Packed & (UINT64_MAX >> 16));
    return D;
  }

  TrieRecord() : Storage(0) {}

  Data load() const { return unpack(Storage); }
  bool compare_exchange_strong(Data &Existing, Data New);

private:
  std::atomic<uint64_t> Storage;
};

class InternalRef4B;

/// 8B reference:
/// - bits  0-47: Offset
/// - bits 48-63: Reserved for other metadata.
class InternalRef {
  enum Counts : size_t {
    NumMetadataBits = 16,
    NumOffsetBits = 64 - NumMetadataBits,
  };

public:
  enum class OffsetKind {
    IndexRecord = 0,
    DataRecord = 1,
    String2B = 2,
  };

  OffsetKind getOffsetKind() const {
    return (OffsetKind)((Data >> NumOffsetBits) & UINT8_MAX);
  }

  FileOffset getFileOffset() const { return FileOffset(getRawOffset()); }

  uint64_t getRawData() const { return Data; }
  uint64_t getRawOffset() const { return Data & (UINT64_MAX >> 16); }

  static InternalRef getFromRawData(uint64_t Data) { return InternalRef(Data); }

  static InternalRef getFromOffset(OffsetKind Kind, FileOffset Offset) {
    assert((uint64_t)Offset.get() <= (UINT64_MAX >> NumMetadataBits) &&
           "Offset must fit in 6B");
    return InternalRef((uint64_t)Kind << NumOffsetBits | Offset.get());
  }

  friend bool operator==(InternalRef LHS, InternalRef RHS) {
    return LHS.Data == RHS.Data;
  }

private:
  InternalRef(OffsetKind Kind, FileOffset Offset)
      : Data((uint64_t)Kind << NumOffsetBits | Offset.get()) {
    assert(Offset.get() == getFileOffset().get());
    assert(Kind == getOffsetKind());
  }
  InternalRef(uint64_t Data) : Data(Data) {}
  uint64_t Data;
};

/// 4B reference:
/// - bits  0-29: Offset
/// - bits 30-31: Reserved for other metadata.
class InternalRef4B {
  enum Counts : size_t {
    NumMetadataBits = 4,
    NumOffsetBits = 32 - NumMetadataBits,
  };

public:
  using OffsetKind = InternalRef::OffsetKind;

  OffsetKind getOffsetKind() const {
    return (OffsetKind)(Data >> NumOffsetBits);
  }

  FileOffset getFileOffset() const {
    uint64_t RawOffset = Data & (UINT32_MAX >> NumMetadataBits);
    return FileOffset(RawOffset << 3);
  }

  /// Shrink to 4B reference.
  static Optional<InternalRef4B> tryToShrink(InternalRef Ref) {
    OffsetKind Kind = Ref.getOffsetKind();
    uint64_t ShiftedKind = (uint64_t)Kind << NumOffsetBits;
    if (ShiftedKind > UINT32_MAX)
      return None;

    uint64_t ShiftedOffset = Ref.getRawOffset();
    assert(isAligned(Align(8), ShiftedOffset));
    ShiftedOffset >>= 3;
    if (ShiftedOffset > (UINT32_MAX >> NumMetadataBits))
      return None;

    return InternalRef4B(ShiftedKind | ShiftedOffset);
  }

  operator InternalRef() const {
    return InternalRef::getFromOffset(getOffsetKind(), getFileOffset());
  }

private:
  friend class InternalRef;
  InternalRef4B(uint32_t Data) : Data(Data) {}
  uint32_t Data;
};

class InternalRefArrayRef {
public:
  size_t size() const { return Size; }
  bool empty() const { return !Size; }

  class iterator
      : public iterator_facade_base<iterator, std::random_access_iterator_tag,
                                    const InternalRef> {
  public:
    bool operator==(const iterator &RHS) const { return I == RHS.I; }
    const InternalRef &operator*() const {
      if (auto *Ref = I.dyn_cast<const InternalRef *>())
        return *Ref;
      LocalProxyFor4B = InternalRef(*I.get<const InternalRef4B *>());
      return *LocalProxyFor4B;
    }
    bool operator<(const iterator &RHS) const {
      if (I == RHS.I)
        return false;
      assert(I.is<const InternalRef *>() == RHS.I.is<const InternalRef *>());
      if (auto *Ref = I.dyn_cast<const InternalRef *>())
        return Ref < RHS.I.get<const InternalRef *>();
      return I.get<const InternalRef4B *>() - RHS.I.get<const InternalRef4B *>();
    }
    ptrdiff_t operator-(const iterator &RHS) const {
      if (I == RHS.I)
        return 0;
      assert(I.is<const InternalRef *>() == RHS.I.is<const InternalRef *>());
      if (auto *Ref = I.dyn_cast<const InternalRef *>())
        return Ref - RHS.I.get<const InternalRef *>();
      return I.get<const InternalRef4B *>() - RHS.I.get<const InternalRef4B *>();
    }
    iterator &operator+=(ptrdiff_t N) {
      if (!N)
        return *this;
      if (auto *Ref = I.dyn_cast<const InternalRef *>())
        I = Ref + N;
      else
        I = I.get<const InternalRef4B *>() + N;
      return *this;
    }
    iterator &operator-=(ptrdiff_t N) {
      if (!N)
        return *this;
      if (auto *Ref = I.dyn_cast<const InternalRef *>())
        I = Ref - N;
      else
        I = I.get<const InternalRef4B *>() - N;
      return *this;
    }

    iterator() = default;

  private:
    friend class InternalRefArrayRef;
    explicit iterator(PointerUnion<const InternalRef *, const InternalRef4B *> I) : I(I) {}
    PointerUnion<const InternalRef *, const InternalRef4B *> I;
    mutable Optional<InternalRef> LocalProxyFor4B;
  };

  bool operator==(const InternalRefArrayRef &RHS) const {
    return size() == RHS.size() && std::equal(begin(), end(), RHS.begin());
  }

  iterator begin() const { return iterator(Begin); }
  iterator end() const { return begin() + Size; }

  /// Array accessor.
  ///
  /// Returns a reference proxy to avoid lifetime issues, since a reference
  /// derived from a InternalRef4B lives inside the iterator.
  iterator::ReferenceProxy operator[](ptrdiff_t N) const { return begin()[N]; }

  bool is4B() const { return Begin.is<const InternalRef4B *>(); }
  bool is8B() const { return Begin.is<const InternalRef *>(); }

  ArrayRef<InternalRef> as8B() const {
    assert(is8B());
    auto *B = Begin.get<const InternalRef *>();
    return makeArrayRef(B, Size);
  }

  ArrayRef<InternalRef4B> as4B() const {
    auto *B = Begin.get<const InternalRef4B *>();
    return makeArrayRef(B, Size);
  }

  InternalRefArrayRef(NoneType = None) {}

  InternalRefArrayRef(ArrayRef<InternalRef> Refs)
      : Begin(Refs.begin()), Size(Refs.size()) {}

  InternalRefArrayRef(ArrayRef<InternalRef4B> Refs)
      : Begin(Refs.begin()), Size(Refs.size()) {}

private:
  PointerUnion<const InternalRef *, const InternalRef4B *> Begin;
  size_t Size = 0;
};

class InternalRefVector {
public:
  void push_back(InternalRef Ref) {
    if (NeedsFull)
      return FullRefs.push_back(Ref);
    if (Optional<InternalRef4B> Small = InternalRef4B::tryToShrink(Ref))
      return SmallRefs.push_back(*Small);
    NeedsFull = true;
    assert(FullRefs.empty());
    FullRefs.reserve(SmallRefs.size() + 1);
    for (InternalRef4B Small : SmallRefs)
      FullRefs.push_back(Small);
    SmallRefs.clear();
  }

  operator InternalRefArrayRef() const {
    assert(SmallRefs.empty() || FullRefs.empty());
    return NeedsFull ? InternalRefArrayRef(FullRefs)
                     : InternalRefArrayRef(SmallRefs);
  }

private:
  bool NeedsFull = false;
  SmallVector<InternalRef4B> SmallRefs;
  SmallVector<InternalRef> FullRefs;
};

/// DataStore record data: 8B + size? + refs? + data + 0
/// - 8-bytes: Header
/// - {0,4,8}-bytes: DataSize     (may be packed in Header)
/// - {0,4,8}-bytes: NumRefs      (may be packed in Header)
/// - NumRefs*{4,8}-bytes: Refs[] (end-ptr is 8-byte aligned)
/// - <data>
/// - 1-byte: 0-term
struct DataRecordHandle {
  /// NumRefs storage: 4B, 2B, 1B, or 0B (no refs). Or, 8B, for alignment
  /// convenience to avoid computing padding later.
  enum class NumRefsFlags : uint8_t {
    Uses0B = 0U,
    Uses1B = 1U,
    Uses2B = 2U,
    Uses4B = 3U,
    Uses8B = 4U,
    Max = Uses8B,
  };

  /// DataSize storage: 8B, 4B, 2B, or 1B.
  enum class DataSizeFlags {
    Uses1B = 0U,
    Uses2B = 1U,
    Uses4B = 2U,
    Uses8B = 3U,
    Max = Uses8B,
  };

  enum class TrieOffsetFlags {
    /// TrieRecord storage: 6B or 4B.
    Uses6B = 0U,
    Uses4B = 1U,
    Max = Uses4B,
  };

  /// Kind of ref stored in Refs[]: InternalRef or InternalRef4B.
  enum class RefKindFlags {
    InternalRef = 0U,
    InternalRef4B = 1U,
    Max = InternalRef4B,
  };

  enum Counts : int {
    NumRefsShift = 0,
    NumRefsBits = 3,
    DataSizeShift = NumRefsShift + NumRefsBits,
    DataSizeBits = 2,
    TrieOffsetShift = DataSizeShift + DataSizeBits,
    TrieOffsetBits = 1,
    RefKindShift = TrieOffsetShift + TrieOffsetBits,
    RefKindBits = 1,
  };
  static_assert(((UINT32_MAX << NumRefsBits) & (uint32_t)NumRefsFlags::Max) ==
                    0,
                "Not enough bits");
  static_assert(((UINT32_MAX << DataSizeBits) & (uint32_t)DataSizeFlags::Max) ==
                    0,
                "Not enough bits");
  static_assert(((UINT32_MAX << TrieOffsetBits) &
                 (uint32_t)TrieOffsetFlags::Max) == 0,
                "Not enough bits");
  static_assert(((UINT32_MAX << RefKindBits) & (uint32_t)RefKindFlags::Max) ==
                    0,
                "Not enough bits");

  struct LayoutFlags {
    NumRefsFlags NumRefs;
    DataSizeFlags DataSize;
    TrieOffsetFlags TrieOffset;
    RefKindFlags RefKind;

    static uint64_t pack(LayoutFlags LF) {
      unsigned Packed = ((unsigned)LF.NumRefs << NumRefsShift) |
                        ((unsigned)LF.DataSize << DataSizeShift) |
                        ((unsigned)LF.TrieOffset << TrieOffsetShift) |
                        ((unsigned)LF.RefKind << RefKindShift);
#ifndef NDEBUG
      LayoutFlags RoundTrip = unpack(Packed);
      assert(LF.NumRefs == RoundTrip.NumRefs);
      assert(LF.DataSize == RoundTrip.DataSize);
      assert(LF.TrieOffset == RoundTrip.TrieOffset);
      assert(LF.RefKind == RoundTrip.RefKind);
#endif
      return Packed;
    }
    static LayoutFlags unpack(uint64_t Storage) {
      assert(Storage <= UINT8_MAX && "Expect storage to fit in a byte");
      LayoutFlags LF;
      LF.NumRefs =
          (NumRefsFlags)((Storage >> NumRefsShift) & ((1U << NumRefsBits) - 1));
      LF.DataSize = (DataSizeFlags)((Storage >> DataSizeShift) &
                                    ((1U << DataSizeBits) - 1));
      LF.TrieOffset = (TrieOffsetFlags)((Storage >> TrieOffsetShift) &
                                        ((1U << TrieOffsetBits) - 1));
      LF.RefKind =
          (RefKindFlags)((Storage >> RefKindShift) & ((1U << RefKindBits) - 1));
      return LF;
    }
  };

  /// Header layout:
  /// - 1-byte:      LayoutFlags
  /// - 1-byte:      1B size field
  /// - {0,2}-bytes: 2B size field
  /// - {4,6}-bytes: TrieRecordOffset
  struct Header {
    uint64_t Packed;
  };

  struct Input {
    FileOffset TrieRecordOffset;
    InternalRefArrayRef Refs;
    ArrayRef<char> Data;
  };

  LayoutFlags getLayoutFlags() const {
    return LayoutFlags::unpack(H->Packed >> 56);
  }
  FileOffset getTrieRecordOffset() const {
    if (getLayoutFlags().TrieOffset == TrieOffsetFlags::Uses4B)
      return FileOffset(H->Packed & UINT32_MAX);
    return FileOffset(H->Packed & (UINT64_MAX >> 16));
  }

  uint64_t getDataSize() const;
  void skipDataSize(LayoutFlags LF, int64_t &RelOffset) const;
  uint32_t getNumRefs() const;
  void skipNumRefs(LayoutFlags LF, int64_t &RelOffset) const;
  int64_t getRefsRelOffset() const;
  int64_t getDataRelOffset() const;

  static uint64_t getTotalSize(uint64_t DataRelOffset, uint64_t DataSize) {
    return DataRelOffset + DataSize + 1;
  }
  uint64_t getTotalSize() const {
    return getDataRelOffset() + getDataSize() + 1;
  }

  struct Layout {
    explicit Layout(const Input &I);

    LayoutFlags Flags{};
    uint64_t TrieRecordOffset = 0;
    uint64_t DataSize = 0;
    uint32_t NumRefs = 0;
    int64_t RefsRelOffset = 0;
    int64_t DataRelOffset = 0;
    uint64_t getTotalSize() const {
      return DataRecordHandle::getTotalSize(DataRelOffset, DataSize);
    }
  };

  InternalRefArrayRef getRefs() const {
    assert(H && "Expected valid handle");
    auto *BeginByte = reinterpret_cast<const char *>(H) + getRefsRelOffset();
    size_t Size = getNumRefs();
    if (!Size)
      return InternalRefArrayRef();
    if (getLayoutFlags().RefKind == RefKindFlags::InternalRef4B)
      return makeArrayRef(reinterpret_cast<const InternalRef4B *>(BeginByte),
                          Size);
    return makeArrayRef(reinterpret_cast<const InternalRef *>(BeginByte), Size);
  }

  ArrayRef<char> getData() const {
    assert(H && "Expected valid handle");
    return makeArrayRef(reinterpret_cast<const char *>(H) + getDataRelOffset(),
                        getDataSize());
  }

  static DataRecordHandle create(function_ref<char *(size_t Size)> Alloc,
                                 const Input &I);
  static Expected<DataRecordHandle>
  createWithError(function_ref<Expected<char *>(size_t Size)> Alloc,
                  const Input &I);
  static DataRecordHandle construct(char *Mem, const Input &I);

  static DataRecordHandle get(const char *Mem) {
    return DataRecordHandle(
        *reinterpret_cast<const DataRecordHandle::Header *>(Mem));
  }

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }

  DataRecordHandle() = default;
  explicit DataRecordHandle(const Header &H) : H(&H) {}

private:
  static DataRecordHandle constructImpl(char *Mem, const Input &I,
                                        const Layout &L);
  const Header *H = nullptr;
};

Expected<DataRecordHandle> DataRecordHandle::createWithError(
    function_ref<Expected<char *>(size_t Size)> Alloc, const Input &I) {
  Layout L(I);
  if (Expected<char *> Mem = Alloc(L.getTotalSize()))
    return constructImpl(*Mem, I, L);
  else
    return Mem.takeError();
}

DataRecordHandle
DataRecordHandle::create(function_ref<char *(size_t Size)> Alloc,
                         const Input &I) {
  Layout L(I);
  return constructImpl(Alloc(L.getTotalSize()), I, L);
}

struct String2BHandle {
  /// Header layout:
  /// - 2-bytes: Length
  struct Header {
    uint16_t Length;
  };

  uint64_t getLength() const { return H->Length; }

  StringRef getString() const {
    assert(H && "Expected valid handle");
    return StringRef(reinterpret_cast<const char *>(H + 1), getLength());
  }

  static String2BHandle create(function_ref<char *(size_t Size)> Alloc,
                               StringRef String) {
    assert(String.size() <= UINT16_MAX);
    char *Mem = Alloc(sizeof(Header) + String.size() + 1);
    Header *H = new (Mem) Header{(uint16_t)String.size()};
    llvm::copy(String, Mem + sizeof(Header));
    Mem[sizeof(Header) + String.size()] = 0;
    return String2BHandle(*H);
  }

  static String2BHandle get(const char *Mem) {
    return String2BHandle(
        *reinterpret_cast<const String2BHandle::Header *>(Mem));
  }

  explicit operator bool() const { return H; }
  const Header &getHeader() const { return *H; }

  String2BHandle() = default;
  explicit String2BHandle(const Header &H) : H(&H) {}

private:
  const Header *H = nullptr;
};

/// On-disk CAS database and action cache (the latter should be separated).
///
/// Here's a top-level description of the current layout (could expose or make
/// this configurable in the future).
///
/// Files:
/// - db/v1.index: HashMappedTrie(name="cas.hashes[sha1]")
/// - db/v1.data: DataStore(name="cas.objects[64K]") for objects <64KB
/// - db/v1.<TrieRecordOffset>: Objects >=64KB
///
/// In theory, these could all be in one file (using a root record that points
/// at the two types of stores), but splitting the files enables setting
/// different max settings.
///
/// Eventually: update UniqueID/CASID to store:
/// - uint64_t: for BuiltinCAS, this is a pointer to Trie record
/// - CASDB*: for knowing how to compare, and for getHash()
///
/// Eventually: add ObjectHandle (update ObjectRef):
/// - uint64_t: for BuiltinCAS, this is a pointer to Data record
/// - CASDB*: for implementing APIs
///
/// Eventually: consider creating a StringPool for strings instead of using
/// RecordDataStore table.
/// - Lookup by prefix tree
/// - Store by suffix tree
class OnDiskCAS : public BuiltinCAS {
public:
  static constexpr StringLiteral IndexTableName = "llvm.cas.index[sha1]";
  static constexpr StringLiteral DataPoolTableName = "llvm.cas.data[sha1]";
  static constexpr StringLiteral ActionCacheTableName =
      "llvm.cas.actions[sha1->sha1]";

  static constexpr StringLiteral IndexFile = "index";
  static constexpr StringLiteral DataPoolFile = "data";
  static constexpr StringLiteral ActionCacheFile = "actions";

  static constexpr StringLiteral FilePrefix = "v1.";
  static constexpr StringLiteral FileSuffixData = ".data";
  static constexpr StringLiteral FileSuffixBlob = ".blob";
  static constexpr StringLiteral FileSuffixBlob0 = ".blob0";

  class TempFile;
  class MappedTempFile;

  struct IndexProxy {
    FileOffset Offset;
    ArrayRef<uint8_t> Hash;
    TrieRecord &ObjectRef;
  };

  IndexProxy indexHash(ArrayRef<uint8_t> Hash);
  Expected<CASID> parseCASIDImpl(ArrayRef<uint8_t> Hash) final {
    return getID(indexHash(Hash));
  }

  Expected<BlobRef> createBlobImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<char> Data) final {
    return getBlobFromProxy(getOrCreateBlob(indexHash(ComputedHash), Data));
  }
  Expected<TreeRef>
  createTreeImpl(ArrayRef<uint8_t> ComputedHash,
                 ArrayRef<NamedTreeEntry> SortedEntries) final {
    return getTreeFromProxy(
        getOrCreateTree(indexHash(ComputedHash), SortedEntries));
  }
  Expected<NodeRef> createNodeImpl(ArrayRef<uint8_t> ComputedHash,
                                   ArrayRef<CASID> References,
                                   ArrayRef<char> Data) final {
    return getNodeFromProxy(
        getOrCreateNode(indexHash(ComputedHash), References, Data));
  }

  struct StringProxy {
    FileOffset IndexOffset;
    TrieRecord::Data Object;
    StringRef String;
  };
  struct BlobProxy {
    FileOffset IndexOffset;
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    ArrayRef<char> Data;
  };
  Expected<BlobProxy> getOrCreateBlob(IndexProxy I, ArrayRef<char> Data);
  Expected<BlobProxy> createStandaloneBlob(IndexProxy &I, ArrayRef<char> Data);

  struct DataRecordProxy {
    FileOffset IndexOffset;
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    DataRecordHandle Record;
  };

  struct ObjectProxy {
    FileOffset IndexOffset;
    TrieRecord::Data Object;
    ArrayRef<uint8_t> Hash;
    Optional<DataRecordHandle> Record;

    /// Blobs and strings that don't have a data record are stored here.
    Optional<ArrayRef<char>> Bytes;

    static ObjectProxy get(BlobProxy Blob) {
      return ObjectProxy{Blob.IndexOffset, Blob.Object, Blob.Hash, None,
                         Blob.Data};
    }
  };
  Expected<DataRecordProxy>
  getOrCreateDataRecord(IndexProxy &I, TrieRecord::ObjectKind OK,
                        DataRecordHandle::Input Input);
  Expected<DataRecordProxy>
  getOrCreateTree(IndexProxy I, ArrayRef<NamedTreeEntry> SortedEntries);
  Expected<DataRecordProxy> getOrCreateNode(IndexProxy I,
                                            ArrayRef<CASID> References,
                                            ArrayRef<char> Data);
  Expected<Optional<DataRecordProxy>> getDataRecord(IndexProxy I) const;

  Expected<MappedTempFile> createTempFile(StringRef FinalPath, uint64_t Size);
  Expected<Optional<BlobProxy>> getBlob(IndexProxy I) const;
  Expected<Optional<StringProxy>> getString(IndexProxy I) const;
  Optional<StringRef> getString(InternalRef I) const;
  Expected<Optional<ObjectProxy>> getObjectProxy(IndexProxy I) const;
  DataRecordHandle getPooledDataRecord(FileOffset Offset) const {
    return DataRecordHandle::get(DataPool.beginData(Offset));
  }

  Optional<CASID> getCASID(InternalRef Ref) const;

  struct PooledDataRecord {
    FileOffset Offset;
    DataRecordHandle Record;
  };
  PooledDataRecord createPooledDataRecord(DataRecordHandle::Input Input);
  void getStandalonePath(TrieRecord::StorageKind SK, const IndexProxy &I,
                         SmallVectorImpl<char> &Path) const;

  CASID getID(const IndexProxy &I) const {
    return getIDFromIndexOffset(I.Offset);
  }
  ArrayRef<uint8_t> getHashImpl(const CASID &ID) const final {
    assert(&ID.getContext() == this && "Expected ID from this CASIDContext");
    OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID);
    assert(P && "Expected to recover pointer from CASID");
    return P->Hash;
  }
  CASID getIDFromIndexOffset(FileOffset Offset) const {
    return CASID::getFromInternalID(*this, Offset.get());
  }

  OnDiskHashMappedTrie::const_pointer getInternalIndexPointer(CASID ID) const;
  Optional<InternalRef> getInternalRef(CASID ID) const;
  Optional<InternalRef> getInternalRef(IndexProxy I,
                                       TrieRecord::Data Object) const;

  IndexProxy
  getIndexProxyFromPointer(OnDiskHashMappedTrie::const_pointer P) const;

  Expected<StringProxy> getOrCreateString(IndexProxy I, StringRef String);
  Expected<InternalRef> getOrCreateStringRef(StringRef String);
  Optional<ObjectKind> getObjectKind(CASID ID) final;

  Expected<BlobRef> getBlob(CASID ID) final;
  Expected<NodeRef> getNode(CASID ID) final;
  Expected<TreeRef> getTree(CASID ID) final;
  Expected<BlobRef> getBlobFromProxy(Expected<BlobProxy> Blob);
  Expected<NodeRef> getNodeFromProxy(Expected<DataRecordProxy> Object);
  Expected<TreeRef> getTreeFromProxy(Expected<DataRecordProxy> Object);

  void print(raw_ostream &OS) const final;

  Expected<CASID> getCachedResult(CASID InputID) final;
  Error putCachedResult(CASID InputID, CASID OutputID) final;

  static Expected<std::unique_ptr<OnDiskCAS>> open(StringRef Path);

private:
  // TreeAPI.
  Optional<NamedTreeEntry> lookupInTree(const TreeRef &Tree,
                                        StringRef Name) const final;
  NamedTreeEntry makeTreeEntry(DataRecordHandle Record, size_t I,
                               ArrayRef<StringRef> NameCache = None) const;
  NamedTreeEntry getInTree(const TreeRef &Tree, size_t I) const final;
  Error forEachEntryInTree(
      const TreeRef &Tree,
      function_ref<Error(const NamedTreeEntry &)> Callback) const final;

  // NodeAPI.
  CASID getReferenceInNode(const NodeRef &Ref, size_t I) const final;
  Error forEachReferenceInNode(const NodeRef &Ref,
                               function_ref<Error(CASID)> Callback) const final;

  StringRef getPathForID(StringRef BaseDir, CASID ID,
                         SmallVectorImpl<char> &Storage);

  Expected<std::unique_ptr<MemoryBuffer>> openFile(StringRef Path);
  Expected<std::unique_ptr<MemoryBuffer>> openFileWithID(StringRef BaseDir,
                                                         CASID ID);

  OnDiskCAS(StringRef RootPath, OnDiskHashMappedTrie Index,
            OnDiskDataAllocator DataPool, OnDiskHashMappedTrie ActionCache);

  /// Mapping from hash to object reference.
  ///
  /// Data type is TrieRecord.
  OnDiskHashMappedTrie Index;

  /// Storage for most objects.
  ///
  /// Data type is DataRecordHandle.
  OnDiskDataAllocator DataPool;

  /// Container for "big" objects mapped in separately.
  template <size_t NumShards> class StandaloneDataMap {
    static_assert(isPowerOf2_64(NumShards), "Expected power of 2");

  public:
    MemoryBufferRef insert(ArrayRef<uint8_t> Hash,
                           std::unique_ptr<MemoryBuffer> Buffer);

    Optional<MemoryBufferRef> lookup(ArrayRef<uint8_t> Hash) const;

  private:
    struct Shard {
      DenseMap<const uint8_t *, std::unique_ptr<MemoryBuffer>> Map;
      mutable std::mutex Mutex;
    };
    Shard &getShard(ArrayRef<uint8_t> Hash) {
      return const_cast<Shard &>(
          const_cast<const StandaloneDataMap *>(this)->getShard(Hash));
    }
    const Shard &getShard(ArrayRef<uint8_t> Hash) const {
      static_assert(NumShards <= 256, "Expected only 8 bits of shard");
      return Shards[Hash[0] % NumShards];
    }

    Shard Shards[NumShards];
  };

  /// Lifetime for "big" objects not in DataPool.
  ///
  /// NOTE: Could use ThreadSafeHashMappedTrie here. For now, doing something
  /// simpler on the assumption there won't be much contention since most data
  /// is not big. If there is contention, and we've already fixed NodeRef
  /// object handles to be cheap enough to use consistently, the fix might be
  /// to use better use of them rather than optimizing this map.
  ///
  /// FIXME: Figure out the right number of shards, if any.
  mutable StandaloneDataMap<16> StandaloneData;

  /// Action cache.
  ///
  /// FIXME: Separate out. Likely change key to be independent from CASID and
  /// stored separately.
  OnDiskHashMappedTrie ActionCache;
  using ActionCacheResultT = std::atomic<uint64_t>;

  std::string RootPath;
  std::string TempPrefix;
};

} // end anonymous namespace

constexpr StringLiteral OnDiskCAS::IndexTableName;
constexpr StringLiteral OnDiskCAS::DataPoolTableName;
constexpr StringLiteral OnDiskCAS::ActionCacheTableName;
constexpr StringLiteral OnDiskCAS::IndexFile;
constexpr StringLiteral OnDiskCAS::DataPoolFile;
constexpr StringLiteral OnDiskCAS::ActionCacheFile;
constexpr StringLiteral OnDiskCAS::FilePrefix;
constexpr StringLiteral OnDiskCAS::FileSuffixData;
constexpr StringLiteral OnDiskCAS::FileSuffixBlob;
constexpr StringLiteral OnDiskCAS::FileSuffixBlob0;

Optional<ObjectKind> OnDiskCAS::getObjectKind(CASID ID) {
  OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID);
  if (!P)
    return None;
  IndexProxy I = getIndexProxyFromPointer(P);
  TrieRecord::Data D = I.ObjectRef.load();
  if (D.SK == TrieRecord::StorageKind::Unknown)
    return None;
  switch (D.OK) {
  case TrieRecord::ObjectKind::Invalid:
    report_fatal_error("invalid object kind detected");
  case TrieRecord::ObjectKind::Node:
    return ObjectKind::Node;
  case TrieRecord::ObjectKind::Blob:
    return ObjectKind::Blob;
  case TrieRecord::ObjectKind::Tree:
    return ObjectKind::Tree;
  case TrieRecord::ObjectKind::String:
    // FIXME: Change this once it's exposed.
    return None;
  }
}

template <size_t N>
MemoryBufferRef
OnDiskCAS::StandaloneDataMap<N>::insert(ArrayRef<uint8_t> Hash,
                                        std::unique_ptr<MemoryBuffer> Buffer) {
  auto &S = getShard(Hash);
  std::lock_guard<std::mutex> Lock(S.Mutex);
  auto &V = S.Map[Hash.data()];
  if (!V)
    V = std::move(Buffer);
  return *V;
}

template <size_t N>
Optional<MemoryBufferRef>
OnDiskCAS::StandaloneDataMap<N>::lookup(ArrayRef<uint8_t> Hash) const {
  auto &S = getShard(Hash);
  std::lock_guard<std::mutex> Lock(S.Mutex);
  auto I = S.Map.find(Hash.data());
  if (I == S.Map.end())
    return None;
  return MemoryBufferRef(*I->second);
}

/// Copy of \a sys::fs::TempFile that skips RemoveOnSignal, which is too
/// expensive to register/unregister at this rate.
///
/// FIXME: Add a TempFileManager that maintains a thread-safe list of open temp
/// files and has a signal handler registerd that removes them all.
class OnDiskCAS::TempFile {
  bool Done = false;
  TempFile(StringRef Name, int FD) : TmpName(std::string(Name)), FD(FD) {}

public:
  /// This creates a temporary file with createUniqueFile.
  static Expected<TempFile> create(const Twine &Model);
  TempFile(TempFile &&Other) { *this = std::move(Other); }
  TempFile &operator=(TempFile &&Other) {
    TmpName = std::move(Other.TmpName);
    FD = Other.FD;
    Other.Done = true;
    Other.FD = -1;
    return *this;
  }

  // Name of the temporary file.
  std::string TmpName;

  // The open file descriptor.
  int FD = -1;

  // Keep this with the given name.
  Error keep(const Twine &Name);
  Error discard();

  // This checks that keep or delete was called.
  ~TempFile() { consumeError(discard()); }
};

class OnDiskCAS::MappedTempFile {
public:
  char *data() const { return Map.data(); }
  size_t size() const { return Map.size(); }

  Error discard() {
    assert(Map && "Map already destroyed");
    Map.unmap();
    return Temp.discard();
  }

  Error keep(const Twine &Name) {
    assert(Map && "Map already destroyed");
    Map.unmap();
    return Temp.keep(Name);
  }

  MappedTempFile(TempFile Temp, sys::fs::mapped_file_region Map)
      : Temp(std::move(Temp)), Map(std::move(Map)) {}

private:
  TempFile Temp;
  sys::fs::mapped_file_region Map;
};

Error OnDiskCAS::TempFile::discard() {
  Done = true;
  if (FD != -1)
    if (std::error_code EC = sys::fs::closeFile(FD))
      return errorCodeToError(EC);
  FD = -1;

  // Always try to close and remove.
  std::error_code RemoveEC;
  if (!TmpName.empty())
    if (std::error_code EC = sys::fs::remove(TmpName))
      return errorCodeToError(EC);
  TmpName = "";

  return Error::success();
}

Error OnDiskCAS::TempFile::keep(const Twine &Name) {
  assert(!Done);
  Done = true;
  // Always try to close and rename.
  std::error_code RenameEC = sys::fs::rename(TmpName, Name);

  if (!RenameEC)
    TmpName = "";

  if (std::error_code EC = sys::fs::closeFile(FD))
    return errorCodeToError(EC);
  FD = -1;

  return errorCodeToError(RenameEC);
}

Expected<OnDiskCAS::TempFile> OnDiskCAS::TempFile::create(const Twine &Model) {
  int FD;
  SmallString<128> ResultPath;
  if (std::error_code EC = sys::fs::createUniqueFile(Model, FD, ResultPath))
    return errorCodeToError(EC);

  TempFile Ret(ResultPath, FD);
  return std::move(Ret);
}

bool TrieRecord::compare_exchange_strong(Data &Existing, Data New) {
  uint64_t ExistingPacked = pack(Existing);
  uint64_t NewPacked = pack(New);
  if (Storage.compare_exchange_strong(ExistingPacked, NewPacked))
    return true;
  Existing = unpack(ExistingPacked);
  return false;
}

DataRecordHandle DataRecordHandle::construct(char *Mem, const Input &I) {
  return constructImpl(Mem, I, Layout(I));
}

DataRecordHandle DataRecordHandle::constructImpl(char *Mem, const Input &I,
                                                 const Layout &L) {
  assert(I.TrieRecordOffset && "Expected an offset into index");
  assert(L.TrieRecordOffset == (uint64_t)I.TrieRecordOffset.get() &&
         "Offset has drifted?");
  char *Next = Mem + sizeof(Header);

  // Fill in Packed and set other data, then come back to construct the header.
  uint64_t Packed = 0;
  Packed |= LayoutFlags::pack(L.Flags) << 56 | L.TrieRecordOffset;

  // Construct DataSize.
  switch (L.Flags.DataSize) {
  case DataSizeFlags::Uses1B:
    assert(I.Data.size() <= UINT8_MAX);
    Packed |= (uint64_t)I.Data.size() << 48;
    break;
  case DataSizeFlags::Uses2B:
    assert(I.Data.size() <= UINT16_MAX);
    Packed |= (uint64_t)I.Data.size() << 32;
    break;
  case DataSizeFlags::Uses4B:
    assert(isAddrAligned(Align(4), Next));
    new (Next) uint32_t(I.Data.size());
    Next += 4;
    break;
  case DataSizeFlags::Uses8B:
    assert(isAddrAligned(Align(8), Next));
    new (Next) uint64_t(I.Data.size());
    Next += 8;
    break;
  }

  // Construct NumRefs.
  //
  // NOTE: May be writing NumRefs even if there are zero refs in order to fix
  // alignment.
  switch (L.Flags.NumRefs) {
  case NumRefsFlags::Uses0B:
    break;
  case NumRefsFlags::Uses1B:
    assert(I.Refs.size() <= UINT8_MAX);
    Packed |= (uint64_t)I.Refs.size() << 48;
    break;
  case NumRefsFlags::Uses2B:
    assert(I.Refs.size() <= UINT16_MAX);
    Packed |= (uint64_t)I.Refs.size() << 32;
    break;
  case NumRefsFlags::Uses4B:
    assert(isAddrAligned(Align(4), Next));
    new (Next) uint32_t(I.Refs.size());
    Next += 4;
    break;
  case NumRefsFlags::Uses8B:
    assert(isAddrAligned(Align(8), Next));
    new (Next) uint64_t(I.Refs.size());
    Next += 8;
    break;
  }

  // Construct Refs[].
  if (!I.Refs.empty()) {
    if (L.Flags.RefKind == RefKindFlags::InternalRef4B) {
      assert(I.Refs.is4B());
      assert(isAddrAligned(Align::Of<InternalRef4B>(), Next));
      for (InternalRef4B Ref : I.Refs.as4B()) {
        new (Next) InternalRef4B(Ref);
        Next += sizeof(InternalRef4B);
      }
    } else {
      assert(I.Refs.is8B());
      assert(isAddrAligned(Align::Of<InternalRef>(), Next));
      for (InternalRef Ref : I.Refs.as8B()) {
        new (Next) InternalRef(Ref);
        Next += sizeof(InternalRef);
      }
    }
  }

  // Construct Data and the trailing null.
  assert(isAddrAligned(Align(8), Next));
  llvm::copy(I.Data, Next);
  Next[I.Data.size()] = 0;

  // Construct the header itself and return.
  Header *H = new (Mem) Header{Packed};
  DataRecordHandle Record(*H);
  assert(Record.getData() == I.Data);
  assert(Record.getNumRefs() == I.Refs.size());
  assert(Record.getRefs() == I.Refs);
  assert(Record.getLayoutFlags().DataSize == L.Flags.DataSize);
  assert(Record.getLayoutFlags().NumRefs == L.Flags.NumRefs);
  assert(Record.getLayoutFlags().RefKind == L.Flags.RefKind);
  assert(Record.getLayoutFlags().TrieOffset == L.Flags.TrieOffset);
  return Record;
}

DataRecordHandle::Layout::Layout(const Input &I) {
  // Start initial relative offsets right after the Header.
  uint64_t RelOffset = sizeof(Header);

  // Initialize the easy stuff.
  DataSize = I.Data.size();
  NumRefs = I.Refs.size();

  // Check refs size.
  Flags.RefKind =
      I.Refs.is4B() ? RefKindFlags::InternalRef4B : RefKindFlags::InternalRef;

  // Set the trie offset.
  TrieRecordOffset = (uint64_t)I.TrieRecordOffset.get();
  assert(TrieRecordOffset <= (UINT64_MAX >> 16));
  Flags.TrieOffset = TrieRecordOffset <= UINT32_MAX ? TrieOffsetFlags::Uses4B
                                                    : TrieOffsetFlags::Uses6B;

  // Find the smallest slot available for DataSize.
  bool Has1B = true;
  bool Has2B = Flags.TrieOffset == TrieOffsetFlags::Uses4B;
  if (DataSize <= UINT8_MAX && Has1B) {
    Flags.DataSize = DataSizeFlags::Uses1B;
    Has1B = false;
  } else if (DataSize <= UINT16_MAX && Has2B) {
    Flags.DataSize = DataSizeFlags::Uses2B;
    Has2B = false;
  } else if (DataSize <= UINT32_MAX) {
    Flags.DataSize = DataSizeFlags::Uses4B;
    RelOffset += 4;
  } else {
    Flags.DataSize = DataSizeFlags::Uses8B;
    RelOffset += 8;
  }

  // Find the smallest slot available for NumRefs. Never sets NumRefs8B here.
  if (!NumRefs) {
    Flags.NumRefs = NumRefsFlags::Uses0B;
  } else if (NumRefs <= UINT8_MAX && Has1B) {
    Flags.NumRefs = NumRefsFlags::Uses1B;
    Has1B = false;
  } else if (NumRefs <= UINT16_MAX && Has2B) {
    Flags.NumRefs = NumRefsFlags::Uses2B;
    Has2B = false;
  } else {
    Flags.NumRefs = NumRefsFlags::Uses4B;
    RelOffset += 4;
  }

  // Helper to "upgrade" either DataSize or NumRefs by 4B to avoid complicated
  // padding rules when reading and writing. This also bumps RelOffset.
  //
  // The value for NumRefs is strictly limited to UINT32_MAX, but it can be
  // stored as 8B. This means we can *always* find a size to grow.
  //
  // NOTE: Only call this once.
  auto GrowSizeFieldsBy4B = [&]() {
    assert(isAligned(Align(4), RelOffset));
    RelOffset += 4;

    assert(Flags.NumRefs != NumRefsFlags::Uses8B &&
           "Expected to be able to grow NumRefs8B");

    // First try to grow DataSize. NumRefs will not (yet) be 8B, and if
    // DataSize is upgraded to 8B it'll already be aligned.
    //
    // Failing that, grow NumRefs.
    if (Flags.DataSize < DataSizeFlags::Uses4B)
      Flags.DataSize = DataSizeFlags::Uses4B; // DataSize: Packed => 4B.
    else if (Flags.DataSize < DataSizeFlags::Uses8B)
      Flags.DataSize = DataSizeFlags::Uses8B; // DataSize: 4B => 8B.
    else if (Flags.NumRefs < NumRefsFlags::Uses4B)
      Flags.NumRefs = NumRefsFlags::Uses4B; // NumRefs: Packed => 4B.
    else
      Flags.NumRefs = NumRefsFlags::Uses8B; // NumRefs: 4B => 8B.
  };

  assert(isAligned(Align(4), RelOffset));
  if (Flags.RefKind == RefKindFlags::InternalRef) {
    // List of 8B refs should be 8B-aligned. Grow one of the sizes to get this
    // without padding.
    if (!isAligned(Align(8), RelOffset))
      GrowSizeFieldsBy4B();

    assert(isAligned(Align(8), RelOffset));
    RefsRelOffset = RelOffset;
    RelOffset += 8 * NumRefs;
  } else {
    // The array of 4B refs doesn't need 8B alignment, but the data will need
    // to be 8B-aligned. Detect this now, and, if necessary, shift everything
    // by 4B by growing one of the sizes.
    uint64_t RefListSize = 4 * NumRefs;
    if (!isAligned(Align(8), RelOffset + RefListSize))
      GrowSizeFieldsBy4B();
    RefsRelOffset = RelOffset;
    RelOffset += RefListSize;
  }

  assert(isAligned(Align(8), RelOffset));
  DataRelOffset = RelOffset;
}

uint64_t DataRecordHandle::getDataSize() const {
  int64_t RelOffset = sizeof(Header);
  auto *DataSizePtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (getLayoutFlags().DataSize) {
  case DataSizeFlags::Uses1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case DataSizeFlags::Uses2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case DataSizeFlags::Uses4B:
    return *reinterpret_cast<const uint32_t *>(DataSizePtr);
  case DataSizeFlags::Uses8B:
    return *reinterpret_cast<const uint64_t *>(DataSizePtr);
  }
}

void DataRecordHandle::skipDataSize(LayoutFlags LF, int64_t &RelOffset) const {
  if (LF.DataSize >= DataSizeFlags::Uses4B)
    RelOffset += 4;
  if (LF.DataSize >= DataSizeFlags::Uses8B)
    RelOffset += 4;
}

uint32_t DataRecordHandle::getNumRefs() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  auto *NumRefsPtr = reinterpret_cast<const char *>(H) + RelOffset;
  switch (LF.NumRefs) {
  case NumRefsFlags::Uses0B:
    return 0;
  case NumRefsFlags::Uses1B:
    return (H->Packed >> 48) & UINT8_MAX;
  case NumRefsFlags::Uses2B:
    return (H->Packed >> 32) & UINT16_MAX;
  case NumRefsFlags::Uses4B:
    return *reinterpret_cast<const uint32_t *>(NumRefsPtr);
  case NumRefsFlags::Uses8B:
    return *reinterpret_cast<const uint64_t *>(NumRefsPtr);
  }
}

void DataRecordHandle::skipNumRefs(LayoutFlags LF, int64_t &RelOffset) const {
  if (LF.NumRefs >= NumRefsFlags::Uses4B)
    RelOffset += 4;
  if (LF.NumRefs >= NumRefsFlags::Uses8B)
    RelOffset += 4;
}

int64_t DataRecordHandle::getRefsRelOffset() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  skipNumRefs(LF, RelOffset);
  return RelOffset;
}

int64_t DataRecordHandle::getDataRelOffset() const {
  LayoutFlags LF = getLayoutFlags();
  int64_t RelOffset = sizeof(Header);
  skipDataSize(LF, RelOffset);
  skipNumRefs(LF, RelOffset);
  uint32_t RefSize = LF.RefKind == RefKindFlags::InternalRef4B ? 4 : 8;
  RelOffset += RefSize * getNumRefs();
  return RelOffset;
}

void OnDiskCAS::print(raw_ostream &OS) const {
  OS << "on-disk-root-path: " << RootPath << "\n";

  struct PoolInfo {
    bool IsString2B;
    int64_t Offset;
  };
  SmallVector<PoolInfo> Pool;

  OS << "\n";
  OS << "index:\n";
  Index.print(OS, [&](ArrayRef<char> Data) {
    assert(Data.size() == sizeof(TrieRecord));
    assert(isAligned(Align::Of<TrieRecord>(), Data.size()));
    auto *R = reinterpret_cast<const TrieRecord *>(Data.data());
    TrieRecord::Data D = R->load();
    OS << "OK=";
    switch (D.OK) {
    case TrieRecord::ObjectKind::Invalid:
      OS << "invalid";
      break;
    case TrieRecord::ObjectKind::Node:
      OS << "node   ";
      break;
    case TrieRecord::ObjectKind::Blob:
      OS << "blob   ";
      break;
    case TrieRecord::ObjectKind::Tree:
      OS << "tree   ";
      break;
    case TrieRecord::ObjectKind::String:
      OS << "string ";
      break;
    }
    OS << " SK=";
    switch (D.SK) {
    case TrieRecord::StorageKind::Unknown:
      OS << "unknown          ";
      break;
    case TrieRecord::StorageKind::DataPool:
      OS << "datapool         ";
      Pool.push_back({/*IsString2B=*/false, D.Offset.get()});
      break;
    case TrieRecord::StorageKind::DataPoolString2B:
      OS << "datapool-string2B";
      Pool.push_back({/*IsString2B=*/true, D.Offset.get()});
      break;
    case TrieRecord::StorageKind::Standalone:
      OS << "standalone-data  ";
      break;
    case TrieRecord::StorageKind::StandaloneBlob:
      OS << "standalone-blob  ";
      break;
    case TrieRecord::StorageKind::StandaloneBlob0:
      OS << "standalone-blob0 ";
      break;
    }
    OS << " Offset=" << (void *)D.Offset.get();
  });
  if (Pool.empty())
    return;

  OS << "\n";
  OS << "pool:\n";
  llvm::sort(
      Pool, [](PoolInfo LHS, PoolInfo RHS) { return LHS.Offset < RHS.Offset; });
  for (PoolInfo PI : Pool) {
    OS << "- addr=" << (void *)PI.Offset << " ";
    if (PI.IsString2B) {
      auto S = String2BHandle::get(DataPool.beginData(FileOffset(PI.Offset)));
      OS << "string length=" << S.getLength();
      OS << " end="
         << (void *)(PI.Offset + sizeof(String2BHandle::Header) +
                     S.getLength() + 1)
         << "\n";
      continue;
    }
    DataRecordHandle D =
        DataRecordHandle::get(DataPool.beginData(FileOffset(PI.Offset)));
    OS << "record refs=" << D.getNumRefs() << " data=" << D.getDataSize()
       << " size=" << D.getTotalSize()
       << " end=" << (void *)(PI.Offset + D.getTotalSize()) << "\n";
  }
}

OnDiskCAS::IndexProxy OnDiskCAS::indexHash(ArrayRef<uint8_t> Hash) {
  OnDiskHashMappedTrie::pointer P = Index.insertLazy(
      Hash, [](FileOffset TentativeOffset,
               OnDiskHashMappedTrie::ValueProxy TentativeValue) {
        assert(TentativeValue.Data.size() == sizeof(TrieRecord));
        assert(
            isAddrAligned(Align::Of<TrieRecord>(), TentativeValue.Data.data()));
        new (TentativeValue.Data.data()) TrieRecord();
      });
  assert(P && "Expected insertion");
  return getIndexProxyFromPointer(P);
}

OnDiskCAS::IndexProxy OnDiskCAS::getIndexProxyFromPointer(
    OnDiskHashMappedTrie::const_pointer P) const {
  assert(P);
  assert(P.getOffset());
  return IndexProxy{P.getOffset(), P->Hash,
                    *const_cast<TrieRecord *>(
                        reinterpret_cast<const TrieRecord *>(P->Data.data()))};
}

OnDiskHashMappedTrie::const_pointer
OnDiskCAS::getInternalIndexPointer(CASID ID) const {
  // Recover the pointer from the FileOffset if ID comes from this CAS.
  if (&ID.getContext() == this) {
    OnDiskHashMappedTrie::const_pointer P =
        Index.recoverFromFileOffset(FileOffset(ID.getInternalID(*this)));
    assert(P && "Expected valid index pointer from direct lookup");
    return P;
  }

  // Fallback to a normal lookup.
  return Index.find(ID.getHash());
}

Optional<InternalRef> OnDiskCAS::getInternalRef(CASID ID) const {
  OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID);
  if (!P)
    return None;
  IndexProxy I = getIndexProxyFromPointer(P);
  return getInternalRef(I, I.ObjectRef.load());
}

Optional<StringRef> OnDiskCAS::getString(InternalRef Ref) const {
  switch (Ref.getOffsetKind()) {
  case InternalRef::OffsetKind::String2B:
    return String2BHandle::get(DataPool.beginData(Ref.getFileOffset()))
        .getString();

  case InternalRef::OffsetKind::DataRecord:
    return toStringRef(
        DataRecordHandle::get(DataPool.beginData(Ref.getFileOffset()))
            .getData());

  case InternalRef::OffsetKind::IndexRecord:
    break;
  }
  if (OnDiskHashMappedTrie::const_pointer P =
          Index.recoverFromFileOffset(Ref.getFileOffset()))
    if (Optional<Expected<StringProxy>> Proxy =
            dereferenceValue(getString(getIndexProxyFromPointer(P))))
      if (Optional<StringProxy> Proxy2 = expectedToOptional(std::move(*Proxy)))
        return Proxy2->String;
  return None;
}

Optional<CASID> OnDiskCAS::getCASID(InternalRef Ref) const {
  switch (Ref.getOffsetKind()) {
  case InternalRef::OffsetKind::String2B:
    /// Strings are not exposed.
    return None;

  case InternalRef::OffsetKind::DataRecord: {
    DataRecordHandle Handle =
        DataRecordHandle::get(DataPool.beginData(Ref.getFileOffset()));
    return getIDFromIndexOffset(Handle.getTrieRecordOffset());
  }

  case InternalRef::OffsetKind::IndexRecord:
    return getIDFromIndexOffset(Ref.getFileOffset());
  }
}

Optional<InternalRef> OnDiskCAS::getInternalRef(IndexProxy I,
                                                TrieRecord::Data Object) const {
  switch (Object.SK) {
  case TrieRecord::StorageKind::Unknown:
    return None;

  case TrieRecord::StorageKind::DataPool:
    return InternalRef::getFromOffset(InternalRef::OffsetKind::DataRecord,
                                      Object.Offset);

  case TrieRecord::StorageKind::DataPoolString2B:
    return InternalRef::getFromOffset(InternalRef::OffsetKind::String2B,
                                      Object.Offset);

  case TrieRecord::StorageKind::Standalone:
  case TrieRecord::StorageKind::StandaloneBlob:
  case TrieRecord::StorageKind::StandaloneBlob0:
    return InternalRef::getFromOffset(InternalRef::OffsetKind::IndexRecord,
                                      I.Offset);
  }
}

Expected<OnDiskCAS::StringProxy>
OnDiskCAS::getOrCreateString(IndexProxy I, StringRef String) {
  assert(String.size() <= UINT16_MAX &&
         "Expected caller to check string fits in 2B size");

  // See if it already exists.
  if (Optional<Expected<StringProxy>> S = dereferenceValue(getString(I)))
    return std::move(*S);

  FileOffset Offset;
  auto Alloc = [&](size_t Size) -> char * {
    OnDiskDataAllocator::pointer P = DataPool.allocate(Size);
    Offset = P.getOffset();
    LLVM_DEBUG({
      dbgs() << "pool-alloc addr=" << (void *)Offset.get() << " size=" << Size
             << " end=" << (void *)(Offset.get() + Size) << "\n";
    });
    return P->data();
  };
  String2BHandle S = String2BHandle::create(Alloc, String);

  TrieRecord::Data StringData;
  StringData.OK = TrieRecord::ObjectKind::String;
  StringData.SK = TrieRecord::StorageKind::DataPoolString2B;
  StringData.Offset = Offset;

  // Try to store the value and confirm that the new value has valid storage.
  //
  // TODO: Find a way to reuse the storage from the new-but-abandoned record
  // handle.
  TrieRecord::Data Existing;
  if (I.ObjectRef.compare_exchange_strong(Existing, StringData))
    return StringProxy{I.Offset, StringData, S.getString()};

  if (Existing.SK == TrieRecord::StorageKind::Unknown)
    return createCorruptObjectError(getID(I));

  return dereferenceValue(getString(I),
                          [&]() { return createCorruptObjectError(getID(I)); });
}

Expected<InternalRef> OnDiskCAS::getOrCreateStringRef(StringRef String) {
  // Make a blob if String is bigger than 64K.
  if (String.size() > UINT16_MAX) {
    BlobProxy Blob;
    IndexProxy I =
        indexHash(BuiltinObjectHasher<HasherT>::hashBlob(toArrayRef(String)));
    if (Error E = getOrCreateBlob(I, toArrayRef(String)).moveInto(Blob))
      return std::move(E);
    return *getInternalRef(I, Blob.Object);
  }

  // Make a string.
  StringProxy S;
  IndexProxy I = indexHash(BuiltinObjectHasher<HasherT>::hashString(String));
  if (Error E = getOrCreateString(I, String).moveInto(S))
    return std::move(E);
  return *getInternalRef(I, S.Object);
}

void OnDiskCAS::getStandalonePath(TrieRecord::StorageKind SK,
                                  const IndexProxy &I,
                                  SmallVectorImpl<char> &Path) const {
  StringRef Suffix;
  switch (SK) {
  default:
    llvm_unreachable("Expected standalone storage kind");

  case TrieRecord::StorageKind::Standalone:
    Suffix = FileSuffixData;
    break;
  case TrieRecord::StorageKind::StandaloneBlob0:
    Suffix = FileSuffixBlob0;
    break;
  case TrieRecord::StorageKind::StandaloneBlob:
    Suffix = FileSuffixBlob;
    break;
  }

  Path.assign(RootPath.begin(), RootPath.end());
  sys::path::append(Path, FilePrefix + Twine(I.Offset.get()) + Suffix);
}

Expected<Optional<OnDiskCAS::StringProxy>>
OnDiskCAS::getString(IndexProxy I) const {
  Optional<OnDiskCAS::ObjectProxy> OP;
  if (Optional<Expected<NoneType>> E = moveValueInto(getObjectProxy(I), OP))
    return std::move(*E);
  assert(OP);

  assert(bool(OP->Record) != bool(OP->Bytes));
  if (OP->Bytes)
    return StringProxy{I.Offset, OP->Object, toStringRef(*OP->Bytes)};

  // Blobs and strings should not have references.
  if (!OP->Record->getRefs().empty())
    return createCorruptObjectError(getID(I));
  return StringProxy{I.Offset, OP->Object, toStringRef(OP->Record->getData())};
}

Expected<Optional<OnDiskCAS::BlobProxy>>
OnDiskCAS::getBlob(IndexProxy I) const {
  Optional<OnDiskCAS::ObjectProxy> OP;
  if (Optional<Expected<NoneType>> E = moveValueInto(getObjectProxy(I), OP))
    return std::move(*E);
  assert(OP);

  assert(bool(OP->Record) != bool(OP->Bytes));
  if (OP->Bytes)
    return BlobProxy{I.Offset, OP->Object, OP->Hash, *OP->Bytes};

  // Blobs should not have references.
  if (!OP->Record->getRefs().empty())
    return createCorruptObjectError(getID(I));
  return BlobProxy{I.Offset, OP->Object, OP->Hash, OP->Record->getData()};
}

Expected<Optional<OnDiskCAS::DataRecordProxy>>
OnDiskCAS::getDataRecord(IndexProxy I) const {
  Optional<OnDiskCAS::ObjectProxy> OP;
  if (Optional<Expected<NoneType>> E = moveValueInto(getObjectProxy(I), OP))
    return std::move(*E);

  assert(OP->Record && "Expected record");
  assert(!OP->Bytes && "Unexpected blob");
  return DataRecordProxy{I.Offset, OP->Object, OP->Hash, *OP->Record};
}

Expected<Optional<OnDiskCAS::ObjectProxy>>
OnDiskCAS::getObjectProxy(IndexProxy I) const {
  TrieRecord::Data Object = I.ObjectRef.load();
  if (Object.SK == TrieRecord::StorageKind::Unknown)
    return None;

  bool Blob0 = false;
  bool Blob = false;
  switch (Object.SK) {
  default:
    return createCorruptObjectError(getID(I));

  case TrieRecord::StorageKind::DataPool: {
    DataRecordHandle Handle =
        DataRecordHandle::get(DataPool.beginData(Object.Offset));
    assert(Handle.getData().end()[0] == 0 && "Null termination");
    return ObjectProxy{I.Offset, Object, I.Hash, Handle, None};
  }

  case TrieRecord::StorageKind::DataPoolString2B: {
    String2BHandle Handle =
        String2BHandle::get(DataPool.beginData(Object.Offset));
    assert(Handle.getString().end()[0] == 0 && "Null termination");
    return ObjectProxy{I.Offset, Object, I.Hash, None,
                       toArrayRef(Handle.getString())};
  }

  case TrieRecord::StorageKind::Standalone:
    break;
  case TrieRecord::StorageKind::StandaloneBlob0:
    Blob = Blob0 = true;
    break;
  case TrieRecord::StorageKind::StandaloneBlob:
    Blob = true;
    break;
  }

  assert(!Object.Offset && "Unexpected offset for standalone objects");

  // Helper for creating the return.
  auto createProxy = [&](MemoryBufferRef Buffer) -> ObjectProxy {
    assert(Buffer.getBuffer().drop_back(Blob0).end()[0] == 0 &&
           "Null termination");
    if (Blob)
      return ObjectProxy{I.Offset, Object, I.Hash, None,
                         toArrayRef(Buffer.getBuffer().drop_back(Blob0))};
    return ObjectProxy{I.Offset, Object, I.Hash,
                       DataRecordHandle::get(Buffer.getBuffer().data()), None};
  };

  // Check if we've loaded it already.
  if (Optional<MemoryBufferRef> Buffer = StandaloneData.lookup(I.Hash))
    return createProxy(*Buffer);

  // Load it from disk.
  SmallString<256> Path;
  getStandalonePath(Object.SK, I, Path);
  ErrorOr<std::unique_ptr<MemoryBuffer>> OwnedBuffer = MemoryBuffer::getFile(
      Path, /*IsText=*/false, /*RequiresNullTerminator=*/true);
  if (!OwnedBuffer)
    return createCorruptObjectError(getID(I));

  MemoryBufferRef Buffer =
      StandaloneData.insert(I.Hash, std::move(*OwnedBuffer));
  return createProxy(Buffer);
}

Expected<OnDiskCAS::MappedTempFile>
OnDiskCAS::createTempFile(StringRef FinalPath, uint64_t Size) {
  assert(Size && "Unexpected request for an empty temp file");
  Expected<TempFile> File = TempFile::create(FinalPath + ".%%%%%%");
  if (!File)
    return File.takeError();

  if (auto EC = sys::fs::resize_file_before_mapping_readwrite(File->FD, Size))
    return createFileError(File->TmpName, EC);

  std::error_code EC;
  sys::fs::mapped_file_region Map(sys::fs::convertFDToNativeFile(File->FD),
                                  sys::fs::mapped_file_region::readwrite, Size,
                                  0, EC);
  if (EC)
    return createFileError(File->TmpName, EC);
  return MappedTempFile(std::move(*File), std::move(Map));
}

Expected<OnDiskCAS::BlobProxy>
OnDiskCAS::createStandaloneBlob(IndexProxy &I, ArrayRef<char> Data) {
  assert(Data.size() > TrieRecord::MaxEmbeddedSize &&
         "Expected a bigger file for external content...");

  bool Blob0 = isAligned(Align(getPageSize()), Data.size());
  TrieRecord::StorageKind SK = Blob0 ? TrieRecord::StorageKind::StandaloneBlob0
                                     : TrieRecord::StorageKind::StandaloneBlob;

  SmallString<256> Path;
  int64_t FileSize = Data.size() + Blob0;
  getStandalonePath(SK, I, Path);

  // write the file.
  Expected<MappedTempFile> File = createTempFile(Path, FileSize);
  if (!File)
    return File.takeError();
  assert(File->size() == (uint64_t)FileSize);
  llvm::copy(Data, File->data());
  if (Blob0)
    File->data()[Data.size()] = 0;
  assert(File->data()[Data.size()] == 0);
  if (Error E = File->keep(Path))
    return std::move(E);

  // Store the object reference. If there was a race, getBlob() will have the
  // new value.
  TrieRecord::Data Existing;
  if (!I.ObjectRef.compare_exchange_strong(
          Existing,
          TrieRecord::Data{SK, TrieRecord::ObjectKind::Blob, FileOffset()})) {
    // If there was a race, confirm that the new value has valid storage.
    if (Existing.SK == TrieRecord::StorageKind::Unknown)
      return createCorruptObjectError(getID(I));

    // Fall through...
  }

  // Get and return the inserted blob.
  return dereferenceValue(getBlob(I),
                          [&]() { return createCorruptObjectError(getID(I)); });
}

Expected<OnDiskCAS::BlobProxy> OnDiskCAS::getOrCreateBlob(IndexProxy I,
                                                          ArrayRef<char> Data) {
  // See if it already exists.
  if (Optional<Expected<BlobProxy>> Blob = dereferenceValue(getBlob(I)))
    return std::move(*Blob);

  if (Data.size() > TrieRecord::MaxEmbeddedSize)
    return createStandaloneBlob(I, Data);

  PooledDataRecord PDR =
      createPooledDataRecord(DataRecordHandle::Input{I.Offset, None, Data});

  TrieRecord::Data Blob;
  Blob.OK = TrieRecord::ObjectKind::Blob;
  Blob.SK = TrieRecord::StorageKind::DataPool;
  Blob.Offset = PDR.Offset;

  // Try to store the value and confirm that the new value has valid storage.
  //
  // TODO: Find a way to reuse the storage from the new-but-abandoned record
  // handle.
  TrieRecord::Data Existing;
  if (!I.ObjectRef.compare_exchange_strong(Existing, Blob))
    if (Existing.SK == TrieRecord::StorageKind::Unknown)
      return createCorruptObjectError(getID(I));

  return dereferenceValue(getBlob(I),
                          [&]() { return createCorruptObjectError(getID(I)); });
}

Expected<OnDiskCAS::DataRecordProxy>
OnDiskCAS::getOrCreateTree(IndexProxy I,
                           ArrayRef<NamedTreeEntry> SortedEntries) {
  InternalRefVector Refs;
  SmallVector<char> Data;
  // Names up front.
  for (const NamedTreeEntry &E : SortedEntries) {
    if (Expected<InternalRef> Ref = getOrCreateStringRef(E.getName()))
      Refs.push_back(*Ref);
    else
      return Ref.takeError();
    Data.push_back((uint8_t)getStableKind(E.getKind()));
  }

  // Then target refs.
  for (const NamedTreeEntry &E : SortedEntries) {
    if (Optional<InternalRef> Ref = getInternalRef(E.getID()))
      Refs.push_back(*Ref);
    else
      return createUnknownObjectError(E.getID());
  }

  // Create the object.
  return getOrCreateDataRecord(I, TrieRecord::ObjectKind::Tree,
                               DataRecordHandle::Input{I.Offset, Refs, Data});
}

Expected<OnDiskCAS::DataRecordProxy>
OnDiskCAS::getOrCreateNode(IndexProxy I, ArrayRef<CASID> References,
                           ArrayRef<char> Data) {
  InternalRefVector Refs;
  for (const CASID &ID : References) {
    if (Optional<InternalRef> Ref = getInternalRef(ID))
      Refs.push_back(*Ref);
    else
      return createUnknownObjectError(ID);
  }

  // Create the object.
  return getOrCreateDataRecord(I, TrieRecord::ObjectKind::Node,
                               DataRecordHandle::Input{I.Offset, Refs, Data});
}

Expected<OnDiskCAS::DataRecordProxy>
OnDiskCAS::getOrCreateDataRecord(IndexProxy &I, TrieRecord::ObjectKind OK,
                                 DataRecordHandle::Input Input) {
  assert(OK != TrieRecord::ObjectKind::Blob &&
         "Expected blobs to be handled elsewhere");

  // See if it already exists.
  if (Optional<Expected<DataRecordProxy>> Record =
          dereferenceValue(getDataRecord(I)))
    return std::move(*Record);

  // Compute the storage kind, allocate it, and create the record.
  TrieRecord::StorageKind SK = TrieRecord::StorageKind::Unknown;
  FileOffset PoolOffset;
  SmallString<256> Path;
  Optional<MappedTempFile> File;
  auto Alloc = [&](size_t Size) -> Expected<char *> {
    if (Size <= TrieRecord::MaxEmbeddedSize) {
      SK = TrieRecord::StorageKind::DataPool;
      OnDiskDataAllocator::pointer P = DataPool.allocate(Size);
      PoolOffset = P.getOffset();
      LLVM_DEBUG({
        dbgs() << "pool-alloc addr=" << (void *)PoolOffset.get()
               << " size=" << Size
               << " end=" << (void *)(PoolOffset.get() + Size) << "\n";
      });
      return P->data();
    }

    SK = TrieRecord::StorageKind::Standalone;
    getStandalonePath(SK, I, Path);
    if (Error E = createTempFile(Path, Size).moveInto(File))
      return std::move(E);
    return File->data();
  };
  DataRecordHandle Record;
  if (Error E =
          DataRecordHandle::createWithError(Alloc, Input).moveInto(Record))
    return std::move(E);
  assert(Record.getData().end()[0] == 0 && "Expected null-termination");
  assert(Record.getData() == Input.Data && "Expected initialization");
  assert(SK != TrieRecord::StorageKind::Unknown);
  assert(bool(File) != bool(PoolOffset) &&
         "Expected either a mapped file or a pooled offset");

  // Check for a race before calling MappedTempFile::keep().
  //
  // Then decide what to do with the file. Better to discard than overwrite if
  // another thread/process has already added this.
  TrieRecord::Data NewObject{SK, OK, PoolOffset};
  TrieRecord::Data Existing = I.ObjectRef.load();
  if (File) {
    if (Existing.SK == TrieRecord::StorageKind::Unknown) {
      // Keep the file!
      if (Error E = File->keep(Path))
        return std::move(E);
    } else {
      File.reset();
    }
  }

  // If we didn't already see a racing/existing write, then try storing the new
  // object. If that races, confirm that the new value has valid storage.
  //
  // TODO: Find a way to reuse the storage from the new-but-abandoned record
  // handle.
  if (Existing.SK == TrieRecord::StorageKind::Unknown)
    if (!I.ObjectRef.compare_exchange_strong(Existing, NewObject))
      if (Existing.SK == TrieRecord::StorageKind::Unknown)
        return createCorruptObjectError(getID(I));

  // Get and return the record.
  return dereferenceValue(getDataRecord(I),
                          [&]() { return createCorruptObjectError(getID(I)); });
}

OnDiskCAS::PooledDataRecord
OnDiskCAS::createPooledDataRecord(DataRecordHandle::Input Input) {
  FileOffset Offset;
  auto Alloc = [&](size_t Size) -> char * {
    OnDiskDataAllocator::pointer P = DataPool.allocate(Size);
    Offset = P.getOffset();
    LLVM_DEBUG({
      dbgs() << "pool-alloc addr=" << (void *)Offset.get() << " size=" << Size
             << " end=" << (void *)(Offset.get() + Size) << "\n";
    });
    return P->data();
  };
  DataRecordHandle Record = DataRecordHandle::create(Alloc, Input);
  assert(Offset && "Should always have an offset");
  return PooledDataRecord{Offset, Record};
}

Expected<BlobRef> OnDiskCAS::getBlobFromProxy(Expected<BlobProxy> Blob) {
  if (Blob)
    return makeBlobRef(getIDFromIndexOffset(Blob->IndexOffset), Blob->Data);
  return Blob.takeError();
}

Expected<NodeRef> OnDiskCAS::getNodeFromProxy(Expected<DataRecordProxy> Node) {
  if (Node)
    return makeNodeRef(getIDFromIndexOffset(Node->IndexOffset),
                       &Node->Record.getHeader(), Node->Record.getNumRefs(),
                       toStringRef(Node->Record.getData()));
  return Node.takeError();
}

Expected<TreeRef> OnDiskCAS::getTreeFromProxy(Expected<DataRecordProxy> Tree) {
  if (Tree)
    return makeTreeRef(getIDFromIndexOffset(Tree->IndexOffset),
                       &Tree->Record.getHeader(),
                       Tree->Record.getNumRefs() / 2);
  return Tree.takeError();
}

Expected<BlobRef> OnDiskCAS::getBlob(CASID ID) {
  if (OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID))
    if (Optional<Expected<BlobProxy>> Blob =
            dereferenceValue(getBlob(getIndexProxyFromPointer(P))))
      return getBlobFromProxy(std::move(*Blob));
  // FIXME: This should not be an error.
  return createUnknownObjectError(ID);
}

Expected<NodeRef> OnDiskCAS::getNode(CASID ID) {
  if (OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID))
    if (Optional<Expected<DataRecordProxy>> Node =
            dereferenceValue(getDataRecord(getIndexProxyFromPointer(P))))
      return getNodeFromProxy(std::move(*Node));
  // FIXME: This should not be an error.
  return createUnknownObjectError(ID);
}

Expected<TreeRef> OnDiskCAS::getTree(CASID ID) {
  if (OnDiskHashMappedTrie::const_pointer P = getInternalIndexPointer(ID))
    if (Optional<Expected<DataRecordProxy>> Tree =
            dereferenceValue(getDataRecord(getIndexProxyFromPointer(P))))
      return getTreeFromProxy(std::move(*Tree));
  // FIXME: This should not be an error.
  return createUnknownObjectError(ID);
}

NamedTreeEntry OnDiskCAS::makeTreeEntry(DataRecordHandle Record, size_t I,
                                        ArrayRef<StringRef> NameCache) const {
  size_t NumNames = Record.getNumRefs() / 2;
  assert(I < NumNames);
  assert(Record.getNumRefs() % 2 == 0);
  StringRef Name;
  if (NameCache.empty()) {
    Optional<StringRef> S = getString(Record.getRefs()[I]);
    // FIXME: Probably should be less fragile...
    if (!S)
      report_fatal_error("corrupt name in tree entry");
    Name = *S;
  } else {
    Name = NameCache[I];
  }

  Optional<CASID> ID = getCASID(Record.getRefs()[I + NumNames]);
  TreeEntry::EntryKind Kind =
      getUnstableKind((StableTreeEntryKind)Record.getData()[I]);
  return NamedTreeEntry(*ID, Kind, Name);
}

Optional<NamedTreeEntry> OnDiskCAS::lookupInTree(const TreeRef &Tree,
                                                 StringRef Name) const {
  DataRecordHandle Record = DataRecordHandle::get(
      reinterpret_cast<char *>(const_cast<void *>(getTreePtr(Tree))));

  if (!Record.getNumRefs())
    return None;

  // Names are at the front.
  InternalRefArrayRef Refs = Record.getRefs();
  size_t NumNames = Record.getNumRefs() / 2;
  SmallVector<StringRef> Names(NumNames);

  auto GetName = [&](InternalRefArrayRef::iterator I) {
    auto &NameI = Names[I - Refs.begin()];
    if (!NameI.empty())
      return NameI;
    Optional<StringRef> S = getString(*I);
    // FIXME: Probably should be less fragile...
    if (!S)
      report_fatal_error("corrupt name in tree entry");
    NameI = *S;
    return *S;
  };

  // Start with a binary search, if there are enough entries.
  //
  // FIXME: Should just use std::lower_bound, but we need the actual iterators
  // to know the index in the NameCache...
  const intptr_t MaxLinearSearchSize = 4;
  auto LastName = Refs.begin() + NumNames;
  auto Last = LastName;
  auto First = Refs.begin();
  while (Last - First > MaxLinearSearchSize) {
    auto I = First + (Last - First) / 2;
    StringRef NameI = GetName(I);
    switch (Name.compare(NameI)) {
    case 0:
      return makeTreeEntry(Record, I - Refs.begin(), Names);
    case -1:
      Last = I;
      break;
    case 1:
      First = I + 1;
      break;
    }
  }

  // Use a linear search for small trees.
  for (; First != Last; ++First)
    if (Name == GetName(First))
      return makeTreeEntry(Record, First - Refs.begin(), Names);

  return None;
}

NamedTreeEntry OnDiskCAS::getInTree(const TreeRef &Tree, size_t I) const {
  DataRecordHandle Record = DataRecordHandle::get(
      reinterpret_cast<char *>(const_cast<void *>(getTreePtr(Tree))));

  return makeTreeEntry(Record, I);
}

Error OnDiskCAS::forEachEntryInTree(
    const TreeRef &Tree,
    function_ref<Error(const NamedTreeEntry &)> Callback) const {
  DataRecordHandle Record = DataRecordHandle::get(
      reinterpret_cast<char *>(const_cast<void *>(getTreePtr(Tree))));

  size_t NumNames = Record.getNumRefs() / 2;
  assert(Record.getNumRefs() % 2 == 0);
  for (size_t I = 0; I != NumNames; ++I)
    if (Error E = Callback(makeTreeEntry(Record, I)))
      return E;
  return Error::success();
}

CASID OnDiskCAS::getReferenceInNode(const NodeRef &Node, size_t I) const {
  DataRecordHandle Record = DataRecordHandle::get(
      reinterpret_cast<char *>(const_cast<void *>(getNodePtr(Node))));

  Optional<CASID> ID = getCASID(Record.getRefs()[I]);
  assert(ID);
  return *ID;
}

Error OnDiskCAS::forEachReferenceInNode(
    const NodeRef &Node, function_ref<Error(CASID)> Callback) const {
  DataRecordHandle Record = DataRecordHandle::get(
      reinterpret_cast<char *>(const_cast<void *>(getNodePtr(Node))));

  for (InternalRef Ref : Record.getRefs())
    if (Error E = Callback(*getCASID(Ref)))
      return E;
  return Error::success();
}

Expected<CASID> OnDiskCAS::getCachedResult(CASID InputID) {
  // Check that InputID is valid.
  Optional<InternalRef> InputRef = getInternalRef(InputID);
  if (!InputRef)
    return createUnknownObjectError(InputID);

  // Check the result cache.
  //
  // FIXME: Failure here should not be an error.
  OnDiskHashMappedTrie::pointer ActionP = ActionCache.find(InputID.getHash());
  if (!ActionP)
    return createResultCacheMissError(InputID);
  const uint64_t Output =
      reinterpret_cast<const ActionCacheResultT *>(ActionP->Data.data())
          ->load();

  // Return the result.
  Optional<CASID> OutputID = getCASID(InternalRef::getFromRawData(Output));
  if (!OutputID)
    return createResultCacheCorruptError(InputID);
  return *OutputID;
}

Error OnDiskCAS::putCachedResult(CASID InputID, CASID OutputID) {
  // Check that both IDs are valid.
  Optional<InternalRef> InputRef = getInternalRef(InputID);
  if (!InputRef)
    return createUnknownObjectError(InputID);

  Optional<InternalRef> OutputRef = getInternalRef(OutputID);
  if (!OutputRef)
    return createUnknownObjectError(OutputID);

  // Insert Input the result cache.
  //
  // FIXME: Consider templating OnDiskHashMappedTrie (really, renaming it to
  // OnDiskHashMappedTrieBase and adding a type-safe layer on top).
  const uint64_t Expected = OutputRef->getRawData();
  OnDiskHashMappedTrie::pointer ActionP = ActionCache.insertLazy(
      InputID.getHash(), [&](FileOffset TentativeOffset,
                             OnDiskHashMappedTrie::ValueProxy TentativeValue) {
        assert(TentativeValue.Data.size() == sizeof(ActionCacheResultT));
        assert(isAddrAligned(Align::Of<ActionCacheResultT>(),
                             TentativeValue.Data.data()));
        new (TentativeValue.Data.data()) ActionCacheResultT(Expected);
      });
  const uint64_t Observed =
      reinterpret_cast<const ActionCacheResultT *>(ActionP->Data.data())
          ->load();

  if (Expected == Observed)
    return Error::success();

  Optional<CASID> ObservedID = getCASID(InternalRef::getFromRawData(Observed));
  if (!ObservedID)
    return createResultCacheCorruptError(InputID);
  return createResultCachePoisonedError(InputID, OutputID, *ObservedID);
}

Expected<std::unique_ptr<OnDiskCAS>> OnDiskCAS::open(StringRef AbsPath) {
  if (std::error_code EC = sys::fs::create_directories(AbsPath))
    return createFileError(AbsPath, EC);

  const StringRef Slash = sys::path::get_separator();
  constexpr uint64_t MB = 1024ull * 1024ull;
  constexpr uint64_t GB = 1024ull * 1024ull * 1024ull;
  Optional<OnDiskHashMappedTrie> Index;
  if (Error E = OnDiskHashMappedTrie::create(
                    AbsPath + Slash + FilePrefix + IndexFile, IndexTableName,
                    sizeof(HashType) * 8,
                    /*DataSize=*/sizeof(TrieRecord), /*MaxFileSize=*/8 * GB,
                    /*MinFileSize=*/MB)
                    .moveInto(Index))
    return std::move(E);

  Optional<OnDiskDataAllocator> DataPool;
  if (Error E =
          OnDiskDataAllocator::create(
              AbsPath + Slash + FilePrefix + DataPoolFile, DataPoolTableName,
              /*MaxFileSize=*/16 * GB, /*MinFileSize=*/MB)
              .moveInto(DataPool))
    return std::move(E);

  Optional<OnDiskHashMappedTrie> ActionCache;
  if (Error E = OnDiskHashMappedTrie::create(
                    AbsPath + Slash + FilePrefix + ActionCacheFile,
                    ActionCacheTableName, sizeof(HashType) * 8,
                    /*DataSize=*/sizeof(ActionCacheResultT), /*MaxFileSize=*/GB,
                    /*MinFileSize=*/MB)
                    .moveInto(ActionCache))
    return std::move(E);

  return std::unique_ptr<OnDiskCAS>(new OnDiskCAS(AbsPath, std::move(*Index),
                                                  std::move(*DataPool),
                                                  std::move(*ActionCache)));
}

OnDiskCAS::OnDiskCAS(StringRef RootPath, OnDiskHashMappedTrie Index,
                     OnDiskDataAllocator DataPool,
                     OnDiskHashMappedTrie ActionCache)
    : Index(std::move(Index)), DataPool(std::move(DataPool)),
      ActionCache(std::move(ActionCache)), RootPath(RootPath.str()) {
  SmallString<128> Temp = RootPath;
  sys::path::append(Temp, "tmp.");
  TempPrefix = Temp.str().str();
}

// FIXME: Proxy not portable. Maybe also error-prone?
constexpr StringLiteral DefaultDirProxy = "/^llvm::cas::builtin::default";
constexpr StringLiteral DefaultName = "llvm.cas.builtin.default";

void cas::getDefaultOnDiskCASStableID(SmallVectorImpl<char> &Path) {
  Path.assign(DefaultDirProxy.begin(), DefaultDirProxy.end());
  llvm::sys::path::append(Path, DefaultName);
}

std::string cas::getDefaultOnDiskCASStableID() {
  SmallString<128> Path;
  getDefaultOnDiskCASStableID(Path);
  return Path.str().str();
}

void cas::getDefaultOnDiskCASPath(SmallVectorImpl<char> &Path) {
  if (!llvm::sys::path::cache_directory(Path))
    report_fatal_error("cannot get default cache directory");
  llvm::sys::path::append(Path, DefaultName);
}

std::string cas::getDefaultOnDiskCASPath() {
  SmallString<128> Path;
  getDefaultOnDiskCASPath(Path);
  return Path.str().str();
}

Expected<std::unique_ptr<CASDB>> cas::createOnDiskCAS(const Twine &Path) {
  // FIXME: An absolute path isn't really good enough. Should open a directory
  // and use openat() for files underneath.
  SmallString<256> AbsPath;
  Path.toVector(AbsPath);
  sys::fs::make_absolute(AbsPath);

  if (AbsPath == getDefaultOnDiskCASStableID())
    AbsPath = StringRef(getDefaultOnDiskCASPath());

  return OnDiskCAS::open(AbsPath);
}
