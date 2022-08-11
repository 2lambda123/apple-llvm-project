//===- llvm/MC/CAS/MCCASObjectV1.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_CAS_MCCASOBJECTV1_H
#define LLVM_MC_CAS_MCCASOBJECTV1_H

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/CAS/CASDB.h"
#include "llvm/CAS/CASID.h"
#include "llvm/MC/CAS/MCCASFormatSchemaBase.h"
#include "llvm/MC/CAS/MCCASReader.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/Support/Endian.h"

namespace llvm {
namespace mccasformats {
namespace v1 {

class MCSchema;
class MCCASBuilder;
class MCCASReader;

// FIXME: Using the same structure from ObjectV1 from CASObjectFormat.
class MCObjectProxy : public cas::ObjectProxy {
public:
  static Expected<MCObjectProxy> get(const MCSchema &Schema,
                                     Expected<cas::ObjectProxy> Ref);
  StringRef getKindString() const;

  /// Return the data skipping the type-id character.
  StringRef getData() const { return cas::ObjectProxy::getData().drop_front(); }

  const MCSchema &getSchema() const { return *Schema; }

  bool operator==(const MCObjectProxy &RHS) const {
    return Schema == RHS.Schema && cas::CASID(*this) == cas::CASID(RHS);
  }

  MCObjectProxy() = delete;

protected:
  MCObjectProxy(const MCSchema &Schema, const cas::ObjectProxy &Node)
      : cas::ObjectProxy(Node), Schema(&Schema) {}

  class Builder {
  public:
    static Expected<Builder> startRootNode(const MCSchema &Schema,
                                           StringRef KindString);
    static Expected<Builder> startNode(const MCSchema &Schema,
                                       StringRef KindString);

    Expected<MCObjectProxy> build();

  private:
    Error startNodeImpl(StringRef KindString);

    Builder(const MCSchema &Schema) : Schema(&Schema) {}
    const MCSchema *Schema;

  public:
    SmallString<256> Data;
    SmallVector<cas::ObjectRef, 16> Refs;
  };

private:
  const MCSchema *Schema;
};

/// Schema for a DAG in a CAS.
class MCSchema final : public RTTIExtends<MCSchema, MCFormatSchemaBase> {
  void anchor() override;

public:
  static char ID;
  Optional<StringRef> getKindString(const cas::ObjectProxy &Node) const;
  Optional<unsigned char> getKindStringID(StringRef KindString) const;

  cas::ObjectRef getRootNodeTypeID() const { return *RootNodeTypeID; }

  /// Check if \a Node is a root (entry node) for the schema. This is a strong
  /// check, since it requires that the first reference matches a complete
  /// type-id DAG.
  bool isRootNode(const cas::ObjectProxy &Node) const override;

  /// Check if \a Node could be a node in the schema. This is a weak check,
  /// since it only looks up the KindString associated with the first
  /// character. The caller should ensure that the parent node is in the schema
  /// before calling this.
  bool isNode(const cas::ObjectProxy &Node) const override;

  Expected<cas::ObjectProxy> createFromMCAssemblerImpl(
      llvm::MachOCASWriter &ObjectWriter, llvm::MCAssembler &Asm,
      const llvm::MCAsmLayout &Layout, raw_ostream *DebugOS) const override;

  Error serializeObjectFile(cas::ObjectProxy RootNode,
                            raw_ostream &OS) const override;

  MCSchema(cas::CASDB &CAS);

  Expected<MCObjectProxy> create(ArrayRef<cas::ObjectRef> Refs,
                                 StringRef Data) const {
    return MCObjectProxy::get(*this, CAS.createProxy(Refs, Data));
  }
  Expected<MCObjectProxy> get(cas::ObjectRef ID) const {
    return MCObjectProxy::get(*this, CAS.getProxy(ID));
  }

private:
  // Two-way map. Should be small enough for linear search from string to
  // index.
  SmallVector<std::pair<unsigned char, StringRef>, 16> KindStrings;

  // Optional as convenience for constructor, which does not return if it can't
  // fill this in.
  Optional<cas::ObjectRef> RootNodeTypeID;

  // Called by constructor. Not thread-safe.
  Error fillCache();
};

/// A type-checked reference to a node of a specific kind.
template <class DerivedT, class FinalT = DerivedT>
class SpecificRef : public MCObjectProxy {
protected:
  static Expected<DerivedT> get(Expected<MCObjectProxy> Ref) {
    if (auto Specific = getSpecific(std::move(Ref)))
      return DerivedT(*Specific);
    else
      return Specific.takeError();
  }

  static Expected<SpecificRef> getSpecific(Expected<MCObjectProxy> Ref) {
    if (!Ref)
      return Ref.takeError();
    if (Ref->getKindString() == FinalT::KindString)
      return SpecificRef(*Ref);
    return createStringError(inconvertibleErrorCode(),
                             "expected MC object '" + FinalT::KindString + "'");
  }

  static Optional<SpecificRef> Cast(MCObjectProxy Ref) {
    if (Ref.getKindString() == FinalT::KindString)
      return SpecificRef(Ref);
    return None;
  }

  SpecificRef(MCObjectProxy Ref) : MCObjectProxy(Ref) {}
};

#define CASV1_SIMPLE_DATA_REF(RefName, IdentifierName)                         \
  class RefName : public SpecificRef<RefName> {                                \
    using SpecificRefT = SpecificRef<RefName>;                                 \
    friend class SpecificRef<RefName>;                                         \
                                                                               \
  public:                                                                      \
    static constexpr StringLiteral KindString = #IdentifierName;               \
    static Expected<RefName> create(MCCASBuilder &MB, StringRef Data);         \
    static Expected<RefName> get(Expected<MCObjectProxy> Ref);                 \
    static Expected<RefName> get(const MCSchema &Schema, cas::ObjectRef ID) {  \
      return get(Schema.get(ID));                                              \
    }                                                                          \
    static Optional<RefName> Cast(MCObjectProxy Ref) {                         \
      auto Specific = SpecificRefT::Cast(Ref);                                 \
      if (!Specific)                                                           \
        return None;                                                           \
      return RefName(*Specific);                                               \
    }                                                                          \
    Expected<uint64_t> materialize(raw_ostream &OS) const {                    \
      OS << getData();                                                         \
      return getData().size();                                                 \
    }                                                                          \
                                                                               \
  private:                                                                     \
    explicit RefName(SpecificRefT Ref) : SpecificRefT(Ref) {}                  \
  };

#define CASV1_SIMPLE_GROUP_REF(RefName, IdentifierName)                        \
  class RefName : public SpecificRef<RefName> {                                \
    using SpecificRefT = SpecificRef<RefName>;                                 \
    friend class SpecificRef<RefName>;                                         \
                                                                               \
  public:                                                                      \
    static constexpr StringLiteral KindString = #IdentifierName;               \
    static Expected<RefName> create(MCCASBuilder &MB,                          \
                                    ArrayRef<cas::ObjectRef> IDs);             \
    static Expected<RefName> get(Expected<MCObjectProxy> Ref) {                \
      auto Specific = SpecificRefT::getSpecific(std::move(Ref));               \
      if (!Specific)                                                           \
        return Specific.takeError();                                           \
      return RefName(*Specific);                                               \
    }                                                                          \
    static Expected<RefName> get(const MCSchema &Schema, cas::ObjectRef ID) {  \
      return get(Schema.get(ID));                                              \
    }                                                                          \
    static Optional<RefName> Cast(MCObjectProxy Ref) {                         \
      auto Specific = SpecificRefT::Cast(Ref);                                 \
      if (!Specific)                                                           \
        return None;                                                           \
      return RefName(*Specific);                                               \
    }                                                                          \
    Expected<uint64_t> materialize(MCCASReader &Reader) const;                 \
                                                                               \
  private:                                                                     \
    explicit RefName(SpecificRefT Ref) : SpecificRefT(Ref) {}                  \
  };

#define MCFRAGMENT_NODE_REF(MCFragmentName, MCEnumName, MCEnumIdentifier)      \
  class MCFragmentName##Ref : public SpecificRef<MCFragmentName##Ref> {        \
    using SpecificRefT = SpecificRef<MCFragmentName##Ref>;                     \
    friend class SpecificRef<MCFragmentName##Ref>;                             \
                                                                               \
  public:                                                                      \
    static constexpr StringLiteral KindString = #MCEnumIdentifier;             \
    static Expected<MCFragmentName##Ref>                                       \
    create(MCCASBuilder &MB, const MCFragmentName &Fragment,                   \
           unsigned FragmentSize);                                             \
    static Expected<MCFragmentName##Ref> get(Expected<MCObjectProxy> Ref) {    \
      auto Specific = SpecificRefT::getSpecific(std::move(Ref));               \
      if (!Specific)                                                           \
        return Specific.takeError();                                           \
      return MCFragmentName##Ref(*Specific);                                   \
    }                                                                          \
    static Expected<MCFragmentName##Ref> get(const MCSchema &Schema,           \
                                             cas::ObjectRef ID) {              \
      return get(Schema.get(ID));                                              \
    }                                                                          \
    static Optional<MCFragmentName##Ref> Cast(MCObjectProxy Ref) {             \
      auto Specific = SpecificRefT::Cast(Ref);                                 \
      if (!Specific)                                                           \
        return None;                                                           \
      return MCFragmentName##Ref(*Specific);                                   \
    }                                                                          \
    Expected<uint64_t> materialize(MCCASReader &Reader) const;                 \
                                                                               \
  private:                                                                     \
    explicit MCFragmentName##Ref(SpecificRefT Ref) : SpecificRefT(Ref) {}      \
  };
#include "llvm/MC/CAS/MCCASObjectV1.def"

class PaddingRef : public SpecificRef<PaddingRef> {
  using SpecificRefT = SpecificRef<PaddingRef>;
  friend class SpecificRef<PaddingRef>;

public:
  static constexpr StringLiteral KindString = "mc:padding";

  static Expected<PaddingRef> create(MCCASBuilder &MB, uint64_t Size);

  static Expected<PaddingRef> get(Expected<MCObjectProxy> Ref);
  static Expected<PaddingRef> get(const MCSchema &Schema, cas::ObjectRef ID) {
    return get(Schema.get(ID));
  }
  static Optional<PaddingRef> Cast(MCObjectProxy Ref) {
    auto Specific = SpecificRefT::Cast(Ref);
    if (!Specific)
      return None;
    return PaddingRef(*Specific);
  }

  Expected<uint64_t> materialize(raw_ostream &OS) const;

private:
  explicit PaddingRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

class MCAssemblerRef : public SpecificRef<MCAssemblerRef> {
  using SpecificRefT = SpecificRef<MCAssemblerRef>;
  friend class SpecificRef<MCAssemblerRef>;

public:
  static constexpr StringLiteral KindString = "mc:assembler";

  static Expected<MCAssemblerRef> get(Expected<MCObjectProxy> Ref);
  static Expected<MCAssemblerRef> get(const MCSchema &Schema,
                                      cas::ObjectRef ID) {
    return get(Schema.get(ID));
  }

  static Expected<MCAssemblerRef>
  create(const MCSchema &Schema, MachOCASWriter &ObjectWriter, MCAssembler &Asm,
         const MCAsmLayout &Layout, raw_ostream *DebugOS = nullptr);

  Error materialize(raw_ostream &OS) const;

  static Optional<MCAssemblerRef> Cast(MCObjectProxy Ref) {
    auto Specific = SpecificRefT::Cast(Ref);
    if (!Specific)
      return None;
    return MCAssemblerRef(*Specific);
  }

private:
  MCAssemblerRef(SpecificRefT Ref) : SpecificRefT(Ref) {}
};

struct DwarfSectionsCache {
  MCSection *DebugInfo;
  MCSection *Line;
  MCSection *Str;
  MCSection *Abbrev;
};

struct AbbrevAndDebugSplit {
  SmallVector<DebugInfoCURef> CURefs;
  SmallVector<DebugAbbrevRef> AbbrevRefs;
};

/// Queries `Asm` for all dwarf sections and returns an object with (possibly
/// null) pointers to them.
DwarfSectionsCache getDwarfSections(MCAssembler &Asm);

class MCCASBuilder {
public:
  cas::CASDB &CAS;
  MachOCASWriter &ObjectWriter;
  const MCSchema &Schema;
  MCAssembler &Asm;
  const MCAsmLayout &Layout;
  raw_ostream *DebugOS;

  MCCASBuilder(const MCSchema &Schema, MachOCASWriter &ObjectWriter,
               MCAssembler &Asm, const MCAsmLayout &Layout,
               raw_ostream *DebugOS)
      : CAS(Schema.CAS), ObjectWriter(ObjectWriter), Schema(Schema), Asm(Asm),
        Layout(Layout), DebugOS(DebugOS), FragmentOS(FragmentData),
        CurrentContext(&Sections), DwarfSections(getDwarfSections(Asm)) {}

  Error prepare();
  Error buildMachOHeader();
  Error buildFragments();
  Error buildRelocations();
  Error buildDataInCodeRegion();
  Error buildSymbolTable();

  void startGroup();
  Error finalizeGroup();

  void startSection(const MCSection *Sec);
  Error finalizeSection();

  void startAtom(const MCSymbol *Atom);
  Error finalizeAtom();

  void addNode(cas::ObjectProxy Node);
  const MCSymbol *getCurrentAtom() const { return CurrentAtom; }

  Error buildFragment(const MCFragment &F, unsigned FragmentSize);

  ArrayRef<MachO::any_relocation_info> getSectionRelocs() const {
    return SectionRelocs;
  }
  ArrayRef<MachO::any_relocation_info> getAtomRelocs() const {
    return AtomRelocs;
  }

  // Scratch space
  SmallString<8> FragmentData;
  raw_svector_ostream FragmentOS;

private:
  friend class MCAssemblerRef;

  // Helper functions.
  Error createStringSection(StringRef S,
                            std::function<Error(StringRef)> CreateFn);

  // If a DWARF Line Section exists, create a DebugLineRef CAS object per
  // function contribution to the line table.
  Error createLineSection();

  /// If a DWARF Debug Info section exists, create a DebugInfoCURef CAS object
  /// for each compile unit (CU) inside the section, and a DebugAbbrevRef CAS
  /// object for the corresponding abbreviation section.
  /// A pair of vectors with the CAS objects is returned.
  /// The CAS objects appear in the same order as in the object file.
  /// If the section doesn't exist, an empty container is returned.
  Expected<AbbrevAndDebugSplit> splitDebugInfoAndAbbrevSections();

  /// If CURefs is non-empty, create a SectionRef CAS object with edges to all
  /// CURefs. Otherwise, no objects are created and `success` is returned.
  Error createDebugInfoSection(ArrayRef<DebugInfoCURef> CURefs);

  /// If AbbrevRefs is non-empty, create a SectionRef CAS object with edges to all
  /// AbbrevRefs. Otherwise, no objects are created and `success` is returned.
  Error createDebugAbbrevSection(ArrayRef<DebugAbbrevRef> AbbrevRefs);

  /// Split the Dwarf Abbrev section using `AbbrevOffsets` (possibly unsorted)
  /// as the split points for the section, creating one DebugAbbrevRef per
  /// _unique_ offset in the input.
  /// Returns a sequence of DebbugAbbrevRefs, sorted by the order in which they
  /// should appear in the object file.
  Expected<SmallVector<DebugAbbrevRef>>
  splitAbbrevSection(ArrayRef<size_t> AbbrevOffsets);

  struct CUSplit {
    SmallVector<MutableArrayRef<char>> SplitCUData;
    SmallVector<size_t> AbbrevOffsets;
  };
  /// Split the data of the __debug_info section it into multiple pieces, one
  /// per Compile Unit(CU) and return them. The abbreviation offset for each CU
  /// is also returned.
  Expected<CUSplit>
  splitDebugInfoSectionData(MutableArrayRef<char> DebugInfoData);

  // If a DWARF String section exists, create a DebugStrRef CAS object per
  // string in the section.
  Error createDebugStrSection();

  /// If there is any padding between one section and the next, create a
  /// PaddingRef CAS object to represent the bytes of Padding between the two
  /// sections.
  Error createPaddingRef(const MCSection *Sec);

  const MCSection *CurrentSection = nullptr;
  const MCSymbol *CurrentAtom = nullptr;

  SmallVector<cas::ObjectRef> Sections, GroupContext, SectionContext,
      AtomContext;
  SmallVector<cas::ObjectRef> *CurrentContext;

  SmallVector<MachO::any_relocation_info> AtomRelocs;
  SmallVector<MachO::any_relocation_info> SectionRelocs;
  DenseMap<const MCFragment *, std::vector<MachO::any_relocation_info>> RelMap;

  DwarfSectionsCache DwarfSections;
};

class MCCASReader {
public:
  raw_ostream &OS;

  std::vector<std::vector<MachO::any_relocation_info>> Relocations;

  MCCASReader(raw_ostream &OS, const Triple &Target, const MCSchema &Schema);
  support::endianness getEndian() {
    return Target.isLittleEndian() ? support::little : support::big;
  }

  Expected<uint64_t> materializeGroup(cas::ObjectRef ID);
  Expected<uint64_t> materializeSection(cas::ObjectRef ID);
  Expected<uint64_t> materializeAtom(cas::ObjectRef ID);

private:
  const Triple &Target;
  const MCSchema &Schema;
};

} // namespace v1
} // namespace mccasformats
} // namespace llvm

#endif // LLVM_MC_CAS_MCCASOBJECTV1_H
