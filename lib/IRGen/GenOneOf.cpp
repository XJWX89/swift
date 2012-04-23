//===--- GenOneOf.cpp - Swift IR Generation For 'oneof' Types -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for algebraic data types (ADTs,
//  or 'oneof' types) in Swift.  This includes creating the IR type as
//  well as emitting the basic access operations.
//
//  The current scheme is that all such types with are represented
//  with an initial word indicating the variant, followed by a union
//  of all the possibilities.  This is obviously completely acceptable
//  to everyone and will not benefit from further refinement.
//
//  As a completely unimportant premature optimization, we do emit
//  types with only a single variant as simple structs wrapping that
//  variant.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/Basic/Optional.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"

#include "GenType.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LValue.h"
#include "Explosion.h"

#include "GenOneOf.h"

using namespace swift;
using namespace irgen;

namespace {
  /// An abstract base class for TypeInfo implementations of oneof types.
  class OneofTypeInfo : public TypeInfo {
  public:
    OneofTypeInfo(llvm::StructType *T, Size S, Alignment A, IsPOD_t isPOD)
      : TypeInfo(T, S, A, isPOD) {}

    llvm::StructType *getStorageType() const {
      return cast<llvm::StructType>(TypeInfo::getStorageType());
    }

    llvm::IntegerType *getDiscriminatorType() const {
      llvm::StructType *Struct = getStorageType();
      return cast<llvm::IntegerType>(Struct->getElementType(0));
    }

    /// Map the given element to the appropriate value in the
    /// discriminator type.
    llvm::ConstantInt *getDiscriminatorIndex(OneOfElementDecl *target) const {
      // FIXME: using a linear search here is fairly ridiculous.
      unsigned index = 0;
      for (auto elt : cast<OneOfType>(target->getDeclContext())->getElements()) {
        if (elt == target) break;
        index++;
      }
      return llvm::ConstantInt::get(getDiscriminatorType(), index);
    }

    virtual void emitInjectionFunctionBody(IRGenFunction &IGF,
                                           OneOfElementDecl *elt,
                                           Explosion &params) const = 0;
  };

  /// A TypeInfo implementation which uses an aggregate.
  class AggregateOneofTypeInfo : public OneofTypeInfo {
  public:
    AggregateOneofTypeInfo(llvm::StructType *T, Size S, Alignment A,
                           IsPOD_t isPOD)
      : OneofTypeInfo(T, S, A, isPOD) {}

    void getSchema(ExplosionSchema &schema) const {
      schema.add(ExplosionSchema::Element::forAggregate(getStorageType(),
                                                        StorageAlignment));
    }

    unsigned getExplosionSize(ExplosionKind kind) const {
      return 1;
    }

    void load(IRGenFunction &IGF, Address addr, Explosion &e) const {
      // FIXME
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      // FIXME
    }

    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      // FIXME
    }

    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      // FIXME
    }

    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      // FIXME
    }

    void manage(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      // FIXME
    }

    void destroy(IRGenFunction &IGF, Address addr) const {
      // FIXME
    }

    void emitInjectionFunctionBody(IRGenFunction &IGF,
                                   OneOfElementDecl *elt,
                                   Explosion &params) const {
      // FIXME
      params.ignoreAndDestroy(IGF, params.size());
      IGF.Builder.CreateRetVoid();
    }
  };

  /// A TypeInfo implementation for singleton oneofs.
  class SingletonOneofTypeInfo : public OneofTypeInfo {
  public:
    static Address getSingletonAddress(IRGenFunction &IGF, Address addr) {
      llvm::Value *singletonAddr =
        IGF.Builder.CreateStructGEP(addr.getAddress(), 0);
      return Address(singletonAddr, addr.getAlignment());
    }

    /// The type info of the singleton member, or null if it carries no data.
    const TypeInfo *Singleton;

    SingletonOneofTypeInfo(llvm::StructType *T, Size S, Alignment A,
                           IsPOD_t isPOD)
      : OneofTypeInfo(T, S, A, isPOD), Singleton(nullptr) {}

    void getSchema(ExplosionSchema &schema) const {
      assert(isComplete());
      if (Singleton) Singleton->getSchema(schema);
    }

    unsigned getExplosionSize(ExplosionKind kind) const {
      assert(isComplete());
      if (!Singleton) return 0;
      return Singleton->getExplosionSize(kind);
    }

    void load(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!Singleton) return;
      Singleton->load(IGF, getSingletonAddress(IGF, addr), e);
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      if (!Singleton) return;
      Singleton->loadAsTake(IGF, getSingletonAddress(IGF, addr), e);
    }

    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!Singleton) return;
      Singleton->assign(IGF, e, getSingletonAddress(IGF, addr));
    }

    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      if (!Singleton) return;
      Singleton->initialize(IGF, e, getSingletonAddress(IGF, addr));
    }

    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      if (Singleton) Singleton->reexplode(IGF, src, dest);
    }

    void manage(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      if (Singleton) Singleton->manage(IGF, src, dest);
    }

    void destroy(IRGenFunction &IGF, Address addr) const {
      if (Singleton && !isPOD(ResilienceScope::Local))
        Singleton->destroy(IGF, getSingletonAddress(IGF, addr));
    }

    void emitInjectionFunctionBody(IRGenFunction &IGF,
                                   OneOfElementDecl *elt,
                                   Explosion &params) const {
      // If this oneof carries no data, the function must take no
      // arguments and return void.
      if (!Singleton) {
        IGF.Builder.CreateRetVoid();
        return;
      }

      // Otherwise, package up the result.
      ExplosionSchema schema(params.getKind());
      Singleton->getSchema(schema);
      if (schema.requiresIndirectResult()) {
        Address returnSlot(params.claimUnmanagedNext(),
                           Singleton->StorageAlignment);
        initialize(IGF, params, returnSlot);
        IGF.Builder.CreateRetVoid();
      } else {
        IGF.emitScalarReturn(params);
      }
    }
  };

  /// A TypeInfo implementation for oneofs with no payload.
  class EnumTypeInfo : public OneofTypeInfo {
  public:
    EnumTypeInfo(llvm::StructType *T, Size S, Alignment A)
      : OneofTypeInfo(T, S, A, IsPOD) {}

    unsigned getExplosionSize(ExplosionKind kind) const {
      assert(isComplete());
      return 1;
    }

    void getSchema(ExplosionSchema &schema) const {
      assert(isComplete());
      schema.add(ExplosionSchema::Element::forScalar(getDiscriminatorType()));
    }

    static Address projectValue(IRGenFunction &IGF, Address addr) {
      return IGF.Builder.CreateStructGEP(addr, 0, Size(0));
    }

    void load(IRGenFunction &IGF, Address addr, Explosion &e) const {
      assert(isComplete());
      e.addUnmanaged(IGF.Builder.CreateLoad(projectValue(IGF, addr),
                                            addr->getName() + ".load"));
    }

    void loadAsTake(IRGenFunction &IGF, Address addr, Explosion &e) const {
      return EnumTypeInfo::load(IGF, addr, e);
    }

    void assign(IRGenFunction &IGF, Explosion &e, Address addr) const {
      assert(isComplete());
      IGF.Builder.CreateStore(e.claimUnmanagedNext(), projectValue(IGF, addr));
    }

    void initialize(IRGenFunction &IGF, Explosion &e, Address addr) const {
      EnumTypeInfo::assign(IGF, e, addr);
    }

    void reexplode(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      src.transferInto(dest, 1);
    }

    void manage(IRGenFunction &IGF, Explosion &src, Explosion &dest) const {
      src.transferInto(dest, 1);
    }

    void destroy(IRGenFunction &IGF, Address addr) const {}

    void emitInjectionFunctionBody(IRGenFunction &IGF,
                                   OneOfElementDecl *elt,
                                   Explosion &params) const {
      IGF.Builder.CreateRet(getDiscriminatorIndex(elt));
    }
  };
}

const TypeInfo *
TypeConverter::convertOneOfType(IRGenModule &IGM, OneOfType *T) {
  llvm::StructType *convertedStruct = IGM.createNominalType(T->getDecl());

  // We don't need a discriminator if this is a singleton ADT.
  if (T->getElements().size() == 1) {
    SingletonOneofTypeInfo *convertedTInfo =
      new SingletonOneofTypeInfo(convertedStruct, Size(0), Alignment(0), IsPOD);
    assert(!IGM.Types.Converted.count(T));
    IGM.Types.Converted.insert(std::make_pair(T, convertedTInfo));

    Type eltType = T->getElement(0)->getArgumentType();

    llvm::Type *storageType;
    if (eltType.isNull()) {
      storageType = IGM.Int8Ty;
      convertedTInfo->StorageAlignment = Alignment(1);
      convertedTInfo->Singleton = nullptr;
    } else {
      const TypeInfo &eltTInfo = getFragileTypeInfo(IGM, eltType);
      assert(eltTInfo.isComplete());
      storageType = eltTInfo.StorageType;
      convertedTInfo->StorageSize = eltTInfo.StorageSize;
      convertedTInfo->StorageAlignment = eltTInfo.StorageAlignment;
      convertedTInfo->Singleton = &eltTInfo;
      convertedTInfo->setPOD(eltTInfo.isPOD(ResilienceScope::Local));
    }

    llvm::Type *body[] = { storageType };
    convertedStruct->setBody(body);

    return convertedTInfo;
  }

  // Otherwise, we need a discriminator.
  llvm::Type *discriminatorType;
  Size discriminatorSize;
  if (T->getElements().size() == 2) {
    discriminatorType = IGM.Int1Ty;
    discriminatorSize = Size(1);
  } else if (T->getElements().size() <= (1 << 8)) {
    discriminatorType = IGM.Int8Ty;
    discriminatorSize = Size(1);
  } else if (T->getElements().size() <= (1 << 16)) {
    discriminatorType = IGM.Int16Ty;
    discriminatorSize = Size(2);
  } else {
    discriminatorType = IGM.Int32Ty;
    discriminatorSize = Size(4);
  }

  SmallVector<llvm::Type*, 2> body;
  body.push_back(discriminatorType);

  Size payloadSize = Size(0);
  Alignment storageAlignment = Alignment(1);
  IsPOD_t isPOD = IsPOD;

  // Figure out how much storage we need for the union.
  for (auto &elt : T->getElements()) {
    // Ignore variants that carry no data.
    Type eltType = elt->getArgumentType();
    if (eltType.isNull()) continue;

    // Compute layout for the type, and ignore variants with
    // zero-size data.
    const TypeInfo &eltTInfo = getFragileTypeInfo(IGM, eltType);
    assert(eltTInfo.isComplete());
    if (eltTInfo.isEmpty(ResilienceScope::Local)) continue;

    // The required payload size is the amount of padding needed to
    // get up to the element's alignment, plus the actual size.
    Size eltPayloadSize = eltTInfo.StorageSize;
    if (eltTInfo.StorageAlignment.getValue() > discriminatorSize.getValue())
      eltPayloadSize += Size(eltTInfo.StorageAlignment.getValue()
                               - discriminatorSize.getValue());

    payloadSize = std::max(payloadSize, eltPayloadSize);
    storageAlignment = std::max(storageAlignment, eltTInfo.StorageAlignment);
    isPOD &= eltTInfo.isPOD(ResilienceScope::Local);
  }

  Size storageSize = discriminatorSize + payloadSize;

  OneofTypeInfo *convertedTInfo;

  // If there's no payload at all, use the enum TypeInfo.
  if (!payloadSize) {
    assert(isPOD);
    convertedTInfo = new EnumTypeInfo(convertedStruct, storageSize,
                                      storageAlignment);

  // Otherwise, add the payload array to the storage.
  } else {
    body.push_back(llvm::ArrayType::get(IGM.Int8Ty, payloadSize.getValue()));

    // TODO: don't always use an aggregate representation.
    convertedTInfo = new AggregateOneofTypeInfo(convertedStruct, storageSize,
                                                storageAlignment, isPOD);
  }

  convertedStruct->setBody(body);
  return convertedTInfo;
}

void swift::irgen::emitLookThroughOneof(IRGenFunction &IGF,
                                        LookThroughOneofExpr *E,
                                        Explosion &explosion) {
  // For now, the oneof never changes anything.
  IGF.emitRValue(E->getSubExpr(), explosion);
}

namespace {
  class LookThroughOneof : public PhysicalPathComponent {
  public:
    LookThroughOneof() : PhysicalPathComponent(sizeof(LookThroughOneof)) {}

    OwnedAddress offset(IRGenFunction &IGF, OwnedAddress addr) const {
      Address project = SingletonOneofTypeInfo::getSingletonAddress(IGF, addr);
      return OwnedAddress(project, addr.getOwner());
    }
  };
}

LValue swift::irgen::emitLookThroughOneofLValue(IRGenFunction &IGF,
                                                LookThroughOneofExpr *E) {
  LValue oneofLV = IGF.emitLValue(E->getSubExpr());
  oneofLV.add<LookThroughOneof>();
  return oneofLV;
}

Optional<Address>
swift::irgen::tryEmitLookThroughOneofAsAddress(IRGenFunction &IGF,
                                               LookThroughOneofExpr *E) {
  Expr *oneof = E->getSubExpr();
  Optional<Address> oneofAddr =
    IGF.tryEmitAsAddress(oneof, IGF.getFragileTypeInfo(oneof->getType()));
  if (!oneofAddr) return Nothing;

  return SingletonOneofTypeInfo::getSingletonAddress(IGF, oneofAddr.getValue());
}

/// Emit a reference to a oneof element decl.
void swift::irgen::emitOneOfElementRef(IRGenFunction &IGF,
                                       OneOfElementDecl *elt,
                                       Explosion &result) {
  // Find the injection function.
  llvm::Function *injection = IGF.IGM.getAddrOfInjectionFunction(elt);

  // If the element is of function type, just emit this as a function
  // reference.  It will always literally be of function type when
  // written this way.
  if (isa<FunctionType>(elt->getType())) {
    result.addUnmanaged(
               llvm::ConstantExpr::getBitCast(injection, IGF.IGM.Int8PtrTy));
    result.addUnmanaged(IGF.IGM.RefCountedNull);
    return;
  }

  // Otherwise, we need to call the injection function (with no
  // arguments, except maybe a temporary result) and expand the result
  // into the explosion.
  IGF.emitNullaryCall(injection, elt->getType(), result);
}

/// Emit the injection function for the given element.
static void emitInjectionFunction(IRGenModule &IGM,
                                  const OneofTypeInfo &oneofTI,
                                  OneOfElementDecl *elt) {
  // Get or create the injection function.
  llvm::Function *fn = IGM.getAddrOfInjectionFunction(elt);

  ExplosionKind explosionKind = ExplosionKind::Minimal;
  IRGenFunction IGF(IGM, Type(), ArrayRef<Pattern*>(), explosionKind,
                    /*uncurry level*/ 0, fn, Prologue::Bare);

  Explosion explosion = IGF.collectParameters();
  oneofTI.emitInjectionFunctionBody(IGF, elt, explosion);
}

/// emitOneOfType - Emit all the declarations associated with this oneof type.
void IRGenModule::emitOneOfType(OneOfType *oneof) {
  const OneofTypeInfo &typeInfo = getFragileTypeInfo(oneof).as<OneofTypeInfo>();
  for (auto elt : oneof->getElements()) {
    emitInjectionFunction(*this, typeInfo, elt);
  }
}
