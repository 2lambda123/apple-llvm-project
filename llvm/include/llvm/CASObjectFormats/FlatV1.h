//===- llvm/CASObjectFormats/FlatV1.h ---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CASOBJECTFORMATS_FLATV1_H
#define LLVM_CASOBJECTFORMATS_FLATV1_H

#include "llvm/CAS/CASID.h"
#include "llvm/CASObjectFormats/CASObjectReader.h"
#include "llvm/CASObjectFormats/Data.h"
#include "llvm/CASObjectFormats/ObjectFormatSchemaBase.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"

namespace llvm {
namespace casobjectformats {
namespace flatv1 {

using data::Fixup;

class ObjectFileSchema;
class CompileUnitBuilder;
class FlatV1ObjectReader;

/// FIXME: This is a copy from NestedV1 implementation. We should unify that.
class ObjectFormatNodeProxy : public cas::NodeProxy {
public:
  static Expected<ObjectFormatNodeProxy> get(const ObjectFileSchema &Schema,
                                             Expected<cas::NodeProxy> Ref);
  StringRef getKindString() const;

  /// Return the data skipping the type-id character.
  StringRef getData() const { return cas::NodeProxy::getData().drop_front(); }

  const ObjectFileSchema &getSchema() const { return *Schema; }

  bool operator==(const ObjectFormatNodeProxy &RHS) const {
    return Schema == RHS.Schema && cas::CASID(*this) == cas::CASID(RHS);
  }

  ObjectFormatNodeProxy() = delete;

protected:
  ObjectFormatNodeProxy(const ObjectFileSchema &Schema,
                        const cas::NodeProxy &Node)
      : cas::NodeProxy(Node), Schema(&Schema) {}

  class Builder {
  public:
    static Expected<Builder> startRootNode(const ObjectFileSchema &Schema,
                                           StringRef KindString);
    static Expected<Builder> startNode(const ObjectFileSchema &Schema,
                                       StringRef KindString);

    Expected<ObjectFormatNodeProxy> build();

  private:
    Error startNodeImpl(StringRef KindString);

    Builder(const ObjectFileSchema &Schema) : Schema(&Schema) {}
    const ObjectFileSchema *Schema;

  public:
    SmallString<256> Data;
    SmallVector<cas::CASID, 16> IDs;
  };

private:
  const ObjectFileSchema *Schema;
};

/// Schema for a DAG in a CAS.
///
/// The root nodes in the schema are entry points. Currently, that's just \a
/// CompileUnitRef. To recognize that the root node is part of the schema, the
/// first reference is used as a kind of type-id.
///
/// Sub-objects in the schema don't need to spend a reference on a type-id.
/// Instead, the first byte is stolen from \a getData().
///
/// The root node type-id is structured as:
class ObjectFileSchema final
    : public RTTIExtends<ObjectFileSchema, ObjectFormatSchemaBase> {
  void anchor() override;

public:
  static char ID;
  Optional<StringRef> getKindString(const cas::NodeProxy &Node) const;
  Optional<unsigned char> getKindStringID(StringRef KindString) const;

  cas::CASID getRootNodeTypeID() const { return *RootNodeTypeID; }

  /// Check if \a Node is a root (entry node) for the schema. This is a strong
  /// check, since it requires that the first reference matches a complete
  /// type-id DAG.
  bool isRootNode(const cas::NodeProxy &Node) const override;

  /// Check if \a Node could be a node in the schema. This is a weak check,
  /// since it only looks up the KindString associated with the first
  /// character. The caller should ensure that the parent node is in the schema
  /// before calling this.
  bool isNode(const cas::NodeProxy &Node) const override;

  Expected<std::unique_ptr<reader::CASObjectReader>>
  createObjectReader(cas::NodeProxy RootNode) const override;

  Expected<cas::NodeProxy>
  createFromLinkGraphImpl(const jitlink::LinkGraph &G,
                          raw_ostream *DebugOS) const override;

  ObjectFileSchema(cas::CASDB &CAS);

  Expected<ObjectFormatNodeProxy> createNode(ArrayRef<cas::CASID> IDs,
                                             StringRef Data) const {
    return ObjectFormatNodeProxy::get(*this, CAS.createNode(IDs, Data));
  }
  Expected<ObjectFormatNodeProxy> getNode(cas::CASID ID) const {
    return ObjectFormatNodeProxy::get(*this, CAS.getNode(ID));
  }

private:
  // Two-way map. Should be small enough for linear search from string to
  // index.
  SmallVector<std::pair<unsigned char, StringRef>, 16> KindStrings;

  // Optional as convenience for constructor, which does not return if it can't
  // fill this in.
  Optional<cas::CASID> RootNodeTypeID;

  // Called by constructor. Not thread-safe.
  Error fillCache();
};

/// A type-checked reference to a node of a specific kind.
template <class DerivedT, class FinalT = DerivedT>
class SpecificRef : public ObjectFormatNodeProxy {
protected:
  static Expected<DerivedT> get(Expected<ObjectFormatNodeProxy> Ref) {
    if (auto Specific = getSpecific(std::move(Ref)))
      return DerivedT(*Specific);
    else
      return Specific.takeError();
  }

  static Expected<SpecificRef>
  getSpecific(Expected<ObjectFormatNodeProxy> Ref) {
    if (!Ref)
      return Ref.takeError();
    if (Ref->getKindString() == FinalT::KindString)
      return SpecificRef(*Ref);
    return createStringError(inconvertibleErrorCode(),
                             "expected object kind '" + FinalT::KindString +
                                 "'");
  }

  SpecificRef(ObjectFormatNodeProxy Ref) : ObjectFormatNodeProxy(Ref) {}
};

class NameRef : public SpecificRef<NameRef> {
  using SpecificRefT = SpecificRef<NameRef>;
  friend class SpecificRef<NameRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:name";

  StringRef getName() const { return getData(); }

  static Expected<NameRef> create(CompileUnitBuilder &CUB, StringRef Name);
  static Expected<NameRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<NameRef> get(const ObjectFileSchema &Schema, cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

private:
  explicit NameRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class SectionRef : public SpecificRef<SectionRef> {
  using SpecificRefT = SpecificRef<SectionRef>;
  friend class SpecificRef<SectionRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:section";

  static Expected<SectionRef> create(CompileUnitBuilder &CUB,
                                     const jitlink::Section &S);
  static Expected<SectionRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<SectionRef> get(const ObjectFileSchema &Schema,
                                  cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

  Expected<reader::CASSection> materialize() const;

private:
  explicit SectionRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class BlockRef : public SpecificRef<BlockRef> {
  using SpecificRefT = SpecificRef<BlockRef>;
  friend class SpecificRef<BlockRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:block";

  static Expected<BlockRef> create(CompileUnitBuilder &CUB,
                                   const jitlink::Block &Block);

  static Expected<BlockRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<BlockRef> get(const ObjectFileSchema &Schema, cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

  Expected<reader::CASBlock> materializeBlock(const FlatV1ObjectReader &Reader,
                                              unsigned BlockIdx) const;
  Error materializeFixups(
      const FlatV1ObjectReader &Reader, unsigned BlockIdx,
      function_ref<Error(const reader::CASBlockFixup &)> Callback) const;

private:
  explicit BlockRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class BlockContentRef : public SpecificRef<BlockContentRef> {
  using SpecificRefT = SpecificRef<BlockContentRef>;
  friend class SpecificRef<BlockContentRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:block-content";

  static Expected<BlockContentRef> create(CompileUnitBuilder &CUB,
                                          StringRef Content);

  static Expected<BlockContentRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<BlockContentRef> get(const ObjectFileSchema &Schema,
                                       cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

private:
  explicit BlockContentRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class SymbolRef : public SpecificRef<SymbolRef> {
  using SpecificRefT = SpecificRef<SymbolRef>;
  friend class SpecificRef<SymbolRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:symbol";

  static Expected<SymbolRef> create(CompileUnitBuilder &CUB,
                                    const jitlink::Symbol &S);

  static Expected<SymbolRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<SymbolRef> get(const ObjectFileSchema &Schema,
                                 cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

  Expected<reader::CASSymbol> materialize(const FlatV1ObjectReader &Reader,
                                          unsigned SymbolIdx) const;

private:
  explicit SymbolRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

/// An object file / compile unit.
///
/// Note: This wrapper eagerly parses target triple, pointer size, and
/// endianness.
///
class CompileUnitRef : public SpecificRef<CompileUnitRef> {
  using SpecificRefT = SpecificRef<CompileUnitRef>;
  friend class SpecificRef<CompileUnitRef>;

public:
  static constexpr StringLiteral KindString = "cas.o:compile-unit";

  static Expected<CompileUnitRef> get(Expected<ObjectFormatNodeProxy> Ref);
  static Expected<CompileUnitRef> get(const ObjectFileSchema &Schema,
                                      cas::CASID ID) {
    return get(Schema.getNode(ID));
  }

  static Expected<CompileUnitRef> create(const ObjectFileSchema &Schema,
                                         const jitlink::LinkGraph &G,
                                         raw_ostream *DebugOS = nullptr);

  Error materialize(FlatV1ObjectReader &OR) const;

private:
  CompileUnitRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class CompileUnitBuilder {
public:
  cas::CASDB &CAS;
  const ObjectFileSchema &Schema;
  Triple TT;
  raw_ostream *DebugOS;

  CompileUnitBuilder(const ObjectFileSchema &Schema, Triple Target,
                     raw_ostream *DebugOS)
      : CAS(Schema.CAS), Schema(Schema), TT(Target), DebugOS(DebugOS) {}

  Error createAndReferenceName(StringRef S);
  Error createAndReferenceContent(StringRef Content);

  Error createSection(const jitlink::Section &S);
  Error createBlock(const jitlink::Block &B);
  Error createSymbol(const jitlink::Symbol &S);

  // Lookup functions. The result must exist in the cache already.
  Expected<unsigned> getSectionIndex(const jitlink::Section &S);
  Expected<unsigned> getBlockIndex(const jitlink::Block &B);
  Expected<unsigned> getSymbolIndex(const jitlink::Symbol &S);

  // Encode the index into compile unit.
  void encodeIndex(unsigned Index);

private:
  friend class CompileUnitRef;

  unsigned recordNode(const ObjectFormatNodeProxy &Ref);
  unsigned commitNode(const ObjectFormatNodeProxy &Ref);
  void pushNodes();

  // Cache all the CASID created.
  DenseMap<cas::CASID, unsigned> CASIDMap;
  std::vector<cas::CASID> IDs;

  // Index Storage.
  std::vector<unsigned> Indexes;
  SmallVector<unsigned, 16> LocalIndexStorage;

  // Lookup cache.
  SmallVector<const jitlink::Section *, 16> Sections;
  SmallVector<const jitlink::Symbol *, 16> Symbols;
  SmallVector<const jitlink::Block *, 16> Blocks;
  std::vector<unsigned> BlockIndexStarts;

  // Temp storage.
  SmallString<256> InlineBuffer;
};

class FlatV1ObjectReader final : public reader::CASObjectReader {
public:
  FlatV1ObjectReader(CompileUnitRef Root) : Root(std::move(Root)) {}

  Triple getTargetTriple() const override { return TT; }

  Error forEachSymbol(
      function_ref<Error(reader::CASSymbolRef, reader::CASSymbol)> Callback)
      const override;

  Expected<reader::CASSection>
  materialize(reader::CASSectionRef Ref) const override;
  Expected<reader::CASBlock>
  materialize(reader::CASBlockRef Ref) const override;
  Expected<reader::CASSymbol>
  materialize(reader::CASSymbolRef Ref) const override;

  Error materializeFixups(reader::CASBlockRef Ref,
                          function_ref<Error(const reader::CASBlockFixup &)>
                              Callback) const override;

  Error materializeCompileUnit();

  Expected<SectionRef> getSectionNode(unsigned SectionI) const {
    unsigned IdxI = getSectionDataIndexOffset(SectionI);
    auto IDI = getIdx(IdxI);
    if (!IDI)
      return IDI.takeError();
    return getNode<SectionRef>(*IDI);
  }

  Expected<unsigned> getBlockDataIndex(unsigned BlockI,
                                       unsigned OffsetI) const {
    unsigned IdxI = getBlockDataIndexOffset(BlockI, OffsetI);
    return getIdx(IdxI);
  }

  template <typename RefT>
  Expected<RefT> getBlockNode(unsigned BlockI, unsigned OffsetI) const {
    auto IDI = getBlockDataIndex(BlockI, OffsetI);
    if (!IDI)
      return IDI.takeError();
    return getNode<RefT>(*IDI);
  }

  Expected<unsigned> getSymbolDataIndex(unsigned SymbolI,
                                        unsigned OffsetI) const {
    unsigned IdxI = getSymbolDataIndexOffset(SymbolI, OffsetI);
    return getIdx(IdxI);
  }

  template <typename RefT>
  Expected<RefT> getSymbolNode(unsigned SymbolI, unsigned OffsetI) const {
    auto IDI = getSymbolDataIndex(SymbolI, OffsetI);
    if (!IDI)
      return IDI.takeError();
    return getNode<RefT>(*IDI);
  }

  bool hasIndirectSymbolNames() const { return HasIndirectSymbolNames; }
  bool hasBlockContentNodes() const { return HasBlockContentNodes; }
  bool hasInlinedSymbols() const { return HasInlinedSymbols; }

  unsigned getSectionsSize() const { return SectionsSize; };
  unsigned getSymbolsSize() const { return SymbolsSize; };
  unsigned getBlocksSize() const { return BlocksSize; };

private:
  friend class CompileUnitRef;

  unsigned getSectionDataIndexOffset(unsigned SectionI) const {
    return SectionI;
  }

  unsigned getBlockDataIndexOffset(unsigned BlockI, unsigned OffsetI) const {
    return BlockIdxs[BlockI] + OffsetI;
  }

  unsigned getSymbolDataIndexOffset(unsigned SymbolI, unsigned OffsetI) const {
    unsigned DefinedSymbolSize = HasIndirectSymbolNames ? 3 : 2;
    if (SymbolI < DefinedSymbolsSize) {
      return SectionsSize + (SymbolI * DefinedSymbolSize) + OffsetI;
    } else {
      return SectionsSize + (DefinedSymbolsSize * DefinedSymbolSize) +
             ((SymbolI - DefinedSymbolsSize) * (DefinedSymbolSize - 1)) +
             OffsetI;
    }
  }

  Expected<unsigned> getIdx(unsigned I) const {
    if (I >= Indexes.size())
      return createStringError(inconvertibleErrorCode(), "Index out of bound");

    return Indexes[I];
  }

  Expected<cas::CASID> getID(unsigned I) const {
    // First ID is the RootTypeID
    auto IDIndex = I + 1;
    if (IDIndex >= IDs.size())
      return createStringError(inconvertibleErrorCode(),
                               "ID Index out of bound");

    return IDs[IDIndex];
  }

  template <typename RefT> Expected<RefT> getNode(unsigned I) const {
    auto ID = getID(I);
    if (!ID)
      return ID.takeError();
    return RefT::get(Root.getSchema(), *ID);
  }

  CompileUnitRef Root;

  Triple TT;

  bool HasIndirectSymbolNames;
  bool HasBlockContentNodes;
  bool HasInlinedSymbols;

  std::vector<cas::CASID> IDs;
  std::vector<unsigned> Indexes;

  unsigned SectionsSize;
  unsigned SymbolsSize;
  unsigned BlocksSize;
  unsigned DefinedSymbolsSize;

  SmallVector<unsigned, 16> BlockIdxs;

  StringRef InlineBuffer;
};

} // namespace flatv1
} // namespace casobjectformats
} // namespace llvm

#endif // LLVM_CASOBJECTFORMATS_FLATV1_H
