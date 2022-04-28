//===- llvm/CAS/CASReference.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CAS_CASREFERENCE_H
#define LLVM_CAS_CASREFERENCE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {

class raw_ostream;

namespace cas {

class CASDB;

namespace testing_helpers {
class HandleFactory;
}

class ObjectHandle;
class ObjectRef;

/// Base class for references to things in \a CASDB.
class ReferenceBase {
public:
  /// Get an internal reference.
  uint64_t getInternalRef(const CASDB &ExpectedCAS) const {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(CAS == &ExpectedCAS && "Extracting reference for the wrong CAS");
#endif
    return InternalRef;
  }

protected:
  void print(raw_ostream &OS, const ObjectHandle &This) const;
  void print(raw_ostream &OS, const ObjectRef &This) const;

  bool hasSameInternalRef(const ReferenceBase &RHS) const {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    assert(CAS == RHS.CAS && "Cannot compare across CAS instances");
#endif
    return InternalRef == RHS.InternalRef;
  }

protected:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  ReferenceBase(const CASDB *CAS, uint64_t InternalRef, bool IsHandle)
      : InternalRef(InternalRef) {
#if LLVM_ENABLE_ABI_BREAKING_CHECKS
    this->CAS = CAS;
#endif
  }

public:
  enum HandleKind {
    TreeKind = 0x1,
    NodeKind = 0x2,
    BlobKind = 0x4,
    AnyDataKind = NodeKind | BlobKind,
    AnyObjectKind = AnyDataKind | TreeKind,
  };

private:
  friend class AnyObjectHandle;
  template <class HandleT, HandleKind> friend class AnyObjectHandleImpl;
  struct AnyObjectHandleTag {};
  ReferenceBase(AnyObjectHandleTag, ReferenceBase Other)
      : ReferenceBase(Other) {}

  uint64_t InternalRef;

#if LLVM_ENABLE_ABI_BREAKING_CHECKS
  const CASDB *CAS;
#endif
};

/// Reference to an object in a \a CASDB instance.
///
/// If you have an ObjectRef, you know the object exists, and you can point at
/// it from new nodes with \a CASDB::storeNode(), but you don't know anything
/// about it. "Loading" the object is a separate step that may not have
/// happened yet, and which can fail (due to filesystem corruption) or
/// introduce latency (if downloading from a remote store).
///
/// \a CASDB::storeNode() takes a list of these, and these are returned by \a
/// CASDB::forEachRef() and \a CASDB::readRef(), which are accessors for nodes,
/// and \a CASDB::getReference().
///
/// \a CASDB::loadObject() will load the referenced object, and returns \a
/// AnyObjectHandle, a variant that knows what kind of entity it is. \a
/// CASDB::getReferenceKind() can expect the type of reference without asking
/// for unloaded objects to be loaded.
///
/// This is a wrapper around a \c uint64_t (and a \a CASDB instance when
/// assertions are on). If necessary, it can be deconstructed and reconstructed
/// using \a Reference::getInternalRef() and \a
/// Reference::getFromInternalRef(), but clients aren't expected to need to do
/// this. These both require the right \a CASDB instance.
class ObjectRef : public ReferenceBase {
public:
  friend bool operator==(const ObjectRef &LHS, const ObjectRef &RHS) {
    return LHS.hasSameInternalRef(RHS);
  }
  friend bool operator!=(const ObjectRef &LHS, const ObjectRef &RHS) {
    return !(LHS == RHS);
  }

  /// Allow a reference to be recreated after it's deconstructed.
  static ObjectRef getFromInternalRef(const CASDB &CAS, uint64_t InternalRef) {
    return ObjectRef(CAS, InternalRef);
  }

  /// Print internal ref and/or CASID. Only suitable for debugging.
  void print(raw_ostream &OS) const { return ReferenceBase::print(OS, *this); }

  LLVM_DUMP_METHOD void dump() const;

private:
  friend class CASDB;
  friend class AnyObjectHandle;
  friend class ReferenceBase;
  friend class testing_helpers::HandleFactory;
  using ReferenceBase::ReferenceBase;
  ObjectRef(const CASDB &CAS, uint64_t InternalRef)
      : ReferenceBase(&CAS, InternalRef, /*IsHandle=*/false) {}
  ObjectRef(ReferenceBase) = delete;
};

/// Handle to a loaded object in a \a CASDB instance.
///
/// ObjectHandle encapulates a *loaded* object in the CAS. You need one
/// of these to inspect the content of an object: to look at its stored
/// data and references.
///
/// In practice, right now you really need/want \a NodeHandle, \a TreeHandle,
/// \a BlobHandle, or one of the variants \a AnyObjectHandle and \a
/// AnyDataHandle.
///
/// TODO: Remove all subclasses (merge with \a NodeHandle) once trees and blobs
/// are gone.
class ObjectHandle : public ReferenceBase {
public:
  friend bool operator==(const ObjectHandle &LHS, const ObjectHandle &RHS) {
    return LHS.hasSameInternalRef(RHS);
  }
  friend bool operator!=(const ObjectHandle &LHS, const ObjectHandle &RHS) {
    return !(LHS == RHS);
  }

  /// Print internal ref and/or CASID. Only suitable for debugging.
  void print(raw_ostream &OS) const { return ReferenceBase::print(OS, *this); }

  LLVM_DUMP_METHOD void dump() const;

private:
  friend class CASDB;
  friend class AnyObjectHandle;
  friend class ReferenceBase;
  friend class testing_helpers::HandleFactory;
  using ReferenceBase::ReferenceBase;
  ObjectHandle(ReferenceBase) = delete;
  ObjectHandle(const CASDB &CAS, uint64_t InternalRef)
      : ReferenceBase(&CAS, InternalRef, /*IsHandle=*/true) {}
  template <class HandleT, HandleKind> friend class AnyObjectHandleImpl;
  static constexpr HandleKind getHandleKind() { return AnyObjectKind; }
};

class AnyDataHandle;

/// Handle to a loaded blob in \a CASDB.
class BlobHandle : public ObjectHandle {
public:
  inline AnyDataHandle getData() const;

private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  BlobHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyObjectHandleImpl;
  static constexpr HandleKind getHandleKind() { return BlobKind; }
};

/// Handle to a loaded node in \a CASDB.
class NodeHandle : public ObjectHandle {
public:
  inline AnyDataHandle getData() const;

private:
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  NodeHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyObjectHandleImpl;
  static constexpr HandleKind getHandleKind() { return NodeKind; }
};

/// Handle to a loaded tree in \a CASDB.
class TreeHandle : public ObjectHandle {
  friend class CASDB;
  friend class testing_helpers::HandleFactory;
  using ObjectHandle::ObjectHandle;
  TreeHandle(ObjectHandle) = delete;
  template <class HandleT, HandleKind> friend class AnyObjectHandleImpl;
  static constexpr HandleKind getHandleKind() { return TreeKind; }
};

/// Type-safe variant between all non-variant subclasses of \a Handle.
/// Besides the types accepted by \a AnyObjectHandle, this could also be a \a
/// or an unadorned \a Handle.
template <class HandleBaseT, ObjectHandle::HandleKind BaseKind>
class AnyObjectHandleImpl : public HandleBaseT {
public:
  template <class HandleT,
            std::enable_if_t<std::is_base_of<HandleBaseT, HandleT>::value &&
                                 (HandleT::getHandleKind() & BaseKind),
                             bool> = false>
  AnyObjectHandleImpl(HandleT H)
      : AnyObjectHandleImpl(H, HandleT::getHandleKind()) {}

  template <class HandleT,
            std::enable_if_t<std::is_base_of<HandleBaseT, HandleT>::value &&
                                 (HandleT::getHandleKind() & BaseKind),
                             bool> = false>
  bool is() const {
    constexpr auto NewKind = HandleT::getHandleKind();
    if (Kind == NewKind)
      return true;
    // Check for NewKind as a base class of Kind. E.g., NodeKind < ObjectKind.
    return (Kind & NewKind) && Kind < NewKind;
  }
  template <class HandleT> HandleT get() const {
    assert(is<HandleT>() && "Expected kind to match");
    return HandleT(typename HandleBaseT::AnyObjectHandleTag{}, *this);
  }
  template <class HandleT> Optional<HandleT> dyn_cast() const {
    if (!is<HandleT>())
      return None;
    return get<HandleT>();
  }

protected:
  using HandleKind = typename HandleBaseT::HandleKind;
  using HandleBaseT::HandleBaseT;
  AnyObjectHandleImpl(HandleBaseT H, HandleKind Kind)
      : HandleBaseT(H), Kind(Kind) {}
  HandleKind Kind;
};

/// Type-safe variant between \a NodeHandle, \a BlobHandle, and \a
/// RawDataHandle.
class AnyDataHandle
    : public AnyObjectHandleImpl<ObjectHandle, ObjectHandle::AnyDataKind> {
  friend class AnyObjectHandle;

public:
  using AnyObjectHandleImpl::AnyObjectHandleImpl;

private:
  friend class BlobHandle;
  friend class NodeHandle;
  AnyDataHandle(BlobHandle H) : AnyObjectHandleImpl::AnyObjectHandleImpl(H) {}
  AnyDataHandle(NodeHandle H) : AnyObjectHandleImpl::AnyObjectHandleImpl(H) {}
};

inline AnyDataHandle BlobHandle::getData() const {
  return AnyDataHandle(*this);
}
inline AnyDataHandle NodeHandle::getData() const {
  return AnyDataHandle(*this);
}

/// Type-safe variant between \a Handle, \a DataHandle, and \a AnyObjectHandle.
class AnyObjectHandle
    : public AnyObjectHandleImpl<ObjectHandle, ObjectHandle::AnyObjectKind> {
public:
  using AnyObjectHandleImpl::AnyObjectHandleImpl::AnyObjectHandleImpl;

  Optional<AnyDataHandle> getData() const {
    if (Kind & AnyDataKind)
      return AnyDataHandle(
          ObjectHandle(ObjectHandle::AnyObjectHandleTag{}, *this), Kind);
    return None;
  }
};

} // namespace cas
} // namespace llvm

#endif // LLVM_CAS_CASREFERENCE_H
